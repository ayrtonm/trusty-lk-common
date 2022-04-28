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
#include <assert.h>
#include <err.h>
#include <kernel/mutex.h>
#include <kernel/vm.h>
#include <lib/console.h>
#include <lib/rand/rand.h>
#include <string.h>
#include <trace.h>

#include "vm_priv.h"

#define LOCAL_TRACE 0

static struct list_node aspace_list = LIST_INITIAL_VALUE(aspace_list);
static mutex_t vmm_lock = MUTEX_INITIAL_VALUE(vmm_lock);

vmm_aspace_t _kernel_aspace;

static void dump_aspace(const vmm_aspace_t* a);
static void dump_region(const vmm_region_t* r);

void vmm_init_preheap(void) {
    /* initialize the kernel address space */
    strlcpy(_kernel_aspace.name, "kernel", sizeof(_kernel_aspace.name));
    _kernel_aspace.base = KERNEL_ASPACE_BASE;
    _kernel_aspace.size = KERNEL_ASPACE_SIZE;
    _kernel_aspace.flags = VMM_ASPACE_FLAG_KERNEL;
    bst_root_initialize(&_kernel_aspace.regions);

    arch_mmu_init_aspace(&_kernel_aspace.arch_aspace, KERNEL_ASPACE_BASE,
                         KERNEL_ASPACE_SIZE, ARCH_ASPACE_FLAG_KERNEL);

    list_add_head(&aspace_list, &_kernel_aspace.node);
}

void vmm_init(void) {}

static inline bool range_contains_range(vaddr_t range_base,
                                        size_t range_size,
                                        vaddr_t query_base,
                                        size_t query_size) {
    vaddr_t range_last;
    vaddr_t query_last;

    ASSERT(range_size > 0);
    ASSERT(query_size > 0);

    ASSERT(!__builtin_add_overflow(range_base, range_size - 1, &range_last));
    ASSERT(!__builtin_add_overflow(query_base, query_size - 1, &query_last));

    return range_base <= query_base && query_last <= range_last;
}

static inline bool is_inside_aspace(const vmm_aspace_t* aspace, vaddr_t vaddr) {
    DEBUG_ASSERT(aspace);
    return range_contains_range(aspace->base, aspace->size, vaddr, 1);
}

/*
 * returns true iff, after potentially adding a guard page at the end of the
 * region, it fits inside the address space pointed to by the first argument.
 */
static bool is_region_inside_aspace(const vmm_aspace_t* aspace,
                                    const vmm_region_t* r) {
    size_t aspace_size = aspace->size;

    DEBUG_ASSERT(aspace);
    DEBUG_ASSERT(aspace->base >= PAGE_SIZE);
    DEBUG_ASSERT(aspace->size > PAGE_SIZE);

    if (!(r->flags & VMM_FLAG_NO_END_GUARD)) {
        /*
         * rather than adding to the region size, shrink the address space
         * size; the former operation can overflow but the latter cannot.
         */
        aspace_size -= PAGE_SIZE;

        /*
         * We do not have to handle the symmetric case for start guards
         * because {KERNEL,USER}_ASPACE_BASE >= PAGE_SIZE must hold.
         * See also vmm_create_aspace.
         */
    }

    return range_contains_range(aspace->base, aspace_size, r->base,
                                r->obj_slice.size);
}

static bool is_inside_region(const vmm_region_t* r, vaddr_t vaddr) {
    DEBUG_ASSERT(r);
    return range_contains_range(r->base, r->obj_slice.size, vaddr, 1);
}

static bool is_range_inside_region(const vmm_region_t* r,
                                   vaddr_t vaddr,
                                   size_t size) {
    DEBUG_ASSERT(r);
    return range_contains_range(r->base, r->obj_slice.size, vaddr, size);
}

static size_t trim_to_aspace(const vmm_aspace_t* aspace,
                             vaddr_t vaddr,
                             size_t size) {
    DEBUG_ASSERT(aspace);
    DEBUG_ASSERT(is_inside_aspace(aspace, vaddr));

    if (size == 0)
        return size;

    size_t offset = vaddr - aspace->base;

    // LTRACEF("vaddr 0x%lx size 0x%zx offset 0x%zx aspace base 0x%lx aspace
    // size 0x%zx\n",
    //        vaddr, size, offset, aspace->base, aspace->size);

    if (offset + size < offset)
        size = ULONG_MAX - offset - 1;

    // LTRACEF("size now 0x%zx\n", size);

    if (offset + size >= aspace->size - 1)
        size = aspace->size - offset;

    // LTRACEF("size now 0x%zx\n", size);

    return size;
}

void vmm_obj_slice_init(struct vmm_obj_slice *slice) {
    slice->obj = NULL;
    obj_ref_init(&slice->obj_ref);
    slice->offset = 0;
    slice->size = 0;
}

/*
 * This will not invoke the destructor on the vmm_obj if it is the last
 * one out, as the vmm lock is held. If we would need to destroy the object,
 * we instead assert fail in debug builds, and with NDEBUG builds leak.
 */
static void vmm_obj_slice_release_locked(struct vmm_obj_slice *slice) {
    bool dead = false;
    if (slice->obj) {
        dead = obj_del_ref(&slice->obj->obj, &slice->obj_ref, NULL);
        slice->obj = NULL;
    }
    ASSERT(!dead);
}

void vmm_obj_slice_release(struct vmm_obj_slice *slice) {
    if (slice->obj) {
        vmm_obj_del_ref(slice->obj, &slice->obj_ref);
        slice->obj = NULL;
    }
}

static void vmm_obj_slice_bind_locked(struct vmm_obj_slice *slice,
                                      struct vmm_obj *obj,
                                      size_t offset,
                                      size_t size) {
    DEBUG_ASSERT(!slice->obj);
    slice->obj = obj;
    /* Use obj_add_ref directly to avoid acquiring the vmm lock. */
    obj_add_ref(&slice->obj->obj, &slice->obj_ref);
    slice->offset = offset;
    slice->size = size;
}

void vmm_obj_slice_bind(struct vmm_obj_slice *slice, struct vmm_obj *obj,
                        size_t offset, size_t size) {
    mutex_acquire(&vmm_lock);
    vmm_obj_slice_bind_locked(slice, obj, offset, size);
    mutex_release(&vmm_lock);
}

