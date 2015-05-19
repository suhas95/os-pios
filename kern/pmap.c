#if LAB >= 3
/*
 * Page mapping and page directory/table management.
 *
 * Copyright (C) 1997 Massachusetts Institute of Technology
 * See section "MIT License" in the file LICENSES for licensing terms.
 *
 * Derived from the MIT Exokernel and JOS.
 * Adapted for PIOS by Bryan Ford at Yale University.
 * Adapted for 64-bit PIOS by Yu Zhang.
 */


#include <inc/types.h>
#include <inc/x86.h>
#include <inc/mmu.h>
#include <inc/cdefs.h>
#include <inc/string.h>
#include <inc/assert.h>
#include <inc/syscall.h>
#include <inc/vm.h>

#include <kern/cpu.h>
#include <kern/trap.h>
#include <kern/proc.h>
#include <kern/pmap.h>

// Statically allocated page directory mapping the kernel's address space.
// We use this as a template for all pdirs for user-level processes.
pte_t pmap_bootpmap_space[NPTENTRIES] gcc_aligned(PAGESIZE);
pte_t *pmap_bootpmap = pmap_bootpmap_space;

// Statically allocated page that we always keep set to all zeros.
uint8_t pmap_zero[PAGESIZE] gcc_aligned(PAGESIZE);

// The maximal page size cpu supports
static uint8_t max_page_entry_level = 2;


// --------------------------------------------------------------
// Set up initial memory mappings and turn on MMU.
// --------------------------------------------------------------



// Set up a four-level page table:
// pmap_bootpml4 is its linear (virtual) address of the root
// Then turn on paging.
// 
// This function only creates mappings in the kernel part of the address space
// (addresses outside of the range between VM_USERLO and VM_USERHI).
// The user part of the address space remains all PTE_ZERO until later.
//
static void pmap_init_bootpmap(pte_t *table, intptr_t vaddr, intptr_t paddr, size_t size, uint16_t perm, int level);

void
pmap_init(void)
{
	if (cpu_onboot()) {
		// detect if cpu supports 1G page
		cpuinfo info;
		cpuid(0x80000001, &info);
		if ((info.edx >> 26) & 0x1) {
			max_page_entry_level = 2;
		} else {
			max_page_entry_level = 1;
		}
		// Initialize pmap_bootpmap, the bootstrap page map.
		// Page map entries corresponding to the user-mode address 
		// space between VM_USERLO and VM_USERHI
		// should all be initialized to PTE_ZERO (see kern/pmap.h).
		// All virtual addresses below and above this user area
		// should be identity-mapped to the same physical addresses,
		// but only accessible in kernel mode (not in user mode).
#if SOL >= 3
		uint32_t mem_range_cnt = *(uint32_t *)mem_ptr(0x1000);
		uint32_t k;
		mem_addr_range *mem_ranges = mem_ptr(0x1004);
		size_t maxmem = 0;
		for (k = 0; k < mem_range_cnt; k++) {
			size_t end = mem_ranges[k].base + mem_ranges[k].size;
			if (end > maxmem)
				maxmem = end;
		}
		maxmem = ROUNDUP(maxmem, PDSIZE(max_page_entry_level));
		pmap_init_bootpmap(pmap_bootpmap, 0, 0, (0x1ULL << 48), 0xFFFF, NPTLVLS); // erase all pages
		pmap_init_bootpmap(pmap_bootpmap, 0, 0, VM_USERLO, PTE_P | PTE_W, NPTLVLS); // map lower kernel address
		pmap_init_bootpmap(pmap_bootpmap, VM_KERNLO, 0, maxmem, PTE_P | PTE_W, NPTLVLS); // map whole physical memory to kernel address
		pmap_bootpmap[PML4SELFOFFSET] = (intptr_t)pmap_bootpmap | PTE_P | PTE_W;
		pmap_bootpmap = mem_ptr(pmap_bootpmap);
#else
		panic("pmap_init() not implemented");
#endif
	}

	// On x86, segmentation maps a VA to a LA (linear addr) and
	// paging maps the LA to a PA.  i.e., VA => LA => PA.  If paging is
	// turned off the LA is used as the PA.  There is no way to
	// turn off segmentation.  At the moment we turn on paging,
	// the code we're executing must be in an identity-mapped memory area
	// where LA == PA according to the page mapping structures.
	// In PIOS this is always the case for the kernel's address space,
	// so we don't have to play any special tricks as in other kernels.

	uintptr_t cr4 = rcr4();
#if SOL >= 2
	cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT; // enable 128-bit XMM instructions
#endif
	lcr4(cr4);

	// Install the bootstrap page map level-4 into the PDBR.
	lcr3(mem_phys(pmap_bootpmap));

	uintptr_t cr0 = rcr0();
	cr0 |= CR0_AM|CR0_NE|CR0_TS;
	cr0 &= ~(CR0_EM);
	lcr0(cr0);

	if (cpu_onboot()) {
		pmap_check();
		pmap_check_adv();
	}
}

// we only map kernel address, linear address and physical address is identical
static void
pmap_init_bootpmap(pte_t *table, intptr_t vaddr, intptr_t paddr, size_t size, uint16_t perm, int level)
{
//	cprintf("[pmap init boot %d] table %p vaddr %p paddr %p size %p perm %x pdsize %p\n", level, table, vaddr, paddr, size, perm, PDSIZE(level));
	// addr and size should be aligned with PAGESIZE
	vaddr = PGADDR(vaddr);
	paddr = PGADDR(paddr);
	size = PGADDR(size);
	intptr_t vhi = vaddr + size;
	uint16_t page_perm = perm | PTE_G | PTE_P;
	uint16_t dir_perm = ((perm != 0xFFFF) ? perm : 0) | PTE_P | PTE_U;
	if (level > 0) {
		page_perm |= PTE_PS;
	}
	while (vaddr < vhi) {
		if (PDOFF(level, vaddr) == 0 && PDOFF(level, paddr) == 0 && vhi - vaddr >= PDSIZE(level)) {
			// we are happy to map aligned pages
			if (level <= max_page_entry_level || perm == 0xFFFF) {
				// map it in a whole
				table[PDX(level, vaddr)] = ((perm != 0xFFFF) ? (paddr | page_perm) : PTE_ZERO);
				vaddr += PDSIZE(level);
				paddr += PDSIZE(level);
				continue;
			}
		}
		// calculate correct size
		size_t size = PDSIZE(level) - PDOFF(level, vaddr);
		if (size > vhi - vaddr) {
			size = vhi - vaddr;
		}
		pageinfo *pi = mem_alloc();
		assert(pi != NULL);
		pte_t *subtable = (pte_t *)mem_pi2phys(pi);
		pmap_init_bootpmap(subtable, PDADDR(level, vaddr), 0, vaddr - PDADDR(level, vaddr), 0xFFFF, level - 1);
		pmap_init_bootpmap(subtable, vaddr, paddr, size, perm, level - 1);
		pmap_init_bootpmap(subtable, vaddr + size, 0, PDSIZE(level) - PDOFF(level, vaddr) - size, 0xFFFF, level - 1);
		table[PDX(level, vaddr)] = (pte_t)subtable | dir_perm;
		vaddr += size;
		paddr += size;
	}
}

