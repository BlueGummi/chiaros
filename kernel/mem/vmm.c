#include <console/printf.h>
#include <limine.h>
#include <mem/pmm.h>
#include <mem/slab.h>
#include <mem/vmm.h>
#include <misc/linker_symbols.h>
#include <spin_lock.h>
#include <stdint.h>
#include <string.h>

struct spinlock vmm_lock = SPINLOCK_INIT;
struct page_table *kernel_pml4 = NULL;
uintptr_t kernel_pml4_phys = 0;
static uint64_t hhdm_offset = 0;
uintptr_t vmm_map_top = VMM_MAP_BASE;

uint64_t sub_offset(uint64_t a) {
    return a - hhdm_offset;
}

void *vmm_map_region(uintptr_t virt_base, uint64_t size, uint64_t flags) {
    void *first = NULL;

    for (uintptr_t virt = virt_base; virt < virt_base + size;
         virt += PAGE_SIZE) {

        uintptr_t phys = (uintptr_t) pmm_alloc_page(false);
        if (virt == virt_base) {
            first = (void *) phys;
        }
        if (phys == (uintptr_t) -1) {
            k_panic("Error: Out of memory mapping region\n");
            return NULL;
        }

        vmm_map_page(virt, phys, flags);
    }

    return first;
}

void vmm_unmap_region(uintptr_t virt_base, uint64_t size) {
    for (uintptr_t virt = virt_base; virt < virt_base + size;
         virt += PAGE_SIZE) {
        vmm_unmap_page(virt);
    }
}

void vmm_init(struct limine_memmap_response *memmap,
              struct limine_executable_address_response *xa, uint64_t offset) {
    hhdm_offset = offset;
    kernel_pml4 = (struct page_table *) pmm_alloc_page(true);
    kernel_pml4_phys = (uintptr_t) kernel_pml4 - hhdm_offset;
    memset(kernel_pml4, 0, PAGE_SIZE);

    uint64_t kernel_phys_start = xa->physical_base;
    uint64_t kernel_virt_start = xa->virtual_base;
    uint64_t kernel_virt_end = (uint64_t) &__kernel_virt_end;
    uint64_t kernel_size = kernel_virt_end - kernel_virt_start;

    for (uint64_t i = 0; i < kernel_size; i += PAGE_SIZE) {
        vmm_map_page(kernel_virt_start + i, kernel_phys_start + i,
                     PAGING_WRITE | PAGING_PRESENT);
    }

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_BAD_MEMORY ||
            entry->type == LIMINE_MEMMAP_RESERVED ||
            entry->type == LIMINE_MEMMAP_ACPI_NVS) {
            continue;
        }

        uint64_t base = entry->base;
        uint64_t len = entry->length;
        uint64_t end = base + len;
        uint64_t flags = PAGING_PRESENT | PAGING_WRITE;

        if (entry->type == LIMINE_MEMMAP_FRAMEBUFFER) {
            flags |= PAGING_WRITETHROUGH;
        }

        uint64_t phys = base;
        while (phys < end) {
            uint64_t virt = phys + hhdm_offset;

            bool can_use_2mb = ((phys % PAGE_2MB) == 0) &&
                               ((virt % PAGE_2MB) == 0) &&
                               ((end - phys) >= PAGE_2MB);

            if (can_use_2mb) {
                vmm_map_2mb_page(virt, phys, flags);
                phys += PAGE_2MB;
            } else {
                vmm_map_page(virt, phys, flags);
                phys += PAGE_SIZE;
            }
        }
    }

    asm volatile("mov %0, %%cr3" : : "r"(kernel_pml4_phys) : "memory");
}

