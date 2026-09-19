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
.section .text.Reset_Handler,"ax",%progbits
.global Reset_Handler
.type Reset_Handler,%function
Reset_Handler:
    bl SystemInit
    ldr r0, =_sidata
    ldr r1, =_sdata
    ldr r2, =_edata
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
4:  ldr r0,=g_vectors
    ldr r1,=0xE000ED08
    str r0,[r1]
    dsb
    isb
    bl main
5:  b 5b
.size Reset_Handler,.-Reset_Handler
.global g100_jump
.type g100_jump,%function
g100_jump:
    cpsid i
    ldr r1,[r0]
    ldr r2,[r0,#4]
    msr msp,r1
    movs r3,#0
    msr control,r3
    msr basepri,r3
    msr faultmask,r3
    dsb
    isb
    bx r2
.size g100_jump,.-g100_jump
.section .note.GNU-stack,"",%progbits
