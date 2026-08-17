.syntax unified
.cpu cortex-m4
.fpu softvfp
.thumb

.global  g_pfnVectors
.global  Default_Handler

.section  .text.Reset_Handler
.weak  Reset_Handler
.type  Reset_Handler, %function

Reset_Handler:
  ldr   sp, =_estack

  /* Copy 2nd-stage boot image from FLASH section .boot_2rd into RAM@0x20000000 */
  ldr r0, =_sboot_2rd_start
  ldr r1, =_sboot_2rd_end
  ldr r2, =0x20000000

CopyBoot2rd:
  cmp r0, r1
  bcs Boot2rdDone
  ldr r3, [r0], #4
  str r3, [r2], #4
  b CopyBoot2rd

Boot2rdDone:
  /* VTOR = RAM vector table base of 2nd-stage boot */
  ldr r0, =0xE000ED08
  ldr r1, =0x20000000
  str r1, [r0]
  dsb
  isb

  /* Load 2nd-stage MSP/Reset vector and jump */
  ldr r0, =0x20000000
  ldr r1, [r0]
  msr msp, r1
  ldr r1, [r0, #4]
  bx  r1
.size  Reset_Handler, .-Reset_Handler

.section  .text.Default_Handler,"ax",%progbits
Default_Handler:
Infinite_Loop:
  b  Infinite_Loop
  .size  Default_Handler, .-Default_Handler

.section  .isr_vector,"a",%progbits
.type  g_pfnVectors, %object

g_pfnVectors:
  .word  _estack
  .word  Reset_Handler
  .word  NMI_Handler
  .word  HardFault_Handler
  .word  MemManage_Handler
  .word  BusFault_Handler
  .word  UsageFault_Handler
  .word  0
  .word  0
  .word  0
  .word  0
  .word  SVC_Handler
  .word  DebugMon_Handler
  .word  0
  .word  PendSV_Handler
  .word  SysTick_Handler

  .size  g_pfnVectors, .-g_pfnVectors

   .weak      NMI_Handler
   .thumb_set NMI_Handler,Default_Handler

   .weak      HardFault_Handler
   .thumb_set HardFault_Handler,Default_Handler

   .weak      MemManage_Handler
   .thumb_set MemManage_Handler,Default_Handler

   .weak      BusFault_Handler
   .thumb_set BusFault_Handler,Default_Handler

   .weak      UsageFault_Handler
   .thumb_set UsageFault_Handler,Default_Handler

   .weak      SVC_Handler
   .thumb_set SVC_Handler,Default_Handler

   .weak      DebugMon_Handler
   .thumb_set DebugMon_Handler,Default_Handler

   .weak      PendSV_Handler
   .thumb_set PendSV_Handler,Default_Handler

   .weak      SysTick_Handler
   .thumb_set SysTick_Handler,Default_Handler
