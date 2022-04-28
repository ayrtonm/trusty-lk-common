/*
 * Copyright (c) 2014 Travis Geiselbrecht
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
#include <kernel/vm.h>
#include "vm_priv.h"

#include <trace.h>
#include <assert.h>
#include <list.h>
#include <stdlib.h>
#include <err.h>
#include <string.h>
#include <pow2.h>
#include <lib/console.h>
#include <kernel/mutex.h>

#define LOCAL_TRACE 0

struct pmm_vmm_obj {
    struct vmm_obj vmm_obj;
    struct list_node page_list;
    size_t chunk_count;
    size_t chunk_size;
    struct vm_page *chunk[];
};

static inline struct pmm_vmm_obj* vmm_obj_to_pmm_obj(struct vmm_obj *vmm_obj)
{
    return containerof(vmm_obj, struct pmm_vmm_obj, vmm_obj);
}

static struct list_node arena_list = LIST_INITIAL_VALUE(arena_list);
static mutex_t lock = MUTEX_INITIAL_VALUE(lock);

#define PAGE_BELONGS_TO_ARENA(page, arena) \
    (((uintptr_t)(page) >= (uintptr_t)(arena)->page_array) && \
     ((uintptr_t)(page) < ((uintptr_t)(arena)->page_array + (arena)->size / PAGE_SIZE * sizeof(vm_page_t))))

#define PAGE_ADDRESS_FROM_ARENA(page, arena) \
    (paddr_t)(((uintptr_t)page - (uintptr_t)(arena)->page_array) / sizeof(vm_page_t)) * PAGE_SIZE + (arena)->base;

#define ADDRESS_IN_ARENA(address, arena) \
    ((address) >= (arena)->base && (address) <= (arena)->base + (arena)->size - 1)

static size_t pmm_free_locked(struct list_node *list);

static inline bool page_is_free(const vm_page_t *page)
{
    DEBUG_ASSERT(page);

    return !(page->flags & VM_PAGE_FLAG_NONFREE);
}

static void clear_page(vm_page_t *page)
{
    paddr_t pa;
    void *kva;

    pa = vm_page_to_paddr(page);
    ASSERT(pa != (paddr_t)-1);

    kva = paddr_to_kvaddr(pa);
    ASSERT(kva);

    memset(kva, 0, PAGE_SIZE);
}

paddr_t vm_page_to_paddr(const vm_page_t *page)
{
    DEBUG_ASSERT(page);

    pmm_arena_t *a;
    list_for_every_entry(&arena_list, a, pmm_arena_t, node) {
        if (PAGE_BELONGS_TO_ARENA(page, a)) {
            return PAGE_ADDRESS_FROM_ARENA(page, a);
        }
    }
    return -1;
}

vm_page_t *paddr_to_vm_page(paddr_t addr)
{
    pmm_arena_t *a;
    list_for_every_entry(&arena_list, a, pmm_arena_t, node) {
        if (addr >= a->base && addr <= a->base + a->size - 1) {
            size_t index = (addr - a->base) / PAGE_SIZE;
            return &a->page_array[index];
        }
    }
    return NULL;
}

status_t pmm_add_arena(pmm_arena_t *arena)
{
    LTRACEF("arena %p name '%s' base 0x%lx size 0x%zx\n", arena, arena->name, arena->base, arena->size);

    DEBUG_ASSERT(arena);
    DEBUG_ASSERT(IS_PAGE_ALIGNED(arena->base));
    DEBUG_ASSERT(IS_PAGE_ALIGNED(arena->size));
    DEBUG_ASSERT(arena->size > 0);

    /* walk the arena list and add arena based on priority order */
    pmm_arena_t *a;
    list_for_every_entry(&arena_list, a, pmm_arena_t, node) {
        if (a->priority > arena->priority) {
            list_add_before(&a->node, &arena->node);
            goto done_add;
        }
    }

    /* walked off the end, add it to the end of the list */
    list_add_tail(&arena_list, &arena->node);

done_add:

    /* zero out some of the structure */
    arena->free_count = 0;
    list_initialize(&arena->free_list);

    /* allocate an array of pages to back this one */
    size_t page_count = arena->size / PAGE_SIZE;
    arena->page_array = boot_alloc_mem(page_count * sizeof(vm_page_t));

    /* initialize all of the pages */
    memset(arena->page_array, 0, page_count * sizeof(vm_page_t));

    /* add them to the free list */
    for (size_t i = 0; i < page_count; i++) {
        vm_page_t *p = &arena->page_array[i];

        list_add_tail(&arena->free_list, &p->node);

        arena->free_count++;
    }

    return NO_ERROR;
}