//
// Allocate a new page map root, initialized from the bootstrap pmlap.
// Returns the new pmap with a reference count of 1.
//
pte_t *
pmap_newpmap(void)
{
	// alloc page for pml4 and pdpt
	// so that pmap_bootpmap only points to kernel space
	pageinfo *pi = mem_alloc();
	assert(pi != NULL);
	mem_incref(pi);
	pte_t *pml4 = mem_pi2ptr(pi);

	pi = mem_alloc();
	assert(pi != NULL);
	mem_incref(pi);
	pte_t *pdp = mem_pi2ptr(pi);

	// Initialize it from the bootstrap page map
//	assert(sizeof(pmap_bootpmap) == PAGESIZE);
	memmove(pml4, pmap_bootpmap, PAGESIZE);
	pml4[0] = mem_phys(pdp) | PTE_A | PTE_P | PTE_W | PTE_U;
	pml4[PML4SELFOFFSET] = mem_phys(pml4) | PTE_P | PTE_W;

	pte_t *boot_pdp = mem_ptr(PTE_ADDR(pmap_bootpmap[0]));
	memmove(pdp, boot_pdp, PAGESIZE);

	return pml4;
}

// Free a page map, and all page map tables and mappings it may contain.
void
pmap_freepmap(pageinfo *pml4pi)
{
	pmap_remove(mem_pi2ptr(pml4pi), VM_USERLO, VM_USERHI-VM_USERLO);
	mem_free(pml4pi);
}

static void pmap_freepd();
static void pmap_freept();

// Free a page directory pointer table and all page mappings it may contain.
// it would also leave kernel space (PTE_KERN) untouched
static void
pmap_freepdp(pageinfo *pdppi)
{
	pte_t *pdpe = mem_pi2ptr(pdppi), *pdpelim = pdpe + NPTENTRIES;
	for (; pdpe < pdpelim; pdpe++) {
		intptr_t pdtaddr = PTE_ADDR(*pdpe);
		if (pdtaddr != PTE_ZERO)
			mem_decref(mem_phys2pi(pdtaddr), pmap_freepd);
	}
	mem_free(pdppi);
}

// Free a page directory table and all page mappings it may contain.
// it would also leave kernel space (PTE_KERN) untouched
static void
pmap_freepd(pageinfo *pdpi)
{
	pte_t *pde = mem_pi2ptr(pdpi), *pdelim = pde + NPTENTRIES;
	for (; pde < pdelim; pde++) {
		intptr_t ptaddr = PTE_ADDR(*pde);
		if (ptaddr != PTE_ZERO)
			mem_decref(mem_phys2pi(ptaddr), pmap_freept);
	}
	mem_free(pdpi);
}

// Free a page table and all page mappings it may contain.
// it would also leave kernel space (PTE_KERN) untouched
static void
pmap_freept(pageinfo *ptpi)
{
	pte_t *pte = mem_pi2ptr(ptpi), *ptelim = pte + NPTENTRIES;
	for (; pte < ptelim; pte++) {
		intptr_t pgaddr = PTE_ADDR(*pte);
		if (pgaddr != PTE_ZERO)
			mem_decref(mem_phys2pi(pgaddr), mem_free);
	}
	mem_free(ptpi);
}

static void (*pmap_freefun[3])(pageinfo *pi) = {pmap_freept, pmap_freepd, pmap_freepdp};

// Given 'pml4', a pointer to a PML4 table, pmap_walk returns
// a pointer to the page table entry (PTE) for user virtual address 'va'.
// This requires walking the four-level page table structure.
//
// Assuming that pmtab points to the current page map table whose level
// is pmlevel, repeatedly do the following until the level equals 1.
// If the relevant lower level page map table doesn't exist in pmtab,
// then:
//    - If writing == 0, pmap_walk returns NULL.
//    - Otherwise, pmap_walk tries to allocate a new lower page map
//	table with mem_alloc.  If this fails, pmap_walk returns NULL.
//    - The new lower page map table is cleared and its refcount 
//      set to 1.
//    - If the relevant lower level page map table has already existed
//      in the table pointed-to by pmtab, but it is read shared and 
//      writing != 0, then copy the lower page map table to obtain an
//      exclusive copy of it and write-enable the entry in pmtab.
//    - If the lower table is a page table, pmap_walk returns a pointer
//      to the requested entry within the lower page table.
//    - Otherwise, let the lower table be pmtab, and do the above job 
//      repeatedly.
//
// Hint: you can turn a pageinfo pointer into the physical address of the
// page it refers to with mem_pi2phys() from kern/mem.h.
//
// Hint 2: the x86 MMU checks permission bits in all of the page map
// level-4, the page-directory-pointer, the page directory and 
// the page table, so it's safe to leave some page permissions
// more permissive than strictly necessary.
static pte_t *pmap_walk_level();

pte_t *
pmap_walk(pte_t *pml4, intptr_t va, bool writing)
{
	assert(va >= VM_USERLO && va < VM_USERHI);

#if SOL >= 3
	pte_t *pte = pmap_walk_level(NPTLVLS, pml4, va, writing);
	return pte;
#else /* not SOL >= 3 */
	// Fill in this function
	return NULL;
#endif /* not SOL >= 3 */
}

// Given a specified level table pointer 'pmtab',
// it returns a pointer to the PTE for user virtual address 'va'.
static pte_t *
pmap_walk_level(int pmlevel, pte_t *pmtab, intptr_t la, bool writing)
{
	//cprintf("[pmap walk level %d] table %p addr %p\n", pmlevel, pmtab, la);
	pte_t *pmte = &pmtab[PDX(pmlevel, la)];	// find entry in the specified level tabke
	pte_t *plowtab;				// will point to lower page map table
	assert(pmlevel > 0);
	int i;

	if (PTE_ADDR(*pmte) != PTE_ZERO) {			// lower ptab already exist?
		*pmte |= PTE_P;
		plowtab = mem_ptr(PTE_ADDR(*pmte));
	} else {				// no - create?
		pageinfo *pi;
		if (!writing || (pi = mem_alloc()) == NULL)
			return NULL;
		mem_incref(pi);
		plowtab = mem_pi2ptr(pi);

		// Clear all mappings in the page table
		int i;
		for (i = 0; i < NPTENTRIES; i++)
			plowtab[i] = PTE_ZERO;

		// The permissions here are overly generous, but they can
		// be further restricted by the permissions in the page table 
		// entries, if necessary.
		*pmte = mem_pi2phys(pi) | PTE_A | PTE_P | PTE_W | PTE_U;
	}

	// If the lower page map table is shared and we're writing, copy it first.
	// Must propagate the read-only status down to the page mappings.
	if (writing && !(*pmte & PTE_W)) {
		if (mem_ptr2pi(plowtab)->refcount == 1) {
			// Lower page map table isn't shared, so we can use in-place;
			// but must propagate the read-only status from the pmlevel
			// down to all individual entrys in lower level.
			int i;
			for (i = 0; i < NPTENTRIES; i++)
				plowtab[i] &= ~PTE_W;
		} else {
			// Lower page map table is or may still be shared - must copy.
			pageinfo *pi = mem_alloc();
			if (pi == NULL)
				return NULL;
			mem_incref(pi);
			pte_t *nplowtab = mem_pi2ptr(pi);

			// Copy all page table entries,
			// incrementing each's refcount
			int i;
			for (i = 0; i < NPTENTRIES; i++) {
				intptr_t pte = plowtab[i];
				nplowtab[i] = pte & ~PTE_W;
				assert(PTE_ADDR(pte) != 0);
				if (PTE_ADDR(pte) != PTE_ZERO)
					mem_incref(mem_phys2pi(PTE_ADDR(pte)));
			}

			// here we need to decrease original page table's refcount
			mem_decref(mem_ptr2pi(plowtab), pmap_freefun[pmlevel-1]);
			plowtab = nplowtab;
		}
		*pmte = mem_phys(plowtab) | PTE_A | PTE_P | PTE_W | PTE_U;
	}

	if (pmlevel == 1)
		return &plowtab[PDX(pmlevel-1, la)];
	else
		return pmap_walk_level(pmlevel-1, mem_ptr(PTE_ADDR(*pmte)), la, writing);
}

