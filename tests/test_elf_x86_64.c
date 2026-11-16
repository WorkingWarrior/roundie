/* linux-x64.rep1 validator (components/roundie_elf/validate_x86_64.c) under
 * ASan/UBSan: every truncation, random corruption, and targeted patches of a
 * valid application. usage: test_elf_x86_64 VALID.so [FILE EXPECTED-ERROR]...
 * where EXPECTED-ERROR "OK" means the file must be accepted. */
#include <assert.h>
#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "roundie_elf_validate.h"

static uint8_t original[ROUNDIE_ELF_MAX_FILE];
static size_t original_size;
static uint8_t work[ROUNDIE_ELF_MAX_FILE];

static size_t load(const char *path, uint8_t *bytes)
{
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    size_t size = fread(bytes, 1, ROUNDIE_ELF_MAX_FILE, file);
    assert(fclose(file) == 0);
    return size;
}

static const char *check(const uint8_t *bytes, size_t size)
{
    static char error[ROUNDIE_ELF_ERROR_SIZE];
    roundie_elf_info_t info;
    /* A copy of exactly size bytes, so ASan sees any read past the end. */
    uint8_t *copy = malloc(size == 0 ? 1 : size);
    assert(copy != NULL);
    memcpy(copy, bytes, size);
    bool valid = roundie_elf_validate_x86_64(copy, size, &info, error);
    free(copy);
    return valid ? "OK" : error;
}

static void expect(const char *what, const char *expected)
{
    const char *result = check(work, original_size);
    if (strcmp(result, expected) != 0) {
        fprintf(stderr, "%s: expected %s, got %s\n", what, expected, result);
        exit(1);
    }
}

static Elf64_Ehdr header(void)
{
    Elf64_Ehdr result;
    memcpy(&result, work, sizeof(result));
    return result;
}

static Elf64_Phdr *segment(unsigned type, unsigned flags)
{
    Elf64_Ehdr ehdr = header();
    for (unsigned i = 0; i < ehdr.e_phnum; i++) {
        Elf64_Phdr *program = (Elf64_Phdr *)(work + ehdr.e_phoff + i * sizeof(Elf64_Phdr));
        if (program->p_type == type && (flags == 0 || (program->p_flags & flags) != 0)) {
            return program;
        }
    }
    return NULL;
}

static Elf64_Dyn *dynamic_entry(int64_t tag)
{
    Elf64_Phdr *dynamic = segment(PT_DYNAMIC, 0);
    assert(dynamic != NULL);
    for (Elf64_Dyn *entry = (Elf64_Dyn *)(work + dynamic->p_offset); entry->d_tag != DT_NULL; entry++) {
        if (entry->d_tag == tag) {
            return entry;
        }
    }
    return NULL;
}

/* File offset of a dynamic pointer, through the segment that maps it. */
static uint64_t offset_of(uint64_t address)
{
    Elf64_Ehdr ehdr = header();
    for (unsigned i = 0; i < ehdr.e_phnum; i++) {
        Elf64_Phdr *program = (Elf64_Phdr *)(work + ehdr.e_phoff + i * sizeof(Elf64_Phdr));
        if (program->p_type == PT_LOAD && address >= program->p_vaddr &&
            address < program->p_vaddr + program->p_filesz) {
            return program->p_offset + address - program->p_vaddr;
        }
    }
    assert(false);
    return 0;
}

static void reset(void)
{
    memcpy(work, original, original_size);
}

