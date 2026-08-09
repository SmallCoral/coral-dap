.syntax unified
.cpu cortex-m3
.thumb

.global g_pfnVectors
.global Reset_Handler
.type Reset_Handler, %function
.type Default_Handler, %function

.section .isr_vector,"a",%progbits
.type g_pfnVectors, %object
g_pfnVectors:
  .word _estack
  .word Reset_Handler
  .rept 57
  .word Default_Handler
  .endr
.size g_pfnVectors, .-g_pfnVectors

.section .text.Reset_Handler,"ax",%progbits
.thumb_func
Reset_Handler:
  ldr r0, =_sidata
  ldr r1, =_sdata
  ldr r2, =_edata

.Lcopy_data:
  cmp r1, r2
  bcs .Lzero_bss
  ldr r3, [r0], #4
  str r3, [r1], #4
  b .Lcopy_data

.Lzero_bss:
  ldr r1, =_sbss
  ldr r2, =_ebss
  movs r3, #0

.Lzero_bss_loop:
  cmp r1, r2
  bcs .Lcall_main
  str r3, [r1], #4
  b .Lzero_bss_loop

.Lcall_main:
  bl main

.Lhang:
  b .Lhang
.size Reset_Handler, .-Reset_Handler

.section .text.Default_Handler,"ax",%progbits
.thumb_func
Default_Handler:
  b .
.size Default_Handler, .-Default_Handler
