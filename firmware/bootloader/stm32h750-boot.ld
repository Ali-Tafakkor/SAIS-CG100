ENTRY(Reset_Handler)
MEMORY {
    FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 128K
    DTCM (rw) : ORIGIN = 0x20000000, LENGTH = 128K
}
_estack = 0x20020000;
SECTIONS {
    .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
    .text : { *(.text*) *(.rodata*) . = ALIGN(4); } > FLASH
    .ARM.extab : { *(.ARM.extab*) } > FLASH
    .ARM.exidx : { *(.ARM.exidx*) } > FLASH
    .data : { . = ALIGN(4); _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > DTCM AT > FLASH
    _sidata = LOADADDR(.data);
    _eimage = LOADADDR(.data) + SIZEOF(.data);
    .bss (NOLOAD) : { . = ALIGN(4); _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > DTCM
    ASSERT(_ebss < _estack - 32768, "Stage 0 stack budget")
    ASSERT(_eimage <= 0x08020000, "Stage 0 exceeds internal flash")
    /DISCARD/ : { *(.comment) *(.note*) }
}
