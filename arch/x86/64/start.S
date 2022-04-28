/*
 * Copyright (c) 2009 Corey Tabaka
 * Copyright (c) 2015 Intel Corporation
 * Copyright (c) 2016 Travis Geiselbrecht
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
#include <asm.h>
#include <arch/x86/descriptor.h>
#include <arch/x86/mmu.h>

/* The magic number for the Multiboot header. */
#define MULTIBOOT_HEADER_MAGIC 0x1BADB002

/* The flags for the Multiboot header. */
#if defined(__ELF__) && 0
#define MULTIBOOT_HEADER_FLAGS 0x00000002
#else
#define MULTIBOOT_HEADER_FLAGS 0x00010002
#endif

/* The magic number passed by a Multiboot-compliant boot loader. */
#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

#define MSR_EFER    0xc0000080
#define EFER_LME    0x00000100
#define EFER_NXE    0x00000800
#define MSR_GS_BASE 0xc0000101

#define PHYS(x) ((x) - KERNEL_BASE + MEMBASE)

#define PAGE_MASK               0xfff
#define PAGE_ALIGN_32BIT        0xfffff000
#define ONE_GB                  0x40000000
#define X86_MMU_PG_NX_IN_HIGH   (1 << 31)
#define ADDR_OFFSET_MASK        ((1 << ADDR_OFFSET) - 1)

/*
 * KERNEL_BASE is 64-bit, we cannot operate 64b-bit integer in 32-bit mode,
 * in order to calculate which PML4E and PDPTE should be filled, right shift
 * 30 bits to get KERNEL_BASE in GB.
 */
#define KERNEL_BASE_IN_GB ((KERNEL_BASE >> 30) & 0xFFFFFFFF)

/*
 * We create mapping in 4KB granularity, check whether maxium memory size
 * is valid for Trusty LK in 4KB granularity.
 */
#define KERNEL_BASE_IN_PAGE ((KERNEL_BASE >> PAGE_DIV_SHIFT) & 0xFFFFFFFF)
#define MAX_MEM_SIZE_IN_PAGE (0xFFFFFFFF - KERNEL_BASE_IN_PAGE)

.section ".text.boot"
.code32

/*
 * Macro used to fill in an entry in the page table for the region of memory
 * beginning at virtual address KERNEL_BASE. This macro can be used to fill
 * in PML4E and PDPTE.
 *
 * Input:
 *  table: PML4 or PDPT
 *  entry: base address of PDPT or PD to be filled in PML4E or PDPTE
 *  shift: Since we need right shift address for 9 bits to calculate
 *         index in PML4 (512GB for each entry), this argument should be 9
 *         when fill in PML4 table.
 *         This argument should be 0 when fill in PDPT table.
 */
.macro map_kernel_base table, entry, shift
    movl $KERNEL_BASE_IN_GB, %ecx
    shrl \shift, %ecx
    andl $ADDR_OFFSET_MASK, %ecx
    shll $3, %ecx
    movl $PHYS(\entry), %eax
    orl  $X86_KERNEL_PD_FLAGS, %eax
    movl $PHYS(\table), %edi
    addl %ecx, %edi
    movl %eax, (%edi)
.endm

/*
 * Macro used to fill in an entry in PD or PT in page table.
 * Before invoking this macro, please fill in:
 *   EDI:  address of first entry to be filled
 *   ECX:  numbers of entry to be filled
 * Input:
 *   src_addr: base address of PT if fill in PDE
 *             base address of physical address if fill in PTE
 *   attr:     X86_KERNEL_PD_FLAGS if fill in PDE
 *             X86_KERNEL_PD_LP_FLAGS if fill in PTE
 */
.macro fill_page_table_entry src_addr, attr
    movl \src_addr, %esi
    orl  \attr, %esi
    xorl %eax, %eax
.Lfill_entry_\@:
    movl %eax, %edx
    shll $PAGE_DIV_SHIFT, %edx
    addl %esi, %edx
    movl %edx, (%edi)
    incl %eax
    addl $8, %edi
    loop .Lfill_entry_\@
.endm

/* Macro used to update mapping attribute for each section */
.macro update_mapping_attribute_of_each_section
    leal PHYS(pt), %edi
    movl $KERNEL_LOAD_OFFSET, %ecx
    shrl $PAGE_DIV_SHIFT, %ecx
    shll $3, %ecx
    addl %ecx, %edi

    /* do not use symbol directly, since we are in 32-bit */
    movl $PHYS(__code_start), %eax
    movl $PHYS(__code_end), %ecx
    addl $PAGE_MASK, %ecx
    subl %eax, %ecx
    shrl $PAGE_DIV_SHIFT, %ecx

    /* clear R/W bit in pte */
