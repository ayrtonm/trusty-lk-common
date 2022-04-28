/*
 * Copyright (c) 2009 Corey Tabaka
 * Copyright (c) 2015-2018 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <debug.h>
#include <arch.h>
#include <arch/ops.h>
#include <arch/x86.h>
#include <arch/x86/mmu.h>
#include <arch/x86/mp.h>
#include <arch/x86/descriptor.h>
#include <arch/fpu.h>
#include <arch/mmu.h>
#include <platform.h>
#include <sys/types.h>
#include <string.h>

/* early stack */
uint8_t _kstack[PAGE_SIZE] __ALIGNED(8);

/* save a pointer to the multiboot information coming in from whoever called us */
/* make sure it lives in .data to avoid it being wiped out by bss clearing */
__SECTION(".data") void *_multiboot_info;

/* main tss */
tss_t system_tss[SMP_MAX_CPUS];
x86_per_cpu_states_t per_cpu_states[SMP_MAX_CPUS];

volatile int cpu_woken_up = 0;

static void init_per_cpu_state(uint cpu)
{
    x86_per_cpu_states_t states;

    /*
     * At this point, BSP has already set up current thread in global state,
     * init global states of AP(s) only.
     */
    if (0 != cpu) {
        states = per_cpu_states[cpu];

        states.cur_thread    = NULL;
        states.syscall_stack = 0;

        write_msr(X86_MSR_GS_BASE, (uint64_t)&states);
    }
}

void arch_early_init(void)
{
    seg_sel_t sel = 0;
    uint cpu_id = 1;

    cpu_id = atomic_add(&cpu_woken_up, cpu_id);

    init_per_cpu_state(cpu_id);

    if (check_fsgsbase_avail()) {
        x86_set_cr4(x86_get_cr4() | X86_CR4_FSGSBASE);
    }

    sel = (seg_sel_t)(cpu_id << 4);
    sel += TSS_SELECTOR;

    /* enable caches here for now */
    clear_in_cr0(X86_CR0_NW | X86_CR0_CD);

    set_global_desc(sel,
            &system_tss[cpu_id],
            sizeof(tss_t),
            1,
            0,
            0,
            SEG_TYPE_TSS,
            0,
            0);
    x86_ltr(sel);

    x86_mmu_early_init();
    platform_init_mmu_mappings();
}

void arch_init(void)
{
    x86_mmu_init();

#ifdef X86_WITH_FPU
    fpu_init();
#endif
}

void arch_chain_load(void *entry, ulong arg0, ulong arg1, ulong arg2, ulong arg3)
{
    PANIC_UNIMPLEMENTED;
}

void arch_enter_uspace(vaddr_t entry_point, vaddr_t user_stack_top, vaddr_t shadow_stack_base, uint32_t flags, ulong arg0)
{
    DEBUG_ASSERT(shadow_stack_base == 0);

    PANIC_UNIMPLEMENTED;
#if 0
    DEBUG_ASSERT(IS_ALIGNED(user_stack_top, 16));

    thread_t *ct = get_current_thread();

    vaddr_t kernel_stack_top = (uintptr_t)ct->stack + ct->stack_size;
    kernel_stack_top = round_down(kernel_stack_top, 16);

    /* set up a default spsr to get into 64bit user space:
     * zeroed NZCV
     * no SS, no IL, no D
     * all interrupts enabled
     * mode 0: EL0t
     */
    uint32_t spsr = 0;

    arch_disable_ints();

    asm volatile(
        "mov    sp, %[kstack];"
        "msr    sp_el0, %[ustack];"
        "msr    elr_el1, %[entry];"
        "msr    spsr_el1, %[spsr];"
        "eret;"
        :
        : [ustack]"r"(user_stack_top),
        [kstack]"r"(kernel_stack_top),
        [entry]"r"(entry_point),
        [spsr]"r"(spsr)
        : "memory");
    __UNREACHABLE;
#endif
}

void arch_set_user_tls(vaddr_t tls_ptr)
{
    thread_t *cur_thread = get_current_thread();

    cur_thread->arch.fs_base = tls_ptr;
    write_msr(X86_MSR_FS_BASE, tls_ptr);
}