//
// Map the physical page 'pi' at user virtual address 'va'.
// The permissions (the low 12 bits) of the page table
//  entry should be set to 'perm | PTE_P'.
//
// Requirements
//   - If there is already a page mapped at 'va', it should be pmap_removed().
//   - If necessary, allocate a page able on demand and insert into 'pml4'.
//   - pi->refcount should be incremented if the insertion succeeds.
//   - The TLB must be invalidated if a page was formerly present at 'va'.
//
// Corner-case hint: Make sure to consider what happens when the same 
// pi is re-inserted at the same virtual address in the same pml4.
// What if this is the only reference to that page?
//
// RETURNS: 
//   a pointer to the inserted PTE on success (same as pmap_walk)
//   NULL, if page table couldn't be allocated
//
// Hint: The reference solution uses pmap_walk, pmap_remove, and mem_pi2phys.
//
pte_t *
pmap_insert(pte_t *pml4, pageinfo *pi, intptr_t va, int perm)
{
//	cprintf("[pmap ins] pml4 %p pi %p paddr %p addr %p\n", pml4, pi, mem_pi2phys(pi), va);
#if SOL >= 3
	pte_t* pte = pmap_walk(pml4, va, 1);
	if (pte == NULL)
		return NULL;

	// We must increment pi->refcount before pmap_remove, so that
	// if pi is already mapped at va (we're just changing perm),
	// we don't lose the page when we decref in pmap_remove.
	mem_incref(pi);

	// Now remove the old mapping in this PTE.
	// WWY: we can simply do this with mem_decref()
	if (PTE_ADDR(*pte) != PTE_ZERO) {
		pmap_inval(pml4, va, PAGESIZE);
		mem_decref(mem_phys2pi(PGADDR(*pte)), mem_free);
	}

	*pte = mem_pi2phys(pi) | perm | PTE_P;
	return pte;
#else /* not SOL >= 3 */
	// Fill in this function
	return NULL;
#endif /* not SOL >= 3 */
}

//
// Unmap the physical pages starting at user virtual address 'va'
// and covering a virtual address region of 'size' bytes.
// The caller must ensure that both 'va' and 'size' are page-aligned.
// If there is no mapping at that address, pmap_remove silently does nothing.
// Clears nominal permissions (SYS_RW flags) as well as mappings themselves.
//
// Details:
//   - The refcount on mapped pages should be decremented atomically.
//   - The physical page should be freed if the refcount reaches 0.
//   - The page table entry corresponding to 'va' should be set to 0.
//     (if such a PTE exists)
//   - The TLB must be invalidated if you remove an entry from
//     the pml4/pdp/pdir/ptab.
//   - If the region to remove covers a whole 4MB page table region,
//     then unmap and free the page table after unmapping all its contents.
//
// Hint: The TA solution is implemented using pmap_lookup,
// 	pmap_inval, and mem_decref.
//
static intptr_t pmap_remove_level();

void
pmap_remove(pte_t *pml4, intptr_t va, size_t size)
{
	assert(PGOFF(size) == 0);	// must be page-aligned
	assert(va >= VM_USERLO && va < VM_USERHI);
	assert(size <= VM_USERHI - va);

//	cprintf("[pmap rm] table %p addr %p size %p\n", pml4, va, size);
#if SOL >= 3
	pmap_inval(pml4, va, size);	// invalidate region we're removing

	intptr_t vahi = va + size;
	while (va < vahi) {
		va = pmap_remove_level(NPTLVLS, pml4, va, vahi);
	}
#else /* not SOL >= 3 */
	// Fill in this function
#endif /* not SOL >= 3 */
}

// pmlevel == 3, pmtab contains a PML4 base address
// pmlevel == 2, pmtab contains a page-directory-pointer base address
// pmlevel == 1, pmtab contains a page-directory base address
// pmlevel == 0, pmtab contains a page table base address
static intptr_t
pmap_remove_level(int pmlevel, pte_t *pmtab, intptr_t va, intptr_t vahi)
{
//	cprintf("[pmap rm level %d] table %p addr %p size %p\n", pmlevel, pmtab, va, vahi - va);
	int i;
	pte_t *pmte;

	assert(pmlevel >= 0);

	// find the entry in the specified level table
	pmte = &pmtab[PDX(pmlevel, va)];

	while (va < vahi) {
		if (PTE_ADDR(*pmte) == PTE_ZERO) {
			// the entry does not points to a lower-level table
			// skip the entire lower-level table region
			pmte++;
			va = PDADDR(pmlevel, va + PDSIZE(pmlevel));
			continue;
		}

		if (PDOFF(pmlevel, va) == 0 && vahi - va >= PDSIZE(pmlevel)) {
			// we can remove an entire lower-level table
			uintptr_t pgaddr = PGADDR(*pmte);
			if (pmlevel == 0) {
				// we are now manipulating lowest page table
				mem_decref(mem_phys2pi(pgaddr), mem_free);
			} else {
				// we are now manipulating upper level page table
				mem_decref(mem_phys2pi(pgaddr), pmap_freefun[pmlevel - 1]);
			}
			*pmte = PTE_ZERO;
			pmte++;
			va += PDSIZE(pmlevel);
			continue;
		}

		// remove partial lower-level table
		// pmlevel should be greater than 0, can't remove partial page
		assert(pmlevel > 0);

		// unshare page entry
		pmap_walk_level(pmlevel, pmtab, va, 1);

		// find correct vahi for lower level
		uintptr_t lvahi = PDADDR(pmlevel, va) + PDSIZE(pmlevel);
		if (PDADDR(pmlevel, va) + PDSIZE(pmlevel) > vahi)
			lvahi = vahi;
		pmap_remove_level(pmlevel - 1, mem_ptr(PTE_ADDR(*pmte)), va, lvahi);
		va = lvahi;
		pmte++;
	}
	return va;
}

//
// Invalidate the TLB entry or entries for a given virtual address range,
// but only if the page tables being edited are the ones
// currently in use by the processor.
//
void
pmap_inval(pte_t *pml4, intptr_t va, size_t size)
{
	// Flush the entry only if we're modifying the current address space.
	proc *p = proc_cur();
	if (p == NULL || p->pml4 == pml4) {
		if (size == PAGESIZE)
			invlpg(va);	// invalidate one page
		else
			lcr3(mem_phys(pml4));	// invalidate everything
	}
}

//
// Virtually copy a range of pages from spml4 to dpml4 (could be the same).
// Uses copy-on-write to avoid the cost of immediate copying:
// instead just copies the mappings and makes both source and dest read-only.
// Returns true if successfull, false if not enough memory for copy.
//
static void pmap_copy_level();

int
pmap_copy(pte_t *spml4, intptr_t sva, pte_t *dpml4, intptr_t dva,
		size_t size)
{
	assert(PDOFF(0, sva) == 0);	// must be 4KB-aligned
	assert(PDOFF(0, dva) == 0);
	assert(PDOFF(0, size) == 0);
	assert(sva >= VM_USERLO && sva < VM_USERHI);
	assert(dva >= VM_USERLO && dva < VM_USERHI);
	assert(size <= VM_USERHI - sva);
	assert(size <= VM_USERHI - dva);

#if SOL >= 3
	// Invalidate both regions we may be modifying
	pmap_inval(spml4, sva, size);
	pmap_inval(dpml4, dva, size);

	intptr_t svahi = sva + size;
	pmap_copy_level(NPTLVLS, spml4, sva, dpml4, dva, svahi);
	return 1;
#else /* not SOL >= 3 */
	panic("pmap_copy() not implemented");
#endif /* not SOL >= 3 */
}