static void targeted(void)
{
    reset();
    expect("unchanged", "OK");
    reset();
    work[EI_CLASS] = ELFCLASS32;
    expect("class", "class-endian-version");
    reset();
    ((Elf64_Ehdr *)work)->e_machine = EM_AARCH64;
    expect("machine", "architecture-format");
    reset();
    ((Elf64_Ehdr *)work)->e_machine = EM_XTENSA;
    expect("machine", "architecture-format");
    reset();
    ((Elf64_Ehdr *)work)->e_type = ET_EXEC;
    expect("type", "architecture-format");
    reset();
    work[EI_OSABI] = ELFOSABI_GNU;
    expect("OS ABI", "OS-ABI");
    reset();
    segment(PT_LOAD, PF_X)->p_flags |= PF_W;
    expect("writable code", "writable-code");
    reset();
    segment(PT_GNU_STACK, 0)->p_flags |= PF_X;
    expect("executable stack", "executable-stack");
    reset();
    segment(PT_GNU_STACK, 0)->p_type = PT_TLS;
    expect("TLS segment", "segment-TLS-or-interpreter");
    reset();
    segment(PT_GNU_STACK, 0)->p_type = PT_INTERP;
    expect("interpreter", "segment-TLS-or-interpreter");
    reset();
    segment(PT_GNU_STACK, 0)->p_type = PT_NULL;
    expect("no stack segment", "segment-type");
    /* Dynamic tags, written over DT_FLAGS (present with -z now). */
    const struct {
        int64_t tag;
        const char *error;
    } tags[] = {
        {DT_NEEDED, "dynamic-dependency"},
        {DT_INIT, "initializer"},
        {DT_FINI, "initializer"},
        {DT_INIT_ARRAY, "initializer"},
        {DT_FINI_ARRAY, "initializer"},
        {DT_PREINIT_ARRAY, "initializer"},
        {DT_TEXTREL, "dynamic-tag:0x16"},
        {DT_RPATH, "dynamic-tag:0xf"},
        {DT_RUNPATH, "dynamic-tag:0x1d"},
        {DT_SYMBOLIC, "dynamic-tag:0x10"},
        {DT_HASH, "dynamic-tag:0x4"},
        {DT_VERNEED, "dynamic-tag:0x6ffffffe"},
        {DT_AUDIT, "dynamic-tag:0x6ffffefc"},
        {DT_FILTER, "dynamic-tag:0x7fffffff"},
    };
    for (unsigned i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
        reset();
        dynamic_entry(DT_FLAGS)->d_tag = tags[i].tag;
        expect("dynamic tag", tags[i].error);
    }
    reset();
    dynamic_entry(DT_FLAGS_1)->d_un.d_val |= DF_1_NODELETE;
    expect("NODELETE", "dynamic-flags");
    reset();
    dynamic_entry(DT_FLAGS)->d_un.d_val |= DF_STATIC_TLS;
    expect("static TLS", "dynamic-flags");
    reset();
    dynamic_entry(DT_GNU_HASH)->d_tag = DT_BIND_NOW;
    expect("no hash", "dynamic-symbols");
    reset();
    dynamic_entry(DT_RELACOUNT)->d_un.d_val += 1;
    expect("relative count", "relative-count");
    reset();
    dynamic_entry(DT_RELACOUNT)->d_un.d_val = UINT64_MAX;
    expect("relative count", "relative-count");
    reset();
    dynamic_entry(DT_PLTGOT)->d_un.d_ptr = segment(PT_LOAD, PF_X)->p_vaddr;
    expect("PLT GOT", "PLT-GOT");
    reset();
    segment(PT_NOTE, 0)->p_vaddr += 0x100000;
    expect("note outside", "note-bounds");
    /* Relocations. */
    uint64_t relocations = offset_of(dynamic_entry(DT_RELA)->d_un.d_ptr);
    Elf64_Rela *first = (Elf64_Rela *)(work + relocations);
    const unsigned forbidden[] = {R_X86_64_IRELATIVE, R_X86_64_NONE, R_X86_64_PC32, R_X86_64_TPOFF64,
                                  R_X86_64_DTPMOD64, R_X86_64_COPY, R_X86_64_32};
    for (unsigned i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); i++) {
        reset();
        first->r_info = ELF64_R_INFO(ELF64_R_SYM(first->r_info), forbidden[i]);
        expect("relocation type", "unsupported-relocation");
    }
    reset();
    assert(ELF64_R_TYPE(first->r_info) == R_X86_64_RELATIVE);
    first->r_offset = segment(PT_LOAD, PF_X)->p_vaddr;
    expect("relocation into code", "relocation-target");
    reset();
    first->r_addend = 0x7fffffff;
    expect("relative outside", "relative-relocation");
    reset();
    first->r_info = ELF64_R_INFO(1, R_X86_64_RELATIVE);
    expect("relative with symbol", "relative-relocation");
    reset();
    Elf64_Rela *plt = (Elf64_Rela *)(work + offset_of(dynamic_entry(DT_JMPREL)->d_un.d_ptr));
    plt->r_info = ELF64_R_INFO(4095, R_X86_64_JUMP_SLOT);
    expect("symbol index", "relocation-symbol-index");
    reset();
    plt->r_info = ELF64_R_INFO(ELF64_R_SYM(plt->r_info), R_X86_64_GLOB_DAT);
    expect("PLT type", "unsupported-relocation");
    /* Symbols: the import's name rewritten in the dynamic string table. */
    reset();
    uint64_t strings = offset_of(dynamic_entry(DT_STRTAB)->d_un.d_ptr);
    char *name = memmem(work + strings, dynamic_entry(DT_STRSZ)->d_un.d_val, "rep_abi_v", 9);
    assert(name != NULL);
    name[9] = '9';
    char expected[ROUNDIE_ELF_ERROR_SIZE];
    snprintf(expected, sizeof(expected), "import:%s", name);
    expect("unknown import", expected);
    reset();
    name = memmem(work + strings, dynamic_entry(DT_STRSZ)->d_un.d_val, "rep_app_entry", 13);
    assert(name != NULL);
    name[0] = 'x';
    expect("no entry symbol", "missing-entry-or-service-import");
    reset();
    ((Elf64_Ehdr *)work)->e_entry += 1;
    expect("entry address", "entry-symbol");
    reset();
    uint64_t hash = offset_of(dynamic_entry(DT_GNU_HASH)->d_un.d_ptr);
    ((uint32_t *)(work + hash))[0] = 100000;
    expect("hash buckets", "hash-table");
    reset();
    uint32_t *words = (uint32_t *)(work + hash);
    words[4 + words[2] * 2] = 1000;
    expect("hash bucket index", "hash-bucket");
}

