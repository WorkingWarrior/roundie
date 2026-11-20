#pragma once
#include <stddef.h>
#include <stdint.h>
#define ELF_SEC_TEXT 0
#define ELF_SEC_BSS 1
#define ELF_SEC_DATA 2
#define ELF_SEC_RODATA 3
#define ELF_SEC_DRLRO 4
#define ELF_SECS 5
typedef struct {uintptr_t addr; size_t size; uintptr_t v_addr;} esp_elf_sec_t;
typedef struct {
    void *ptext;
    void *pdata;
    esp_elf_sec_t sec[5];
    int (*entry)(int, char **);
} esp_elf_t;
int esp_elf_init(esp_elf_t *image);
int esp_elf_relocate(esp_elf_t *image, const uint8_t *bytes);
void esp_elf_deinit(esp_elf_t *image);
void elf_set_symbol_resolver(uintptr_t (*resolver)(const char *));
