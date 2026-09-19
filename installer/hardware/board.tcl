# Exact MainBoard v2.6 / H750IBK6, single W25Q128JV. No SD initialization.
set QUADSPI 1
source scripts/stlink-h750.cfg
reset_config srst_only srst_nogate connect_assert_srst
adapter speed 1000

proc g100_word {a} { return [lindex [read_memory $a 32 1] 0] }
proc g100_hardware_check {} {
    if {[g100_word 0x5c001000] != 0x20036450} {
        error "Unsupported MCU ID/revision"
    }
    if {[lindex [read_memory 0x1ff1e880 16 1] 0] != 128} {
        error "Unsupported internal Flash capacity"
    }
}
proc g100_target_check {} {
    global G100_EXPECTED_UID
    g100_hardware_check
    if {![info exists G100_EXPECTED_UID] || [llength $G100_EXPECTED_UID] != 3} {
        error "A three-word UID guard is required"
    }
    set actual [read_memory 0x1ff1e800 32 3]
    for {set i 0} {$i < 3} {incr i} {
        if {[lindex $actual $i] != [lindex $G100_EXPECTED_UID $i]} {
            error "Physical UID changed: refusing operation"
        }
    }
}
proc g100_af {base pin af pull} {
    set s [expr {2*$pin}]
    set a [expr {$base+32+4*($pin/8)}]
    set t [expr {4*($pin%8)}]
    mmw [expr {$base+4}] 0 [expr {1<<$pin}]
    mmw [expr {$base+8}] [expr {3<<$s}] 0
    mmw [expr {$base+12}] [expr {$pull<<$s}] [expr {3<<$s}]
    mmw $a [expr {$af<<$t}] [expr {15<<$t}]
    mmw $base [expr {2<<$s}] [expr {3<<$s}]
}
proc g100_qspi_read_map {} {
    # Caller has halted the board. Use only read opcode 0B and conservative clock.
    mmw 0x580244E0 0x32 0
    mmw 0x580244D4 0x4000 0
    if {[g100_word 0x52005000] & 1} {
        mmw 0x52005000 2 0
        sleep 2
    }
    mww 0x52005000 0
    mww 0x5200500c 0x1b
    g100_af 0x58020400 6 10 1
    g100_af 0x58020400 2 9 0
    g100_af 0x58021400 8 10 0
    g100_af 0x58021400 9 10 0
    g100_af 0x58021000 2 9 1
    g100_af 0x58021400 6 9 1
    mww 0x52005004 0x00170300
    mww 0x52005000 0x13000011
    mww 0x52005014 0x0d20250b
    flash probe 1
}