static vmm_region_t* alloc_region_struct(const char* name,
                                         vaddr_t base,
                                         size_t size,
                                         uint flags,
                                         uint arch_mmu_flags) {
    DEBUG_ASSERT(name);

    vmm_region_t* r = calloc(1, sizeof(vmm_region_t));
    if (!r)
        return NULL;

    strlcpy(r->name, name, sizeof(r->name));
    r->base = base;
    r->flags = flags;
    r->arch_mmu_flags = arch_mmu_flags;
    vmm_obj_slice_init(&r->obj_slice);
    r->obj_slice.size = size;

    return r;
}

static size_t vmm_flags_guard(uint low_flags, uint high_flags) {
    if ((low_flags & VMM_FLAG_NO_END_GUARD) &&
        (high_flags & VMM_FLAG_NO_START_GUARD)) {
        /*
         * Both regions have reported that they don't need a guard page on the
         * potentially touching side.
         */
        return 0;
    }

    return PAGE_SIZE;
}

static size_t vmm_rguard(vmm_region_t *low, vmm_region_t *high) {
    if (low->base >= high->base) {
        /*
         * Skip returning guard page if the regions are out of order to avoid
         * possible overflow on last region in address space.
         */
        return 0;
    }
    return vmm_flags_guard(low->flags, high->flags);
}

/* Match any region that overlap */
static int vmm_region_cmp(struct bst_node *_a, struct bst_node *_b) {
    vmm_region_t *a = containerof(_a, vmm_region_t, node);
    vmm_region_t *b = containerof(_b, vmm_region_t, node);

    if (b->base > a->base + (a->obj_slice.size - 1) + vmm_rguard(a, b)) {
        return 1;
    }
    if (a->base > b->base + (b->obj_slice.size - 1) + vmm_rguard(b, a)) {
        return -1;
    }
    return 0;
}

/* add a region to the appropriate spot in the address space list,
 * testing to see if there's a space */
static status_t add_region_to_aspace(vmm_aspace_t* aspace, vmm_region_t* r) {
    DEBUG_ASSERT(aspace);
    DEBUG_ASSERT(r);

    LTRACEF("aspace %p base 0x%lx size 0x%zx r %p base 0x%lx size 0x%zx flags 0x%x\n",
            aspace, aspace->base, aspace->size, r, r->base, r->obj_slice.size,
            r->flags);

    /* only try if the region will at least fit in the address space */
    if (r->obj_slice.size == 0 ||
        !is_region_inside_aspace(aspace, r)) {
        LTRACEF("region was out of range\n");
        return ERR_OUT_OF_RANGE;
    }

    if (bst_insert(&aspace->regions, &r->node, vmm_region_cmp)) {
        return NO_ERROR;
    }

    LTRACEF("couldn't find spot\n");
    vmm_region_t *r_coll  = bst_search_type(&aspace->regions, r,
                                            vmm_region_cmp, vmm_region_t, node);
    LTRACEF("colliding r %p base 0x%lx size 0x%zx flags 0x%x\n",
            r_coll, r_coll->base, r_coll->obj_slice.size, r_coll->flags);
    return ERR_NO_MEMORY;
}

/*
 *  Try to pick the spot within specified gap
 *
 *  Arch can override this to impose it's own restrictions.
 */
__WEAK vaddr_t arch_mmu_pick_spot(arch_aspace_t* aspace,
                                  vaddr_t base,
                                  uint prev_region_arch_mmu_flags,
                                  vaddr_t end,
                                  uint next_region_arch_mmu_flags,
                                  vaddr_t alignment,
                                  size_t size,
                                  uint arch_mmu_flags) {
    /* just align it by default */
    return align(base, alignment);
}

/**
 * next_spot() - Finds the next valid mapping location in a range
 * @low:            Lowest virtual address available for use
 * @high:           Highest virtual address available for use
 * @align:          Virtual address alignment requested
 * @size:           Size of region requested
 * @arch_mmu_flags: Flags to pass to the mmu in case of restrictions.
 * @out:            Output parameter for the base of a range matching the
 *                  requirements, of size @size. Only valid if next_spot()
 *                  returns true.
 *
 * Finds the lowest region available in a range subject to alignment, size,
 * and MMU constraints.
 *
 * Return: Whether a region was found. If false, *@out is invalid. If
 *         true, *@out is the base of a legal range to map at.
 */
static inline bool next_spot(arch_aspace_t* aspace,
                             uint prev_region_arch_mmu_flags,
                             uint next_region_arch_mmu_flags,
                             vaddr_t low,
                             vaddr_t high,
                             vaddr_t align,
                             size_t size,
                             uint arch_mmu_flags,
                             vaddr_t* out) {
    DEBUG_ASSERT(aspace);
    DEBUG_ASSERT(out);

    vaddr_t candidate = arch_mmu_pick_spot(
            aspace, low, prev_region_arch_mmu_flags, high,
            next_region_arch_mmu_flags, align, size, arch_mmu_flags);

    if ((candidate < low) || (candidate > high)) {
        /* arch_mmu_pick_spot sent the base address out of range */
        return false;
    }

    vaddr_t candidate_end;
    if (__builtin_add_overflow(candidate, size - 1, &candidate_end)) {
        /* Virtual address region would wrap around */
        return false;
    }

    if (candidate_end > high) {
        /* Virtual address stretches out of range */
        return false;
    }

    *out = candidate;
    return true;
}

/**
 * extract_gap() - Finds the gap between two used regions
 * @aspace:   The address space we are working in
 * @low:      The lower virtual region. May be null to indicate the area below
 *            the first region.
 * @high:     The higher virtual region. May be null to indicate the area above
 *            the last region.
 * @gap_low:  Output parameter for the lowest open address.
 * @gap_high: Output parameter for the highest open address.
 *
 * Finds the largest gap of open (unused) addresses inside an address space
 * @aspace that is separated from any adjacent virtual regions (@low, @high)
 * by a guard page. When there is no higher adjacent virtual region, the gap
 * is still separated from the end of the address space by one guard page.
 * Calculating a pointer to the element one past the end of a allocation can
 * therefore only trigger a pointer overflow if the element size is greater
 * than or equal to a guard page.
 *
 * Return: Whether a gap was found. If the return value is false, the output
 *         parameters may be invalid. If true, all addresses between
 *         *@gap_low and *@gap_high inclusive are unmapped.
 */
