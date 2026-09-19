.syntax unified
.cpu cortex-m7
.thumb
.section .text
.global large_probe
.type large_probe, %function
.thumb_func
large_probe:
    movw r0, #0x5a5a
    movt r0, #0xa5a5
    bx lr
.balign 256
.global large_entry
.type large_entry, %function
.thumb_func
large_entry:
    bl large_probe
    ldr r1, =0xa5a55a5a
    cmp r0, r1
    bne failed_probe
    ldr r1, =0x240000c4
    str r0, [r1]
    ldr r3, original_entry
    bx r3
failed_probe:
    b failed_probe
.balign 4
original_entry:
    .word 0xabcdef01
.ltorg
.section .note.GNU-stack,"",%progbits
