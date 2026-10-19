#include "roundie_elf_validate.h"
#include <elf.h>
#include <stdio.h>
#include <string.h>
#include "rep/rep_app.h"

static bool range(size_t size, uint32_t offset, uint32_t count)
{
    return offset <= size && count <= size - offset;
}

static bool within(uint32_t address, uint32_t count, const roundie_elf_section_t *section)
{
    return address >= section->address && address - section->address <= section->size &&
        count <= section->size - (address - section->address);
}

static int mapped(const roundie_elf_info_t *info, uint32_t address, uint32_t count)
{
    for (int i = 0; i < 5; i++) {
        if (info->sections[i].size != 0 && within(address, count, &info->sections[i])) {
            return i;
        }
    }
    return -1;
}

static Elf32_Shdr section(const uint8_t *bytes, const Elf32_Ehdr *header, unsigned index)
{
    Elf32_Shdr result;
    memcpy(&result, bytes + header->e_shoff + index * sizeof(result), sizeof(result));
    return result;
}

static const char *string(const uint8_t *bytes, const Elf32_Shdr *strings, uint32_t offset)
{
    if (offset >= strings->sh_size ||
        memchr(bytes + strings->sh_offset + offset, 0, strings->sh_size - offset) == NULL) {
        return NULL;
    }
    return (const char *)bytes + strings->sh_offset + offset;
}

bool roundie_elf_import_allowed(const char *name)
{
    return name != NULL && (strcmp(name, "rep_abi_v1") == 0 || strcmp(name, "rep_abi_v2") == 0);
}