//
// pmlevel == 3, spmtab & dpmtab => source/destination pml4 table
// pmlevel == 2, spmtab & dpmtab => source/destination pdp table
// pmlevel == 1, spmtab & dpmtab => source/destination page directory table
// pmlevel == 0, spmtab & dpmtab => source/destination page table
//
static void
pmap_copy_level(int pmlevel, pte_t *spmtab, intptr_t sva, pte_t *dpmtab, 
		intptr_t dva, intptr_t svahi)
{
//int i;
	if (sva >= svahi)
		return;

	assert(pmlevel >= 0);

	pte_t *spmte = &spmtab[PDX(pmlevel, sva)];
	pte_t *dpmte = &dpmtab[PDX(pmlevel, dva)];

	while (sva < svahi) {
		if (PDOFF(pmlevel, sva) == 0 && PDOFF(pmlevel, dva) == 0 && svahi - sva >= PDSIZE(pmlevel)) {
			// we can share an entire lower-level table
			if (PTE_ADDR(*dpmte) != PTE_ZERO) {
				// remove old page mapping first
				// if *dpmte equals to *spmte, refcount will be greater than 1, so it is safe to use pmap_remove_level()
				pmap_remove_level(pmlevel, dpmtab, dva, dva + PDSIZE(pmlevel));
			}
			assert(PTE_ADDR(*dpmte) == PTE_ZERO);

			// remove write permissions and copy mappings
			*spmte &= ~(uint64_t)PTE_W;
			*dpmte = *spmte;

			if (PTE_ADDR(*spmte) != PTE_ZERO) {
				mem_incref(mem_phys2pi(PTE_ADDR(*spmte)));
			}

			spmte++, dpmte++;
			sva += PDSIZE(pmlevel);
			dva += PDSIZE(pmlevel);
			continue;
		}

		// find correct vahi for lower level
		// best align sva and dva, at least make one aligned to PDSIZE(pmlevel)
		size_t size = PDSIZE(pmlevel);
		// sva or dva not aligned
		if (PDOFF(pmlevel, sva) > PDOFF(pmlevel, dva)) {
			size -= PDOFF(pmlevel, sva);
		} else {
			size -= PDOFF(pmlevel, dva);
		}
		// svahi or dvahi beyond PDSIZE
		if (sva + size > svahi) {
			size = svahi - sva;
		}

		// copy partial lower-level table
		if (PTE_ADDR(*spmte) == PTE_ZERO) {
			// source is invalid, remove dest as well
			pmap_remove_level(pmlevel, dpmtab, dva, dva + size);
		} else {
			// source is valid, copy it
			// we must guarantee that lower-level table exists
			if (PTE_ADDR(*dpmte) == PTE_ZERO) {
				pmap_walk_level(pmlevel, dpmtab, dva, 1);
			}
			assert(PTE_ADDR(*dpmte) != PTE_ZERO);
			pmap_copy_level(pmlevel - 1, mem_ptr(PTE_ADDR(*spmte)), sva, mem_ptr(PTE_ADDR(*dpmte)), dva, sva + size);
		}
		dva += size;
		sva += size;
		// move to next page entry only if address is aligned
		if (PDOFF(pmlevel, sva) == 0) {
			spmte++;
		}
		if (PDOFF(pmlevel, dva) == 0) {
			dpmte++;
		}
	}
}

//
// Transparently handle a page fault entirely in the kernel, if possible.
// If the page fault was caused by a write to a copy-on-write page,
// then performs the actual page copy on demand and calls trap_return().
// If the fault wasn't due to the kernel's copy on write optimization,
// however, this function just returns so the trap gets blamed on the user.
//
void
pmap_pagefault(trapframe *tf)
{
	// Read processor's CR2 register to find the faulting linear address.
	uintptr_t fva = rcr2();

#if SOL >= 3
	// It can't be our problem unless it's a write fault in user space!
	if (fva < VM_USERLO || fva >= VM_USERHI || !(tf->err & PFE_WR)) {
		cprintf("pmap_pagefault: fva %p err %x\n", fva, tf->err);
		return;
	}

	proc *p = proc_cur();
	int pmlevel = NPTLVLS;
	pte_t *pmtab = p->pml4;
	while ( pmlevel >= 1) {
		pte_t *pmte = &pmtab[PDX(pmlevel, fva)];
		if (!(*pmte & PTE_P)) {
			cprintf("pmap_pagefault: %d-level pmte for fva %p doesn't exist *pmte %p\n", pmlevel, fva, *pmte);
			return;		// ptab doesn't exist at all - blame user
		}
		pmtab = mem_ptr(PTE_ADDR(*pmte)), pmlevel--;
	}

	// Find the page table entry, copying the page table if it's shared.
	pte_t *pte = pmap_walk(p->pml4, fva, 1);
	if ((*pte & (SYS_READ | SYS_WRITE | PTE_P)) !=
			(SYS_READ | SYS_WRITE | PTE_P)) {
		cprintf("pmap_pagefault: page for fva %p doesn't exist proc %p *pte %p\n", fva, p, pte);
		return;		// page doesn't exist at all - blame user
	}
	assert(!(*pte & PTE_W));

	// Find the "shared" page.  If refcount is 1, we have the only ref!
	intptr_t pg = PTE_ADDR(*pte);
	if (pg == PTE_ZERO || mem_phys2pi(pg)->refcount > 1) {
		pageinfo *npi = mem_alloc(); assert(npi);
		mem_incref(npi);
		intptr_t npg = mem_pi2phys(npi);
		memmove((void*)npg, (void*)pg, PAGESIZE); // copy the page
		if (pg != PTE_ZERO)
			mem_decref(mem_phys2pi(pg), mem_free); // drop old ref
		pg = npg;
	}
	*pte = pg | SYS_RW | PTE_A | PTE_D | PTE_W | PTE_U | PTE_P;

	// Make sure the old mapping doesn't get used anymore
	pmap_inval(p->pml4, PGADDR(fva), PAGESIZE);

	trap_return(tf);
#else /* not SOL >= 3 */
	// Fill in the rest of this code.
#endif /* not SOL >= 3 */
}

//
// Helper function for pmap_merge: merge a single memory page
// that has been modified in both the source and destination.
// If conflicting writes to a single byte are detected on the page,
// print a warning to the console and remove the page from the destination.
// If the destination page is read-shared, be sure to copy it before modifying!
//
void
pmap_mergepage(pte_t *rpte, pte_t *spte, pte_t *dpte, intptr_t dva)
{
#if SOL >= 3
	uint8_t *rpg = mem_ptr(PTE_ADDR(*rpte));
	uint8_t *spg = mem_ptr(PTE_ADDR(*spte));
	uint8_t *dpg = mem_ptr(PTE_ADDR(*dpte));
//	if (mem_phys(dpg) == pmap_zero) return;	// Conflict - just leave dest unmapped

	// Make sure the destination page isn't shared
	if (mem_phys(dpg) == PTE_ZERO || mem_ptr2pi(dpg)->refcount > 1) {
		pageinfo *npi = mem_alloc(); assert(npi);
		mem_incref(npi);
		uint8_t *npg = mem_pi2ptr(npi);
		memmove(npg, dpg, PAGESIZE); // copy the page
		if (dpg != (uint8_t*)PTE_ZERO)
			mem_decref(mem_ptr2pi(dpg), mem_free); // drop old ref
		dpg = npg;
		*dpte = mem_phys(npg) |
			SYS_RW | PTE_A | PTE_D | PTE_W | PTE_U | PTE_P;
	}

	// Do a byte-by-byte diff-and-merge into the destination
	int i;
	for (i = 0; i < PAGESIZE; i++) {
		if (spg[i] == rpg[i])
			continue;	// unchanged in source - leave dest
		if (dpg[i] == rpg[i]) {
			dpg[i] = spg[i];	// unchanged in dest - use src
			continue;
		}

		cprintf("pmap_mergepage: conflict at dva %p\n", dva);
		mem_decref(mem_phys2pi(PTE_ADDR(*dpte)), mem_free);
		*dpte = PTE_ZERO;
		return;
	}
#else /* not SOL >= 3 */
	panic("pmap_mergepage() not implemented");
#endif /* not SOL >= 3 */
}

