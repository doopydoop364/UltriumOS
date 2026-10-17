/* Layout for native Ultrium programs: a read+execute segment (headers, code, read-only data) and a
   read+write segment (data, bss) on its own page. The file is kept compact (the data segment follows
   the code in the file but starts on a fresh page in memory). */
ENTRY(_start)
PHDRS
{
    text PT_LOAD FILEHDR PHDRS FLAGS(5);
    data PT_LOAD FLAGS(6);
}
SECTIONS
{
    . = 0x400000 + SIZEOF_HEADERS;
    .text : { *(.text .text.*) } :text
    .rodata : { *(.rodata .rodata.*) } :text
    . = ALIGN(0x1000);
    .data : { *(.data .data.*) } :data
    .bss : { *(.bss .bss.* COMMON) } :data
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.gnu*) *(.got*) *(.dynsym) *(.dynstr) *(.dynamic) *(.hash) *(.interp) *(.rela*) *(.plt*) }
}