static int pmm_vmm_obj_check_flags(struct vmm_obj *obj, uint *arch_mmu_flags)
{
    return 0; /* Allow any flags for now */
}

static int pmm_vmm_obj_get_page(struct vmm_obj *obj, size_t offset,
                                paddr_t *paddr, size_t *paddr_size)
{
    struct pmm_vmm_obj *pmm_obj = vmm_obj_to_pmm_obj(obj);
    size_t index;
    size_t chunk_offset;

    index = offset / pmm_obj->chunk_size;
    chunk_offset = offset % pmm_obj->chunk_size;

    if (index >= pmm_obj->chunk_count) {
        return ERR_OUT_OF_RANGE;
    }
    *paddr = vm_page_to_paddr(pmm_obj->chunk[index]) + chunk_offset;
    *paddr_size = pmm_obj->chunk_size - chunk_offset;
    return 0;
}

static void pmm_vmm_obj_destroy(struct vmm_obj *obj)
{
    struct pmm_vmm_obj *pmm_obj = vmm_obj_to_pmm_obj(obj);

    pmm_free(&pmm_obj->page_list);
    free(pmm_obj);
}

static struct vmm_obj_ops pmm_vmm_obj_ops = {
    .check_flags = pmm_vmm_obj_check_flags,
    .get_page = pmm_vmm_obj_get_page,
    .destroy = pmm_vmm_obj_destroy,
};

static struct pmm_vmm_obj *pmm_alloc_obj(size_t chunk_count, size_t chunk_size)
{
    struct pmm_vmm_obj *pmm_obj;

    DEBUG_ASSERT(chunk_size % PAGE_SIZE == 0);

    if (chunk_count == 0)
        return NULL;

    pmm_obj = calloc(
            1, sizeof(*pmm_obj) + sizeof(pmm_obj->chunk[0]) * chunk_count);
    if (!pmm_obj) {
        return NULL;
    }
    pmm_obj->chunk_count = chunk_count;
    pmm_obj->chunk_size = chunk_size;
    list_initialize(&pmm_obj->page_list);

    return pmm_obj;
}

static size_t pmm_arena_find_free_run(pmm_arena_t *a, uint count,
                                      uint8_t alignment_log2) {
    if (alignment_log2 < PAGE_SIZE_SHIFT)
        alignment_log2 = PAGE_SIZE_SHIFT;

    /* walk the list starting at alignment boundaries.
     * calculate the starting offset into this arena, based on the
     * base address of the arena to handle the case where the arena
     * is not aligned on the same boundary requested.
     */
    paddr_t rounded_base = round_up(a->base, 1UL << alignment_log2);
    if (rounded_base < a->base || rounded_base > a->base + (a->size - 1))
        return ~0UL;

    uint aligned_offset = (rounded_base - a->base) / PAGE_SIZE;
    uint start = aligned_offset;
    LTRACEF("starting search at aligned offset %u\n", start);
    LTRACEF("arena base 0x%lx size %zu\n", a->base, a->size);

retry:
    /*
     * Search while we're still within the arena and have a chance of finding a
     * slot (start + count < end of arena)
     */
    while ((start < a->size / PAGE_SIZE) &&
            ((start + count) <= a->size / PAGE_SIZE)) {
        vm_page_t *p = &a->page_array[start];
        for (uint i = 0; i < count; i++) {
            if (p->flags & VM_PAGE_FLAG_NONFREE) {
                /* this run is broken, break out of the inner loop.
                 * start over at the next alignment boundary
                 */
                start = round_up(start - aligned_offset + i + 1,
                                 1UL << (alignment_log2 - PAGE_SIZE_SHIFT)) +
                        aligned_offset;
                goto retry;
            }
            p++;
        }

        /* we found a run */
        LTRACEF("found run from pn %u to %u\n", start, start + count);
        return start;
    }
    return ~0UL;
}

