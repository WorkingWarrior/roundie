#include <stdio.h>
#include <stdlib.h>
#include "roundie_elf_validate.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        return 2;
    }
    FILE *file = fopen(argv[1], "rb");
    if (file == NULL) {
        return 2;
    }
    uint8_t bytes[ROUNDIE_ELF_MAX_FILE + 1];
    size_t size = fread(bytes, 1, sizeof(bytes), file);
    bool failed = ferror(file) != 0;
    failed = fclose(file) != 0 || failed;
    roundie_elf_info_t info;
    char error[ROUNDIE_ELF_ERROR_SIZE];
    if (failed || !roundie_elf_validate(bytes, size, &info, error)) {
        fprintf(stderr, "ELF rejected: %s\n", failed ? "file-read" : error);
        return 3;
    }
    printf("{\"size\":%zu,\"code\":%u,\"data\":%u,\"relocations\":%u,\"imports\":%u,\"abi\":%u}\n",
           size, info.code_bytes, info.data_bytes, info.relocations, info.imports, info.abi_version);
    return 0;
}
