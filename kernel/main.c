#include "requests.h"
#include <acpi/hpet.h>
#include <acpi/lapic.h>
#include <acpi/print.h>
#include <acpi/uacpi_interface.h>
#include <asm.h>
#include <boot/gdt.h>
#include <boot/smap.h>
#include <console/printf.h>
#include <devices/generic_disk.h>
#include <devices/registry.h>
#include <drivers/ahci.h>
#include <drivers/ata.h>
#include <drivers/nvme.h>
#include <flanterm/backends/fb.h>
#include <flanterm/flanterm.h>
#include <fs/detect.h>
#include <fs/ext2.h>
#include <fs/ext2_print.h>
#include <fs/fat.h>
#include <int/idt.h>
#include <limine.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/slab.h>
#include <mem/vmm.h>
#include <misc/cmdline.h>
#include <misc/dbg.h>
#include <misc/linker_symbols.h>
#include <misc/logo.h>
#include <mp/core.h>
#include <mp/mp.h>
#include <pci/pci.h>
#include <pit.h>
#include <rust.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <sleep.h>
#include <spin_lock.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <time/print.h>
#include <uacpi/event.h>
#include <uacpi/resources.h>
#include <uacpi/tables.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

struct scheduler global_sched = {0};
uint64_t a_rsdp = 0;
char *g_root_part = "";
struct vfs_node *g_root_node = NULL;
struct vfs_mount *g_mount_list_head; // TODO: migrate these globals

void k_main(void) {
    uint64_t c_cnt = mp_request.response->cpu_count;
    k_printf_init(framebuffer_request.response->framebuffers[0]);
    struct limine_hhdm_response *r = hhdm_request.response;
    k_printf("%s", OS_LOGO_SMALL);
    a_rsdp = rsdp_request.response->address;
    mp_wakeup_processors(mp_request.response);
    enable_smap_smep_umip();
    gdt_install();
    init_physical_allocator(r->offset, memmap_request);
    vmm_init(memmap_request.response, xa_request.response, r->offset);
    slab_init();
    lapic = vmm_map_phys(0xFEE00000UL, 4096);
    idt_alloc(c_cnt);
    cmdline_parse(cmdline_request.response->cmdline);
    idt_install(0);
    uacpi_init();

    registry_setup();
    registry_print_devices();

    hpet_init();
    asm volatile("sti");

    k_printf("done\n");
    sleep(8);
    k_printf("one second later\n");

    while (1) {
        asm("hlt");
    }

    scheduler_init(&global_sched, c_cnt);

    for (uint64_t i = 0; i < c_cnt; i++) {
        struct per_core_scheduler *s =
            kmalloc(sizeof(struct per_core_scheduler));
        scheduler_local_init(s, i);
        struct thread *t = thread_create(k_sch_main);
        scheduler_add_thread(&global_sched, t);
    }

    //    struct thread *t = thread_create(registry_print_devices);
    //    scheduler_add_thread(&global_sched, t);

    struct core *c = kmalloc(sizeof(struct core));
    c->state = IDLE;
    c->id = 0;
    wrmsr(MSR_GS_BASE, (uint64_t) c);
    scheduler_rebalance(&global_sched);
    lapic_init();
    mp_inform_of_cr3();

    asm volatile("sti");
    scheduler_start(local_schs[0]);
    while (1) {
        asm("hlt");
    }
}