static inline bool extract_gap(vmm_aspace_t* aspace,
                               vmm_region_t* low,
                               vmm_region_t* high,
                               vaddr_t* gap_low,
                               vaddr_t* gap_high) {
    vaddr_t gap_high_val;

    DEBUG_ASSERT(aspace);
    DEBUG_ASSERT(gap_low);
    DEBUG_ASSERT(gap_high);
    DEBUG_ASSERT(aspace->size != 0);

    if (low) {
        if (__builtin_add_overflow(low->base, low->obj_slice.size, gap_low)) {
            /* No valid address exists above the low region */
            return false;
        }
        if (__builtin_add_overflow(*gap_low,
                                   PAGE_SIZE,
                                   gap_low)) {
            /* No valid address exists above the low region + guard page */
            return false;
        }
    } else {
        *gap_low = aspace->base;
        /* Assume no adjacent address space so no guard page needed */
    }

    if (high) {
        DEBUG_ASSERT(high->base != 0);
        gap_high_val = high->base - 1;
    } else {
        gap_high_val = aspace->base + (aspace->size - 1);
    }

    /*
     * Add a guard page even when the area is above highest region. We do so
     * because it is common and legal to calculate a pointer just beyond a
     * memory allocation. If we place an allocation at the very end of a
     * virtual address space, calculating a pointer just beyond the allocation
     * causes the pointer to wrap which is undefined behavior.
     */
    if (__builtin_sub_overflow(gap_high_val,
                               PAGE_SIZE,
                               &gap_high_val)) {
        /*
         * No valid address exists below the high region + guard page (OR the
         * virtual address space is unexpectedly smaller than one guard page)
         */
        return false;
    }

    if ((*gap_low) > gap_high_val) {
        /* No gap available */
        return false;
    }

    *gap_high = gap_high_val;

    return true;
}

/**
 * scan_gap() - Searches between two vm regions for usable spots
 * @aspace:         The address space to search in
 * @low:            The vm region below the search area. May be null to
 *                  indicate the bottom of the address space.
 * @high:           The vm region above the search area. May be null to
 *                  indicate the top of the address space.
 * @alignment:      The required alignment for the new region
 * @size:           How large the new region needs to be
 * @arch_mmu_flags: Architecture specific MMU flags for the new region
 *
 * Finds the number of different candidate offsets for a new region to be
 * created between two others.
 *
 * The result can be higher than reality if arch_mmu_pick_spot() employs exotic
 * requirements, but any value less than the return of scan_gap() will still
 * be valid for spot_in_gap().
 *
 * Return: The number of different places the region could be created within
 *         the gap.
 */
static inline size_t scan_gap(vmm_aspace_t* aspace,
                              vmm_region_t* low,
                              vmm_region_t* high,
                              vaddr_t alignment,
                              size_t size,
                              uint arch_mmu_flags) {
    vaddr_t low_addr;
    vaddr_t high_addr;
    if (!extract_gap(aspace, low, high, &low_addr, &high_addr)) {
        /* There's no gap, so there are no available positions */
        return 0;
    }

    uint low_flags = low ? low->arch_mmu_flags : ARCH_MMU_FLAG_INVALID;
    uint high_flags = high ? high->arch_mmu_flags : ARCH_MMU_FLAG_INVALID;

    vaddr_t first_base;
    arch_aspace_t* arch_aspace = &aspace->arch_aspace;
    if (!next_spot(arch_aspace, low_flags, high_flags, low_addr, high_addr,
                   alignment, size, arch_mmu_flags, &first_base)) {
        /*
         * We couldn't find a first place, so there are no available
         * positions.
         */
        return 0;
    }

    /* Estimate that the last position will be the last page aligned slot */
    vaddr_t final_base = round_down(high_addr - (size - 1), PAGE_SIZE);
    /* If we can't map at that address, shrink it by a page each time. */
    while (!next_spot(arch_aspace, low_flags, high_flags, final_base, high_addr,
                      alignment, size, arch_mmu_flags, &final_base)) {
        if ((final_base - first_base) < PAGE_SIZE) {
            /* There's only one location available in the region. */
            break;
        }
        final_base -= PAGE_SIZE;
    }

    /*
     * first_base and final_base now point to the lower and upper mapping
     * bounds.
     * We assume that every page in between would be a legal mapping. If it
     * would not, the worst consequence will be having less randomness than
     * expected since we know all addresses in the range will have a
     * valid next_spot().
     */

    return ((final_base - first_base) >> PAGE_SIZE_SHIFT) + 1;
}

/**
 * spot_in_gap() - Pick a specific available mapping range
 * @aspace:         The address space in which the mapping will take place
 * @low:            The lower virtual region. May be null to indicate the
 *                  area below the first region.
 * @high:           The higher virtual region. May be null to indicate the
 *                  area above the last region.
 * @align:          The requested alignment of the region
 * @size:           The requested size of the region
 * @arch_mmu_flags: The requested MMU flags (RWX etc)
 * @index:          Which possibility to map the region at. This value must
 *                  be less than the value returned by scan_gap() for the
 *                  same query.
 *
 * spot_in_gap() picks one of several possible regions within a gap, using the
 * provided index to select which one.
 *
 * This function is intended to be used in concert with scan_gap().
 * After running scan_gap(), the size returned will be a max (exclusive) for
 * the value of @index to this function, which should then not fail.
 *
 * Return: The virtual address that the mapping should be performed at.
 */
static inline vaddr_t spot_in_gap(vmm_aspace_t* aspace,
                                  vmm_region_t* low,
                                  vmm_region_t* high,
                                  vaddr_t align,
                                  size_t size,
                                  uint arch_mmu_flags,
                                  size_t index) {
    vaddr_t low_addr;
    vaddr_t high_addr;
    if (!extract_gap(aspace, low, high, &low_addr, &high_addr)) {
        panic("spot_in_gap() called on a 0-size region\n");
    }

    uint low_flags = low ? low->arch_mmu_flags : ARCH_MMU_FLAG_INVALID;
    uint high_flags = high ? high->arch_mmu_flags : ARCH_MMU_FLAG_INVALID;

    vaddr_t base;
    arch_aspace_t* arch_aspace = &aspace->arch_aspace;
    if (!next_spot(arch_aspace, low_flags, high_flags, low_addr, high_addr,
                   align, size, arch_mmu_flags, &base)) {
        panic("spot_in_gap() called on a region with no available mappings\n");
    }

    base += index * PAGE_SIZE;

    if (!next_spot(arch_aspace, low_flags, high_flags, base, high_addr, align,
                   size, arch_mmu_flags, &base)) {
        panic("spot_in_gap() with an index with no mapping option\n");
    }

    return base;
}