// 
// Merge differences between a reference snapshot represented by rpml4
// and a source address space spml4 into a destination address space dpml4.
//
static void pmap_merge_level();

int
pmap_merge(pte_t *rpml4, pte_t *spml4, intptr_t sva,
		pte_t *dpml4, intptr_t dva, size_t size)
{
	assert(PDOFF(0, sva) == 0);	// must be 4KB-aligned
	assert(PDOFF(0, dva) == 0);
	assert(PDOFF(0, size) == 0);
	assert(sva >= VM_USERLO && sva < VM_USERHI);
	assert(dva >= VM_USERLO && dva < VM_USERHI);
	assert(size <= VM_USERHI - sva);
	assert(size <= VM_USERHI - dva);

#if SOL >= 3
	// Invalidate the source and destination regions we may be modifying.
	// (We may remove permissions from the source for copy-on-write.)
	// No need to invalidate rpdir since rpdirs are never loaded.
	pmap_inval(spml4, sva, size);
	pmap_inval(dpml4, dva, size);

	pmap_merge_level(NPTLVLS, rpml4, spml4, sva, dpml4, dva, sva + size);
	return 1;
#else /* not SOL >= 3 */
	panic("pmap_merge() not implemented");
#endif /* not SOL >= 3 */
}

static void
pmap_merge_level(int pmlevel, pte_t *rpmtab, pte_t *spmtab, intptr_t sva,
		pte_t *dpmtab, intptr_t dva, intptr_t svahi)
{
	if (sva >= svahi)
		return;

	assert(pmlevel >= 0);

	pte_t *rpmte = &rpmtab[PDX(pmlevel, sva)];
	pte_t *spmte = &spmtab[PDX(pmlevel, sva)];
	pte_t *dpmte = &dpmtab[PDX(pmlevel, dva)];

	while (sva < svahi) {
		// TODO, now assume perfectly aligned
		if (*spmte == *rpmte) {
			// unchanged in source, do nothing
		} else if (*dpmte == *rpmte) {
			// unchanged in dest, copy from source
			pmap_copy_level(pmlevel, spmtab, sva, dpmtab, dva, sva + PDSIZE(pmlevel));
		} else {
			if (pmlevel > 0) {
				// jump into lower level
				// rpmte and spmte can be PTE_ZERO, but dpmte can't
				pte_t *rlpmtab = mem_ptr(PTE_ADDR(*rpmte));
				pte_t *slpmtab = mem_ptr(PTE_ADDR(*spmte));
				pte_t *dlpmtab = mem_ptr(PTE_ADDR(*dpmte));
				if (rlpmtab == NULL) rlpmtab = mem_ptr(PTE_ZERO);
				if (slpmtab == NULL) slpmtab = mem_ptr(PTE_ZERO);
				if (PTE_ADDR(dlpmtab) == PTE_ZERO)
					dlpmtab = pmap_walk_level(pmlevel, dpmtab, dva, 1);
				assert(PTE_ADDR(dlpmtab) != PTE_ZERO);
				uintptr_t lsvahi = PDADDR(pmlevel, sva) + PDSIZE(pmlevel);
				if (lsvahi > svahi) lsvahi = svahi;
				pmap_merge_level(pmlevel - 1, rlpmtab, slpmtab, sva, dlpmtab, dva, lsvahi);
			} else {
				// use mergepage
				pmap_mergepage(rpmte, spmte, dpmte, dva);
			}
		}
		rpmte++;
		spmte++;
		dpmte++;
		sva += PDSIZE(pmlevel);
		dva += PDSIZE(pmlevel);
	}
}

static void pmap_setperm_level();
//
// Set the nominal permission bits on a range of virtual pages to 'perm'.
// Adding permission to a nonexistent page maps zero-filled memory.
// It's OK to add SYS_READ and/or SYS_WRITE permission to a PTE_ZERO mapping;
// this causes the pmap_zero page to be mapped read-only (PTE_P but not PTE_W).
// If the user gives SYS_WRITE permission to a PTE_ZERO mapping,
// the page fault handler copies the zero page when the first write occurs.
//
int
pmap_setperm(pte_t *pml4, intptr_t va, size_t size, int perm)
{
	assert(PGOFF(va) == 0);
	assert(PGOFF(size) == 0);
	assert(va >= VM_USERLO && va < VM_USERHI);
	assert(size <= VM_USERHI - va);
	assert((perm & ~(SYS_RW)) == 0);

#if SOL >= 3
	pmap_inval(pml4, va, size);	// invalidate region we're modifying

	// Determine the nominal and actual bits to set or clear
	uint64_t pteand, pteor;
	if (!(perm & SYS_READ))		// clear all permissions
		pteand = ~(SYS_RW | PTE_W | PTE_P), pteor = 0;
	else if (!(perm & SYS_WRITE))	// read-only permission
		pteand = ~(SYS_WRITE | PTE_W),
		pteor = (SYS_READ | PTE_U | PTE_P | PTE_A);
	else	// nominal read/write (but don't add PTE_W to shared mappings!)
		pteand = ~0, pteor = (SYS_RW | PTE_U | PTE_P | PTE_A | PTE_D);

	uintptr_t vahi = va + size;
	pmap_setperm_level(NPTLVLS, pml4, va, vahi, pteand, pteor);
	return 1;
#else /* not SOL >= 3 */
	panic("pmap_merge() not implemented");
#endif /* not SOL >= 3 */
}

static void
pmap_setperm_level(int pmlevel, pte_t *pmtab, uintptr_t va, uintptr_t vahi, uint64_t pteand, uint64_t pteor)
{
	int i;
	assert(pmlevel >= 0);

	while (va < vahi) {
		pte_t *pmte = &pmtab[PDX(pmlevel, va)];

		if (PTE_ADDR(*pmte) == PTE_ZERO) {
			// no such page exists
			if (pteor == 0) {
				// we can just jump over
				va = PDADDR(pmlevel, va) + PDSIZE(pmlevel);
				continue;
			}
		}

		if (pmlevel > 0) {
			// find & unshare PTE
			pmap_walk_level(pmlevel, pmtab, va, 1);
		}

		if (PDOFF(pmlevel, va) == 0 && vahi - va >= PDSIZE(pmlevel)) {
			// we can set an entire lower-level table
			if (pmlevel == 0) {
				// just set perm
				*pmte = (*pmte & pteand) | pteor;
			} else {
				// do it recursively
				pmap_setperm_level(pmlevel - 1, mem_ptr(PTE_ADDR(*pmte)), va, va + PDSIZE(pmlevel), pteand, pteor);
			}
			pmte++;
			va += PDSIZE(pmlevel);
			continue;
		}

		// find correct vahi for lower level
		uintptr_t lvahi = PDADDR(pmlevel, va) + PDSIZE(pmlevel);
		if (PDADDR(pmlevel, va) + PDSIZE(pmlevel) > vahi)
			lvahi = vahi;
		pmap_setperm_level(pmlevel - 1, mem_ptr(PTE_ADDR(*pmte)), va, lvahi, pteand, pteor);
		va = lvahi;
		pmte++;
	}
}
//
// This function returns the physical address of the page containing 'va',
// defined by the page directory 'pdir'.  The hardware normally performs
// this functionality for us!  We define our own version to help check
// the pmap_check() function; it shouldn't be used elsewhere.
//
static intptr_t
va2pa(pte_t *pmtab, intptr_t va)
{
	int pmlevel = NPTLVLS;
	pte_t *ptab;
	while (pmlevel >= 1) {
		pmtab = &pmtab[PDX(pmlevel, va)];
		if (!(*pmtab & PTE_P))
			return ~0;
		ptab = mem_ptr(PTE_ADDR(*pmtab));
		pmlevel --;
		pmtab = ptab;
	}
	if (!(ptab[PDX(0, va)] & PTE_P))
		return ~0;
	return PTE_ADDR(ptab[PDX(0, va)]);
}

