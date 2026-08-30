.syntax unified
.cpu cortex-m4
.thumb

.global Reset_Handler
.global Default_Handler

.section .isr_vector, "a", %progbits
.word _estack
.word Reset_Handler
.word NMI_Handler
.word HardFault_Handler
.word MemManage_Handler
.word BusFault_Handler
.word UsageFault_Handler
.word 0
.word 0
.word 0
.word 0
.word SVC_Handler
.word DebugMon_Handler
.word 0
.word PendSV_Handler
.word SysTick_Handler


.section .text.Reset_Handler
.type Reset_Handler, %function

Reset_Handler:
    /* Copy .data from Flash to RAM */
    ldr r0, =_sdata
    ldr r1, =_edata
    ldr r2, =_sidata      /* need to add this symbol - see below */
copy_data_loop:
    cmp r0, r1
    bhs copy_data_done
    ldr r3, [r2], #4
    str r3, [r0], #4
    b copy_data_loop
copy_data_done:

    /* Zero .bss */
    ldr r0, =_sbss
    ldr r1, =_ebss
    movs r2, #0
zero_bss_loop:
    cmp r0, r1
    bhs zero_bss_done
    str r2, [r0], #4
    b zero_bss_loop
zero_bss_done:

    bl main
    b .

.section .text.Default_Handler
.type Default_Handler, %function

Default_Handler:
    b .


.weak NMI_Handler
.thumb_set NMI_Handler, Default_Handler

.weak HardFault_Handler
.thumb_set HardFault_Handler, Default_Handler

.weak MemManage_Handler
.thumb_set MemManage_Handler, Default_Handler

.weak BusFault_Handler
.thumb_set BusFault_Handler, Default_Handler

.weak UsageFault_Handler
.thumb_set UsageFault_Handler, Default_Handler

.weak SVC_Handler
.thumb_set SVC_Handler, Default_Handler

.weak DebugMon_Handler
.thumb_set DebugMon_Handler, Default_Handler

.weak PendSV_Handler
.thumb_set PendSV_Handler, Default_Handler

.weak SysTick_Handler
.thumb_set SysTick_Handler, Default_Handler