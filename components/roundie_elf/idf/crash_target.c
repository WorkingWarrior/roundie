/* Target part of the crash record (M9): the ELF digest and the panic hook.
 *
 * The panic hook records the exception context in the retained crash record
 * (roundie_elf_crash.h), then hands over to the unchanged ESP-IDF handler.
 * Linked with -Wl,--wrap=esp_panic_handler (CMakeLists.txt). It runs in the
 * panic context with the other core stalled and possibly with Flash cache
 * disabled: IRAM code, RTC data, inline helpers and IRAM backtrace functions
 * only; no logging, locks or allocation. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_attr.h"
#include "esp_cpu_utils.h"
#include "esp_debug_helpers.h"
#include "esp_memory_utils.h"
#include "esp_private/panic_internal.h"
#include "psa/crypto.h"
#include "xtensa_context.h"
#include "roundie_elf_crash.h"

bool roundie_elf_crash_digest(const uint8_t *bytes, size_t size, uint8_t digest[32])
{
    size_t length = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, bytes, size, digest, 32, &length) == PSA_SUCCESS &&
        length == 32;
}

void __real_esp_panic_handler(panic_info_t *info);
void __wrap_esp_panic_handler(panic_info_t *info);

void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info)
{
    if (info != NULL && info->frame != NULL) {
        const XtExcFrame *frame = info->frame;
        roundie_elf_crash_fault_t fault = {
            .pc = (uint32_t)frame->pc,
            .exccause = (uint32_t)frame->exccause,
            .excvaddr = (uint32_t)frame->excvaddr,
            .sp = (uint32_t)frame->a1,
            .exception = (uint32_t)info->exception,
            .core = (uint32_t)info->core,
        };
        /* The same walk and sanity checks as esp_backtrace_print_from_frame:
         * stop at the first frame whose stack pointer or PC is not sane. */
        esp_backtrace_frame_t walk = {
            .pc = frame->pc, .sp = frame->a1, .next_pc = frame->a0, .exc_frame = frame,
        };
        fault.frames[0] = esp_cpu_process_stack_pc(walk.pc);
        fault.depth = 1;
        bool sane = esp_stack_ptr_is_sane(walk.sp);
        while (sane && fault.depth < ROUNDIE_ELF_CRASH_FRAMES && walk.next_pc != 0) {
            if (!esp_backtrace_get_next_frame(&walk)) {
                sane = false;
            } else {
                fault.frames[fault.depth] = esp_cpu_process_stack_pc(walk.pc);
                fault.depth++;
            }
        }
        roundie_elf_crash_fault(&fault);
    }
    __real_esp_panic_handler(info);
}