/**
 * alloc_spot() - Find a place in the address space for a new virtual region
 * @aspace:         The address space to search within
 * @size:           How large of a spot is required
 * @align_pow2:     Alignment requirements for the gap in bits
 * @arch_mmu_flags: Architecture-specifc MMU flags (RWX etc)
 *
 * Finds a space in the virtual memory space which is currently unoccupied,
 * is legal to map according to the MMU, is at least as large as @size,
 * and aligned as @align_pow2.
 *
 * If ASLR is enabled, this spot will also be *randomized* from amongst all
 * legal positions.
 * If ASLR is disabled, it will bias towards the lowest legal virtual address.
 *
 * This function does not actually mutate the aspace and reserve the region.
 * That is the responsibility of the caller.
 *
 * Return: The value of the first address for the new region if one was found,
 *         or -1 if no region was found.
 */
static vaddr_t alloc_spot(vmm_aspace_t* aspace,
                          size_t size,
                          uint8_t align_pow2,
                          uint arch_mmu_flags) {
    DEBUG_ASSERT(aspace);
    DEBUG_ASSERT(size > 0 && IS_PAGE_ALIGNED(size));

    LTRACEF("aspace %p size 0x%zx align %hhu\n", aspace, size, align_pow2);

    if (align_pow2 < PAGE_SIZE_SHIFT)
        align_pow2 = PAGE_SIZE_SHIFT;
    vaddr_t align = 1UL << align_pow2;

    vaddr_t spot;
    vmm_region_t* left = NULL;
    vmm_region_t* right;

    /*
     * TODO: When ASLR is enabled, pick a random address and check if it is
     * available with bst_search before falling back to examine every region.
     */

    /* Figure out how many options we have to size randomness appropriately */
    size_t choices = 0;
    bst_for_every_entry(&aspace->regions, right, vmm_region_t, node) {
        choices += scan_gap(aspace, left, right, align, size, arch_mmu_flags);
        left = right;
    }
    right = NULL;
    choices += scan_gap(aspace, left, right, align, size, arch_mmu_flags);
    if (!choices) {
        /* No available choices, bail */
        return (vaddr_t)-1;
    }

    /* Grab the index through all choices */
#ifdef ASLR
    size_t index = rand_get_size(choices - 1);
#else
    size_t index = 0;
#endif
    left = NULL;
    bst_for_every_entry(&aspace->regions, right, vmm_region_t, node) {
        size_t local_spots =
                scan_gap(aspace, left, right, align, size, arch_mmu_flags);
        if (local_spots > index) {
            spot = spot_in_gap(aspace, left, right, align, size,
                               arch_mmu_flags, index);
            goto done;
        } else {
            index -= local_spots;
        }
        left = right;
    }
    right = NULL;
    spot = spot_in_gap(aspace, left, right, align, size, arch_mmu_flags,
                       index);

done:
    return spot;
}

bool vmm_find_spot(vmm_aspace_t* aspace, size_t size, vaddr_t* out) {
    mutex_acquire(&vmm_lock);
    *out = alloc_spot(aspace, size, PAGE_SIZE_SHIFT, 0);
    mutex_release(&vmm_lock);
    return *out != (vaddr_t)(-1);
}

/* allocate a region structure and stick it in the address space */
static status_t alloc_region(vmm_aspace_t* aspace,
                                  const char* name,
                                  size_t size,
                                  vaddr_t vaddr,
                                  uint8_t align_pow2,
                                  uint vmm_flags,
                                  uint region_flags,
                                  uint arch_mmu_flags,
                                  vmm_region_t** out) {
    DEBUG_ASSERT((vmm_flags & VMM_REGION_FLAG_INTERNAL_MASK) == 0);
    /* make a region struct for it and stick it in the list */
    vmm_region_t* r = alloc_region_struct(name, vaddr, size,
                                          region_flags | vmm_flags,
                                          arch_mmu_flags);
    if (!r)
        return ERR_NO_MEMORY;

    /* if they ask us for a specific spot, put it there */
    if (vmm_flags & VMM_FLAG_VALLOC_SPECIFIC) {
        /* stick it in the list, checking to see if it fits */
        status_t ret = add_region_to_aspace(aspace, r);
        if (ret < 0) {
            /* didn't fit */
            free(r);
            return ret;
        }
    } else {
        /* allocate a virtual slot for it */
        if ((vmm_flags & VMM_FLAG_NO_START_GUARD) ||
            (vmm_flags & VMM_FLAG_NO_END_GUARD)) {
            LTRACEF("invalid allocation request: only requests for a specific"
                    " spot may disable guard pages before/after allocation\n");
            return ERR_INVALID_ARGS;
        }

        vaddr = alloc_spot(aspace, size, align_pow2, arch_mmu_flags);
        LTRACEF("alloc_spot returns 0x%lx\n", vaddr);

        if (vaddr == (vaddr_t)-1) {
            LTRACEF("failed to find spot\n");
            free(r);
            return ERR_NO_MEMORY;
        }

        r->base = (vaddr_t)vaddr;

        /* add it to the region list */
        ASSERT(bst_insert(&aspace->regions, &r->node, vmm_region_cmp));
    }

    if (out) {
        *out = r;
    }
    return NO_ERROR;
}