.Lupdate_code_section:
    movl (%edi), %edx
    andl $~X86_MMU_PG_RW, %edx
    movl %edx, (%edi)
    addl $8, %edi
    loop .Lupdate_code_section

    /* from now, EDI indicates physical address of __rodata_start */
    movl $PHYS(__rodata_start), %eax
    movl $PHYS(__rodata_end), %ecx
    addl $PAGE_MASK, %ecx
    subl %eax, %ecx
    shrl $PAGE_DIV_SHIFT, %ecx

    /* clear R/W bit and set XD bit in pte */
.Lupdate_rodata_section:
    movl (%edi), %edx
    andl $~X86_MMU_PG_RW, %edx
    movl %edx, (%edi)
    leal 4(%edi), %esi
    movl (%esi), %edx
    orl  $X86_MMU_PG_NX_IN_HIGH, %edx
    movl %edx, (%esi)
    addl $8, %edi
    loop .Lupdate_rodata_section

    /* from now, EDI indicates physical address of __data_start */
    movl $PHYS(__data_start), %eax
    movl $PHYS(__data_end), %ecx
    addl $PAGE_MASK, %ecx
    subl %eax, %ecx
    shrl $PAGE_DIV_SHIFT, %ecx

    /* set XD bit in pte */
.Lupdate_data_section:
    leal 4(%edi), %esi
    movl (%esi), %edx
    orl  $X86_MMU_PG_NX_IN_HIGH, %edx
    movl %edx, (%esi)
    addl $8, %edi
    loop .Lupdate_data_section

    /* from now, EDI indicates physical address of __bss_start */
    movl $PHYS(__bss_start), %eax
    movl $PHYS(__bss_end), %ecx
    addl $PAGE_MASK, %ecx
    subl %eax, %ecx
    shrl $PAGE_DIV_SHIFT, %ecx

    /* set XD bit in pte */
.Lupdate_bss_section:
    leal 4(%edi), %esi
    movl (%esi), %edx
    orl  $X86_MMU_PG_NX_IN_HIGH, %edx
    movl %edx, (%esi)
    addl $8, %edi
    loop .Lupdate_bss_section
.endm

/* add mapping up to upper_mem, upper_mem suppports up to 512GB */
.macro map_up_to_uppper_memory
    /* we have already mapped to MEMBASE+KERNEL_LOAD_OFFSET+1GB */
    movl $ONE_GB, %eax
    shrl $PAGE_DIV_SHIFT, %eax

    leal PHYS(mmu_initial_mappings), %edi
    movl 0x10(%edi), %edx
    movl 0x14(%edi), %ebx
    shrl $PAGE_DIV_SHIFT, %edx
    shll $20, %ebx
    /* EBX indicates totally how many pages(4KB) should be mapped */
    addl %edx, %ebx

    /* EBX stores how many pages(4KB) still unmapped */
    subl %eax, %ebx

    movl %ebx, %ecx
    /* upalign memory size to GB */
    addl $0x3ffff, %ecx
    /* ECX indicates how many GB should be mapped */
    shrl $18, %ecx

    /* start to allocate boot memory for PDs */
    movl $PHYS(boot_alloc_end), %edi
    movl (%edi), %edx
    /* upalign to page size */
    addl $PAGE_MASK, %edx
    /* EDX indicates new PD base address (page aligned address of _end) */
    andl $PAGE_ALIGN_32BIT, %edx

    /* store first new PD base address in ESI */
    movl %edx, %esi
    orl  $X86_KERNEL_PD_FLAGS, %edx

    movl $PHYS(pdpt), %edi
    movl $KERNEL_BASE_IN_GB, %eax
    /* EAX indicates PDPTE we have already mapped */
    andl $ADDR_OFFSET_MASK, %eax
    /* EAX indicates PDPTE we should start to fill in now */
    incl %eax
    shll $3, %eax
    addl %eax, %edi
