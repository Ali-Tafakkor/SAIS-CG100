.syntax unified
.cpu cortex-m7
.thumb
.section .isr_vector,"a",%progbits
.balign 1024
.global g_vectors
g_vectors:
    .word _estack
    .word Reset_Handler
    .rept 13
    .word Default_Handler
    .endr
    .word SysTick_Handler
    .rept 240
    .word Default_Handler
    .endr
.size g_vectors,.-g_vectors
.section .text.Reset_Handler,"ax",%progbits
.global Reset_Handler
.type Reset_Handler,%function
Reset_Handler:
    cpsid i
    /* NEVER call vendor SystemInit here: it stops the SDRAM/CPU clock. */
    ldr r0,=_sidata
    ldr r1,=_sdata
    ldr r2,=_edata
1:  cmp r1,r2
    bcs 2f
    ldr r3,[r0],#4
    str r3,[r1],#4
    b 1b
2:  ldr r1,=_sbss
    ldr r2,=_ebss
    movs r3,#0
3:  cmp r1,r2
    bcs 4f
    str r3,[r1],#4
    b 3b
4:  ldr r0,=_siitcm
    ldr r1,=_sitcm
    ldr r2,=_eitcm
6:  cmp r1,r2
    bcs 7f
    ldr r3,[r0],#4
    str r3,[r1],#4
    b 6b
7:  ldr r0,=g_vectors
    ldr r1,=0xE000ED08
    str r0,[r1]
    dsb
    isb
    bl SystemCoreClockUpdate
    cpsie i
    bl main
5:  b 5b
.size Reset_Handler,.-Reset_Handler
.section .note.GNU-stack,"",%progbits
