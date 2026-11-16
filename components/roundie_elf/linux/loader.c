/* Loader port (loader.h) for linux-x64.rep1 (docs/native-apps.md, N4).
 *
 * The validated bytes go into a sealed memfd, so what ld.so maps is exactly
 * what was validated; dlopen(RTLD_NOW | RTLD_LOCAL) maps and binds them. ld.so
 * looks up imports in the whole process, not through the manager's resolver,
 * so after loading every relocation slot is compared with the value the
 * resolver and the image define; any difference unloads the image before its
 * entry runs. None of this isolates the application from the process. */

#define _GNU_SOURCE
#include "loader.h"
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "esp_log.h"

#define SEGMENTS_MAX 8U

typedef struct {
    uintptr_t begin;
    uintptr_t end;
    uint32_t flags;
} segment_t;

static void *handle;
static int file = -1;
static uintptr_t base;
static uintptr_t entry_address;
static segment_t segments[SEGMENTS_MAX];
static unsigned segment_count;
static segment_t relro; /* read-only after relocation, whole pages */

enum {
    LOADER_MEMFD = 1,
    LOADER_SEAL,
    LOADER_MAP,
    LOADER_CONTENT,
    LOADER_DLOPEN,
    LOADER_BINDING,
    LOADER_ENTRY,
};

roundie_elf_memory_t roundie_elf_loader_memory(void)
{
    /* The desktop reports no heap figures; the image limits are the validator's. */
    roundie_elf_memory_t memory = {0};
    return memory;
}

bool roundie_elf_loader_reserve(const roundie_elf_memory_t *memory, const roundie_elf_info_t *info)
{
    (void)memory;
    (void)info;
    return true;
}

int roundie_elf_loader_init(void)
{
    handle = NULL;
    file = -1;
    base = 0;
    entry_address = 0;
    segment_count = 0;
    relro = (segment_t){0};
    return 0;
}

/* The validated image's bytes at a virtual address, through its PT_LOAD. */
static const void *at(const uint8_t *image, uint64_t address, uint64_t bytes)
{
    const Elf64_Ehdr *header = (const Elf64_Ehdr *)image;
    const Elf64_Phdr *programs = (const Elf64_Phdr *)(image + header->e_phoff);
    for (unsigned i = 0; i < header->e_phnum; i++) {
        const Elf64_Phdr *program = &programs[i];
        if (program->p_type == PT_LOAD && address >= program->p_vaddr &&
            address - program->p_vaddr <= program->p_filesz &&
            bytes <= program->p_filesz - (address - program->p_vaddr)) {
            return image + program->p_offset + (address - program->p_vaddr);
        }
    }
    return NULL;
}

/* Compare one relocation table's slots with what the relocations must produce. */
static bool bindings_match(const Elf64_Rela *table, uint64_t count, const Elf64_Sym *symbols,
                           const char *strings, roundie_elf_resolver_t resolver)
{
    for (uint64_t i = 0; i < count; i++) {
        const Elf64_Rela *relocation = &table[i];
        uint32_t type = ELF64_R_TYPE(relocation->r_info);
        const Elf64_Sym *symbol = &symbols[ELF64_R_SYM(relocation->r_info)];
        uint64_t expected;
        if (type == R_X86_64_RELATIVE) {
            expected = base + (uint64_t)relocation->r_addend;
        } else if (symbol->st_shndx == SHN_UNDEF) {
            uintptr_t service = resolver(strings + symbol->st_name);
            if (service == 0) {
                return false;
            }
            expected = service + (uint64_t)relocation->r_addend;
        } else {
            expected = base + symbol->st_value;
            if (type == R_X86_64_64) {
                expected += (uint64_t)relocation->r_addend;
            }
        }
        uint64_t slot;
        memcpy(&slot, (const void *)(base + relocation->r_offset), sizeof(slot));
        if (slot != expected) {
            ESP_LOGE("elf", "binding at +%#" PRIx64 " is %#" PRIx64 ", expected %#" PRIx64,
                     (uint64_t)relocation->r_offset, slot, expected);
            return false;
        }
    }
    return true;
}

static bool verify_bindings(const uint8_t *image, roundie_elf_resolver_t resolver)
{
    const Elf64_Ehdr *header = (const Elf64_Ehdr *)image;
    const Elf64_Phdr *programs = (const Elf64_Phdr *)(image + header->e_phoff);
    const Elf64_Dyn *dynamic = NULL;
    for (unsigned i = 0; i < header->e_phnum; i++) {
        if (programs[i].p_type == PT_DYNAMIC) {
            dynamic = (const Elf64_Dyn *)(image + programs[i].p_offset);
        }
    }
    uint64_t values[DT_NUM] = {0};
    for (; dynamic != NULL && dynamic->d_tag != DT_NULL; dynamic++) {
        if (dynamic->d_tag >= 0 && dynamic->d_tag < DT_NUM) {
            values[dynamic->d_tag] = dynamic->d_un.d_val;
        }
    }
    const Elf64_Sym *symbols = at(image, values[DT_SYMTAB], sizeof(Elf64_Sym));
    const char *strings = at(image, values[DT_STRTAB], values[DT_STRSZ]);
    const Elf64_Rela *table = at(image, values[DT_RELA], values[DT_RELASZ]);
    const Elf64_Rela *plt = at(image, values[DT_JMPREL], values[DT_PLTRELSZ]);
    if (symbols == NULL || strings == NULL || (values[DT_RELASZ] != 0 && table == NULL) ||
        (values[DT_PLTRELSZ] != 0 && plt == NULL)) {
        return false;
    }
    return bindings_match(table, values[DT_RELASZ] / sizeof(Elf64_Rela), symbols, strings, resolver) &&
        bindings_match(plt, values[DT_PLTRELSZ] / sizeof(Elf64_Rela), symbols, strings, resolver);
}

