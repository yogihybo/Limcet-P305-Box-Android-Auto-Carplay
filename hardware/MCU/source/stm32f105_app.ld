/* Linker script for STM32F105RBT6 Application Firmware (can_app.bin) */
ENTRY(Reset_Handler)

MEMORY
{
    /* Real factory bootloader's app-validation routine (0x08001844) confirms
     * the real application vector table base is 0x08003000, not 0x08004000 --
     * see docs/MCU_FIRMWARE_VERIFIED_FINDINGS.md sections 5 and 6.
     * 128K total flash - 12K bootloader = 116K available to the application. */
    FLASH (rx)      : ORIGIN = 0x08003000, LENGTH = 116K
    RAM   (xrw)     : ORIGIN = 0x20000000, LENGTH = 64K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
    /* The vector table goes at the start of FLASH */
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > FLASH

    /* Program code and constants */
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

    /* Initialized data loaded from FLASH into RAM */
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

    /* Uninitialized data in RAM */
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

    /* Discard unused debugging/metadata */
    /DISCARD/ :
    {
        *(.note*)
        *(.comment)
        *(.ARM.exidx*)
        *(.ARM.extab*)
    }
}