.Lfill_upper_mem_pdpte:
    movl %edx, (%edi)
    addl $0x4, %edi
    movl $0, (%edi)
    addl $PAGE_SIZE, %edx
    addl $0x4, %edi
    loop .Lfill_upper_mem_pdpte

    /* EDI indicates PDE we should start to fill in */
    movl %esi, %edi
    /* EDX indicates new PT base address with page directory attribute */
    movl %edx, %esi
    /* ESI indicates base address of first PTE now */
    andl $PAGE_ALIGN_32BIT, %esi

    movl %ebx, %ecx
    /* upalign to 2MB */
    addl $ADDR_OFFSET_MASK, %ecx
    /* ECX indicates how many 2MB should be mapped */
    shrl $9, %ecx

.Lfill_upper_mem_pde:
    movl %edx, (%edi)
    addl $0x4, %edi
    movl $0, (%edi)
    addl $PAGE_SIZE, %edx
    addl $0x4, %edi
    loop .Lfill_upper_mem_pde

    movl $MEMBASE, %eax
    addl $ONE_GB, %eax
    /* we have already mapped up to PA MEMBASE+KERNEL_LOAD_OFFSET+1GB */
    addl $KERNEL_LOAD_OFFSET, %eax
    /* EAX indicates first index of 4KB physical address to fill in PTE */
    shrl $PAGE_DIV_SHIFT, %eax
    /* ECX indicates how many PTEs should be filled */
    movl %ebx, %ecx
    movl %edx, %ebx
    /* EBX incates page aligned address behind last PT */
    andl $PAGE_ALIGN_32BIT, %ebx
.Lfill_upper_mem_pte:
    movl %eax, %edx
    /* EDX indicates high 32-bit value of physical address */
    shrl $20, %edx
    movl 4(%esi), %edi
    movl %edx, (%edi)
    movl %eax, %edx
    /* EDX indicates low 32-bit value of physical address */
    shll $12, %edx
    addl $X86_KERNEL_PD_LP_FLAGS, %edx
    movl %edx, (%esi)
    incl %eax
    addl $8, %esi
    loop .Lfill_upper_mem_pte

    movl $PHYS(boot_alloc_end), %edi
    movl %ebx, (%edi)
.endm

.global _start
_start:
    jmp real_start

.align 8

.type multiboot_header,STT_OBJECT
multiboot_header:
    /* magic */
    .int MULTIBOOT_HEADER_MAGIC
    /* flags */
    .int MULTIBOOT_HEADER_FLAGS
    /* checksum */
    .int -(MULTIBOOT_HEADER_MAGIC + MULTIBOOT_HEADER_FLAGS)

#if !defined(__ELF__) || 1
    /* header_addr */
    .int PHYS(multiboot_header)
    /* load_addr */
    .int PHYS(_start)
    /* load_end_addr */
    .int PHYS(__data_end)
    /* bss_end_addr */
    .int PHYS(__bss_end)
    /* entry_addr */
    .int PHYS(real_start)
#endif

real_start:
    cmpl $MULTIBOOT_BOOTLOADER_MAGIC, %eax

    jne .Lno_multiboot_info
    movl %ebx, PHYS(_multiboot_info)

    /* ESI stores multiboot info address */
    leal (%ebx), %esi

    /*
     * first member in multiboot info structure is flag. If bit 0 in the flag
     * word is set, then mem_lower and mem_high indicate the amount of lower
     * and upper memory in kilobytes. Currently, we support this mode only.
     */
    movl 0(%esi), %ecx
    andl $0x1, %ecx
    jz  .Lno_multiboot_info

    /* ECX stores high memory (KB) provided by multiboot info */
    movl 0x8(%esi), %ecx

    /* halt machine if memory size to small */
    movl $PHYS(__bss_end), %edx
    movl $PHYS(__code_start), %eax
    subl %eax, %edx
    addl $MEMBASE, %edx
    shrl $10, %edx
    cmpl %edx, %ecx
    jc .Lhalt

    movl %ecx, %edx
    shrl $2, %edx
    movl $MEMBASE, %eax
    shrl $PAGE_DIV_SHIFT, %eax

    /* EDX = (upper_mem - MEMBASE) / 4KB */
    subl %eax, %edx

    /* halt machine if memory size to large */
    cmpl $MAX_MEM_SIZE_IN_PAGE, %edx
    jnc .Lhalt

    /* EDX = (upper_mem - MEMBASE) / 4GB */
    shrl $20, %edx
    /* ECX = (upper_mem - MEMBASE) % 4GB */
    shll $10, %ecx

    leal PHYS(mmu_initial_mappings), %esi
    /*
     * EDI points to size of first entry in mmu_initial_mappings.
     * Contents of this entry will be used to init pmm arena,
     * mmu_initial_mappings is defined in platform.c.
     */
    leal 0x10(%esi), %edi
    movl %ecx, (%edi)
    movl %edx, 4(%edi)