static status_t vmm_map_obj_locked(vmm_aspace_t* aspace, vmm_region_t* r,
                                   uint arch_mmu_flags) {
    /* map all of the pages */
    /* XXX use smarter algorithm that tries to build runs */
    status_t err;
    size_t off = 0;
    struct vmm_obj *vmm_obj = r->obj_slice.obj;
    while (off < r->obj_slice.size) {
        paddr_t pa;
        vaddr_t va;
        size_t pa_size;
        err = vmm_obj->ops->get_page(vmm_obj, off + r->obj_slice.offset, &pa,
                                     &pa_size);
        if (err) {
            goto err_map_loop;
        }
        pa_size = MIN(pa_size, r->obj_slice.size - off);

        DEBUG_ASSERT(IS_PAGE_ALIGNED(pa));
        DEBUG_ASSERT(pa_size);
        DEBUG_ASSERT(IS_PAGE_ALIGNED(pa_size));

        if (__builtin_add_overflow(r->base, off, &va)) {
            DEBUG_ASSERT(false);
        }
        DEBUG_ASSERT(IS_PAGE_ALIGNED(va));
        DEBUG_ASSERT(va <= r->base + (r->obj_slice.size - 1));
        err = arch_mmu_map(&aspace->arch_aspace, va, pa, pa_size / PAGE_SIZE,
                           arch_mmu_flags);
        if (err) {
            goto err_map_loop;
        }
        off += pa_size;
    }

    return NO_ERROR;

err_map_loop:
    arch_mmu_unmap(&aspace->arch_aspace, r->base, off / PAGE_SIZE);
    return err;
}


status_t vmm_reserve_space(vmm_aspace_t* aspace,
                           const char* name,
                           size_t size,
                           vaddr_t vaddr) {
    status_t ret;

    LTRACEF("aspace %p name '%s' size 0x%zx vaddr 0x%lx\n", aspace, name, size,
            vaddr);

    DEBUG_ASSERT(aspace);
    DEBUG_ASSERT(IS_PAGE_ALIGNED(vaddr));
    DEBUG_ASSERT(IS_PAGE_ALIGNED(size));

    if (!name)
        name = "";

    if (!aspace)
        return ERR_INVALID_ARGS;
    if (size == 0)
        return NO_ERROR;
    if (!IS_PAGE_ALIGNED(vaddr) || !IS_PAGE_ALIGNED(size))
        return ERR_INVALID_ARGS;

    if (!is_inside_aspace(aspace, vaddr))
        return ERR_OUT_OF_RANGE;

    /* trim the size */
    size = trim_to_aspace(aspace, vaddr, size);

    mutex_acquire(&vmm_lock);

    /* lookup how it's already mapped */
    uint arch_mmu_flags = 0;
    arch_mmu_query(&aspace->arch_aspace, vaddr, NULL, &arch_mmu_flags);

    /* build a new region structure */
    ret = alloc_region(aspace, name, size, vaddr, 0, VMM_FLAG_VALLOC_SPECIFIC,
                       VMM_REGION_FLAG_RESERVED, arch_mmu_flags, NULL);

    mutex_release(&vmm_lock);
    return ret;
}

void vmm_obj_add_ref(struct vmm_obj* obj, struct obj_ref* ref) {
    mutex_acquire(&vmm_lock);
    obj_add_ref(&obj->obj, ref);
    mutex_release(&vmm_lock);
}

void vmm_obj_del_ref(struct vmm_obj* obj, struct obj_ref* ref) {
    bool destroy;
    mutex_acquire(&vmm_lock);
    destroy = obj_del_ref(&obj->obj, ref, NULL);
    mutex_release(&vmm_lock);
    if (destroy) {
        obj->ops->destroy(obj);
    }
}

bool vmm_obj_has_only_ref(struct vmm_obj* obj, struct obj_ref* ref) {
    bool has_only_ref;
    mutex_acquire(&vmm_lock);
    has_only_ref = obj_has_only_ref(&obj->obj, ref);
    mutex_release(&vmm_lock);
    return has_only_ref;
}

status_t vmm_alloc_obj(vmm_aspace_t* aspace, const char* name,
                       struct vmm_obj* vmm_obj, size_t offset, size_t size,
                       void** ptr, uint8_t align_log2, uint vmm_flags,
                       uint arch_mmu_flags) {
    status_t ret;

    LTRACEF("aspace %p name '%s' obj %p offset 0x%zx size 0x%zx\n",
            aspace, name, vmm_obj, offset, size);
    LTRACEF("ptr %p align %hhu vmm_flags 0x%x arch_mmu_flags 0x%x\n",
            ptr ? *ptr : 0, align_log2, vmm_flags, arch_mmu_flags);

    DEBUG_ASSERT(aspace);
    DEBUG_ASSERT(vmm_obj);
    DEBUG_ASSERT(vmm_obj->ops);
    DEBUG_ASSERT(IS_PAGE_ALIGNED(offset));
    DEBUG_ASSERT(IS_PAGE_ALIGNED(size));
    DEBUG_ASSERT(ptr);

    if (!ptr) {
        ret = ERR_INVALID_ARGS;
        goto err_missing_ptr;
    }

    if (!name) {
        name = "";
    }

    vaddr_t vaddr = 0;

    /* if they're asking for a specific spot, copy the address */
    if (vmm_flags & VMM_FLAG_VALLOC_SPECIFIC) {
        vaddr = (vaddr_t)*ptr;
    }

    ret = vmm_obj->ops->check_flags(vmm_obj, &arch_mmu_flags);
    if (ret) {
        LTRACEF("check_flags failed\n");
        goto err_check_flags;
    }

    mutex_acquire(&vmm_lock);

    /* allocate a region and put it in the aspace list */
    vmm_region_t* r;
    ret = alloc_region(aspace, name, size, vaddr, align_log2,
                                vmm_flags, VMM_REGION_FLAG_PHYSICAL,
                                arch_mmu_flags, &r);
    if (ret) {
        LTRACEF("alloc_region failed\n");
        goto err_alloc_region;
    }

    vmm_obj_slice_bind_locked(&r->obj_slice, vmm_obj, offset, size);
    ret = vmm_map_obj_locked(aspace, r, arch_mmu_flags);
    if (ret) {
        goto err_map_obj;
    }

    /* return the vaddr */
    *ptr = (void*)r->base;

    mutex_release(&vmm_lock);
    return NO_ERROR;

err_map_obj:
    vmm_obj_slice_release_locked(&r->obj_slice);
    bst_delete(&aspace->regions, &r->node);
    free(r);
err_alloc_region:
    mutex_release(&vmm_lock);
err_check_flags:
err_missing_ptr:
    return ret;
}

