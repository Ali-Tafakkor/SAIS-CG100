ENTRY(test_http)
MEMORY { CODE (rwx) : ORIGIN = 0x08000000, LENGTH = 2M }
SECTIONS {
 .text : { *(.text*) *(.rodata*) } > CODE
 .ARM.exidx : { *(.ARM.exidx*) } > CODE
 .data : { *(.data*) } > CODE
 .bss (NOLOAD) : { *(.bss*) *(COMMON) } > CODE
}
