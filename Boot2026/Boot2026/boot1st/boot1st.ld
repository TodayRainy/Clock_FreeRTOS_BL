ENTRY(Reset_Handler)

MEMORY
{
    RAM (xrw)   : ORIGIN = 0x20000000, LENGTH = 128K
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 128K
}

SECTIONS
{
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } >FLASH

    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        . = ALIGN(4);
    } >FLASH

    .boot_2rd :
    {
        . = ALIGN(4);
        _sboot_2rd_start = .;
        KEEP(*(.boot_2rd))
        . = ALIGN(4);
        _sboot_2rd_end = .;
    } >FLASH

    /DISCARD/ :
    {
        libc.a ( * )
        libm.a ( * )
        libgcc.a ( * )
    }
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
