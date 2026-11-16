/* linux-x64.rep1 validation (docs/native-apps.md, N2).
 *
 * ld.so ignores section headers: it maps the PT_LOAD segments and reads the
 * dynamic segment, the dynamic symbol table, the GNU hash table and the
 * relocation tables found through it. Those are what is checked here, against
 * the same policy as the Xtensa validator: only the ABI's REP imports, no
 * dependencies, no code that runs before the entry (initializers, IFUNC
 * resolvers, IRELATIVE), no TLS, no writable code. Every rejection names its
 * rule. No allocation or execution; input may be unaligned. */
#include "roundie_elf_validate.h"
#include <elf.h>
#include <stdio.h>
#include <string.h>
#include "rep/rep_app.h"

#define PAGE_BYTES 4096U
#define MAX_LOADS 8U
#define MAX_SPAN 262144U
#define MAX_SYMBOLS 1024U
#define MAX_RELOCATIONS 1024U
#define MAX_HASH_WORDS 1024U

typedef struct {
    const uint8_t *bytes;
    size_t size;
    Elf64_Phdr loads[MAX_LOADS];
    unsigned load_count;
} image_t;

static bool range(size_t size, uint64_t offset, uint64_t count)
{
    return offset <= size && count <= size - offset;
}

static bool in_segment(uint64_t address, uint64_t count, uint64_t begin, uint64_t length)
{
    return address >= begin && address - begin <= length && count <= length - (address - begin);
}

/* File offset of [address, address + count) within one segment's file bytes. */
static bool file_offset(const image_t *image, uint64_t address, uint64_t count, uint64_t *offset)
{
    for (unsigned i = 0; i < image->load_count; i++) {
        const Elf64_Phdr *load = &image->loads[i];
        if (in_segment(address, count, load->p_vaddr, load->p_filesz)) {
            *offset = load->p_offset + (address - load->p_vaddr);
            return true;
        }
    }
    return false;
}

/* Whether [address, address + count) lies in one segment's memory, writable
 * or executable as asked. */
static bool mapped(const image_t *image, uint64_t address, uint64_t count, uint32_t flag)
{
    for (unsigned i = 0; i < image->load_count; i++) {
        const Elf64_Phdr *load = &image->loads[i];
        if ((flag == 0 || (load->p_flags & flag) != 0) &&
            in_segment(address, count, load->p_vaddr, load->p_memsz)) {
            return true;
        }
    }
    return false;
}

static uint32_t word(const image_t *image, uint64_t offset)
{
    uint32_t value;
    memcpy(&value, image->bytes + offset, sizeof(value));
    return value;
}

static Elf64_Shdr section(const image_t *image, const Elf64_Ehdr *header, unsigned index)
{
    Elf64_Shdr result;
    memcpy(&result, image->bytes + header->e_shoff + (uint64_t)index * sizeof(result), sizeof(result));
    return result;
}

static const char *section_name(const image_t *image, const Elf64_Shdr *names, uint32_t offset)
{
    if (offset >= names->sh_size ||
        memchr(image->bytes + names->sh_offset + offset, 0, names->sh_size - offset) == NULL) {
        return NULL;
    }
    return (const char *)image->bytes + names->sh_offset + offset;
}