status_t vmm_alloc_physical_etc(vmm_aspace_t* aspace,
                                const char* name,
                                size_t size,
                                void** ptr,
                                uint8_t align_log2,
                                paddr_t* paddr,
                                uint paddr_count,
                                uint vmm_flags,
                                uint arch_mmu_flags) {
    status_t ret;
    uint i;
    size_t page_size;

    LTRACEF("aspace %p name '%s' size 0x%zx ptr %p paddr 0x%lx... vmm_flags 0x%x "
            "arch_mmu_flags 0x%x\n",
            aspace, name, size, ptr ? *ptr : 0, paddr[0], vmm_flags,
            arch_mmu_flags);

    DEBUG_ASSERT(aspace);
    DEBUG_ASSERT(ptr);
    for (i = 0; i < paddr_count; i++) {
        DEBUG_ASSERT(IS_PAGE_ALIGNED(paddr[i]));
    }
    DEBUG_ASSERT(IS_PAGE_ALIGNED(size));

    if (!name)
        name = "";

    if (!aspace)
        return ERR_INVALID_ARGS;
    if (size == 0)
        return NO_ERROR;
    if (!paddr_count)
        return ERR_INVALID_ARGS;
    page_size = size / paddr_count;
    if (!IS_PAGE_ALIGNED(paddr[0]) || !IS_PAGE_ALIGNED(page_size))
        return ERR_INVALID_ARGS;

    if (!ptr) {
        return ERR_INVALID_ARGS;
    }

    vaddr_t vaddr = 0;

    /* if they're asking for a specific spot, copy the address */
    if (vmm_flags & VMM_FLAG_VALLOC_SPECIFIC) {
        vaddr = (vaddr_t)*ptr;
    }

    mutex_acquire(&vmm_lock);

    /* allocate a region and put it in the aspace list */
    vmm_region_t* r;
    ret = alloc_region(aspace, name, size, vaddr, align_log2, vmm_flags,
                       VMM_REGION_FLAG_PHYSICAL, arch_mmu_flags, &r);
    if (ret) {
        goto err_alloc_region;
    }

    /* return the vaddr */
    *ptr = (void*)r->base;

    /* map all of the pages */
    for (i = 0; i < paddr_count; i++) {
        int err = arch_mmu_map(&aspace->arch_aspace, r->base + i * page_size,
                               paddr[i], page_size / PAGE_SIZE, arch_mmu_flags);
        LTRACEF("arch_mmu_map returns %d\n", err);
    }

    ret = NO_ERROR;

err_alloc_region:
    mutex_release(&vmm_lock);
    return ret;
}

static status_t vmm_alloc_pmm(vmm_aspace_t* aspace,
                              const char* name,
                              size_t size,
                              void** ptr,
                              uint8_t align_pow2,
                              uint vmm_flags,
                              uint arch_mmu_flags,
                              uint32_t pmm_alloc_flags,
                              uint8_t pmm_alloc_align_pow2) {
    status_t ret;
    struct vmm_obj *vmm_obj;
    struct obj_ref vmm_obj_ref = OBJ_REF_INITIAL_VALUE(vmm_obj_ref);

    size = round_up(size, PAGE_SIZE);
    if (size == 0)
        return ERR_INVALID_ARGS;

    ret = pmm_alloc(&vmm_obj, &vmm_obj_ref, size / PAGE_SIZE,
                    pmm_alloc_flags, pmm_alloc_align_pow2);
    if (ret) {
        LTRACEF("failed to allocate enough pages (asked for %zu)\n",
                size / PAGE_SIZE);
        return ret;
    }
    ret = vmm_alloc_obj(aspace, name, vmm_obj, 0, size, ptr, align_pow2,
                        vmm_flags, arch_mmu_flags);
    vmm_obj_del_ref(vmm_obj, &vmm_obj_ref);
    return ret;
}

status_t vmm_alloc_contiguous(vmm_aspace_t* aspace,
                              const char* name,
                              size_t size,
                              void** ptr,
                              uint8_t align_pow2,
                              uint vmm_flags,
                              uint arch_mmu_flags) {
    return vmm_alloc_pmm(aspace, name, size, ptr, align_pow2, vmm_flags,
                         arch_mmu_flags, PMM_ALLOC_FLAG_CONTIGUOUS, align_pow2);
}

status_t vmm_alloc(vmm_aspace_t* aspace,
                   const char* name,
                   size_t size,
                   void** ptr,
                   uint8_t align_pow2,
                   uint vmm_flags,
                   uint arch_mmu_flags) {
    return vmm_alloc_pmm(aspace, name, size, ptr, align_pow2, vmm_flags,
                         arch_mmu_flags, 0, 0);
}

static vmm_region_t* vmm_find_region(const vmm_aspace_t* aspace,
                                     vaddr_t vaddr) {
    vmm_region_t* r;

    DEBUG_ASSERT(aspace);

    if (!aspace)
        return NULL;

    vaddr = round_down(vaddr, PAGE_SIZE);

    /* search the region list */
    vmm_region_t r_ref;
    r_ref.flags = VMM_FLAG_NO_START_GUARD | VMM_FLAG_NO_END_GUARD;
    r_ref.base = vaddr;
    r_ref.obj_slice.size = PAGE_SIZE;
    r = bst_search_type(&aspace->regions, &r_ref, vmm_region_cmp, vmm_region_t,
                        node);
    if (!r) {
        return NULL;
    }
    if (!is_inside_region(r, vaddr)) {
        DEBUG_ASSERT(vaddr == r->base - PAGE_SIZE || vaddr == r->base + r->obj_slice.size);
        /* don't return regions that only overlap with guard page */
        return NULL;
    }
    return r;
}

status_t vmm_get_obj(const vmm_aspace_t *aspace, vaddr_t vaddr, size_t size,
                     struct vmm_obj_slice *slice) {
    status_t ret = NO_ERROR;

    DEBUG_ASSERT(slice);

    if (size == 0) {
        return ERR_INVALID_ARGS;
    }

    mutex_acquire(&vmm_lock);

    struct vmm_region *region = vmm_find_region(aspace, vaddr);
    if (!region) {
        ret = ERR_NOT_FOUND;
        goto out;
    }

    /* vmm_find_region already checked that vaddr is in region */
    vaddr_t last;
    if (__builtin_add_overflow(vaddr, size - 1, &last)) {
        /* vaddr + size overflows, this can't be a valid mapping */
        ret = ERR_INVALID_ARGS;
        goto out;
    }

    /*
     * region base / size should already be invariant checked, so we
     * need not check for overflow
     */
    vaddr_t region_last = region->base + (region->obj_slice.size - 1);
    if (region_last < last) {
        /* signal that we got an object, the whole range is not inside */
        ret = ERR_OUT_OF_RANGE;
        goto out;
    }

    if (!region->obj_slice.obj) {
        /* while the range is inside a region, there's no backing obj */
        ret = ERR_OUT_OF_RANGE;
        goto out;
    }

    /*
     * This should not overflow since the region is mapped already and our
     * vmm_obj uses size_t for its get_page() offset calculation. If we
     * extend to a larger type on 32-bit systems, we will need to switch to
     * using another type for slice representation.
     */
    size_t offset = (vaddr - region->base) + region->obj_slice.offset;

    /* all checks passed, we can update slice now */

    slice->obj = region->obj_slice.obj;
    slice->size = size;
    slice->offset = offset;
    /* direct use of obj_add_ref to operate inside the vmm mutex */
    obj_add_ref(&slice->obj->obj, &slice->obj_ref);

out:
    mutex_release(&vmm_lock);
    return ret;
}

