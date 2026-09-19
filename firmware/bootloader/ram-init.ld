ENTRY(Reset_Handler)
MEMORY {
    CODE (rx) : ORIGIN = 0x00004000, LENGTH = 48K
    DATA (rw) : ORIGIN = 0x20000000, LENGTH = 128K
}
_estack = 0x20020000;
SECTIONS {
    .isr_vector : { KEEP(*(.isr_vector)) } > CODE
    .text : { *(.text*) *(.rodata*) . = ALIGN(4); } > CODE
    .ARM.extab : { *(.ARM.extab*) } > CODE
    .ARM.exidx : { *(.ARM.exidx*) } > CODE
    .data : { . = ALIGN(4); _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > DATA AT > CODE
    _sidata = LOADADDR(.data);
    _eimage = LOADADDR(.data)+SIZEOF(.data);
    .bss (NOLOAD) : { . = ALIGN(4); _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > DATA
    ASSERT(_ebss <= _estack-32768, "RAM initializer stack")
    /DISCARD/ : { *(.comment) *(.note*) }
}