.Lno_multiboot_info:

    /* load our new gdt by physical pointer */
    lgdt PHYS(_gdtr_phys)

    /* load our data selectors */
    movw $DATA_SELECTOR, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %ss
    movw %ax, %gs
    movw %ax, %ss

    /* load initial stack pointer */
    movl $PHYS(_kstack + 4096), %esp

    /* We need to jump to our sane 32 bit CS */
    pushl $CODE_SELECTOR
    pushl $PHYS(.Lfarjump)
    lret

.Lfarjump:

    /* zero the bss section */
bss_setup:
    movl $PHYS(__bss_start), %edi /* starting address of the bss */
    movl $PHYS(__bss_end), %ecx   /* find the length of the bss in bytes */
    subl %edi, %ecx
    shrl $2, %ecx       /* convert to 32 bit words, since the bss is aligned anyway */
.Lzero_bss_sec:
    movl $0, (%edi)
    addl $4, %edi
    loop .Lzero_bss_sec

paging_setup:
    /*
     * Preparing 64 bit paging, we will use 4KB pages covering upper memory
     * for initial bootstrap, this page table will be 1 to 1
     */

    /* PAE bit must be enabled for 64 bit paging */
    mov %cr4, %eax
    btsl $(5), %eax
    mov %eax, %cr4

    /* load the physical pointer to the top level page table */
    movl $PHYS(pml4_trampoline), %eax
    mov %eax, %cr3

    /* Long Mode Enabled at this point */
    movl $MSR_EFER ,%ecx
    rdmsr
    orl $EFER_LME,%eax
    /* NXE bit should be set, since we update XD bit in page table */
    orl $EFER_NXE,%eax
    wrmsr

    /* start to create mapping for bootstrap code */
    /* setting the first PML4E with a PDP table reference */
    movl $PHYS(pdpt_trampoline_low), %eax
    orl  $X86_KERNEL_PD_FLAGS, %eax
    movl %eax, PHYS(pml4_trampoline)

    /* setting the first PDPTE with a Page table reference */
    movl $PHYS(pd_trampoline_low), %eax
    orl  $X86_KERNEL_PD_FLAGS, %eax
    movl %eax, PHYS(pdpt_trampoline_low)

    /* setting the corresponding PML4E to map from KERNEL_BASE */
    map_kernel_base pml4_trampoline, pdpt_trampoline_high, $9

    /* setting the corresponding PDPTE to map from KERNEL_BASE */
    map_kernel_base pdpt_trampoline_high, pd_trampoline_high, $0

    movl $KERNEL_BASE, %ecx
    shrl $PD_SHIFT, %ecx
    andl $ADDR_OFFSET_MASK, %ecx
    shll $3, %ecx

    movl $PHYS(pd_trampoline_high), %edi
    addl %ecx, %edi

    /* setting the corresponding PDE to map from KERNEL_BASE */
    movl $PHYS(pt_trampoline), %esi
    orl  $X86_KERNEL_PD_FLAGS, %esi
    movl %esi, (%edi)

    movl $MEMBASE, %eax
    /* calculate offset of MEMBASE in PD */
    shrl $PD_SHIFT, %eax
    shll $3, %eax
    movl $PHYS(pd_trampoline_low), %edi
    addl %eax, %edi

    /* setting PDE to map from MEMBASE */
    movl $PHYS(pt_trampoline), %esi
    orl  $X86_KERNEL_PD_FLAGS, %esi
    movl %esi, (%edi)

    movl $KERNEL_LOAD_OFFSET, %ecx
    /* calculate offset of MEMBASE in PT */
    shrl $PAGE_DIV_SHIFT, %ecx
    shll $3, %ecx
    movl $PHYS(pt_trampoline), %edi
    addl %ecx, %edi

    /* calculate PA start to map */
    movl $MEMBASE, %eax
    addl $KERNEL_LOAD_OFFSET, %eax

    /* setting PTE to map PA from MEMBASE+KERNEL_LOAD_OFFSET */
    orl  $X86_KERNEL_PD_LP_FLAGS, %eax
    movl %eax, (%edi)

    /*
     * same formula for page where _gdt resides, since we need to
     * update CS during mode switch.
     * _gdt has been aligned to 4KB, it's safe to be used here
     *
     */
    movl $PHYS(_gdt), %eax
    movl $MEMBASE, %edx
    subl %edx, %eax

    /* calculate offset of PHYS(_gdt) in PT */
    shrl $PAGE_DIV_SHIFT, %eax
    shll $3, %eax
    movl $PHYS(pt_trampoline), %edi
    addl %eax, %edi

    /* setting PTE to map PA from PHYS(_gdt) */
    movl $PHYS(_gdt), %eax
    orl  $X86_KERNEL_PD_LP_FLAGS, %eax
    movl %eax, (%edi)

    /* start to create mapping for run time */
    /* setting the corresponding PML4E to map from KERNEL_BASE */
    map_kernel_base pml4, pdpt, $9

    /* setting the corresponding PDPTE to map from KERNEL_BASE */
    map_kernel_base pdpt, pd, $0

    /*
     * calculate memory size to be mapped
     * CAUTION: we have only reserved page tables to cover 1GB mapping
     */
    leal PHYS(mmu_initial_mappings), %edx
    /* ECX indicates low 32-bit mmu_initial_mappings[0].size */
    movl 0x10(%edx), %ecx
    /* EAX indicates high 32-bit mmu_initial_mappings[0].size */
    movl 0x14(%edx), %eax
    shrl $30, %ecx
    shll $2, %eax
    /* EAX indicates memsize in GB */
    addl %ecx, %eax

    /* reload memsize in case memsize + MEMBASE < 1GB */
    movl 0x10(%edx), %ecx
    /*
     * EAX indicates memsize + MEMBASE in GB
     * ECX = (EAX > 1GB) ? (1GB - MEMBASE) : memsize
     */
    test %eax, %eax
    jz  .Lsize_smaller_than_one_gb
    map_up_to_uppper_memory
    movl $ONE_GB, %ecx
