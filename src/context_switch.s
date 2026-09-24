.syntax unified
.cpu cortex-m4
.thumb

// Import the global pointers managed by C code
.extern current_task
.extern next_task

.global PendSV_Handler
.type PendSV_Handler, %function
.equ DWT_CYCCNT, 0xE0001004

/*
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
    */
PendSV_Handler:
    // --- capture t1 ---
    ldr r12, =DWT_CYCCNT
    ldr r12, [r12]
    ldr r3, =switch_t1
    str r12, [r3]

    mrs   r0, psp
    stmdb r0!, {r4-r11}

    ldr   r1, =current_task
    ldr   r2, [r1]                  // r2 = old task TCB
    str   r0, [r2]                  // save old task SP

    // --- account CPU time of old task ---
    ldr   r12, =DWT_CYCCNT
    ldr   r12, [r12]                // r12 = current cycle count

    ldr   r0, [r2, #48]             // r0 = old->last_resume_cycle
    sub   r12, r12, r0              // elapsed = now - last_resume_cycle

    ldr   r0, [r2, #44]             // r0 = old->cpu_cycles_total
    add   r0, r0, r12
    str   r0, [r2, #44]             // old->cpu_cycles_total += elapsed

    // --- get next task ---
    ldr   r3, =next_task
    ldr   r3, [r3]                  // r3 = new task TCB

    str   r3, [r1]                  // current_task = new task

    // --- start CPU accounting for new task ---
    ldr   r12, =DWT_CYCCNT
    ldr   r12, [r12]
    str   r12, [r3, #48]            // new->last_resume_cycle = now

    // --- restore new task context ---
    ldr   r0, [r3]
    ldmia r0!, {r4-r11}
    msr   psp, r0

    // --- capture t2 ---
    ldr r12, =DWT_CYCCNT
    ldr r12, [r12]
    ldr r0, =switch_t2
    str r12, [r0]

    bx lr