static status_t pmm_alloc_pages_locked(struct list_node *page_list,
                                       struct vm_page *pages[], uint count,
                                       uint32_t flags, uint8_t align_log2)
{
    uint allocated = 0;
    size_t free_run_start = ~0UL;
    struct list_node tmp_page_list = LIST_INITIAL_VALUE(tmp_page_list);

    /* align_log2 is only supported when PMM_ALLOC_FLAG_CONTIGUOUS is set */
    ASSERT(!align_log2 || (flags & PMM_ALLOC_FLAG_CONTIGUOUS));

    if ((flags & PMM_ALLOC_FLAG_CONTIGUOUS) && (count == 1) &&
        (align_log2 <= PAGE_SIZE_SHIFT)) {
        /* pmm_arena_find_free_run is slow. Skip it if any page will do */
        flags &= ~PMM_ALLOC_FLAG_CONTIGUOUS;
    }

    /* walk the arenas in order, allocating as many pages as we can from each */
    pmm_arena_t *a;
    list_for_every_entry(&arena_list, a, pmm_arena_t, node) {
        if (flags & PMM_ALLOC_FLAG_KMAP && !(a->flags & PMM_ARENA_FLAG_KMAP)) {
            /* caller requested mapped pages, but arena a is not mapped */
            continue;
        }

        if (flags & PMM_ALLOC_FLAG_CONTIGUOUS) {
            free_run_start = pmm_arena_find_free_run(a, count, align_log2);
            if (free_run_start == ~0UL) {
                continue;
            }
        }

        while (allocated < count) {
            vm_page_t *page;
            if (flags & PMM_ALLOC_FLAG_CONTIGUOUS) {
                DEBUG_ASSERT(free_run_start < a->size / PAGE_SIZE);
                page = &a->page_array[free_run_start++];
                DEBUG_ASSERT(!(page->flags & VM_PAGE_FLAG_NONFREE));
                DEBUG_ASSERT(list_in_list(&page->node));
                list_delete(&page->node);
            } else {
                page = list_remove_head_type(&a->free_list, vm_page_t, node);
                if (!page)
                    break;
            }

            clear_page(page);

            a->free_count--;

            page->flags |= VM_PAGE_FLAG_NONFREE;
            if (pages && (!allocated || !(flags & PMM_ALLOC_FLAG_CONTIGUOUS))) {
                /*
                 * If PMM_ALLOC_FLAG_CONTIGUOUS is set, then @pages has a single
                 * entry, otherwise it has @count entries.
                 */
                pages[allocated] = page;
            }
            list_add_tail(&tmp_page_list, &page->node);

            allocated++;
        }
    }

    if (allocated != count) {
        pmm_free_locked(&tmp_page_list);
        return ERR_NO_MEMORY;
    }
    if (page_list) {
        list_splice_tail(page_list, &tmp_page_list);
    }
    return 0;
}

status_t pmm_alloc(struct vmm_obj **objp, struct obj_ref* ref, uint count,
                   uint32_t flags, uint8_t align_log2)
{
    status_t ret;
    struct pmm_vmm_obj *pmm_obj;

    DEBUG_ASSERT(objp);
    DEBUG_ASSERT(ref);
    DEBUG_ASSERT(!obj_ref_active(ref));
    DEBUG_ASSERT(count > 0);

    LTRACEF("count %u\n", count);
    if (flags & PMM_ALLOC_FLAG_CONTIGUOUS) {
        /*
         * When allocating a physically contiguous region we don't need a
         * pointer to every page. Allocate an object with one large page
         * instead. This also allows the vmm to map the contiguous region more
         * efficiently when the hardware supports it.
         */
        pmm_obj = pmm_alloc_obj(1, count * PAGE_SIZE);
    } else {
        pmm_obj = pmm_alloc_obj(count, PAGE_SIZE);
    }
    if (!pmm_obj) {
        return ERR_NO_MEMORY;
    }

    mutex_acquire(&lock);
    ret = pmm_alloc_pages_locked(&pmm_obj->page_list, pmm_obj->chunk, count,
                                 flags, align_log2);
    mutex_release(&lock);

    if (ret) {
        free(pmm_obj);
        return ret;
    }

    vmm_obj_init(&pmm_obj->vmm_obj, ref, &pmm_vmm_obj_ops);
    *objp = &pmm_obj->vmm_obj;
    return 0;
}

