/* elf_loader 1.3.5 rounds .text size up to four bytes and uses the rounded
 * virtual range when mapping pointers. Keep the next section outside it.
 * This augments the toolchain's default shared-ELF script. */
SECTIONS
{
    .rodata ALIGN(4) : { *(.rodata .rodata.* .gnu.linkonce.r.*) }
}
INSERT AFTER .text;
