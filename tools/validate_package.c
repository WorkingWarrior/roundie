#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/sha.h>
#include "roundie_package_format.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        return 2;
    }
    FILE *file = fopen(argv[1], "rb");
    uint8_t *bytes = malloc(ROUNDIE_PACKAGE_MAX + 1);
    if (file == NULL || bytes == NULL) {
        if (file != NULL) {
            fclose(file);
        }
        free(bytes);
        return 2;
    }
    size_t size = fread(bytes, 1, ROUNDIE_PACKAGE_MAX + 1, file);
    bool okay = ferror(file) == 0;
    okay = fclose(file) == 0 && okay;
    roundie_package_metadata_t metadata;
    roundie_elf_info_t elf;
    char error[ROUNDIE_ELF_ERROR_SIZE] = "file-read";
    uint8_t digest[32];
    if (okay) {
        okay = roundie_package_decode(bytes, size, size, &metadata, error);
    }
    if (okay && !roundie_package_compatible(&metadata)) {
        snprintf(error, sizeof(error), "package-incompatible");
        okay = false;
    }
    if (okay) {
        okay = SHA256(bytes + ROUNDIE_PACKAGE_PREFIX, metadata.elf_size, digest) != NULL &&
            memcmp(digest, metadata.elf_digest, 32) == 0;
        if (!okay) {
            snprintf(error, sizeof(error), "package-ELF-digest");
        }
    }
    if (okay) {
        okay = roundie_elf_validate(bytes + ROUNDIE_PACKAGE_PREFIX, metadata.elf_size, &elf, error);
        if (okay && elf.abi_version != metadata.abi) {
            snprintf(error, sizeof(error), "package-ELF-ABI");
            okay = false;
        }
    }
    free(bytes);
    if (!okay) {
        fprintf(stderr, "Package rejected: %s\n", error);
        return 3;
    }
    printf("{\"format\":1,\"size\":%zu,\"elf_size\":%u,\"abi\":%u,\"code\":%u,\"data\":%u}\n",
           size, metadata.elf_size, elf.abi_version, elf.code_bytes, elf.data_bytes);
    return 0;
}
