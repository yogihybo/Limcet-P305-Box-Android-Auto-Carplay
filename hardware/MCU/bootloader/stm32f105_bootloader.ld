/* Linker script for STM32F105 Resident IAP Bootloader (12 KB) */
ENTRY(Reset_Handler)

MEMORY
{
    /* Real factory bootloader reserves 0x08000000-0x080027FF (10K) for code/data.
     * Page 5 (0x08002800-0x08002FFF, 2K) is strictly reserved for the 0x5AA5
     * interrupted-update recovery flag at 0x08002FFE.
     * 0x08003000 is the real application vector table base. */
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 10K
    RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 64K
}

_estack = ORIGIN(RAM) + 0x2000;

SECTIONS
{
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > FLASH

    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    /DISCARD/ :
    {
        *(.ARM.exidx*)
        *(.ARM.extab*)
    }
}