static bool vmm_region_is_match(vmm_region_t* r,
                                vaddr_t va,
                                size_t size,
                                uint32_t flags) {
    if (!r) {
        return false;
    }
    if (flags & VMM_FREE_REGION_FLAG_EXPAND) {
        return is_range_inside_region(r, va, size);
    } else {
        return r->base == va && r->obj_slice.size == size;
    }
}

status_t vmm_free_region_etc(vmm_aspace_t* aspace,
                             vaddr_t vaddr,
                             size_t size,
                             uint32_t flags) {
    DEBUG_ASSERT(aspace);

    mutex_acquire(&vmm_lock);

    vmm_region_t* r = vmm_find_region(aspace, vaddr);
    if (!vmm_region_is_match(r, vaddr, size, flags)) {
        mutex_release(&vmm_lock);
        return ERR_NOT_FOUND;
    }

    /* remove it from aspace */
    bst_delete(&aspace->regions, &r->node);

    /* unmap it */
    arch_mmu_unmap(&aspace->arch_aspace, r->base,
                   r->obj_slice.size / PAGE_SIZE);

    mutex_release(&vmm_lock);

    /* release our hold on the backing object, if any */
    vmm_obj_slice_release(&r->obj_slice);

    /* free it */
    free(r);

    return NO_ERROR;
}

status_t vmm_free_region(vmm_aspace_t* aspace, vaddr_t vaddr) {
    return vmm_free_region_etc(aspace, vaddr, 1, VMM_FREE_REGION_FLAG_EXPAND);
}

status_t vmm_create_aspace(vmm_aspace_t** _aspace,
                           const char* name,
                           uint flags) {
    status_t err;

    /* Make sure the kernel and user address spaces are not adjacent */
    STATIC_ASSERT(USER_ASPACE_BASE >= PAGE_SIZE);
    STATIC_ASSERT(KERNEL_ASPACE_BASE >= PAGE_SIZE);
    STATIC_ASSERT(((KERNEL_ASPACE_BASE < USER_ASPACE_BASE) &&
                   (KERNEL_ASPACE_BASE + KERNEL_ASPACE_SIZE) <=
                   (USER_ASPACE_BASE - PAGE_SIZE)) ||
                  ((USER_ASPACE_BASE < KERNEL_ASPACE_BASE) &&
                   (USER_ASPACE_BASE + USER_ASPACE_SIZE) <=
                   (KERNEL_ASPACE_BASE - PAGE_SIZE)));

    DEBUG_ASSERT(_aspace);

    vmm_aspace_t* aspace = calloc(1, sizeof(vmm_aspace_t));
    if (!aspace)
        return ERR_NO_MEMORY;

    if (name)
        strlcpy(aspace->name, name, sizeof(aspace->name));
    else
        strlcpy(aspace->name, "unnamed", sizeof(aspace->name));

    aspace->flags = flags;

    if (aspace->flags & VMM_ASPACE_FLAG_KERNEL) {
        aspace->base = KERNEL_ASPACE_BASE;
        aspace->size = KERNEL_ASPACE_SIZE;
    } else {
        aspace->base = USER_ASPACE_BASE;
        aspace->size = USER_ASPACE_SIZE;
    }

    /* initialize the arch specific component to our address space */
    err = arch_mmu_init_aspace(&aspace->arch_aspace, aspace->base, aspace->size,
                               (aspace->flags & VMM_ASPACE_FLAG_KERNEL)
                                       ? ARCH_ASPACE_FLAG_KERNEL
                                       : 0);
    if (err < 0) {
        free(aspace);
        return err;
    }

    list_clear_node(&aspace->node);
    bst_root_initialize(&aspace->regions);

    mutex_acquire(&vmm_lock);
    list_add_head(&aspace_list, &aspace->node);
    mutex_release(&vmm_lock);

    *_aspace = aspace;

    return NO_ERROR;
}

status_t vmm_free_aspace(vmm_aspace_t* aspace) {
    DEBUG_ASSERT(aspace);

    /* pop it out of the global aspace list */
    mutex_acquire(&vmm_lock);
    if (!list_in_list(&aspace->node)) {
        mutex_release(&vmm_lock);
        return ERR_INVALID_ARGS;
    }
    list_delete(&aspace->node);

    /* free all of the regions */

    vmm_region_t* r;
    bst_for_every_entry(&aspace->regions, r, vmm_region_t, node) {
        /* unmap it */
        arch_mmu_unmap(&aspace->arch_aspace, r->base,
                       r->obj_slice.size / PAGE_SIZE);

        /* mark it as unmapped (only used for debug assert below) */
        r->obj_slice.size = 0;
    }
    mutex_release(&vmm_lock);

    /* without the vmm lock held, free all of the pmm pages and the structure */
    bst_for_every_entry(&aspace->regions, r, vmm_region_t, node) {
        DEBUG_ASSERT(!r->obj_slice.size);
        bst_delete(&aspace->regions, &r->node);

        /* release our hold on the backing object, if any */
        vmm_obj_slice_release(&r->obj_slice);

        /* free it */
        free(r);
    }

    /* make sure the current thread does not map the aspace */
    thread_t* current_thread = get_current_thread();
    if (current_thread->aspace == aspace) {
        THREAD_LOCK(state);
        current_thread->aspace = NULL;
        vmm_context_switch(aspace, NULL);
        THREAD_UNLOCK(state);
    }

    /* destroy the arch portion of the aspace */
    arch_mmu_destroy_aspace(&aspace->arch_aspace);

    /* free the aspace */
    free(aspace);

    return NO_ERROR;
}