void vmm_map_2mb_page(uintptr_t virt, uintptr_t phys, uint64_t flags) {
    if (virt == 0 || (virt & 0x1FFFFF) || (phys & 0x1FFFFF)) {
        k_panic(
            "vmm_map_2mb_page: addresses must be 2MiB aligned and non-zero\n");
    }

    uint64_t L2 = (virt >> 21) & 0x1FF;
    uint64_t L3 = (virt >> 30) & 0x1FF;
    uint64_t L4 = (virt >> 39) & 0x1FF;

    struct page_table *current_table = kernel_pml4;

    pte_t *entry = &current_table->entries[L4];
    if (!(*entry & PAGING_PRESENT)) {
        struct page_table *new_table =
            (struct page_table *) pmm_alloc_page(true);
        uintptr_t new_table_phys = (uintptr_t) new_table - hhdm_offset;
        memset(new_table, 0, PAGE_SIZE);
        *entry = new_table_phys | PAGING_PRESENT | PAGING_WRITE;
    }
    current_table =
        (struct page_table *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);

    entry = &current_table->entries[L3];
    if (!(*entry & PAGING_PRESENT)) {
        struct page_table *new_table =
            (struct page_table *) pmm_alloc_page(true);
        uintptr_t new_table_phys = (uintptr_t) new_table - hhdm_offset;
        memset(new_table, 0, PAGE_SIZE);
        *entry = new_table_phys | PAGING_PRESENT | PAGING_WRITE;
    }
    current_table =
        (struct page_table *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);

    entry = &current_table->entries[L2];
    *entry =
        (phys & PAGING_PHYS_MASK) | flags | PAGING_PRESENT | PAGING_2MB_page;

    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

void vmm_map_page(uintptr_t virt, uintptr_t phys, uint64_t flags) {
    if (virt == 0) {
        k_panic("CANNOT MAP PAGE 0x0!!!\n");
    }

    uint64_t L1 = (virt >> 12) & 0x1FF;
    uint64_t L2 = (virt >> 21) & 0x1FF;
    uint64_t L3 = (virt >> 30) & 0x1FF;
    uint64_t L4 = (virt >> 39) & 0x1FF;

    struct page_table *current_table = kernel_pml4;

    pte_t *entry = &current_table->entries[L4];
    if (!(*entry & PAGING_PRESENT)) {

        struct page_table *new_table =
            (struct page_table *) pmm_alloc_page(true);
        uintptr_t new_table_phys = (uintptr_t) new_table - hhdm_offset;
        memset(new_table, 0, PAGE_SIZE);

        *entry = new_table_phys | PAGING_PRESENT | PAGING_WRITE;
    }
    current_table =
        (struct page_table *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);

    entry = &current_table->entries[L3];
    if (!(*entry & PAGING_PRESENT)) {

        struct page_table *new_table =
            (struct page_table *) pmm_alloc_page(true);
        uintptr_t new_table_phys = (uintptr_t) new_table - hhdm_offset;
        memset(new_table, 0, PAGE_SIZE);

        *entry = new_table_phys | PAGING_PRESENT | PAGING_WRITE;
    }
    current_table =
        (struct page_table *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);

    entry = &current_table->entries[L2];
    if (!(*entry & PAGING_PRESENT)) {

        struct page_table *new_table =
            (struct page_table *) pmm_alloc_page(true);
        uintptr_t new_table_phys = (uintptr_t) new_table - hhdm_offset;
        memset(new_table, 0, PAGE_SIZE);

        *entry = new_table_phys | PAGING_PRESENT | PAGING_WRITE;
    }
    current_table =
        (struct page_table *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);

    entry = &current_table->entries[L1];
    *entry = (phys & PAGING_PHYS_MASK) | flags | PAGING_PRESENT;

    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

void vmm_unmap_page(uintptr_t virt) {

    uint64_t L1 = (virt >> 12) & 0x1FF;
    uint64_t L2 = (virt >> 21) & 0x1FF;
    uint64_t L3 = (virt >> 30) & 0x1FF;
    uint64_t L4 = (virt >> 39) & 0x1FF;

    struct page_table *current_table = kernel_pml4;
    pte_t *entry = &current_table->entries[L4];
    if (!(*entry & PAGING_PRESENT))
        return;
    current_table =
        (struct page_table *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);

    entry = &current_table->entries[L3];
    if (!(*entry & PAGING_PRESENT))
        return;
    current_table =
        (struct page_table *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);

    entry = &current_table->entries[L2];
    if (!(*entry & PAGING_PRESENT))
        return;
    current_table =
        (struct page_table *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);

    entry = &current_table->entries[L1];
    *entry &= ~PAGING_PRESENT;
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

uintptr_t vmm_get_phys(uintptr_t virt) {

    uint64_t L1 = (virt >> 12) & 0x1FF;
    uint64_t L2 = (virt >> 21) & 0x1FF;
    uint64_t L3 = (virt >> 30) & 0x1FF;
    uint64_t L4 = (virt >> 39) & 0x1FF;

    struct page_table *current_table = kernel_pml4;

    pte_t *entry = &current_table->entries[L4];
    if (!(*entry & PAGING_PRESENT))
        return (uintptr_t) -1;

    current_table =
        (struct page_table *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);
    entry = &current_table->entries[L3];
    if (!(*entry & PAGING_PRESENT))
        return (uintptr_t) -1;

    current_table =
        (struct page_table *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);
    entry = &current_table->entries[L2];
    if (!(*entry & PAGING_PRESENT))
        return (uintptr_t) -1;

    current_table =
        (struct page_table *) ((*entry & PAGING_PHYS_MASK) + hhdm_offset);
    entry = &current_table->entries[L1];
    if (!(*entry & PAGING_PRESENT))
        return (uintptr_t) -1;

    return (*entry & PAGING_PHYS_MASK) + (virt & 0xFFF);
}

void *vmm_map_phys(uint64_t addr, uint64_t len) {

    uintptr_t phys_start = PAGE_ALIGN_DOWN(addr);
    uintptr_t offset = addr - phys_start;

    uint64_t total_len = len + offset;
    uint64_t total_pages = (total_len + PAGE_SIZE - 1) / PAGE_SIZE;

    if (vmm_map_top + total_pages * PAGE_SIZE > VMM_MAP_LIMIT) {
        return NULL;
    }

    uintptr_t virt_start = vmm_map_top;
    vmm_map_top += total_pages * PAGE_SIZE;

    for (uint64_t i = 0; i < total_pages; i++) {
        vmm_map_page(virt_start + i * PAGE_SIZE, phys_start + i * PAGE_SIZE,
                     PAGING_PRESENT | PAGING_WRITE);
    }

    return (void *) (virt_start + offset);
}

void vmm_unmap_virt(void *addr, uint64_t len) {
    uintptr_t virt_addr = (uintptr_t) addr;
    uintptr_t page_offset = virt_addr & (PAGE_SIZE - 1);
    uintptr_t aligned_virt = PAGE_ALIGN_DOWN(virt_addr);

    uint64_t total_len = len + page_offset;
    uint64_t total_pages = (total_len + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t i = 0; i < total_pages; i++) {
        vmm_unmap_page(aligned_virt + i * PAGE_SIZE);
    }
}
