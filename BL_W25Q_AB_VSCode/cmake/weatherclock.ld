/* Application 从0x08010000执行，最大长度与 XBIN 契约一致，为448 KiB。 */
MEMORY
{
    FLASH  (rx)  : ORIGIN = 0x08010000, LENGTH = 448K
    RAM    (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
    CCMRAM (xrw) : ORIGIN = 0x10000000, LENGTH = 64K
}

_Min_Heap_Size = 0x1000;
_Min_Stack_Size = 0x1000;

INCLUDE stm32f407_sections.ld