void vmm_context_switch(vmm_aspace_t* oldspace, vmm_aspace_t* newaspace) {
    DEBUG_ASSERT(thread_lock_held());

    arch_mmu_context_switch(newaspace ? &newaspace->arch_aspace : NULL);
}

void vmm_set_active_aspace(vmm_aspace_t* aspace) {
    LTRACEF("aspace %p\n", aspace);

    thread_t* t = get_current_thread();
    DEBUG_ASSERT(t);

    if (aspace == t->aspace)
        return;

    /* grab the thread lock and switch to the new address space */
    THREAD_LOCK(state);
    vmm_aspace_t* old = t->aspace;
    t->aspace = aspace;
    vmm_context_switch(old, t->aspace);
    THREAD_UNLOCK(state);
}

static void dump_region(const vmm_region_t* r) {
    DEBUG_ASSERT(r);

    printf("\tregion %p: name '%s' range 0x%lx - 0x%lx size 0x%zx flags 0x%x "
           "mmu_flags 0x%x\n",
           r, r->name, r->base, r->base + (r->obj_slice.size - 1),
           r->obj_slice.size, r->flags, r->arch_mmu_flags);
}

static void dump_aspace(const vmm_aspace_t* a) {
    DEBUG_ASSERT(a);

    printf("aspace %p: name '%s' range 0x%lx - 0x%lx size 0x%zx flags 0x%x\n",
           a, a->name, a->base, a->base + (a->size - 1), a->size, a->flags);

    printf("regions:\n");
    vmm_region_t* r;
    bst_for_every_entry(&a->regions, r, vmm_region_t, node) {
        dump_region(r);
    }
}

static int cmd_vmm(int argc, const cmd_args* argv) {
    if (argc < 2) {
    notenoughargs:
        printf("not enough arguments\n");
    usage:
        printf("usage:\n");
        printf("%s aspaces\n", argv[0].str);
        printf("%s alloc <size> <align_pow2>\n", argv[0].str);
        printf("%s alloc_physical <paddr> <size> <align_pow2>\n", argv[0].str);
        printf("%s alloc_contig <size> <align_pow2>\n", argv[0].str);
        printf("%s free_region <address>\n", argv[0].str);
        printf("%s create_aspace\n", argv[0].str);
        printf("%s create_test_aspace\n", argv[0].str);
        printf("%s free_aspace <address>\n", argv[0].str);
        printf("%s set_test_aspace <address>\n", argv[0].str);
        return ERR_GENERIC;
    }

    static vmm_aspace_t* test_aspace;
    if (!test_aspace)
        test_aspace = vmm_get_kernel_aspace();

    if (!strcmp(argv[1].str, "aspaces")) {
        vmm_aspace_t* a;
        list_for_every_entry(&aspace_list, a, vmm_aspace_t, node) {
            dump_aspace(a);
        }
    } else if (!strcmp(argv[1].str, "alloc")) {
        if (argc < 4)
            goto notenoughargs;

        void* ptr = (void*)0x99;
        status_t err = vmm_alloc(test_aspace, "alloc test", argv[2].u, &ptr,
                                 argv[3].u, 0, 0);
        printf("vmm_alloc returns %d, ptr %p\n", err, ptr);
    } else if (!strcmp(argv[1].str, "alloc_physical")) {
        if (argc < 4)
            goto notenoughargs;

        void* ptr = (void*)0x99;
        status_t err = vmm_alloc_physical(test_aspace, "physical test",
                                          argv[3].u, &ptr, argv[4].u, argv[2].u,
                                          0, ARCH_MMU_FLAG_UNCACHED_DEVICE);
        printf("vmm_alloc_physical returns %d, ptr %p\n", err, ptr);
    } else if (!strcmp(argv[1].str, "alloc_contig")) {
        if (argc < 4)
            goto notenoughargs;

        void* ptr = (void*)0x99;
        status_t err = vmm_alloc_contiguous(test_aspace, "contig test",
                                            argv[2].u, &ptr, argv[3].u, 0, 0);
        printf("vmm_alloc_contig returns %d, ptr %p\n", err, ptr);
    } else if (!strcmp(argv[1].str, "free_region")) {
        if (argc < 2)
            goto notenoughargs;

        status_t err = vmm_free_region(test_aspace, (vaddr_t)argv[2].u);
        printf("vmm_free_region returns %d\n", err);
    } else if (!strcmp(argv[1].str, "create_aspace")) {
        vmm_aspace_t* aspace;
        status_t err = vmm_create_aspace(&aspace, "test", 0);
        printf("vmm_create_aspace returns %d, aspace %p\n", err, aspace);
    } else if (!strcmp(argv[1].str, "create_test_aspace")) {
        vmm_aspace_t* aspace;
        status_t err = vmm_create_aspace(&aspace, "test", 0);
        printf("vmm_create_aspace returns %d, aspace %p\n", err, aspace);
        if (err < 0)
            return err;

        test_aspace = aspace;
        get_current_thread()->aspace = aspace;
        thread_sleep(1);  // XXX hack to force it to reschedule and thus load
                          // the aspace
    } else if (!strcmp(argv[1].str, "free_aspace")) {
        if (argc < 2)
            goto notenoughargs;

        vmm_aspace_t* aspace = (void*)argv[2].u;
        if (test_aspace == aspace)
            test_aspace = NULL;

        if (get_current_thread()->aspace == aspace) {
            get_current_thread()->aspace = NULL;
            thread_sleep(1);  // hack
        }

        status_t err = vmm_free_aspace(aspace);
        printf("vmm_free_aspace returns %d\n", err);
    } else if (!strcmp(argv[1].str, "set_test_aspace")) {
        if (argc < 2)
            goto notenoughargs;

        test_aspace = (void*)argv[2].u;
        get_current_thread()->aspace = test_aspace;
        thread_sleep(1);  // XXX hack to force it to reschedule and thus load
                          // the aspace
    } else {
        printf("unknown command\n");
        goto usage;
    }

    return NO_ERROR;
}

STATIC_COMMAND_START
#if LK_DEBUGLEVEL > 0
STATIC_COMMAND("vmm", "virtual memory manager", &cmd_vmm)
#endif
STATIC_COMMAND_END(vmm);
