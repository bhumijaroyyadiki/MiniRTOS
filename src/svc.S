.syntax unified
.cpu cortex-m4
.thumb

.global SVC_Handler
.type SVC_Handler, %function

// References to external symbols defined in your C code
.extern task_a_tcb

SVC_Handler:
    // 1. Determine whether the caller used MSP or PSP
    tst    lr, #4
    ite    eq
    mrseq  r3, msp
    mrsne  r3, psp

    // 2. Extract the SVC number from the instruction
    ldr    r2, [r3, #24]    // Get saved PC from stack
    ldrh   r2, [r2, #-2]    // Read the halfword instruction right before PC
    and    r2, r2, #0xFF    // Mask out the SVC number

    // 3. Evaluate the SVC command
    cmp    r2, #0
    beq    svc_bootstrap
    b      svc_unknown      // Any other number (like the old #1) is now handled as unknown

svc_bootstrap:
    // Your verified Week 1 bootstrapper to launch the initial environment
    ldr    r0, =task_a_tcb
    ldr    r0, [r0]
    ldmia  r0!, {r4-r11}    // Pop cold registers from initial stack
    msr    psp, r0          // Set up the process stack pointer
    isb
    ldr    lr, =0xFFFFFFFD  // Return to Thread mode using PSP
    bx     lr

svc_unknown:
    b      svc_unknown

.size SVC_Handler, . - SVC_Handler