// check pmap_insert, pmap_remove, &c
void
pmap_check(void)
{
	extern pageinfo *mem_freelist;

	pageinfo *pi, *pi0, *pi1, *pi2, *pi3, *pi4;
	pageinfo *fl;
	pte_t *ptep, *ptep1;
	int i;

	// should be able to allocate three pages
	pi0 = pi1 = pi2 = pi3 = pi4 = 0;
	pi0 = mem_alloc();
	pi1 = mem_alloc();
	pi2 = mem_alloc();
	pi3 = mem_alloc();
	pi4 = mem_alloc();

	assert(pi0);
	assert(pi1 && pi1 != pi0);
	assert(pi2 && pi2 != pi1 && pi2 != pi0);
	assert(pi3 && pi3 != pi2 && pi3 != pi1 && pi3 != pi0);
	assert(pi4 && pi4 != pi3 && pi4 != pi2 && pi4 != pi1 && pi4 != pi0);

	// temporarily steal the rest of the free pages
	fl = mem_freelist;
	mem_freelist = NULL;

	// should be no free memory
	assert(mem_alloc() == NULL);

	// there is no free memory, so we can't allocate a page table 
	assert(pmap_insert(pmap_bootpmap, pi2, VM_USERLO, 0) == NULL);

	// free pi0, pi1 and try again: pi0 and pi1 should be used for page table
	mem_free(pi0);
	mem_free(pi1);
	assert(pmap_insert(pmap_bootpmap, pi2, VM_USERLO, 0) != NULL);
	assert(PTE_ADDR(((pte_t *)PTE_ADDR(pmap_bootpmap[PDX(3, VM_USERLO)]))[PDX(2, VM_USERLO)]) == mem_pi2phys(pi1)); // pi2 is used for PDPT
	assert(PTE_ADDR(((pte_t *)PTE_ADDR(((pte_t *)PTE_ADDR(pmap_bootpmap[PDX(3, VM_USERLO)]))[PDX(2, VM_USERLO)]))[PDX(1, VM_USERLO)]) == mem_pi2phys(pi0)); // pi1 is used for PDT
	assert(va2pa(pmap_bootpmap, VM_USERLO) == mem_pi2phys(pi2));
	assert(pi2->refcount == 1);
	assert(pi1->refcount == 1);
	assert(pi0->refcount == 1);

	// should be able to map pi3 at VM_USERLO+PAGESIZE
	// because pi0 and pi1 is already allocated for page table
	assert(pmap_insert(pmap_bootpmap, pi3, VM_USERLO+PAGESIZE, 0));
	assert(va2pa(pmap_bootpmap, VM_USERLO+PAGESIZE) == mem_pi2phys(pi3));
	assert(pi3->refcount == 1);

	// should be no free memory
	assert(mem_alloc() == NULL);

	// should be able to map pi3 at VM_USERLO+PAGESIZE
	// because it's already there
	assert(pmap_insert(pmap_bootpmap, pi3, VM_USERLO+PAGESIZE, 0));
	assert(va2pa(pmap_bootpmap, VM_USERLO+PAGESIZE) == mem_pi2phys(pi3));
	assert(pi3->refcount == 1);

	// pi3 should NOT be on the free list
	// could hapien in ref counts are handled slopiily in pmap_insert
	assert(mem_alloc() == NULL);

	// check that pmap_walk returns a pointer to the pte
	ptep = mem_ptr(PTE_ADDR(((pte_t *)PTE_ADDR(((pte_t *)PTE_ADDR(pmap_bootpmap[PDX(3, VM_USERLO+PAGESIZE)]))[PDX(2, VM_USERLO+PAGESIZE)]))[PDX(1, VM_USERLO+PAGESIZE)]));
	assert(pmap_walk(pmap_bootpmap, VM_USERLO+PAGESIZE, 0)
		== ptep+PDX(0, VM_USERLO+PAGESIZE));

	// should be able to change permissions too.
	assert(pmap_insert(pmap_bootpmap, pi3, VM_USERLO+PAGESIZE, PTE_U));
	assert(va2pa(pmap_bootpmap, VM_USERLO+PAGESIZE) == mem_pi2phys(pi3));
	assert(pi3->refcount == 1);
	assert(*pmap_walk(pmap_bootpmap, VM_USERLO+PAGESIZE, 0) & PTE_U);
	assert(((pte_t *)PTE_ADDR(((pte_t *)PTE_ADDR(pmap_bootpmap[PDX(3, VM_USERLO+PAGESIZE)]))[PDX(2, VM_USERLO+PAGESIZE)]))[PDX(1, VM_USERLO+PAGESIZE)] & PTE_U);
	
	// should not be able to map at VM_USERLO+PTSIZE
	// because we need a free page for a page table
	assert(pmap_insert(pmap_bootpmap, pi0, VM_USERLO+PTSIZE, 0) == NULL);

	// insert pi2 at VM_USERLO+PAGESIZE (replacing pi3)
	assert(pmap_insert(pmap_bootpmap, pi2, VM_USERLO+PAGESIZE, 0));
	assert(!(*pmap_walk(pmap_bootpmap, VM_USERLO+PAGESIZE, 0) & PTE_U));

	// should have pi2 at both +0 and +PAGESIZE, pi3 nowhere, ...
	assert(va2pa(pmap_bootpmap, VM_USERLO+0) == mem_pi2phys(pi2));
	assert(va2pa(pmap_bootpmap, VM_USERLO+PAGESIZE) == mem_pi2phys(pi2));
	// ... and ref counts should reflect this
	assert(pi2->refcount == 2);
	assert(pi3->refcount == 0);

	// pi3 should be returned by mem_alloc
	assert(mem_alloc() == pi3);

	// unmapping pi2 at VM_USERLO+0 should keep pi2 at +PAGESIZE
	pmap_remove(pmap_bootpmap, VM_USERLO+0, PAGESIZE);
	assert(va2pa(pmap_bootpmap, VM_USERLO+0) == ~0);
	assert(va2pa(pmap_bootpmap, VM_USERLO+PAGESIZE) == mem_pi2phys(pi2));
	assert(pi2->refcount == 1);
	assert(pi3->refcount == 0);
	assert(mem_alloc() == NULL);	// still should have no pages free

	// unmapping pi2 at VM_USERLO+PAGESIZE should free it
	pmap_remove(pmap_bootpmap, VM_USERLO+PAGESIZE, PAGESIZE);
	assert(va2pa(pmap_bootpmap, VM_USERLO+0) == ~0);
	assert(va2pa(pmap_bootpmap, VM_USERLO+PAGESIZE) == ~0);
	assert(pi2->refcount == 0);
	assert(pi3->refcount == 0);

	// so it should be returned by page_alloc
	assert(mem_alloc() == pi2);

	// should once again have no free memory
	assert(mem_alloc() == NULL);

	// should be able to pmap_insert to change a page
	// and see the new data immediately.
	memset(mem_pi2ptr(pi2), 1, PAGESIZE);
	memset(mem_pi2ptr(pi3), 2, PAGESIZE);
	pmap_insert(pmap_bootpmap, pi2, VM_USERLO, 0);
	assert(pi2->refcount == 1);
	assert(*(int*)VM_USERLO == 0x01010101);
	pmap_insert(pmap_bootpmap, pi3, VM_USERLO, 0);
	assert(*(int*)VM_USERLO == 0x02020202);
	assert(pi3->refcount == 1);
	assert(pi2->refcount == 0);
	assert(mem_alloc() == pi2);
	pmap_remove(pmap_bootpmap, VM_USERLO, PAGESIZE);
	assert(pi3->refcount == 0);
	assert(mem_alloc() == pi3);

	// now use a pmap_remove on a large region to take pi0 and pi1 back
	pmap_remove(pmap_bootpmap, VM_USERLO, VM_USERHI-VM_USERLO);
	assert(PTE_ADDR(((pte_t *)PTE_ADDR(pmap_bootpmap[PDX(3, VM_USERLO)]))[PDX(2, VM_USERLO)]) == PTE_ZERO); // pi1 was used for PDT
	assert(pi0->refcount == 0);
	assert(pi1->refcount == 0);
	assert(mem_alloc() == pi1);
	assert(mem_alloc() == pi0);
	assert(mem_freelist == NULL);

	// test pmap_remove with large, non-ptable-aligned regions
	mem_free(pi1);
	mem_free(pi0);
	uintptr_t va = VM_USERLO;
	assert(pmap_insert(pmap_bootpmap, pi4, va, 0));
	assert(pmap_insert(pmap_bootpmap, pi4, va+PAGESIZE, 0));
	assert(pmap_insert(pmap_bootpmap, pi4, va+PTSIZE-PAGESIZE, 0));
	assert(PTE_ADDR(((pte_t *)PTE_ADDR(pmap_bootpmap[PDX(3, VM_USERLO)]))[PDX(2, VM_USERLO)]) == mem_pi2phys(pi0));
	assert(PTE_ADDR(((pte_t *)PTE_ADDR(((pte_t *)PTE_ADDR(pmap_bootpmap[PDX(3, VM_USERLO)]))[PDX(2, VM_USERLO)]))[PDX(1, VM_USERLO)]) == mem_pi2phys(pi1));
	assert(mem_freelist == NULL);
	mem_free(pi2);
	assert(pmap_insert(pmap_bootpmap, pi4, va+PTSIZE, 0));
	assert(pmap_insert(pmap_bootpmap, pi4, va+PTSIZE+PAGESIZE, 0));
	assert(pmap_insert(pmap_bootpmap, pi4, va+PTSIZE*2-PAGESIZE, 0));
	assert(PTE_ADDR(((pte_t *)PTE_ADDR(((pte_t *)PTE_ADDR(pmap_bootpmap[PDX(3, VM_USERLO+PTSIZE)]))[PDX(2, VM_USERLO+PTSIZE)]))[PDX(1, VM_USERLO+PTSIZE)]) == mem_pi2phys(pi2));
	assert(mem_freelist == NULL);
	mem_free(pi3);
	assert(pmap_insert(pmap_bootpmap, pi4, va+PTSIZE*2, 0));
	assert(pmap_insert(pmap_bootpmap, pi4, va+PTSIZE*2+PAGESIZE, 0));
	assert(pmap_insert(pmap_bootpmap, pi4, va+PTSIZE*3-PAGESIZE*2, 0));
	assert(pmap_insert(pmap_bootpmap, pi4, va+PTSIZE*3-PAGESIZE, 0));
	assert(PTE_ADDR(((pte_t *)PTE_ADDR(((pte_t *)PTE_ADDR(pmap_bootpmap[PDX(3, VM_USERLO+PTSIZE*2)]))[PDX(2, VM_USERLO+PTSIZE*2)]))[PDX(1, VM_USERLO+PTSIZE*2)]) == mem_pi2phys(pi3));
	assert(mem_freelist == NULL);
	assert(pi0->refcount == 1);
	assert(pi1->refcount == 1);
	assert(pi2->refcount == 1);
	assert(pi3->refcount == 1);
	assert(pi4->refcount == 10);
	pmap_remove(pmap_bootpmap, va+PAGESIZE, PTSIZE*3-PAGESIZE*2);
	assert(pi4->refcount == 2);
	assert(pi2->refcount == 0);
	assert(mem_alloc() == pi2);
	assert(mem_freelist == NULL);
	pmap_remove(pmap_bootpmap, va, PTSIZE*3-PAGESIZE);
	assert(pi4->refcount == 1);
	assert(pi1->refcount == 0);
	assert(mem_alloc() == pi1);
	assert(mem_freelist == NULL);
	pmap_remove(pmap_bootpmap, va+PTSIZE*3-PAGESIZE, PAGESIZE);
	assert(pi4->refcount == 0);	// pi3 might or might not also be freed
	pmap_remove(pmap_bootpmap, va+PAGESIZE, PTSIZE*3);
	assert(pi3->refcount == 0);
	mem_alloc(); mem_alloc();	// collect pi4 and pi3
	assert(mem_freelist == NULL);
#if 0
	// check pointer arithmetic in pmap_walk
	mem_free(pi4);
	va = VM_USERLO + PTSIZE + PAGESIZE;
	ptep = pmap_walk(pmap_bootpmap, va, 1);
	ptep1 = mem_ptr(PTE_ADDR(((pte_t *)PTE_ADDR(((pte_t *)PTE_ADDR(pmap_bootpmap[PDX(3, va)]))[PDX(2, va)]))[PDX(1, va)]));
	cprintf("ptep %llx ptep1 %llx\n", ptep, ptep1);
	assert(ptep == ptep1 + PDX(1, va));
#endif
	pmap_remove(pmap_bootpmap, VM_USERLO, VM_USERHI - VM_USERLO);
	assert(mem_alloc() == pi0);

	// give free list back
	mem_freelist = fl;

	// free the pages we filched
	mem_free(pi0);
	mem_free(pi1);
	mem_free(pi2);
	mem_free(pi3);
	mem_free(pi4);

#if LAB >= 9
#else
	cprintf("pmap_check() succeeded!\n");
#endif
#if LAB >= 99
	// More things we should test:
	// - does trap() call pmap_fault() before recovery?
	// - does syscall_checkva avoid wraparound issues?
#endif
}