static const char *entry_name(uint32_t abi_version)
{
    if (abi_version == 1) {
        return "rep_app_entry";
    }
    return abi_version == 2 ? "rep_app_entry_v2" : "rep_app_entry_v3";
}

int roundie_elf_loader_relocate(const uint8_t *bytes, size_t size, roundie_elf_resolver_t resolver)
{
    file = memfd_create("rep-app", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (file < 0) {
        return LOADER_MEMFD;
    }
    size_t written = 0;
    while (written < size) {
        ssize_t result = write(file, bytes + written, size - written);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return LOADER_MEMFD;
        }
        written += (size_t)result;
    }
    if (fcntl(file, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) != 0) {
        return LOADER_SEAL;
    }
    /* From here the file cannot change: validate what ld.so will read. */
    uint8_t *image = mmap(NULL, size, PROT_READ, MAP_PRIVATE, file, 0);
    if (image == MAP_FAILED) {
        return LOADER_MAP;
    }
    int result = 0;
    roundie_elf_info_t info;
    char error[ROUNDIE_ELF_ERROR_SIZE];
    if (memcmp(image, bytes, size) != 0 || !roundie_elf_validate_x86_64(image, size, &info, error)) {
        result = LOADER_CONTENT;
        goto done;
    }
    char path[32];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", file);
    handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        ESP_LOGE("elf", "dlopen: %s", dlerror());
        result = LOADER_DLOPEN;
        goto done;
    }
    struct link_map *map = NULL;
    if (dlinfo(handle, RTLD_DI_LINKMAP, &map) != 0 || map == NULL) {
        result = LOADER_DLOPEN;
        goto done;
    }
    base = map->l_addr;
    const Elf64_Ehdr *header = (const Elf64_Ehdr *)image;
    const Elf64_Phdr *programs = (const Elf64_Phdr *)(image + header->e_phoff);
    for (unsigned i = 0; i < header->e_phnum && segment_count < SEGMENTS_MAX; i++) {
        if (programs[i].p_type == PT_LOAD) {
            segments[segment_count].begin = base + programs[i].p_vaddr;
            segments[segment_count].end = base + programs[i].p_vaddr + programs[i].p_memsz;
            segments[segment_count].flags = programs[i].p_flags;
            segment_count++;
        }
        if (programs[i].p_type == PT_GNU_RELRO) {
            uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE);
            relro.begin = (base + programs[i].p_vaddr) & ~(page - 1);
            relro.end = (base + programs[i].p_vaddr + programs[i].p_memsz + page - 1) & ~(page - 1);
        }
    }
    if (!verify_bindings(image, resolver)) {
        result = LOADER_BINDING;
        goto done;
    }
    entry_address = base + header->e_entry;
    void *named = dlsym(handle, entry_name(info.abi_version));
    if (named == NULL || (uintptr_t)named != entry_address) {
        result = LOADER_ENTRY;
        goto done;
    }
done:
    munmap(image, size);
    return result;
}

void roundie_elf_loader_describe(uint32_t instance, const roundie_elf_info_t *info)
{
    ESP_LOGI("elf", "instance=%" PRIu32 " base=%#" PRIxPTR " entry=%#" PRIxPTR " code=%" PRIu32
             " data=%" PRIu32 " relocations=%" PRIu32 " (dlopen, sealed memfd)",
             instance, base, entry_address, info->code_bytes, info->data_bytes, info->relocations);
}

roundie_elf_entry_t roundie_elf_loader_entry(void)
{
    roundie_elf_entry_t entry;
    _Static_assert(sizeof(entry) == sizeof(entry_address), "Loader entry pointer representation");
    memcpy(&entry, &entry_address, sizeof(entry));
    return entry;
}

bool roundie_elf_loader_contains(const void *pointer, uint32_t bytes, roundie_elf_region_t region)
{
    uintptr_t address = (uintptr_t)pointer;
    for (unsigned i = 0; i < segment_count; i++) {
        const segment_t *part = &segments[i];
        bool code = (part->flags & PF_X) != 0;
        if (code != (region == ROUNDIE_ELF_REGION_CODE)) {
            continue;
        }
        if (region == ROUNDIE_ELF_REGION_WRITABLE &&
            ((part->flags & PF_W) == 0 || (address < relro.end && address + bytes > relro.begin))) {
            continue;
        }
        if (address >= part->begin && address - part->begin <= part->end - part->begin &&
            bytes <= part->end - part->begin - (address - part->begin)) {
            return true;
        }
    }
    return false;
}

void roundie_elf_loader_unload(void)
{
    if (handle != NULL) {
        if (dlclose(handle) != 0) {
            ESP_LOGE("elf", "dlclose: %s", dlerror());
        }
        handle = NULL;
        /* No NODELETE, no dependencies, no unique symbols: the mapping must be gone. */
        Dl_info owner;
        if (segment_count > 0 && dladdr((const void *)segments[0].begin, &owner) != 0) {
            ESP_LOGE("elf", "image still mapped after dlclose");
        }
    }
    if (file >= 0) {
        close(file);
        file = -1;
    }
    base = 0;
    entry_address = 0;
    segment_count = 0;
    relro = (segment_t){0};
}

void *roundie_elf_loader_alloc(uint32_t bytes)
{
    return malloc(bytes);
}

void roundie_elf_loader_free(void *pointer)
{
    free(pointer);
}