size_t pmm_alloc_range(paddr_t address, uint count, struct list_node *list)
{
    LTRACEF("address 0x%lx, count %u\n", address, count);

    DEBUG_ASSERT(list);

    uint allocated = 0;
    if (count == 0)
        return 0;

    address = round_down(address, PAGE_SIZE);

    mutex_acquire(&lock);

    /* walk through the arenas, looking to see if the physical page belongs to it */
    pmm_arena_t *a;
    list_for_every_entry(&arena_list, a, pmm_arena_t, node) {
        while (allocated < count && ADDRESS_IN_ARENA(address, a)) {
            size_t index = (address - a->base) / PAGE_SIZE;

            DEBUG_ASSERT(index < a->size / PAGE_SIZE);

            vm_page_t *page = &a->page_array[index];
            if (page->flags & VM_PAGE_FLAG_NONFREE) {
                /* we hit an allocated page */
                break;
            }

            DEBUG_ASSERT(list_in_list(&page->node));

            list_delete(&page->node);
            page->flags |= VM_PAGE_FLAG_NONFREE;
            list_add_tail(list, &page->node);

            a->free_count--;
            allocated++;
            address += PAGE_SIZE;
        }

        if (allocated == count)
            break;
    }

    mutex_release(&lock);
    return allocated;
}

static size_t pmm_free_locked(struct list_node *list)
{
    LTRACEF("list %p\n", list);

    DEBUG_ASSERT(list);

    uint count = 0;
    while (!list_is_empty(list)) {
        vm_page_t *page = list_remove_head_type(list, vm_page_t, node);

        DEBUG_ASSERT(!list_in_list(&page->node));
        DEBUG_ASSERT(page->flags & VM_PAGE_FLAG_NONFREE);

        /* see which arena this page belongs to and add it */
        pmm_arena_t *a;
        list_for_every_entry(&arena_list, a, pmm_arena_t, node) {
            if (PAGE_BELONGS_TO_ARENA(page, a)) {
                page->flags &= ~VM_PAGE_FLAG_NONFREE;

                list_add_head(&a->free_list, &page->node);
                a->free_count++;
                count++;
                break;
            }
        }
    }

    return count;
}

size_t pmm_free(struct list_node *list)
{
    size_t ret;
    LTRACEF("list %p\n", list);

    DEBUG_ASSERT(list);

    mutex_acquire(&lock);
    ret = pmm_free_locked(list);
    mutex_release(&lock);

    return ret;
}

size_t pmm_free_page(vm_page_t *page)
{
    DEBUG_ASSERT(page);

    struct list_node list;
    list_initialize(&list);

    list_add_head(&list, &page->node);

    return pmm_free(&list);
}

/* physically allocate a run from arenas marked as KMAP */
void *pmm_alloc_kpages(uint count, struct list_node *list)
{
    LTRACEF("count %u\n", count);

    // XXX do fast path for single page


    paddr_t pa;
    size_t alloc_count = pmm_alloc_contiguous(count, PAGE_SIZE_SHIFT, &pa, list);
    if (alloc_count == 0)
        return NULL;

    return paddr_to_kvaddr(pa);
}

size_t pmm_free_kpages(void *_ptr, uint count)
{
    LTRACEF("ptr %p, count %u\n", _ptr, count);

    uint8_t *ptr = (uint8_t *)_ptr;

    struct list_node list;
    list_initialize(&list);

    while (count > 0) {
        vm_page_t *p = paddr_to_vm_page(vaddr_to_paddr(ptr));
        if (p) {
            list_add_tail(&list, &p->node);
        }

        ptr += PAGE_SIZE;
        count--;
    }

    return pmm_free(&list);
}

size_t pmm_alloc_contiguous(uint count, uint8_t alignment_log2, paddr_t *pa, struct list_node *list)
{
    status_t ret;
    struct vm_page *page;
    LTRACEF("count %u, align %u\n", count, alignment_log2);

    if (count == 0)
        return 0;
    if (alignment_log2 < PAGE_SIZE_SHIFT)
        alignment_log2 = PAGE_SIZE_SHIFT;

    mutex_acquire(&lock);
    ret = pmm_alloc_pages_locked(list, &page, count, PMM_ALLOC_FLAG_KMAP |
                                 PMM_ALLOC_FLAG_CONTIGUOUS, alignment_log2);
    mutex_release(&lock);
    if (ret) {
        return 0;
    }
    if (pa) {
        *pa = vm_page_to_paddr(page);
    }

    return count;
}

static void dump_page(const vm_page_t *page)
{
    DEBUG_ASSERT(page);

    printf("page %p: address 0x%lx flags 0x%x\n", page, vm_page_to_paddr(page), page->flags);
}