bool roundie_elf_validate(const void *input, size_t size, roundie_elf_info_t *out,
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
    CHECK(input != NULL && size >= sizeof(Elf32_Ehdr), "truncated-header");
    CHECK(size <= ROUNDIE_ELF_MAX_FILE, "file-limit");
    const uint8_t *bytes = input;
    Elf32_Ehdr header;
    memcpy(&header, bytes, sizeof(header));
    CHECK(memcmp(header.e_ident, "\177ELF", 4) == 0, "magic");
    CHECK(header.e_ident[EI_CLASS] == ELFCLASS32 && header.e_ident[EI_DATA] == ELFDATA2LSB &&
          header.e_ident[EI_VERSION] == EV_CURRENT, "class-endian-version");
    CHECK(header.e_machine == EM_XTENSA && header.e_type == ET_DYN &&
          header.e_version == EV_CURRENT, "architecture-format");
    CHECK(header.e_ehsize == sizeof(header) && header.e_shentsize == sizeof(Elf32_Shdr) &&
          header.e_shnum > 0 && header.e_shnum <= 128 &&
          header.e_shstrndx < header.e_shnum && header.e_shstrndx != 0 &&
          (header.e_shoff & 3U) == 0 &&
          range(size, header.e_shoff, header.e_shnum * sizeof(Elf32_Shdr)), "section-headers");
    CHECK(header.e_phnum <= 16 && header.e_phentsize == sizeof(Elf32_Phdr) &&
          (header.e_phoff & 3U) == 0 &&
          range(size, header.e_phoff, header.e_phnum * sizeof(Elf32_Phdr)), "program-headers");
    for (unsigned i = 0; i < header.e_phnum; i++) {
        Elf32_Phdr program;
        memcpy(&program, bytes + header.e_phoff + i * sizeof(program), sizeof(program));
        CHECK(range(size, program.p_offset, program.p_filesz) &&
              program.p_memsz >= program.p_filesz && program.p_memsz <= ROUNDIE_ELF_MAX_FILE &&
              program.p_vaddr <= UINT32_MAX - program.p_memsz &&
              program.p_type != PT_TLS && program.p_type != PT_INTERP, "segment-bounds-or-TLS");
    }
    Elf32_Shdr names = section(bytes, &header, header.e_shstrndx);
    CHECK(names.sh_type == SHT_STRTAB && range(size, names.sh_offset, names.sh_size), "section-names");
    const char *const loaded_names[] = {".text", ".bss", ".data", ".rodata", ".data.rel.ro"};
    unsigned metadata = 0;
    bool loaded[5] = {false};
    uint32_t alignments[5] = {0};
    for (unsigned i = 0; i < header.e_shnum; i++) {
        Elf32_Shdr current = section(bytes, &header, i);
        CHECK(current.sh_type == SHT_NOBITS || range(size, current.sh_offset, current.sh_size), "section-bounds");
        CHECK(current.sh_addr <= UINT32_MAX - current.sh_size, "section-address-overflow");
        CHECK(current.sh_addralign <= 16 &&
              (current.sh_addralign == 0 || (current.sh_addralign & (current.sh_addralign - 1)) == 0),
              "section-alignment");
        CHECK((current.sh_flags & (SHF_TLS | SHF_COMPRESSED)) == 0 &&
              current.sh_type != SHT_REL && current.sh_type != SHT_INIT_ARRAY &&
              current.sh_type != SHT_FINI_ARRAY && current.sh_type != SHT_PREINIT_ARRAY,
              "unsupported-section-or-TLS");
        const char *name = string(bytes, &names, current.sh_name);
        CHECK(name != NULL, "section-name-bounds");
        if (strcmp(name, ".rep.abi") == 0) {
            uint32_t words[3];
            CHECK(current.sh_type == SHT_PROGBITS && current.sh_size == sizeof(words), "ABI-metadata");
            memcpy(words, bytes + current.sh_offset, sizeof(words));
            CHECK(words[0] == UINT32_C(0x31504552) &&
                  (words[1] == REP_ABI_VERSION || words[1] == 2) && words[2] == 12,
                  "ABI-version");
            out->abi_version = words[1];
            metadata++;
        }
        for (int j = 0; j < 5; j++) {
            if (strcmp(name, loaded_names[j]) != 0) {
                continue;
            }
            CHECK(!loaded[j] && (current.sh_flags & SHF_ALLOC) != 0 &&
                  current.sh_type == (j == 1 ? SHT_NOBITS : SHT_PROGBITS), "load-section-format");
            CHECK(j != 0 || ((current.sh_flags & SHF_EXECINSTR) != 0 &&
                             (current.sh_addr & 3U) == 0 && (current.sh_offset & 3U) == 0), "text-format");
            CHECK(j == 0 || j == 3 || (current.sh_flags & SHF_WRITE) != 0, "data-format");
            out->sections[j] = (roundie_elf_section_t){current.sh_addr, current.sh_offset, current.sh_size};
            loaded[j] = true;
            alignments[j] = current.sh_addralign;
            if (j == 0) {
                out->code_bytes = (current.sh_size + 3U) & ~3U;
                CHECK(range(size, current.sh_offset, out->code_bytes), "text-padding-bounds");
            } else if (current.sh_size != 0) {
                CHECK(current.sh_size <= ROUNDIE_ELF_MAX_DATA, "data-limit");
                out->data_bytes += current.sh_size + (current.sh_addralign > 1 ? current.sh_addralign - 1 : 0);
            }
        }
    }
    CHECK(metadata == 1, "missing-or-duplicate-ABI");
    CHECK(out->code_bytes > 0 && out->code_bytes <= ROUNDIE_ELF_MAX_CODE &&
          out->data_bytes <= ROUNDIE_ELF_MAX_DATA, "memory-limit");
    CHECK(out->sections[0].address <= UINT32_MAX - out->code_bytes, "rounded-text-overflow");
    for (unsigned i = 1; i < 5; i++) {
        const roundie_elf_section_t *data = &out->sections[i];
        CHECK(data->size == 0 || out->sections[0].address + out->code_bytes <= data->address ||
              data->address + data->size <= out->sections[0].address, "rounded-text-overlap");
    }
    CHECK(header.e_entry != 0 && within(header.e_entry, 3, &out->sections[0]), "entry-address");
    out->entry = header.e_entry;
    for (int i = 0; i < 5; i++) {
        for (int j = i + 1; j < 5; j++) {
            const roundie_elf_section_t *a = &out->sections[i];
            const roundie_elf_section_t *b = &out->sections[j];
            CHECK(a->size == 0 || b->size == 0 || a->address + a->size <= b->address ||
                  b->address + b->size <= a->address, "overlapping-load-sections");
        }
    }
    bool entry_symbol = false;
    for (unsigned i = 0; i < header.e_shnum; i++) {
        Elf32_Shdr symbols = section(bytes, &header, i);
        if (symbols.sh_type != SHT_DYNSYM && symbols.sh_type != SHT_SYMTAB) {
            continue;
        }
        CHECK(symbols.sh_entsize == sizeof(Elf32_Sym) && symbols.sh_size % sizeof(Elf32_Sym) == 0 &&
              symbols.sh_size / sizeof(Elf32_Sym) <= 1024 && symbols.sh_link < header.e_shnum &&
              (symbols.sh_offset & 3U) == 0, "symbol-table");
        Elf32_Shdr strings = section(bytes, &header, symbols.sh_link);
        CHECK(strings.sh_type == SHT_STRTAB, "symbol-strings");
        for (unsigned j = 0; j < symbols.sh_size / sizeof(Elf32_Sym); j++) {
            Elf32_Sym symbol;
            memcpy(&symbol, bytes + symbols.sh_offset + j * sizeof(symbol), sizeof(symbol));
            const char *name = string(bytes, &strings, symbol.st_name);
            CHECK(name != NULL, "symbol-name-bounds");
            CHECK(ELF32_ST_TYPE(symbol.st_info) != STT_TLS, "TLS-symbol");
            if (symbol.st_shndx == SHN_UNDEF && name[0] != '\0') {
                if (!roundie_elf_import_allowed(name) ||
                    strcmp(name, out->abi_version == 1 ? "rep_abi_v1" : "rep_abi_v2") != 0) {
                    snprintf(error, ROUNDIE_ELF_ERROR_SIZE, "import:%.64s", name);
                    return false;
                }
                out->imports++;
            }
            if (strcmp(name, out->abi_version == 1 ? "rep_app_entry" : "rep_app_entry_v2") == 0) {
                CHECK(symbol.st_value == header.e_entry && ELF32_ST_TYPE(symbol.st_info) == STT_FUNC &&
                      symbol.st_shndx < header.e_shnum && symbol.st_shndx != SHN_UNDEF, "entry-symbol");
                entry_symbol = true;
            }
        }
    }
    CHECK(entry_symbol && out->imports > 0, "missing-entry-or-service-import");
    for (unsigned i = 0; i < header.e_shnum; i++) {
        Elf32_Shdr relocations = section(bytes, &header, i);
        if (relocations.sh_type != SHT_RELA) {
            continue;
        }
        CHECK(relocations.sh_link < header.e_shnum && relocations.sh_entsize == sizeof(Elf32_Rela) &&
              relocations.sh_size % sizeof(Elf32_Rela) == 0 &&
              (relocations.sh_offset & 3U) == 0, "relocation-table");
        Elf32_Shdr symbols = section(bytes, &header, relocations.sh_link);
        CHECK(symbols.sh_type == SHT_DYNSYM || symbols.sh_type == SHT_SYMTAB, "relocation-symbols");
        Elf32_Shdr strings = section(bytes, &header, symbols.sh_link);
        out->relocations += relocations.sh_size / sizeof(Elf32_Rela);
        CHECK(out->relocations <= 1024, "relocation-limit");
        for (unsigned j = 0; j < relocations.sh_size / sizeof(Elf32_Rela); j++) {
            Elf32_Rela relocation;
            memcpy(&relocation, bytes + relocations.sh_offset + j * sizeof(relocation), sizeof(relocation));
            unsigned type = ELF32_R_TYPE(relocation.r_info);
            unsigned symbol_index = ELF32_R_SYM(relocation.r_info);
            CHECK(type == 2 || type == 3 || type == 4 || type == 5, "unsupported-relocation");
            CHECK(symbol_index < symbols.sh_size / sizeof(Elf32_Sym), "relocation-symbol-index");
            int target = mapped(out, relocation.r_offset, 4);
            CHECK(target >= 0 && (relocation.r_offset & 3U) == 0 && alignments[target] >= 4 &&
                  (out->sections[target].address & 3U) == 0, "relocation-target");
            CHECK(type == 2 || relocation.r_addend == 0, "unsupported-relocation-addend");
            if (type == 5) {
                CHECK(symbol_index == 0 && target != 1, "relative-relocation");
                roundie_elf_section_t *destination = &out->sections[target];
                uint32_t value;
                memcpy(&value, bytes + destination->offset + relocation.r_offset - destination->address, 4);
                CHECK(mapped(out, value, 1) >= 0, "relative-value");
            } else if (type == 3 || type == 4) {
                Elf32_Sym symbol;
                memcpy(&symbol, bytes + symbols.sh_offset + symbol_index * sizeof(symbol), sizeof(symbol));
                const char *name = string(bytes, &strings, symbol.st_name);
                CHECK(symbol.st_shndx == SHN_UNDEF && symbol.st_value == 0 &&
                      roundie_elf_import_allowed(name), "relocation-import");
            }
        }
    }
    return true;
#undef CHECK
}
