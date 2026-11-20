/* Guarded stack of the Application Executor (M9.1).
 *
 * Native applications run their callbacks on the executor's stack. Until
 * M9 that was a 12 KiB internal-RAM stack: an application recursing past its
 * end wrote into neighbouring heap blocks, and the FreeRTOS canary noticed
 * only at a later context switch, if at all (docs/elf-runtime-hardening-m9-1.md).
 *
 * Layout, ascending, in one 128 KiB PSRAM block aligned to the 64 KiB MMU page:
 *
 *   [guard_low, stack_low)   64 KiB guard page, unmapped in the MMU
 *   [stack_low, limit)       40 KiB red zone, mapped
 *   [limit, stack_high)      24 KiB usable stack
 *
 * Early detection: the ESP32-S3 assist-debug stack-pointer monitor of the
 * executor's core raises an interrupt when SP drops below `limit`. The red
 * zone is larger than the largest frame one Xtensa ENTRY can allocate
 * (32760 B) plus the interrupt frame pushed on the interrupted stack, so the
 * stack pointer, and every write relative to it, is still mapped when the
 * monitor fires and the panic handler can run.
 * Last line: a frame larger than the red zone (alloca/VLA) or a monitor
 * masked by an interrupt level >= 3 reaches the unmapped guard page instead
 * of other memory, as long as the first write lands within the 64 KiB guard.
 * One SP drop beyond red zone plus guard (about 104 KiB, alloca/VLA) skips
 * the guard and is caught only by the monitor, possibly after writes. The
 * guard-access fault path (cache error panic) is not yet tested on a board.
 *
 * Neither is memory isolation: the application still shares the address
 * space and can write anywhere through a pointer. */
#include "executor_stack.h"
#include <inttypes.h>
#include <stdint.h>
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_private/esp_cache_private.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "hal/cache_hal.h"
#include "hal/mmu_hal.h"
#include "soc/assist_debug_reg.h"
#include "soc/extmem_reg.h"
#include "soc/interrupts.h"
#include "soc/soc.h"
#include "xtensa_context.h"
#include "roundie_elf_crash.h"
#include "sdkconfig.h"

#define GUARD_BYTES ((uint32_t)CONFIG_MMU_PAGE_SIZE)
#define STACK_BYTES 65536U
#define RED_ZONE_BYTES 40960U
#define EXECUTOR_CORE 1
#define EXECUTOR_PRIORITY 1

_Static_assert(GUARD_BYTES == 65536U, "the guard is one 64 KiB MMU page");
_Static_assert(RED_ZONE_BYTES >= 32760U + 4096U, "red zone: largest ENTRY frame plus interrupt frame and slack");
_Static_assert(RED_ZONE_BYTES < STACK_BYTES, "usable stack above the red zone");

static const char *TAG = "elf_stack";

static StaticTask_t executor_tcb; /* internal DRAM, as FreeRTOS requires */
static uint8_t *block;            /* never freed once the guard is unmapped */
static roundie_elf_crash_stack_t layout;
static TaskHandle_t executor_task;
static intr_handle_t monitor_interrupt;
static volatile bool self_test;
static volatile uint32_t self_test_hits;
static volatile uint32_t self_test_pc;

static void IRAM_ATTR NOINLINE_ATTR unmap_guard(uint32_t guard)
{
    /* As esp_mmu_unmap(): caches frozen and interrupts off while the entry
     * changes, then drop any line of the page that the cache still holds. */
    esp_cache_freeze_caches_disable_interrupts();
    mmu_hal_unmap_region(0, guard, GUARD_BYTES);
    esp_cache_sync_ops_enter_critical_section();
    cache_hal_invalidate_addr(guard, GUARD_BYTES);
    esp_cache_sync_ops_exit_critical_section();
    esp_cache_unfreeze_caches_enable_interrupts();
}

static bool prepare_block(void)
{
    if (block != NULL) {
        return true;
    }
    uint8_t *candidate = heap_caps_aligned_alloc(GUARD_BYTES, GUARD_BYTES + STACK_BYTES,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (candidate == NULL) {
        ESP_LOGE(TAG, "no 64 KiB-aligned PSRAM block for the executor stack");
        return false;
    }
    uint32_t guard = (uint32_t)candidate;
    uint32_t physical = 0;
    mmu_target_t target;
    if (!esp_ptr_external_ram(candidate) || guard % GUARD_BYTES != 0U ||
        !mmu_hal_vaddr_to_paddr(0, guard, &physical, &target)) {
        ESP_LOGE(TAG, "executor stack block %p is not a mapped PSRAM page", (void *)candidate);
        heap_caps_free(candidate);
        return false;
    }
    /* Write back and drop the page's cached lines while it is still mapped:
     * no dirty line may later be evicted to an unmapped address. */
    if (esp_cache_msync(candidate, GUARD_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE) !=
        ESP_OK) {
        ESP_LOGE(TAG, "cache write-back of the guard page failed");
        heap_caps_free(candidate);
        return false;
    }
    unmap_guard(guard);
    block = candidate; /* from here on, never back to the heap */
    bool guard_mapped = mmu_hal_vaddr_to_paddr(0, guard, &physical, &target);
    bool stack_mapped = mmu_hal_vaddr_to_paddr(0, guard + GUARD_BYTES, &physical, &target);
    if (guard_mapped || !stack_mapped) {
        ESP_LOGE(TAG, "guard page unmap check failed: guard=%d stack=%d", guard_mapped, stack_mapped);
        return false;
    }
    layout = (roundie_elf_crash_stack_t){
        .guard_low = guard,
        .stack_low = guard + GUARD_BYTES,
        .limit = guard + GUARD_BYTES + RED_ZONE_BYTES,
        .stack_high = guard + GUARD_BYTES + STACK_BYTES,
        .armed = 0,
    };
    roundie_elf_crash_executor(&layout);
    return true;
}

bool roundie_elf_stack_create(TaskFunction_t entry, TaskHandle_t *task)
{
    if (!prepare_block() || layout.stack_high == 0U) {
        return false;
    }
    executor_task = xTaskCreateStaticPinnedToCore(entry, "rep_executor", STACK_BYTES, NULL, EXECUTOR_PRIORITY,
                                                  (StackType_t *)(block + GUARD_BYTES), &executor_tcb,
                                                  EXECUTOR_CORE);
    *task = executor_task;
    ESP_LOGI(TAG, "guard=%08" PRIx32 " stack=%08" PRIx32 " limit=%08" PRIx32 " top=%08" PRIx32 " task=%p",
             layout.guard_low, layout.stack_low, layout.limit, layout.stack_high, (void *)executor_task);
    return executor_task != NULL;
}

static inline void monitor_disable(void)
{
    REG_CLR_BIT(ASSIST_DEBUG_CORE_1_INTERRUPT_ENA_REG, ASSIST_DEBUG_CORE_1_SP_SPILL_MIN_ENA);
    REG_WRITE(ASSIST_DEBUG_CORE_1_INTERRUPT_CLR_REG, ASSIST_DEBUG_CORE_1_SP_SPILL_MIN_CLR);
}

/* The stack pointer the executor had when this interrupt was taken.
 * _frxt_int_enter stores the interrupted SP, which is also the base of the
 * saved interrupt frame, in pxTopOfStack (the TCB's first member) before it
 * switches to the interrupt stack; XtExcFrame.a1 holds the SP of the
 * interrupted code. 0: the frame lies in the guard page (an overflow). */
static uint32_t IRAM_ATTR interrupted_sp(void)
{
    uint32_t frame = (uint32_t)executor_tcb.pxDummy1;
    if (frame < layout.stack_low || frame + sizeof(XtExcFrame) > layout.stack_high) {
        return 0;
    }
    return (uint32_t)((const XtExcFrame *)frame)->a1;
}

/* SP_SPILL_MIN: the monitored core's "SP" went below SP_MIN. The handler
 * runs on the interrupt stack (internal RAM); the interrupted executor frame
 * lies in the mapped red zone.
 *
 * Measured on the 1.85B (0.3.58): the monitor also fires while no stack
 * pointer is below the limit, in the executor during a context switch from
 * a blocking lock (monitor PC in vPortEnterCritical). Hypothesis: the
 * comparator sees transient a1 values of rotated register windows. So the
 * trigger alone is not trusted: the handler aborts only if the executor is
 * the interrupted task and its saved SP is below the limit; other hits are
 * counted, cleared and the monitor is re-enabled. A real overflow that the
 * check misses (SP back above the limit before a masked interrupt is taken)
 * stayed in the mapped red zone; one that goes further reaches the guard. */
static void IRAM_ATTR monitor_isr(void *unused)
{
    (void)unused;
    uint32_t pc = REG_READ(ASSIST_DEBUG_CORE_1_SP_PC_REG);
    monitor_disable();
    if (self_test) {
        self_test_pc = pc;
        self_test_hits++;
        return;
    }
    if (xTaskGetCurrentTaskHandleForCore(EXECUTOR_CORE) == executor_task) {
        uint32_t sp = interrupted_sp();
        if (sp < layout.limit) {
            roundie_elf_crash_monitor(pc, true);
            esp_system_abort("rep_executor stack limit crossed");
        }
    }
    roundie_elf_crash_monitor_spurious();
    REG_SET_BIT(ASSIST_DEBUG_CORE_1_INTERRUPT_ENA_REG, ASSIST_DEBUG_CORE_1_SP_SPILL_MIN_ENA);
}

static void monitor_enable(uint32_t limit)
{
    REG_WRITE(ASSIST_DEBUG_CORE_1_SP_MAX_REG, UINT32_MAX);
    REG_WRITE(ASSIST_DEBUG_CORE_1_SP_MIN_REG, limit);
    REG_WRITE(ASSIST_DEBUG_CORE_1_INTERRUPT_CLR_REG, ASSIST_DEBUG_CORE_1_SP_SPILL_MIN_CLR);
    /* RLS gates the interrupt signal, ENA the monitor itself (as in the IDF
     * RISC-V assist_debug_ll: interrupt enable vs. monitor enable). */
    REG_SET_BIT(ASSIST_DEBUG_CORE_1_INTERRUPT_RLS_REG, ASSIST_DEBUG_CORE_1_SP_SPILL_MIN_RLS);
    REG_SET_BIT(ASSIST_DEBUG_CORE_1_INTERRUPT_ENA_REG, ASSIST_DEBUG_CORE_1_SP_SPILL_MIN_ENA);
}

/* A call with its own frame: ENTRY lowers SP by at least 288 bytes. */
static void NOINLINE_ATTR probe_frame(void)
{
    volatile uint8_t pad[256];
    pad[0] = 1;
    pad[sizeof(pad) - 1] = pad[0];
}

/* Nested calls deeper than the 16 physical register windows, so the descent
 * runs through window-overflow exceptions as an application recursion does. */
static uint32_t NOINLINE_ATTR probe_depth(uint32_t depth)
{
    volatile uint8_t pad[64];
    pad[0] = (uint8_t)depth;
    if (depth == 0U) {
        return pad[0];
    }
    return probe_depth(depth - 1U) + pad[0];
}

#define PROBE_DEPTH 24U
#define MONITOR_SP_UNSTABLE 10U /* middle of the measured 7..12 window */
#define PROBE_DEPTH_DROP 1024U /* far less than PROBE_DEPTH frames of >= 96 bytes */

/* Three checks with the abort suppressed:
 *  1. one frame below SP_MIN fires, and the latched PC lies in that frame's
 *     function, so the hit is that frame and not one of the unrelated
 *     triggers measured on the 1.85B (monitor_isr);
 *  2. a recursion through window overflows below SP_MIN fires;
 *  3. at the real limit, neither call fires within the window (a hit of
 *     the unrelated triggers, about 40/s, fails it rarely, and closed).
 * The TRM gives no text for the monitor; this test is the evidence on each
 * boot, and a failure leaves native applications disabled. */
static bool self_test_monitor(uint32_t limit)
{
    uint32_t sp = (uint32_t)esp_cpu_get_sp();
    self_test_hits = 0;
    self_test_pc = 0;
    self_test = true;
    monitor_enable(sp - 64U);
    probe_frame();
    esp_rom_delay_us(20);
    monitor_disable();
    uint32_t positive = self_test_hits;
    uint32_t latched = self_test_pc;
    bool in_probe = latched - (uint32_t)&probe_frame < 64U;
    self_test_hits = 0;
    monitor_enable(sp - PROBE_DEPTH_DROP);
    (void)probe_depth(PROBE_DEPTH);
    esp_rom_delay_us(20);
    monitor_disable();
    uint32_t deep = self_test_hits;
    self_test_hits = 0;
    monitor_enable(limit);
    probe_frame();
    (void)probe_depth(PROBE_DEPTH);
    esp_rom_delay_us(20);
    uint32_t negative = self_test_hits;
    if (positive == 0U || !in_probe || deep == 0U || negative != 0U) {
        monitor_disable();
        self_test = false;
        ESP_LOGE(TAG,
                 "monitor self-test failed: hits=%" PRIu32 " pc=%08" PRIx32 " probe=%08" PRIx32 " deep=%" PRIu32
                 " spurious=%" PRIu32,
                 positive, latched, (uint32_t)&probe_frame, deep, negative);
        return false;
    }
    /* Armed: from here a hit with the executor's SP below the limit aborts. */
    self_test = false;
    ESP_LOGI(TAG, "monitor self-test passed: hits=%" PRIu32 " pc=%08" PRIx32 " deep=%" PRIu32, positive, latched,
             deep);
    return true;
}

bool roundie_elf_stack_arm(void)
{
    uint32_t sp = (uint32_t)esp_cpu_get_sp();
    if (layout.stack_high == 0U || esp_cpu_get_core_id() != EXECUTOR_CORE ||
        xTaskGetCurrentTaskHandle() != executor_task || sp <= layout.limit || sp > layout.stack_high) {
        ESP_LOGE(TAG, "arm: not on the guarded executor stack (core %d sp %08" PRIx32 ")",
                 esp_cpu_get_core_id(), sp);
        return false;
    }
    /* Autoload may prefetch lines past an access; recorded for the guard
     * page analysis (PENDING in the M9.1 document). */
    ESP_LOGI(TAG, "dcache autoload ctrl=%08" PRIx32, REG_READ(EXTMEM_DCACHE_AUTOLOAD_CTRL_REG));
    if (monitor_interrupt == NULL) {
        esp_err_t error = esp_intr_alloc(ETS_ASSIST_DEBUG_INTR_SOURCE, ESP_INTR_FLAG_LEVEL3, monitor_isr, NULL,
                                         &monitor_interrupt);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "monitor interrupt: %s", esp_err_to_name(error));
            monitor_interrupt = NULL;
            return false;
        }
    }
    /* SP_UNSTABLE is not reset by a software restart (the bootloader only
     * enables the assist-debug clock), so program it on every arm. Measured on
     * the 1.85B: 7..12 pass the self-test 20/20; <= 6 adds hits at the limit,
     * >= 13 misses short excursions, 0 detects nothing. */
    REG_WRITE(ASSIST_DEBUG_CORE_1_SP_UNSTABLE_REG, MONITOR_SP_UNSTABLE);
    if (!self_test_monitor(layout.limit)) {
        return false;
    }
    layout.armed = 1;
    roundie_elf_crash_executor(&layout);
    return true;
}

bool roundie_elf_stack_recheck(uint32_t *lost)
{
    uint32_t mask = 0;
    uint32_t physical = 0;
    mmu_target_t target;
    if (layout.stack_high == 0U || layout.armed == 0U || xTaskGetCurrentTaskHandle() != executor_task) {
        *lost = ROUNDIE_ELF_GUARD_LOST_NOT_ARMED;
        return false;
    }
    if (mmu_hal_vaddr_to_paddr(0, layout.guard_low, &physical, &target)) {
        mask |= ROUNDIE_ELF_GUARD_LOST_MMU_GUARD;
    }
    if (!mmu_hal_vaddr_to_paddr(0, layout.stack_low, &physical, &target)) {
        mask |= ROUNDIE_ELF_GUARD_LOST_MMU_STACK;
    }
    if (REG_READ(ASSIST_DEBUG_CORE_1_SP_MIN_REG) != layout.limit) {
        mask |= ROUNDIE_ELF_GUARD_LOST_SP_MIN;
    }
    if (REG_READ(ASSIST_DEBUG_CORE_1_SP_MAX_REG) != UINT32_MAX) {
        mask |= ROUNDIE_ELF_GUARD_LOST_SP_MAX;
    }
    if ((REG_READ(ASSIST_DEBUG_CORE_1_INTERRUPT_ENA_REG) & ASSIST_DEBUG_CORE_1_SP_SPILL_MIN_ENA) == 0U) {
        mask |= ROUNDIE_ELF_GUARD_LOST_ENABLE;
    }
    if ((REG_READ(ASSIST_DEBUG_CORE_1_INTERRUPT_RLS_REG) & ASSIST_DEBUG_CORE_1_SP_SPILL_MIN_RLS) == 0U) {
        mask |= ROUNDIE_ELF_GUARD_LOST_RELEASE;
    }
    if (REG_READ(ASSIST_DEBUG_CORE_1_SP_UNSTABLE_REG) != MONITOR_SP_UNSTABLE) {
        mask |= ROUNDIE_ELF_GUARD_LOST_UNSTABLE;
    }
    *lost = mask;
    ESP_LOGI(TAG, "recheck after sleep: lost=%02" PRIx32, mask);
    bool armed = (mask & (ROUNDIE_ELF_GUARD_LOST_MMU_GUARD | ROUNDIE_ELF_GUARD_LOST_MMU_STACK)) == 0U &&
        roundie_elf_stack_arm();
    if (!armed) {
        monitor_disable();
        layout.armed = 0;
        roundie_elf_crash_executor(&layout);
        ESP_LOGE(TAG, "recheck failed: native applications stay disabled until restart");
    }
    return armed;
}