static void dump_arena(const pmm_arena_t *arena, bool dump_pages)
{
    DEBUG_ASSERT(arena);

    printf("arena %p: name '%s' base 0x%lx size 0x%zx priority %u flags 0x%x\n",
           arena, arena->name, arena->base, arena->size, arena->priority, arena->flags);
    printf("\tpage_array %p, free_count %zu\n",
           arena->page_array, arena->free_count);

    /* dump all of the pages */
    if (dump_pages) {
        for (size_t i = 0; i < arena->size / PAGE_SIZE; i++) {
            dump_page(&arena->page_array[i]);
        }
    }

    /* dump the free pages */
    printf("\tfree ranges:\n");
    ssize_t last = -1;
    for (size_t i = 0; i < arena->size / PAGE_SIZE; i++) {
        if (page_is_free(&arena->page_array[i])) {
            if (last == -1) {
                last = i;
            }
        } else {
            if (last != -1) {
                printf("\t\t0x%lx - 0x%lx\n", arena->base + last * PAGE_SIZE, arena->base + i * PAGE_SIZE);
            }
            last = -1;
        }
    }

    if (last != -1) {
        printf("\t\t0x%lx - 0x%lx\n",  arena->base + last * PAGE_SIZE, arena->base + arena->size);
    }
}

static int cmd_pmm(int argc, const cmd_args *argv)
{
    if (argc < 2) {
notenoughargs:
        printf("not enough arguments\n");
usage:
        printf("usage:\n");
        printf("%s arenas\n", argv[0].str);
        printf("%s alloc <count>\n", argv[0].str);
        printf("%s alloc_range <address> <count>\n", argv[0].str);
        printf("%s alloc_kpages <count>\n", argv[0].str);
        printf("%s alloc_contig <count> <alignment>\n", argv[0].str);
        printf("%s dump_alloced\n", argv[0].str);
        printf("%s free_alloced\n", argv[0].str);
        return ERR_GENERIC;
    }

    static struct list_node allocated = LIST_INITIAL_VALUE(allocated);

    if (!strcmp(argv[1].str, "arenas")) {
        pmm_arena_t *a;
        list_for_every_entry(&arena_list, a, pmm_arena_t, node) {
            dump_arena(a, false);
        }
    } else if (!strcmp(argv[1].str, "dump_alloced")) {
        vm_page_t *page;

        list_for_every_entry(&allocated, page, vm_page_t, node) {
            dump_page(page);
        }
    } else if (!strcmp(argv[1].str, "alloc_range")) {
        if (argc < 4) goto notenoughargs;

        struct list_node list;
        list_initialize(&list);

        uint count = pmm_alloc_range(argv[2].u, argv[3].u, &list);
        printf("alloc returns %u\n", count);

        vm_page_t *p;
        list_for_every_entry(&list, p, vm_page_t, node) {
            printf("\tpage %p, address 0x%lx\n", p, vm_page_to_paddr(p));
        }

        /* add the pages to the local allocated list */
        struct list_node *node;
        while ((node = list_remove_head(&list))) {
            list_add_tail(&allocated, node);
        }
    } else if (!strcmp(argv[1].str, "alloc_kpages")) {
        if (argc < 3) goto notenoughargs;

        void *ptr = pmm_alloc_kpages(argv[2].u, NULL);
        printf("pmm_alloc_kpages returns %p\n", ptr);
    } else if (!strcmp(argv[1].str, "alloc_contig")) {
        if (argc < 4) goto notenoughargs;

        struct list_node list;
        list_initialize(&list);

        paddr_t pa;
        size_t ret = pmm_alloc_contiguous(argv[2].u, argv[3].u, &pa, &list);
        printf("pmm_alloc_contiguous returns %zu, address 0x%lx\n", ret, pa);
        printf("address %% align = 0x%lx\n", pa % argv[3].u);

        /* add the pages to the local allocated list */
        struct list_node *node;
        while ((node = list_remove_head(&list))) {
            list_add_tail(&allocated, node);
        }
    } else if (!strcmp(argv[1].str, "free_alloced")) {
        size_t err = pmm_free(&allocated);
        printf("pmm_free returns %zu\n", err);
    } else {
        printf("unknown command\n");
        goto usage;
    }

    return NO_ERROR;
}

STATIC_COMMAND_START
#if LK_DEBUGLEVEL > 0
STATIC_COMMAND("pmm", "physical memory manager", &cmd_pmm)
#endif
STATIC_COMMAND_END(pmm);




