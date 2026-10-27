/*
 * CardputerOS app linker script.
 *
 * The loader copies the code segment into executable RAM and the data segment into
 * normal RAM, then adds the load offset to every absolute address kept in the
 * relocations (--emit-relocs). The link addresses below are only used to tell the
 * two segments apart; they never exist on the device.
 */
ENTRY(cpos_app_entry)

MEMORY
{
    code (rx) : ORIGIN = 0x10000000, LENGTH = 1M
    data (rw) : ORIGIN = 0x20000000, LENGTH = 1M
}

PHDRS
{
    code PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    .text : ALIGN(4)
    {
        *(.literal .text .literal.* .text.*)
        . = ALIGN(4);
    } > code :code

    .rodata : ALIGN(4)
    {
        *(.rodata .rodata.* .srodata .srodata.*)
        . = ALIGN(4);
    } > data :data

    .cpos_app : ALIGN(4)
    {
        KEEP(*(.cpos_app))
    } > data :data

    .init_array : ALIGN(4)
    {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) .init_array))
        __init_array_end = .;
    } > data :data

    /* the Xtensa toolchain emits constructors as .ctors; they run last-to-first */
    .ctors : ALIGN(4)
    {
        __ctors_start = .;
        KEEP(*(.ctors))
        KEEP(*(.ctors.*))
        __ctors_end = .;
    } > data :data

    .data : ALIGN(4)
    {
        *(.data .data.* .sdata .sdata.*)
        . = ALIGN(4);
    } > data :data

    .bss (NOLOAD) : ALIGN(4)
    {
        *(.bss .bss.* .sbss .sbss.* COMMON)
        . = ALIGN(4);
    } > data :data

    /DISCARD/ :
    {
        *(.xt.prop .xt.prop.* .xt.lit .xt.lit.* .xt.profile_*)
        *(.comment .note .note.* .eh_frame .eh_frame_hdr .fini_array .fini_array.* .preinit_array)
        *(.xtensa.info .debug_gdb_scripts)
    }
}
