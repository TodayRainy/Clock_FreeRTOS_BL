    .section .text
    .syntax unified
    .thumb
    .global JumpApp
    .type JumpApp, %function
JumpApp:
    ldr sp, [r0, #0]
    ldr r1, [r0, #4]
    bx  r1
    .size JumpApp, .-JumpApp
