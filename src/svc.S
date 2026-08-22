.syntax unified
    .cpu cortex-m4
    .thumb

    .global SVC_Handler
    .type SVC_Handler, %function

SVC_Handler:
    tst    lr, #4
    ite    eq
    mrseq  r3, msp
    mrsne  r3, psp

    ldr    r2, [r3, #24]
    ldrh   r2, [r2, #-2]
    and    r2, r2, #0xFF

    cmp    r2, #0
    beq    svc_bootstrap
    cmp    r2, #1
    beq    svc_context_switch
    b      svc_unknown

svc_bootstrap:
    ldr    r0, =task_a_tcb
    ldr    r0, [r0]
    ldmia  r0!, {r4-r11}
    msr    psp, r0
    isb
    ldr    lr, =0xFFFFFFFD
    bx     lr

svc_context_switch:
    mrs    r2, psp
    stmdb  r2!, {r4-r11}
    str    r2, [r0]
    ldr    r2, [r1]
    ldmia  r2!, {r4-r11}
    msr    psp, r2
    bx     lr

svc_unknown:
    b      svc_unknown

    .size SVC_Handler, . - SVC_Handler