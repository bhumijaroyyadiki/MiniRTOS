.syntax unified
.cpu cortex-m4
.thumb

// Import the global pointers managed by C code
.extern current_task
.extern next_task

.global PendSV_Handler
.type PendSV_Handler, %function

PendSV_Handler:
    mrs   r0, psp
    stmdb r0!, {r4-r11}

    ldr   r1, =current_task
    ldr   r2, [r1]          // r2 = OLD tcb pointer (still correct, since yield() no longer overwrote it)
    str   r0, [r2]          // old_tcb->sp = r0

    ldr   r3, =next_task
    ldr   r3, [r3]          // r3 = NEW tcb pointer
    str   r3, [r1]          // NOW commit: current_task = next_task

    ldr   r0, [r3]          // r0 = new_tcb->sp   (correct: dereference the TCB, not the stack address)
    ldmia r0!, {r4-r11}
    msr   psp, r0
    bx    lr
    
    .size PendSV_Handler, . - PendSV_Handler