/* xorshift; reproducible. */
static uint64_t state = 0x9E3779B97F4A7C15ULL;
static uint64_t next(void)
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

int main(int argc, char **argv)
{
    assert(argc >= 2 && argc % 2 == 0);
    original_size = load(argv[1], original);
    reset();
    targeted();
    unsigned truncations = 0;
    for (size_t size = 0; size < original_size; size++) {
        if (strcmp(check(original, size), "OK") == 0) {
            fprintf(stderr, "truncation to %zu bytes accepted\n", size);
            return 1;
        }
        truncations++;
    }
    unsigned accepted = 0;
    unsigned mutations = 20000;
    for (unsigned i = 0; i < mutations; i++) {
        reset();
        unsigned changes = 1 + (unsigned)(next() % 4);
        for (unsigned j = 0; j < changes; j++) {
            /* Mostly the headers and dynamic metadata; the code bytes don't matter. */
            size_t limit = next() % 4 == 0 ? original_size : 0x500;
            work[next() % limit] ^= (uint8_t)(1U << (next() % 8));
        }
        if (strcmp(check(work, original_size), "OK") == 0) {
            accepted++;
        }
    }
    for (int i = 2; i < argc; i += 2) {
        static uint8_t bytes[ROUNDIE_ELF_MAX_FILE];
        size_t size = load(argv[i], bytes);
        const char *result = check(bytes, size);
        if (strcmp(result, argv[i + 1]) != 0) {
            fprintf(stderr, "%s: expected %s, got %s\n", argv[i], argv[i + 1], result);
            return 1;
        }
    }
    printf("x86-64 validation PASS: targeted, %u truncations rejected, %u mutations (%u still valid), %d files\n",
           truncations, mutations, accepted, (argc - 2) / 2);
    return 0;
}