// test pmap_setperm, pmap_copy, pmap_merge, pmap_setperm
void
pmap_check_adv(void)
{
	extern pageinfo *mem_freelist;

	pageinfo *pi, *pi0, *pi1, *pi2, *pi3, *pi4;
	pageinfo *fl;
	pte_t *ptep, *ptep1;
	int i;

	// should be able to allocate three pages
	pi0 = pi1 = pi2 = pi3 = pi4 = 0;
	pi0 = mem_alloc();
	pi1 = mem_alloc();
	pi2 = mem_alloc();
	pi3 = mem_alloc();
	pi4 = mem_alloc();

	// temporarily steal the rest of the free pages
	fl = mem_freelist;
	mem_freelist = NULL;

	// free pi0, pi1 and try again: pi0 and pi1 should be used for page table
	mem_free(pi0);
	mem_free(pi1);
	assert(pmap_insert(pmap_bootpmap, pi4, VM_USERLO, 0) != NULL);

	// should be able to set no permission
	assert(pmap_setperm(pmap_bootpmap, VM_USERLO + PAGESIZE, PAGESIZE, 0) != 0);

	// should be able to set read permission
	assert(pmap_setperm(pmap_bootpmap, VM_USERLO + PAGESIZE, PAGESIZE, SYS_READ) != 0);
	assert(*(int *)(VM_USERLO + PAGESIZE) == 0);
	mem_free(pi2);
	assert(pmap_setperm(pmap_bootpmap, VM_USERLO + 2 * PAGESIZE, PTSIZE, SYS_READ) != 0);
	assert(*(int *)(VM_USERLO + PTSIZE) == 0);

	// should be able to set write permission
	assert(pmap_setperm(pmap_bootpmap, VM_USERLO + 2 * PAGESIZE, PTSIZE, SYS_READ|SYS_WRITE) != 0);
	assert(*(int *)(VM_USERLO + 2 * PAGESIZE) == 0);

	// revert original page tables
	pmap_remove(pmap_bootpmap, VM_USERLO + PTSIZE, PTSIZE);
	assert(mem_alloc() == pi2);
	assert(mem_alloc() == NULL);
	pmap_remove(pmap_bootpmap, VM_USERLO, PTSIZE);
	assert(mem_alloc() == pi0);
	assert(mem_alloc() == pi4);
	assert(mem_alloc() == NULL);
	pmap_remove(pmap_bootpmap, VM_USERLO, VM_USERHI - VM_USERLO);
	assert(mem_alloc() == pi1);
	assert(mem_alloc() == NULL);

	// give free list back
	mem_freelist = fl;

	// free the pages we filched
	mem_free(pi0);
	mem_free(pi1);
	mem_free(pi2);
	mem_free(pi3);
	mem_free(pi4);
}

static uint16_t
pmap_scan(pte_t *table, pte_t left, pte_t right, pte_t *start, pte_t *end, uint16_t mask)
{
	while (left < right && !(table[left] & PTE_P)) {
		left++;
	}
	if (left < right) {
		if (start != NULL) {
			*start = left;
		}
		uint16_t perm = table[left] & mask;
		while ((left < right) && ((table[left] & mask) == perm)) {
			left++;
		}
		if (end != NULL) {
			*end = left;
		}
		return perm;
	} else {
		return 0;
	}
}

static char *
pmap_perm_string(uint16_t perm)
{
	static char buf[10];
	buf[0] = '[';
	buf[1] = (perm & SYS_WRITE) ? 'W' : '-';
	buf[2] = (perm & SYS_READ) ? 'R' : '-';
	buf[3] = (perm & PTE_G) ? 'G' : '-';
	buf[4] = (perm & PTE_PS) ? 'S' : '-';
	buf[5] = (perm & PTE_U) ? 'u' : 's';
	buf[6] = (perm & PTE_W) ? 'w' : 'r';
	buf[7] = (perm & PTE_P) ? 'p' : '-';
	buf[8] = ']';
	buf[9] = 0;
	return buf;
}

void
pmap_print(pte_t *pml4)
{
	uintptr_t *pml4t, *pdpt, *pdt, *pt;
	// these addresses should be canonical
	pt = (uintptr_t *)((uintptr_t)PML4SELFOFFSET << PDSHIFT(3));
	pdt = (uintptr_t *)((uintptr_t)pt | (PDADDR(3, pt) >> NPTBITS));
	pdpt = (uintptr_t *)((uintptr_t)pdt | (PDADDR(2, pdt) >> NPTBITS));
	pml4t = (uintptr_t *)((uintptr_t)pdpt | (PDADDR(1, pdpt) >> NPTBITS));
	pt = (uintptr_t *)((uintptr_t)pt | CANONICALSIGNEXTENSION);
	pdt = (uintptr_t *)((uintptr_t)pdt | CANONICALSIGNEXTENSION);
	pdpt = (uintptr_t *)((uintptr_t)pdpt | CANONICALSIGNEXTENSION);
	pml4t = (uintptr_t *)((uintptr_t)pml4t | CANONICALSIGNEXTENSION);

	cprintf("PML4 %p\n", pml4);
	pte_t *cpml4 = rcr3();
	lcr3(mem_phys(pml4));
	uint16_t perm = 0;
	uint16_t mask = PTE_P | PTE_W | PTE_U | PTE_PS | PTE_G | PTE_AVAIL;
	int half = 0; // 0 for higher half, 1 for lower half
	uintptr_t i;
	for (half = 0; half < 2; half++) {
		pte_t pml4_start = (half ? (NPTENTRIES >> 1) : 0);
		pte_t pml4_end = (half ? (NPTENTRIES >> 1) : 0);
		uintptr_t ext = half * CANONICALSIGNEXTENSION;
		while ((perm = pmap_scan(pml4t, pml4_end, NPTENTRIES >> (1 - half), &pml4_start, &pml4_end, mask)) != 0) {
			cprintf("|-- PML4E(%03x) %016llx-%016llx %016llx                 %s", pml4_end - pml4_start, (pml4_start << PDSHIFT(3)) + ext, (pml4_end << PDSHIFT(3)) + ext, (pml4_end - pml4_start) << PDSHIFT(3), pmap_perm_string(perm & mask));
			for (i = pml4_start; i < pml4_end; i++)
				cprintf("\t%llx", pml4t[i]);
			cputs("\n");
			pte_t pdp_start = pml4_start << NPTBITS;
			pte_t pdp_end = pml4_start << NPTBITS;
			while ((perm = pmap_scan(pdpt, pdp_end, pml4_end << NPTBITS, &pdp_start, &pdp_end, mask)) != 0) {
				cprintf("    |-- PDPE(%05x) %016llx-%016llx %016llx            %s", pdp_end - pdp_start, (pdp_start << PDSHIFT(2)) + ext, (pdp_end << PDSHIFT(2)) + ext, (pdp_end - pdp_start) << PDSHIFT(2), pmap_perm_string(perm & mask));
				for (i = pdp_start; i < pdp_end; i++)
					cprintf("\t%llx", pdpt[i]);
				cputs("\n");
				pte_t pd_start = pdp_start << NPTBITS;
				pte_t pd_end = pdp_start << NPTBITS;
				while ((perm = pmap_scan(pdt, pd_end, pdp_end << NPTBITS, &pd_start, &pd_end, mask)) != 0) {
					cprintf("        |-- PDE(%07x) %016llx-%016llx %016llx       %s", pd_end - pd_start, (pd_start << PDSHIFT(1)) + ext, (pd_end << PDSHIFT(1)) + ext, (pd_end - pd_start) << PDSHIFT(1), pmap_perm_string(perm & mask));
					if (!(perm & PTE_PS)) {
						for (i = pd_start; i < pd_end; i++)
							cprintf("\t%llx", pdt[i]);
						cputs("\n");
						pte_t pt_start = pd_start << NPTBITS;
						pte_t pt_end = pd_start << NPTBITS;
						while ((perm = pmap_scan(pt, pt_end, pd_end << NPTBITS, &pt_start, &pt_end, mask)) != 0) {
							cprintf("            |-- PTE(%09x) %016llx-%016llx %016llx %s", pt_end - pt_start, (pt_start << PDSHIFT(0)) | ext, (pt_end << PDSHIFT(0)) + ext, (pt_end - pt_start) << PDSHIFT(0), pmap_perm_string(perm & mask));
							if (pt_end - pt_start < 0x40) {
								for (i = pt_start; i < pt_end; i++) {
									if (!((i - pt_start) & 0xf))
										cputs("\n");
									cprintf("\t%llx", pt[i]);
								}
							} else {
								cputs("\t...");
							}
							cputs("\n");
						}
					} else {
						if (pd_end - pd_start < 0x40) {
							for (i = pd_start; i < pd_end; i++) {
								if (!((i - pd_start) & 0xf))
									cputs("\n");
								cprintf("\t%llx", pdt[i]);
							}
						} else {
							cputs("\t...");
						}
						cputs("\n");
					}
				}
			}
		}
	}
	lcr3((uintptr_t)cpml4);
}

#endif /* LAB >= 3 */