.Lsize_smaller_than_one_gb:
    movl %ecx, %ebx
    /* calculate offset of PD to map from KERNEL_BASE */
    movl $KERNEL_BASE, %edx
    shrl $PD_SHIFT, %edx
    andl $ADDR_OFFSET_MASK, %edx
    shll $3, %edx
    movl $PHYS(pd), %edi
    addl %edx, %edi

    /* calculate how many 2MB PDE should be used */
    shrl $PD_SHIFT, %ecx

    /* fill in PDEs */
    fill_page_table_entry $PHYS(pt), $X86_KERNEL_PD_FLAGS

    movl %ebx, %ecx
    /* calculate how many 4KB PTE should be used */
    shrl $PAGE_DIV_SHIFT, %ecx
    movl $PHYS(pt), %edi

    /* fill in PTEs */
    fill_page_table_entry $MEMBASE, $X86_KERNEL_PD_LP_FLAGS

    update_mapping_attribute_of_each_section

    /* enabling paging and we are in 32 bit compatibility mode */
    movl %cr0, %eax
    btsl $(31), %eax
    movl %eax, %cr0

    /*
     * stack for mode switch, since from this point stack
     * provided by bootloader is not mapped any more
     */
    movl $PHYS(__kstack_tmp), %eax
    movl %eax, %esp

    /* Using another long jump to be on 64 bit mode
    after this we will be on real 64 bit mode */
    pushl $CODE_64_SELECTOR     /*Need to put it in a the right CS*/
    pushl $PHYS(farjump64)
    lret

.align 8
.code64
farjump64:
    /* branch to our high address */
    mov  $highaddr, %rax
    jmp  *%rax

highaddr:
    /* switch page table now */
    movq $PHYS(pml4), %rax
    movq %rax, %cr3

    /* load the high kernel stack */
    movq  $(_kstack + 4096), %rsp

    /* reload the gdtr */
    lgdt _gdtr

    /* set up gs base */
    leaq per_cpu_states(%rip), %rax

    movq %rax, %rdx
    shrq $32, %rdx
    movq $MSR_GS_BASE, %rcx
    /*
     * RCX - MSR index (MSR_GS_BASE)
     * EDX - high 32 bits value to write
     * EAX - low 32 bits value to write
     * MSR(RCX)(MSR_GS_BSSE) = EDX:EAX
     */
    wrmsr

    /* set up the idt */
    call setup_idt

    /* call the main module */
    call lk_main

.Lhalt:                     /* just sit around waiting for interrupts */
    hlt                     /* interrupts will unhalt the processor */
    pause
    jmp .Lhalt              /* so jump back to halt to conserve power */

/* stack used for mode switch, need to reside in first page from _start */
.global __kstack_tmp
.align 8
__kstack_tmp_start:
.fill 32, 1, 0
__kstack_tmp:
