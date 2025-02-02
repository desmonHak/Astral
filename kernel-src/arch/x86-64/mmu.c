#include <arch/mmu.h>
#include <limine.h>
#include <kernel/pmm.h>
#include <string.h>
#include <logging.h>
#include <kernel/interrupt.h>
#include <kernel/vmm.h>
#include <arch/cpu.h>
#include <arch/smp.h>

#define ADDRMASK (uint64_t)0x7ffffffffffff000
#define   PTMASK (uint64_t)0b111111111000000000000
#define   PDMASK (uint64_t)0b111111111000000000000000000000
#define PDPTMASK (uint64_t)0b111111111000000000000000000000000000000
#define PML4MASK (uint64_t)0b111111111000000000000000000000000000000000000000

#define INTERMEDIATE_FLAGS ARCH_MMU_FLAGS_WRITE | ARCH_MMU_FLAGS_READ | ARCH_MMU_FLAGS_USER

// returns hhdm address of next entry

static inline uint64_t* next(uint64_t entry) {
	if (entry == 0)
		return NULL;
	return MAKE_HHDM((entry & ADDRMASK));
}

#define DEPTH_PDPT 1
#define DEPTH_PD 2
#define DEPTH_PT 3

// returns pointer to the entry

static uint64_t *get_page(pagetableptr_t top, void *vaddr) {
	uint64_t *pml4 = MAKE_HHDM(top);
	uintptr_t addr = (uintptr_t)vaddr;
	uintptr_t ptoffset = (addr & PTMASK) >> 12;
	uintptr_t pdoffset = (addr & PDMASK) >> 21;
	uintptr_t pdptoffset = (addr & PDPTMASK) >> 30;
	uintptr_t pml4offset = (addr & PML4MASK) >> 39;

	uint64_t *pdpt = next(pml4[pml4offset]);
	if (pdpt == NULL)
		return NULL;

	uint64_t *pd = next(pdpt[pdptoffset]);
	if (pd == NULL)
		return NULL;
	
	uint64_t *pt = next(pd[pdoffset]);
	if (pt == NULL)
		return NULL;

	return pt + ptoffset;
}

// inserts an entry

static bool add_page(pagetableptr_t top, void *vaddr, uint64_t entry, int depth) {
	uint64_t *pml4 = MAKE_HHDM(top);
	uintptr_t addr = (uintptr_t)vaddr;
	uintptr_t ptoffset = (addr & PTMASK) >> 12;
	uintptr_t pdoffset = (addr & PDMASK) >> 21;
	uintptr_t pdptoffset = (addr & PDPTMASK) >> 30;
	uintptr_t pml4offset = (addr & PML4MASK) >> 39;

	if (depth == DEPTH_PDPT) {
		pml4[pml4offset] = entry;
		return true;
	}

	uint64_t *pdpt = next(pml4[pml4offset]);
	if (pdpt == NULL) {
		pdpt = pmm_allocpage(PMM_SECTION_DEFAULT);
		if (pdpt == NULL)
			return false;
		pml4[pml4offset] = (uint64_t)pdpt | INTERMEDIATE_FLAGS;
		pdpt = MAKE_HHDM(pdpt);
		memset(pdpt, 0, PAGE_SIZE);
	}
	
	if (depth == DEPTH_PD) {
		pdpt[pdptoffset] = entry;
		return true;
	}
	
	uint64_t *pd = next(pdpt[pdptoffset]);
	if (pd == NULL) {
		pd = pmm_allocpage(PMM_SECTION_DEFAULT);
		if (pd == NULL)
			return false;
		pdpt[pdptoffset] = (uint64_t)pd | INTERMEDIATE_FLAGS;
		pd = MAKE_HHDM(pd);
		memset(pd, 0, PAGE_SIZE);
	}
	
	if (depth == DEPTH_PT) {
		pd[pdoffset] = entry;
		return true;
	}
	
	uint64_t *pt = next(pd[pdoffset]);
	if (pt == NULL) {
		pt = pmm_allocpage(PMM_SECTION_DEFAULT);
		if (pt == NULL)
			return false;
		pd[pdoffset] = (uint64_t)pt | INTERMEDIATE_FLAGS;
		pt = MAKE_HHDM(pt);
		memset(pt, 0, PAGE_SIZE);
	}

	pt[ptoffset] = entry;

	return true;
}