bool roundie_elf_validate_x86_64(const void *input, size_t size, roundie_elf_info_t *out,
                                 char error[ROUNDIE_ELF_ERROR_SIZE])
{
#define CHECK(condition, message) do { \
    if (!(condition)) { \
        snprintf(error, ROUNDIE_ELF_ERROR_SIZE, "%s", message); \
        return false; \
    } \
} while (0)
    if (out == NULL || error == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    error[0] = '\0';
    CHECK(input != NULL && size >= sizeof(Elf64_Ehdr), "truncated-header");
    CHECK(size <= ROUNDIE_ELF_MAX_FILE, "file-limit");
    image_t image = {.bytes = input, .size = size};
    Elf64_Ehdr header;
    memcpy(&header, image.bytes, sizeof(header));
    CHECK(memcmp(header.e_ident, "\177ELF", 4) == 0, "magic");
    CHECK(header.e_ident[EI_CLASS] == ELFCLASS64 && header.e_ident[EI_DATA] == ELFDATA2LSB &&
          header.e_ident[EI_VERSION] == EV_CURRENT, "class-endian-version");
    CHECK(header.e_machine == EM_X86_64 && header.e_type == ET_DYN &&
          header.e_version == EV_CURRENT, "architecture-format");
    /* ELFOSABI_GNU marks IFUNC or unique symbols. */
    CHECK(header.e_ident[EI_OSABI] == ELFOSABI_SYSV, "OS-ABI");
    CHECK(header.e_ehsize == sizeof(header) && header.e_shentsize == sizeof(Elf64_Shdr) &&
          header.e_shnum > 0 && header.e_shnum <= 128 &&
          header.e_shstrndx < header.e_shnum && header.e_shstrndx != 0 &&
          range(size, header.e_shoff, (uint64_t)header.e_shnum * sizeof(Elf64_Shdr)), "section-headers");
    CHECK(header.e_phnum > 0 && header.e_phnum <= 16 && header.e_phentsize == sizeof(Elf64_Phdr) &&
          range(size, header.e_phoff, (uint64_t)header.e_phnum * sizeof(Elf64_Phdr)), "program-headers");

    /* Segments: what ld.so maps. */
    Elf64_Phdr dynamic_segment = {0};
    Elf64_Phdr relro = {0};
    unsigned dynamic_count = 0;
    unsigned stack_count = 0;
    unsigned relro_count = 0;
    Elf64_Phdr parsed[16];
    unsigned parsed_count = 0;
    uint64_t code_bytes = 0;
    uint64_t data_bytes = 0;
    for (unsigned i = 0; i < header.e_phnum; i++) {
        Elf64_Phdr program;
        memcpy(&program, image.bytes + header.e_phoff + (uint64_t)i * sizeof(program), sizeof(program));
        CHECK(program.p_type != PT_TLS && program.p_type != PT_INTERP, "segment-TLS-or-interpreter");
        if (program.p_type == PT_LOAD) {
            CHECK(image.load_count < MAX_LOADS, "segment-count");
            CHECK(range(size, program.p_offset, program.p_filesz) && program.p_memsz >= program.p_filesz &&
                  program.p_memsz <= MAX_SPAN && program.p_vaddr <= MAX_SPAN - program.p_memsz,
                  "segment-bounds");
            CHECK(program.p_align >= PAGE_BYTES && (program.p_align & (program.p_align - 1)) == 0 &&
                  program.p_vaddr % program.p_align == program.p_offset % program.p_align, "segment-alignment");
            CHECK((program.p_flags & PF_W) == 0 || (program.p_flags & PF_X) == 0, "writable-code");
            CHECK(image.load_count == 0 ||
                  image.loads[image.load_count - 1].p_vaddr + image.loads[image.load_count - 1].p_memsz <=
                  program.p_vaddr, "segment-order");
            if ((program.p_flags & PF_X) != 0) {
                code_bytes += program.p_memsz;
            } else {
                data_bytes += program.p_memsz;
            }
            image.loads[image.load_count] = program;
            image.load_count++;
        } else if (program.p_type == PT_DYNAMIC) {
            dynamic_segment = program;
            dynamic_count++;
        } else if (program.p_type == PT_GNU_STACK) {
            CHECK((program.p_flags & PF_X) == 0, "executable-stack");
            stack_count++;
        } else if (program.p_type == PT_GNU_RELRO) {
            relro = program;
            relro_count++;
        } else {
            CHECK(program.p_type == PT_NOTE || program.p_type == PT_GNU_PROPERTY ||
                  program.p_type == PT_GNU_EH_FRAME, "segment-type");
            parsed[parsed_count] = program;
            parsed_count++;
        }
    }
    CHECK(image.load_count > 0 && dynamic_count == 1 && stack_count == 1 && relro_count <= 1,
          "missing-or-duplicate-segment");
    CHECK(code_bytes > 0 && code_bytes <= ROUNDIE_ELF_MAX_CODE && data_bytes <= ROUNDIE_ELF_MAX_DATA,
          "memory-limit");
    /* ld.so protects whole pages: RELRO may end at the page end of its segment. */
    bool relro_valid = relro_count == 0;
    for (unsigned i = 0; i < image.load_count && !relro_valid; i++) {
        const Elf64_Phdr *load = &image.loads[i];
        uint64_t page_end = (load->p_vaddr + load->p_memsz + PAGE_BYTES - 1) & ~(uint64_t)(PAGE_BYTES - 1);
        relro_valid = (load->p_flags & PF_W) != 0 &&
            in_segment(relro.p_vaddr, relro.p_memsz, load->p_vaddr, page_end - load->p_vaddr);
    }
    CHECK(relro_valid, "RELRO-bounds");
    /* ld.so parses notes (GNU properties) from the mapped image. */
    for (unsigned i = 0; i < parsed_count; i++) {
        uint64_t offset;
        CHECK(file_offset(&image, parsed[i].p_vaddr, parsed[i].p_filesz, &offset) &&
              offset == parsed[i].p_offset && parsed[i].p_memsz == parsed[i].p_filesz, "note-bounds");
    }
    out->code_bytes = (uint32_t)code_bytes;
    out->data_bytes = (uint32_t)data_bytes;

    /* Dynamic segment: an allowlist of tags. */
    uint64_t dynamic_offset;
    CHECK(dynamic_segment.p_filesz % sizeof(Elf64_Dyn) == 0 &&
          dynamic_segment.p_filesz == dynamic_segment.p_memsz &&
          mapped(&image, dynamic_segment.p_vaddr, dynamic_segment.p_memsz, PF_W) &&
          file_offset(&image, dynamic_segment.p_vaddr, dynamic_segment.p_filesz, &dynamic_offset) &&
          dynamic_offset == dynamic_segment.p_offset, "dynamic-segment");
    uint64_t tags[DT_NUM] = {0};
    bool seen[DT_NUM] = {false};
    uint64_t gnu_hash = 0;
    uint64_t relative_count = 0;
    bool relative_count_seen = false;
    bool terminated = false;
    for (uint64_t i = 0; i < dynamic_segment.p_filesz / sizeof(Elf64_Dyn) && !terminated; i++) {
        Elf64_Dyn entry;
        memcpy(&entry, image.bytes + dynamic_offset + i * sizeof(entry), sizeof(entry));
        switch (entry.d_tag) {
        case DT_NULL:
            terminated = true;
            break;
        case DT_NEEDED:
            CHECK(false, "dynamic-dependency");
            break;
        case DT_INIT:
        case DT_FINI:
        case DT_INIT_ARRAY:
        case DT_FINI_ARRAY:
        case DT_INIT_ARRAYSZ:
        case DT_FINI_ARRAYSZ:
        case DT_PREINIT_ARRAY:
        case DT_PREINIT_ARRAYSZ:
            CHECK(false, "initializer");
            break;
        case DT_STRTAB:
        case DT_SYMTAB:
        case DT_STRSZ:
        case DT_SYMENT:
        case DT_RELA:
        case DT_RELASZ:
        case DT_RELAENT:
        case DT_JMPREL:
        case DT_PLTRELSZ:
        case DT_PLTREL:
        case DT_PLTGOT:
            CHECK(!seen[entry.d_tag], "dynamic-duplicate");
            seen[entry.d_tag] = true;
            tags[entry.d_tag] = entry.d_un.d_val;
            break;
        case DT_GNU_HASH:
            CHECK(gnu_hash == 0, "dynamic-duplicate");
            gnu_hash = entry.d_un.d_ptr;
            CHECK(gnu_hash != 0, "hash-table");
            break;
        case DT_RELACOUNT:
            CHECK(!relative_count_seen, "dynamic-duplicate");
            relative_count_seen = true;
            relative_count = entry.d_un.d_val;
            break;
        case DT_BIND_NOW:
            break;
        case DT_FLAGS:
            CHECK((entry.d_un.d_val & ~(uint64_t)DF_BIND_NOW) == 0, "dynamic-flags");
            break;
        case DT_FLAGS_1:
            CHECK((entry.d_un.d_val & ~(uint64_t)DF_1_NOW) == 0, "dynamic-flags");
            break;
        default:
            snprintf(error, ROUNDIE_ELF_ERROR_SIZE, "dynamic-tag:%#llx", (unsigned long long)entry.d_tag);
            return false;
        }
    }
    CHECK(terminated, "dynamic-unterminated");
    CHECK(!seen[DT_PLTGOT] || ((tags[DT_PLTGOT] & 7U) == 0 && mapped(&image, tags[DT_PLTGOT], 24, PF_W)),
          "PLT-GOT");
    CHECK(seen[DT_SYMTAB] && seen[DT_STRTAB] && seen[DT_STRSZ] && gnu_hash != 0 &&
          seen[DT_SYMENT] && tags[DT_SYMENT] == sizeof(Elf64_Sym), "dynamic-symbols");

    /* .rep.abi and the dynamic symbol table's extent come from the sections. */
    Elf64_Shdr names = section(&image, &header, header.e_shstrndx);
    CHECK(names.sh_type == SHT_STRTAB && range(size, names.sh_offset, names.sh_size), "section-names");
    unsigned metadata = 0;
    unsigned dynamic_symbols = 0;
    uint64_t symbol_count = 0;
    for (unsigned i = 0; i < header.e_shnum; i++) {
        Elf64_Shdr current = section(&image, &header, i);
        CHECK(current.sh_type == SHT_NOBITS || range(size, current.sh_offset, current.sh_size), "section-bounds");
        CHECK((current.sh_flags & (SHF_TLS | SHF_COMPRESSED)) == 0 &&
              current.sh_type != SHT_REL && current.sh_type != SHT_INIT_ARRAY &&
              current.sh_type != SHT_FINI_ARRAY && current.sh_type != SHT_PREINIT_ARRAY,
              "unsupported-section-or-TLS");
        const char *name = section_name(&image, &names, current.sh_name);
        CHECK(name != NULL, "section-name-bounds");
        if (strcmp(name, ".rep.abi") == 0) {
            uint32_t words[3];
            CHECK(current.sh_type == SHT_PROGBITS && current.sh_size == sizeof(words), "ABI-metadata");
            memcpy(words, image.bytes + current.sh_offset, sizeof(words));
            CHECK(words[0] == UINT32_C(0x31504552) &&
                  (words[1] == REP_ABI_VERSION || words[1] == 2 || words[1] == 3) && words[2] == 12,
                  "ABI-version");
            out->abi_version = words[1];
            metadata++;
        }
        if (current.sh_type == SHT_DYNSYM) {
            uint64_t offset;
            CHECK(current.sh_addr == tags[DT_SYMTAB] && current.sh_entsize == sizeof(Elf64_Sym) &&
                  current.sh_size % sizeof(Elf64_Sym) == 0 && current.sh_size / sizeof(Elf64_Sym) > 0 &&
                  current.sh_size / sizeof(Elf64_Sym) <= MAX_SYMBOLS &&
                  file_offset(&image, current.sh_addr, current.sh_size, &offset) &&
                  offset == current.sh_offset, "symbol-table");
            symbol_count = current.sh_size / sizeof(Elf64_Sym);
            dynamic_symbols++;
        }
    }
    CHECK(metadata == 1, "missing-or-duplicate-ABI");
    CHECK(dynamic_symbols == 1, "symbol-table");
    uint64_t symbols_offset;
    uint64_t strings_offset;
    CHECK(file_offset(&image, tags[DT_SYMTAB], symbol_count * sizeof(Elf64_Sym), &symbols_offset), "symbol-table");
    CHECK(tags[DT_STRSZ] > 0 && file_offset(&image, tags[DT_STRTAB], tags[DT_STRSZ], &strings_offset),
          "symbol-strings");

    /* GNU hash: every index ld.so can reach stays inside the symbol table and
     * every chain ends there. DT_HASH is not accepted. */
    uint64_t hash_offset;
    CHECK(file_offset(&image, gnu_hash, 16, &hash_offset) && (gnu_hash & 7U) == 0, "hash-table");
    uint32_t buckets = word(&image, hash_offset);
    uint32_t first = word(&image, hash_offset + 4);
    uint32_t bloom_words = word(&image, hash_offset + 8);
    uint32_t bloom_shift = word(&image, hash_offset + 12);
    CHECK(buckets > 0 && buckets <= MAX_HASH_WORDS && bloom_words > 0 && bloom_words <= MAX_HASH_WORDS &&
          (bloom_words & (bloom_words - 1)) == 0 && bloom_shift < 64 && first > 0 && first <= symbol_count,
          "hash-table");
    uint64_t chains = symbol_count - first;
    uint64_t hash_bytes = 16 + (uint64_t)bloom_words * 8 + (uint64_t)buckets * 4 + chains * 4;
    CHECK(file_offset(&image, gnu_hash, hash_bytes, &hash_offset), "hash-table");
    uint64_t bucket_offset = hash_offset + 16 + (uint64_t)bloom_words * 8;
    for (uint32_t i = 0; i < buckets; i++) {
        uint32_t index = word(&image, bucket_offset + (uint64_t)i * 4);
        CHECK(index == 0 || (index >= first && index < symbol_count), "hash-bucket");
    }
    CHECK(chains == 0 || (word(&image, bucket_offset + (uint64_t)buckets * 4 + (chains - 1) * 4) & 1U) != 0,
          "hash-chain");

    /* Symbols: imports from the ABI's allowlist; nothing that runs at load. */
    bool entry_symbol = false;
    const char *entry_name = out->abi_version == 1 ? "rep_app_entry" :
        out->abi_version == 2 ? "rep_app_entry_v2" : "rep_app_entry_v3";
    const char *strings = (const char *)image.bytes + strings_offset;
    for (uint64_t i = 1; i < symbol_count; i++) {
        Elf64_Sym symbol;
        memcpy(&symbol, image.bytes + symbols_offset + i * sizeof(symbol), sizeof(symbol));
        CHECK(symbol.st_name < tags[DT_STRSZ] &&
              memchr(strings + symbol.st_name, 0, tags[DT_STRSZ] - symbol.st_name) != NULL, "symbol-name-bounds");
        const char *name = strings + symbol.st_name;
        unsigned type = ELF64_ST_TYPE(symbol.st_info);
        unsigned bind = ELF64_ST_BIND(symbol.st_info);
        CHECK(type != STT_TLS, "TLS-symbol");
        CHECK(type != STT_GNU_IFUNC, "IFUNC-symbol");
        CHECK(bind == STB_LOCAL || bind == STB_GLOBAL || bind == STB_WEAK, "symbol-binding");
        if (symbol.st_shndx == SHN_UNDEF) {
            if (!roundie_elf_import_allowed(name) || !roundie_elf_import_matches(name, out->abi_version)) {
                snprintf(error, ROUNDIE_ELF_ERROR_SIZE, "import:%.64s", name);
                return false;
            }
            CHECK(bind == STB_GLOBAL && symbol.st_value == 0, "import-binding");
            out->imports++;
            continue;
        }
        CHECK(symbol.st_shndx < SHN_LORESERVE && symbol.st_shndx < header.e_shnum &&
              mapped(&image, symbol.st_value, symbol.st_size, 0), "symbol-address");
        if (strcmp(name, entry_name) == 0) {
            CHECK(symbol.st_value == header.e_entry && type == STT_FUNC && bind == STB_GLOBAL &&
                  ELF64_ST_VISIBILITY(symbol.st_other) == STV_DEFAULT, "entry-symbol");
            entry_symbol = true;
        }
    }
    CHECK(header.e_entry != 0 && mapped(&image, header.e_entry, 1, PF_X), "entry-address");
    out->entry = (uint32_t)header.e_entry;
    CHECK(entry_symbol && out->imports > 0, "missing-entry-or-service-import");
    for (unsigned i = 0; i < image.load_count; i++) {
        if ((image.loads[i].p_flags & PF_X) != 0) {
            out->sections[0] = (roundie_elf_section_t){(uint32_t)image.loads[i].p_vaddr,
                (uint32_t)image.loads[i].p_offset, (uint32_t)image.loads[i].p_memsz};
            break;
        }
    }

    /* Relocations: RELATIVE, GLOB_DAT, JUMP_SLOT and 64, into writable memory. */
    const struct {
        uint64_t address;
        uint64_t bytes;
        bool present;
        bool plt;
    } tables[2] = {
        {tags[DT_RELA], tags[DT_RELASZ], seen[DT_RELA] || seen[DT_RELASZ], false},
        {tags[DT_JMPREL], tags[DT_PLTRELSZ], seen[DT_JMPREL] || seen[DT_PLTRELSZ], true},
    };
    CHECK(!tables[0].present || (seen[DT_RELA] && seen[DT_RELASZ] && seen[DT_RELAENT] &&
                                 tags[DT_RELAENT] == sizeof(Elf64_Rela)), "relocation-table");
    CHECK(!tables[1].present || (seen[DT_JMPREL] && seen[DT_PLTRELSZ] && seen[DT_PLTREL] &&
                                 tags[DT_PLTREL] == DT_RELA), "relocation-table");
    /* glibc applies the first DT_RELACOUNT entries as RELATIVE without reading their type. */
    uint64_t leading_relative = 0;
    bool counting = true;
    for (unsigned t = 0; t < 2; t++) {
        if (!tables[t].present) {
            continue;
        }
        uint64_t offset;
        CHECK(tables[t].bytes % sizeof(Elf64_Rela) == 0 &&
              file_offset(&image, tables[t].address, tables[t].bytes, &offset), "relocation-table");
        CHECK(tables[t].bytes / sizeof(Elf64_Rela) <= MAX_RELOCATIONS - out->relocations, "relocation-limit");
        out->relocations += (uint32_t)(tables[t].bytes / sizeof(Elf64_Rela));
        for (uint64_t j = 0; j < tables[t].bytes / sizeof(Elf64_Rela); j++) {
            Elf64_Rela relocation;
            memcpy(&relocation, image.bytes + offset + j * sizeof(relocation), sizeof(relocation));
            uint64_t type = ELF64_R_TYPE(relocation.r_info);
            uint64_t index = ELF64_R_SYM(relocation.r_info);
            CHECK(tables[t].plt ? type == R_X86_64_JUMP_SLOT :
                  (type == R_X86_64_RELATIVE || type == R_X86_64_GLOB_DAT || type == R_X86_64_64),
                  "unsupported-relocation");
            CHECK((relocation.r_offset & 7U) == 0 && mapped(&image, relocation.r_offset, 8, PF_W),
                  "relocation-target");
            counting = counting && t == 0 && type == R_X86_64_RELATIVE;
            if (counting) {
                leading_relative++;
            }
            if (type == R_X86_64_RELATIVE) {
                CHECK(index == 0 && relocation.r_addend >= 0 &&
                      mapped(&image, (uint64_t)relocation.r_addend, 0, 0), "relative-relocation");
                continue;
            }
            CHECK(index > 0 && index < symbol_count, "relocation-symbol-index");
            Elf64_Sym symbol;
            memcpy(&symbol, image.bytes + symbols_offset + index * sizeof(symbol), sizeof(symbol));
            if (symbol.st_shndx == SHN_UNDEF) {
                /* Checked above against the ABI's allowlist. */
                CHECK(relocation.r_addend == 0, "relocation-import");
            } else {
                CHECK(type != R_X86_64_64 ||
                      (relocation.r_addend >= 0 && symbol.st_value <= MAX_SPAN &&
                       mapped(&image, symbol.st_value + (uint64_t)relocation.r_addend, 0, 0)), "relocation-value");
                CHECK(type == R_X86_64_64 || relocation.r_addend == 0, "relocation-addend");
            }
        }
    }
    CHECK(relative_count <= leading_relative, "relative-count");
    return true;
#undef CHECK
}
