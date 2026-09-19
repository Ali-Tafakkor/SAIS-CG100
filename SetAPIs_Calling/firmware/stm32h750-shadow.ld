ENTRY(Reset_Handler)
MEMORY {
    SDRAM_CODE (rx) : ORIGIN = 0xC0000000, LENGTH = 0x3FF000
    STATUS (rw) : ORIGIN = 0x24000000, LENGTH = 0x80
    RAM_D1 (rw) : ORIGIN = 0x24000100, LENGTH = 0x7FF00
    ETH_RX_DESC (rw) : ORIGIN = 0x30000000, LENGTH = 0x80
    ETH_TX_DESC (rw) : ORIGIN = 0x30000080, LENGTH = 0x80
    ETH_RX_POOL (rw) : ORIGIN = 0x30000100, LENGTH = 0x5F00
    DTCM (rw) : ORIGIN = 0x20000000, LENGTH = 128K
    ITCM (rx) : ORIGIN = 0x00000000, LENGTH = 64K
}
_estack = 0x20020000;
_stack_bottom = 0x20010000;
SECTIONS {
    .isr_vector : { KEEP(*(.isr_vector)) } > SDRAM_CODE
    .text : { *(.text*) *(.rodata*) KEEP(*(.init)) KEEP(*(.fini)) . = ALIGN(4); } > SDRAM_CODE
    .ARM.extab : { *(.ARM.extab*) } > SDRAM_CODE
    .ARM.exidx : { *(.ARM.exidx*) } > SDRAM_CODE
    .itcm_text : { . = ALIGN(4); _sitcm = .; *(.itcm_text*) . = ALIGN(4); _eitcm = .; } > ITCM AT > SDRAM_CODE
    _siitcm = LOADADDR(.itcm_text);
    .status (NOLOAD) : { KEEP(*(.status)) } > STATUS
    .data : { . = ALIGN(4); _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > RAM_D1 AT > SDRAM_CODE
    _sidata = LOADADDR(.data);
    _eimage = LOADADDR(.data) + SIZEOF(.data);
    .bss (NOLOAD) : { . = ALIGN(4); _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; _end = .; end = .; } > RAM_D1
    _heap_limit = 0x24080000;
    .RxDecripSection 0x30000000 (NOLOAD) : { KEEP(*(.RxDecripSection)) } > ETH_RX_DESC
    .TxDecripSection 0x30000080 (NOLOAD) : { KEEP(*(.TxDecripSection)) } > ETH_TX_DESC
    .Rx_PoolSection 0x30000100 (NOLOAD) : { KEEP(*(.Rx_PoolSection)) } > ETH_RX_POOL
    ASSERT(_eimage <= 0xC03FF000, "SDRAM/NOR image slot capacity exceeded: preflight required; SD disabled")
    ASSERT(_ebss <= _heap_limit - 65536, "Less than 64 KiB bounded C heap")
    ASSERT(SIZEOF(.status) <= 128, "Application status overlaps boot handoff")
    ASSERT(SIZEOF(.RxDecripSection) <= 128, "RX descriptor overflow")
    ASSERT(SIZEOF(.TxDecripSection) <= 128, "TX descriptor overflow")
    ASSERT(SIZEOF(.Rx_PoolSection) <= 0x5F00, "RX pool overflow")
    /DISCARD/ : { *(.comment) }
}