static void destroy(uint64_t *table, int depth) {
	for (int i = 0; i < (depth == 3 ? 256 : 512); ++i) {
		void *addr = (void *)((table[i] & ADDRMASK));
		if (addr == NULL)
			continue;

		if (depth > 0)
			destroy(MAKE_HHDM(addr), depth - 1);

		pmm_release(addr);
	}
}

void arch_mmu_destroytable(pagetableptr_t table) {
	destroy(MAKE_HHDM(table), 3);
	pmm_release(table);
}

bool arch_mmu_map(pagetableptr_t table, void *paddr, void *vaddr, mmuflags_t flags) {
	uint64_t entry = ((uintptr_t)paddr & ADDRMASK) | flags;
	return add_page(table, vaddr, entry, 0);
}

void arch_mmu_unmap(pagetableptr_t table, void *vaddr) {
	uint64_t *entry = get_page(table, vaddr);
	if (entry == NULL)
		return;
	*entry = 0;
}

void arch_mmu_remap(pagetableptr_t table, void *paddr, void *vaddr, mmuflags_t flags) {
	uint64_t *entryptr = get_page(table, vaddr);
	if (entryptr == NULL)
		return;
	uintptr_t addr = paddr == NULL ? (*entryptr & ADDRMASK) : ((uintptr_t)paddr & ADDRMASK);
	*entryptr = addr | flags;
}

void *arch_mmu_getphysical(pagetableptr_t table, void *vaddr) {
	uint64_t *entry = get_page(table, vaddr);
	if (entry == NULL)
		return NULL;
	return (void *)(*entry & ADDRMASK);
}

bool arch_mmu_ispresent(pagetableptr_t table, void *vaddr) {
	uint64_t *entry = get_page(table, vaddr);
	return entry == NULL ? false : *entry;
}

bool arch_mmu_iswritable(pagetableptr_t table, void *vaddr) {
	uint64_t *entry = get_page(table, vaddr);
	return entry == NULL ? false : *entry & ARCH_MMU_FLAGS_WRITE;
}

#define ARCH_MMU_FLAGS_DIRTY (1 << 6)

bool arch_mmu_isdirty(pagetableptr_t table, void *vaddr) {
	uint64_t *entry = get_page(table, vaddr);
	return entry == NULL ? false : *entry & ARCH_MMU_FLAGS_DIRTY;
}

#define FLAGS_MASK (ARCH_MMU_FLAGS_WRITE | ARCH_MMU_FLAGS_READ | ARCH_MMU_FLAGS_NOEXEC | ARCH_MMU_FLAGS_USER)

bool arch_mmu_getflags(pagetableptr_t table, void *vaddr, mmuflags_t *mmuflagsp) {
	uint64_t *entry = get_page(table, vaddr);
	if (entry == NULL)
	       return false;

	*mmuflagsp = *entry & FLAGS_MASK;

	return true;
}

void arch_mmu_switch(pagetableptr_t table) {
	asm volatile("mov %%rax, %%cr3" : : "a"(table));
}

// hhdm pointer to template to be used for new mappings and smp bootup
static pagetableptr_t template;

pagetableptr_t arch_mmu_newtable() {
	pagetableptr_t table = pmm_allocpage(PMM_SECTION_DEFAULT);
	if (table == NULL)
		return NULL;
	memcpy(MAKE_HHDM(table), template, PAGE_SIZE);
	return table;
}

static void *shootdown_page;
static size_t shootdown_size;
static spinlock_t shootdown_lock;
static int shootdown_remaining = 0;

static inline void do_invalidate(void *page, size_t size) {
	// if a full reload was requested or we are doing a big release on 
	if (page == NULL || size >= 128 * PAGE_SIZE) {
		// TODO if global pages are ever supported, we should disable them in CR4, flush, and then reenable them
		asm volatile("mov %%cr3, %%rax; mov %%rax, %%cr3;" : : : "rax", "memory");
	} else {
		for (uintptr_t i = 0; i < size; i += PAGE_SIZE) {
			uintptr_t ptr = (uintptr_t)page + i;
			asm volatile ("invlpg (%%rax)" : : "a"(ptr) : "memory");
		}
	}
}

void arch_mmu_tlbipi(isr_t *isr, context_t *context) {
	do_invalidate(shootdown_page, shootdown_size);
	__atomic_sub_fetch(&shootdown_remaining, 1, __ATOMIC_SEQ_CST);
}

// if page == NULL, this will do a userspace shootdown that flushes the whole tlb
void arch_mmu_invalidate_range(void *page, size_t size) {
	__assert(((uintptr_t)page % PAGE_SIZE) == 0);
	thread_t *thread = current_thread();

	bool do_shootdown = // do shootdown if
		current_thread() // scheduler is up
		&& arch_smp_cpusawake >= 2 // and there are multiple cpus in the system
		&& (page >= KERNELSPACE_START // and its either in the kernel
		|| ((page == NULL || (page >= USERSPACE_START && page < USERSPACE_END)) // or in userspace...
			&& thread->proc && thread->proc->runningthreadcount > 1)); // in a process which has multiple threads running

	int old_ipl;
	if (do_shootdown) {
		old_ipl = interrupt_raiseipl(IPL_DPC);
		spinlock_acquire(&shootdown_lock);

		shootdown_page = page;
		shootdown_size = size;
		shootdown_remaining = arch_smp_cpusawake - 1;

		for (int i = 0; i < arch_smp_cpusawake; ++i) {
			if (smp_cpus[i] == current_cpu())
				continue;

			arch_smp_sendipi(smp_cpus[i], &smp_cpus[i]->isr[0xfe], ARCH_SMP_IPI_TARGET, false);
		}
	}

	do_invalidate(page, size);

	if (do_shootdown) {
		while (__atomic_load_n(&shootdown_remaining, __ATOMIC_SEQ_CST)) CPU_PAUSE();

		spinlock_release(&shootdown_lock);
		interrupt_loweripl(old_ipl);
	}
}

extern void *_text_start;
extern void *_data_start;
extern void *_rodata_start;
extern void *_text_end;
extern void *_data_end;
extern void *_rodata_end; 

static void *kerneladdr[6] = {
	&_text_start,
	&_text_end,
	&_data_start,
	&_data_end,
	&_rodata_start,
	&_rodata_end
};

static mmuflags_t kernelflags[3] = {
	ARCH_MMU_FLAGS_READ,
	ARCH_MMU_FLAGS_READ | ARCH_MMU_FLAGS_WRITE | ARCH_MMU_FLAGS_NOEXEC,
	ARCH_MMU_FLAGS_READ | ARCH_MMU_FLAGS_NOEXEC
};

extern volatile struct limine_memmap_request pmm_liminemap;

static volatile struct limine_kernel_address_request kaddrreq = {
	.id = LIMINE_KERNEL_ADDRESS_REQUEST,
	.revision = 0
};

#define ERROR_PRESENT 1
#define ERROR_WRITE   2
#define ERROR_FETCH   16

static void pfisr(isr_t *self, context_t *ctx) {
	thread_t *thread = current_thread();
	interrupt_set(true);
	int vmmerror = 0;
	if (ctx->error & ERROR_PRESENT)
		vmmerror |= VMM_ACTION_READ;
	
	if (ctx->error & ERROR_WRITE)
		vmmerror |= VMM_ACTION_WRITE;
	
	if (ctx->error & ERROR_FETCH)
		vmmerror |= VMM_ACTION_EXEC;

	if (vmm_pagefault((void *)ctx->cr2, ctx->cs != 8, vmmerror) == false) {
		if (thread && thread->usercopyctx) {
			memcpy(ctx, thread->usercopyctx, sizeof(context_t));
			thread->usercopyctx = NULL;
			CTX_RET(ctx) = EFAULT;
		} else if (ARCH_CONTEXT_ISUSER(ctx)) {
			signal_signalthread(thread, SIGSEGV, true);
		} else {
			_panic("Page Fault", ctx);
		}
	}
}

static void gpfisr(isr_t *self, context_t *ctx) {
	thread_t *thread = current_thread();
	if (thread->usercopyctx) {
		memcpy(ctx, thread->usercopyctx, sizeof(context_t));
		thread->usercopyctx = NULL;
		CTX_RET(ctx) = EFAULT;
	} else if (ARCH_CONTEXT_ISUSER(ctx)) {
		signal_signalthread(thread, SIGSEGV, true);
	} else {
		_panic("General Protection Fault", ctx);
	}
}

void arch_mmu_init() {
	template = pmm_allocpage(PMM_SECTION_DEFAULT);
	__assert(template);
	template = MAKE_HHDM(template);
	memset(template, 0, PAGE_SIZE);

	for (int i = 256; i < 512; ++i) {
		uint64_t *entry = pmm_allocpage(PMM_SECTION_DEFAULT);
		__assert(entry);
		memset(MAKE_HHDM(entry), 0, PAGE_SIZE);
		template[i] = (uint64_t)entry | INTERMEDIATE_FLAGS;
	}

	// TODO use 2mb pages
	// populate hhdm

	for (size_t i = 0; i < pmm_liminemap.response->entry_count; ++i) {
		struct limine_memmap_entry *e = pmm_liminemap.response->entries[i];
		if (e->type != LIMINE_MEMMAP_USABLE && e->type != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE && e->type != LIMINE_MEMMAP_KERNEL_AND_MODULES && e->type != LIMINE_MEMMAP_FRAMEBUFFER)
			continue;

		for (uint64_t i = 0; i < e->length; i += PAGE_SIZE) {
			uint64_t entry = ((e->base + i) & ADDRMASK) | ARCH_MMU_FLAGS_READ | ARCH_MMU_FLAGS_WRITE | ARCH_MMU_FLAGS_NOEXEC;
			__assert(add_page(FROM_HHDM(template), MAKE_HHDM((void *)(e->base + i)), entry, 0));
		}
	}

	__assert(kaddrreq.response);

	// populate kernel

	for (int i = 0; i < 3; ++i) {
		size_t len = (uintptr_t)kerneladdr[i*2+1] - (uintptr_t)kerneladdr[i*2];
		uintptr_t baseptr = (uintptr_t)kerneladdr[i*2];
		uintptr_t physicalbase = (uintptr_t)kerneladdr[i*2] - kaddrreq.response->virtual_base + kaddrreq.response->physical_base;

		for (uintptr_t off = 0; off < len; off += PAGE_SIZE) {
			uint64_t entry = ((physicalbase + off) & ADDRMASK) | kernelflags[i];
			__assert(add_page(FROM_HHDM(template), (void *)(baseptr + off), entry, 0));
		}
	}

	arch_mmu_apswitch();
}

void arch_mmu_apswitch() {
	arch_mmu_switch(FROM_HHDM(template));
	interrupt_register(13, gpfisr, NULL, IPL_IGNORE);
	interrupt_register(14, pfisr, NULL, IPL_IGNORE);
	interrupt_register(0xfe, arch_mmu_tlbipi, ARCH_EOI, IPL_IGNORE);
}
