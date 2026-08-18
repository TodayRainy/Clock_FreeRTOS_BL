/* STM32F407 Bootloader 固定占用内部 Flash 前48 KiB。 */
MEMORY
{
    FLASH  (rx)  : ORIGIN = 0x08000000, LENGTH = 48K
    RAM    (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
    CCMRAM (xrw) : ORIGIN = 0x10000000, LENGTH = 64K
}

_Min_Heap_Size = 0x200;
_Min_Stack_Size = 0x200;

INCLUDE stm32f407_sections.ld

