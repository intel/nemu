/*
 * QEMU PC System Emulator
 *
 * Copyright (c) 2003-2004 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/i386/pc.h"
#include "hw/char/serial.h"
#include "hw/i386/apic.h"
#include "hw/i386/topology.h"
#include "sysemu/cpus.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_bus.h"
#include "hw/nvram/fw_cfg.h"
#include "hw/smbios/smbios.h"
#include "hw/loader.h"
#include "elf.h"
#include "hw/timer/mc146818rtc.h"
#include "hw/timer/i8254.h"
#include "hw/input/i8042.h"
#include "hw/pci/msi.h"
#include "hw/sysbus.h"
#include "sysemu/sysemu.h"
#include "sysemu/numa.h"
#include "sysemu/kvm.h"
#include "sysemu/qtest.h"
#include "kvm_i386.h"
#include "exec/memory.h"
#include "exec/address-spaces.h"
#include "sysemu/arch_init.h"
#include "qemu/bitmap.h"
#include "qemu/config-file.h"
#include "qemu/error-report.h"
#include "qemu/option.h"
#include "hw/acpi/acpi.h"
#include "hw/acpi/cpu_hotplug.h"
#include "hw/boards.h"
#include "hw/pci/pci_host.h"
#include "acpi-build.h"
#include "hw/mem/pc-dimm.h"
#include "qapi/error.h"
#include "qapi/qapi-visit-common.h"
#include "qapi/visitor.h"
#include "qom/cpu.h"
#include "hw/nmi.h"
#include "hw/i386/intel_iommu.h"

/* debug PC/ISA interrupts */
//#define DEBUG_IRQ

#ifdef DEBUG_IRQ
#define DPRINTF(fmt, ...)                                       \
    do { printf("CPUIRQ: " fmt , ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...)
#endif

#define FW_CFG_ACPI_TABLES (FW_CFG_ARCH_LOCAL + 0)
#define FW_CFG_SMBIOS_ENTRIES (FW_CFG_ARCH_LOCAL + 1)
#define FW_CFG_IRQ0_OVERRIDE (FW_CFG_ARCH_LOCAL + 2)
#define FW_CFG_E820_TABLE (FW_CFG_ARCH_LOCAL + 3)

#define E820_NR_ENTRIES		16

struct e820_entry {
    uint64_t address;
    uint64_t length;
    uint32_t type;
} QEMU_PACKED __attribute((__aligned__(4)));

struct e820_table {
    uint32_t count;
    struct e820_entry entry[E820_NR_ENTRIES];
} QEMU_PACKED __attribute((__aligned__(4)));

static struct e820_table e820_reserve;
static struct e820_entry *e820_table;
static unsigned e820_entries;

void gsi_handler(void *opaque, int n, int level)
{
    GSIState *s = opaque;

    DPRINTF("pc: %s GSI %d\n", level ? "raising" : "lowering", n);
    if (n < ISA_NUM_IRQS) {
        qemu_set_irq(s->i8259_irq[n], level);
    }
    qemu_set_irq(s->ioapic_irq[n], level);
}

static void ioport80_write(void *opaque, hwaddr addr, uint64_t data,
                           unsigned size)
{
}

static uint64_t ioport80_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0xffffffffffffffffULL;
}

/* MSDOS compatibility mode FPU exception support */
static qemu_irq ferr_irq;

void pc_register_ferr_irq(qemu_irq irq)
{
    ferr_irq = irq;
}

static void ioportF0_write(void *opaque, hwaddr addr, uint64_t data,
                           unsigned size)
{
    qemu_irq_lower(ferr_irq);
}

static uint64_t ioportF0_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0xffffffffffffffffULL;
}

/* IRQ handling */
int cpu_get_pic_interrupt(CPUX86State *env)
{
    X86CPU *cpu = x86_env_get_cpu(env);
    int intno;

    if (!kvm_irqchip_in_kernel()) {
        intno = apic_get_interrupt(cpu->apic_state);
        if (intno >= 0) {
            return intno;
        }
        /* read the irq from the PIC */
        if (!apic_accept_pic_intr(cpu->apic_state)) {
            return -1;
        }
    }

    intno = pic_read_irq(isa_pic);
    return intno;
}

static void pic_irq_request(void *opaque, int irq, int level)
{
    CPUState *cs = first_cpu;
    X86CPU *cpu = X86_CPU(cs);

    DPRINTF("pic_irqs: %s irq %d\n", level? "raise" : "lower", irq);
    if (cpu->apic_state && !kvm_irqchip_in_kernel()) {
        CPU_FOREACH(cs) {
            cpu = X86_CPU(cs);
            if (apic_accept_pic_intr(cpu->apic_state)) {
                apic_deliver_pic_intr(cpu->apic_state, level);
            }
        }
    } else {
        if (level) {
            cpu_interrupt(cs, CPU_INTERRUPT_HARD);
        } else {
            cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
        }
    }
}

/* PC cmos mappings */
#define REG_EQUIPMENT_BYTE          0x14


/* convert boot_device letter to something recognizable by the bios */
static int boot_device2nibble(char boot_device)
{
    switch(boot_device) {
    case 'a':
    case 'c':
        return 0x02; /* hard drive boot */
    case 'd':
        return 0x03; /* CD-ROM boot */
    case 'n':
        return 0x04; /* Network boot */
    }
    return 0;
}

static void set_boot_dev(ISADevice *s, const char *boot_device, Error **errp)
{
#define PC_MAX_BOOT_DEVICES 3
    int nbds, bds[3] = { 0, };
    int i;

    nbds = strlen(boot_device);
    if (nbds > PC_MAX_BOOT_DEVICES) {
        error_setg(errp, "Too many boot devices for PC");
        return;
    }
    for (i = 0; i < nbds; i++) {
        bds[i] = boot_device2nibble(boot_device[i]);
        if (bds[i] == 0) {
            error_setg(errp, "Invalid boot device for PC: '%c'",
                       boot_device[i]);
            return;
        }
    }
    rtc_set_memory(s, 0x3d, (bds[1] << 4) | bds[0]);
    rtc_set_memory(s, 0x38, (bds[2] << 4) | (fd_bootchk ? 0x0 : 0x1));
}

static void pc_boot_set(void *opaque, const char *boot_device, Error **errp)
{
    set_boot_dev(opaque, boot_device, errp);
}

void pc_cmos_init(PCMachineState *pcms,
                  ISADevice *s)
{
    int val;

    /* various important CMOS locations needed by PC/Bochs bios */

    /* memory size */
    /* base memory (first MiB) */
    val = MIN(pcms->below_4g_mem_size / 1024, 640);
    rtc_set_memory(s, 0x15, val);
    rtc_set_memory(s, 0x16, val >> 8);
    /* extended memory (next 64MiB) */
    if (pcms->below_4g_mem_size > 1024 * 1024) {
        val = (pcms->below_4g_mem_size - 1024 * 1024) / 1024;
    } else {
        val = 0;
    }
    if (val > 65535)
        val = 65535;
    rtc_set_memory(s, 0x17, val);
    rtc_set_memory(s, 0x18, val >> 8);
    rtc_set_memory(s, 0x30, val);
    rtc_set_memory(s, 0x31, val >> 8);
    /* memory between 16MiB and 4GiB */
    if (pcms->below_4g_mem_size > 16 * 1024 * 1024) {
        val = (pcms->below_4g_mem_size - 16 * 1024 * 1024) / 65536;
    } else {
        val = 0;
    }
    if (val > 65535)
        val = 65535;
    rtc_set_memory(s, 0x34, val);
    rtc_set_memory(s, 0x35, val >> 8);
    /* memory above 4GiB */
    val = pcms->above_4g_mem_size / 65536;
    rtc_set_memory(s, 0x5b, val);
    rtc_set_memory(s, 0x5c, val >> 8);
    rtc_set_memory(s, 0x5d, val >> 16);

    object_property_add_link(OBJECT(pcms), "rtc_state",
                             TYPE_ISA_DEVICE,
                             (Object **)&pcms->rtc,
                             object_property_allow_set_link,
                             OBJ_PROP_LINK_UNREF_ON_RELEASE, &error_abort);
    object_property_set_link(OBJECT(pcms), OBJECT(s),
                             "rtc_state", &error_abort);

    set_boot_dev(s, MACHINE(pcms)->boot_order, &error_fatal);

    val = 0;
    val |= 0x02; /* FPU is there */
    val |= 0x04; /* PS/2 mouse installed */
    rtc_set_memory(s, REG_EQUIPMENT_BYTE, val);
}

int e820_add_entry(uint64_t address, uint64_t length, uint32_t type)
{
    int index = le32_to_cpu(e820_reserve.count);
    struct e820_entry *entry;

    if (type != E820_RAM) {
        /* old FW_CFG_E820_TABLE entry -- reservations only */
        if (index >= E820_NR_ENTRIES) {
            return -EBUSY;
        }
        entry = &e820_reserve.entry[index++];

        entry->address = cpu_to_le64(address);
        entry->length = cpu_to_le64(length);
        entry->type = cpu_to_le32(type);

        e820_reserve.count = cpu_to_le32(index);
    }

    /* new "etc/e820" file -- include ram too */
    e820_table = g_renew(struct e820_entry, e820_table, e820_entries + 1);
    e820_table[e820_entries].address = cpu_to_le64(address);
    e820_table[e820_entries].length = cpu_to_le64(length);
    e820_table[e820_entries].type = cpu_to_le32(type);
    e820_entries++;

    return e820_entries;
}

int e820_get_num_entries(void)
{
    return e820_entries;
}

bool e820_get_entry(int idx, uint32_t type, uint64_t *address, uint64_t *length)
{
    if (idx < e820_entries && e820_table[idx].type == cpu_to_le32(type)) {
        *address = le64_to_cpu(e820_table[idx].address);
        *length = le64_to_cpu(e820_table[idx].length);
        return true;
    }
    return false;
}

/* Enables contiguous-apic-ID mode, for compatibility */
static bool compat_apic_id_mode;

/* Calculates initial APIC ID for a specific CPU index
 *
 * Currently we need to be able to calculate the APIC ID from the CPU index
 * alone (without requiring a CPU object), as the QEMU<->Seabios interfaces have
 * no concept of "CPU index", and the NUMA tables on fw_cfg need the APIC ID of
 * all CPUs up to max_cpus.
 */
static uint32_t x86_cpu_apic_id_from_index(unsigned int cpu_index)
{
    uint32_t correct_id;
    static bool warned;

    correct_id = x86_apicid_from_cpu_idx(smp_cores, smp_threads, cpu_index);
    if (compat_apic_id_mode) {
        if (cpu_index != correct_id && !warned && !qtest_enabled()) {
            error_report("APIC IDs set in compatibility mode, "
                         "CPU topology won't match the configuration");
            warned = true;
        }
        return cpu_index;
    } else {
        return correct_id;
    }
}

static void pc_build_smbios(PCMachineState *pcms)
{
    uint8_t *smbios_tables, *smbios_anchor;
    size_t smbios_tables_len, smbios_anchor_len;
    struct smbios_phys_mem_area *mem_array;
    unsigned i, array_count;
    MachineState *ms = MACHINE(pcms);
    X86CPU *cpu = X86_CPU(ms->possible_cpus->cpus[0].cpu);

    /* tell smbios about cpuid version and features */
    smbios_set_cpuid(cpu->env.cpuid_version, cpu->env.features[FEAT_1_EDX]);

    smbios_tables = smbios_get_table_legacy(&smbios_tables_len);
    if (smbios_tables) {
        fw_cfg_add_bytes(pcms->fw_cfg, FW_CFG_SMBIOS_ENTRIES,
                         smbios_tables, smbios_tables_len);
    }

    /* build the array of physical mem area from e820 table */
    mem_array = g_malloc0(sizeof(*mem_array) * e820_get_num_entries());
    for (i = 0, array_count = 0; i < e820_get_num_entries(); i++) {
        uint64_t addr, len;

        if (e820_get_entry(i, E820_RAM, &addr, &len)) {
            mem_array[array_count].address = addr;
            mem_array[array_count].length = len;
            array_count++;
        }
    }
    smbios_get_tables(mem_array, array_count,
                      &smbios_tables, &smbios_tables_len,
                      &smbios_anchor, &smbios_anchor_len);
    g_free(mem_array);

    if (smbios_anchor) {
        fw_cfg_add_file(pcms->fw_cfg, "etc/smbios/smbios-tables",
                        smbios_tables, smbios_tables_len);
        fw_cfg_add_file(pcms->fw_cfg, "etc/smbios/smbios-anchor",
                        smbios_anchor, smbios_anchor_len);
    }
}

static FWCfgState *bochs_bios_init(AddressSpace *as, PCMachineState *pcms)
{
    FWCfgState *fw_cfg;
    uint64_t *numa_fw_cfg;
    int i;
    const CPUArchIdList *cpus;
    MachineClass *mc = MACHINE_GET_CLASS(pcms);

    fw_cfg = fw_cfg_init_io_dma(FW_CFG_IO_BASE, FW_CFG_IO_BASE + 4, as);
    fw_cfg_add_i16(fw_cfg, FW_CFG_NB_CPUS, pcms->boot_cpus);

    /* FW_CFG_MAX_CPUS is a bit confusing/problematic on x86:
     *
     * For machine types prior to 1.8, SeaBIOS needs FW_CFG_MAX_CPUS for
     * building MPTable, ACPI MADT, ACPI CPU hotplug and ACPI SRAT table,
     * that tables are based on xAPIC ID and QEMU<->SeaBIOS interface
     * for CPU hotplug also uses APIC ID and not "CPU index".
     * This means that FW_CFG_MAX_CPUS is not the "maximum number of CPUs",
     * but the "limit to the APIC ID values SeaBIOS may see".
     *
     * So for compatibility reasons with old BIOSes we are stuck with
     * "etc/max-cpus" actually being apic_id_limit
     */
    fw_cfg_add_i16(fw_cfg, FW_CFG_MAX_CPUS, (uint16_t)pcms->apic_id_limit);
    fw_cfg_add_i64(fw_cfg, FW_CFG_RAM_SIZE, (uint64_t)ram_size);
    fw_cfg_add_bytes(fw_cfg, FW_CFG_ACPI_TABLES,
                     acpi_tables, acpi_tables_len);
    fw_cfg_add_i32(fw_cfg, FW_CFG_IRQ0_OVERRIDE, kvm_allows_irq0_override());

    fw_cfg_add_bytes(fw_cfg, FW_CFG_E820_TABLE,
                     &e820_reserve, sizeof(e820_reserve));
    fw_cfg_add_file(fw_cfg, "etc/e820", e820_table,
                    sizeof(struct e820_entry) * e820_entries);

    /* allocate memory for the NUMA channel: one (64bit) word for the number
     * of nodes, one word for each VCPU->node and one word for each node to
     * hold the amount of memory.
     */
    numa_fw_cfg = g_new0(uint64_t, 1 + pcms->apic_id_limit + nb_numa_nodes);
    numa_fw_cfg[0] = cpu_to_le64(nb_numa_nodes);
    cpus = mc->possible_cpu_arch_ids(MACHINE(pcms));
    for (i = 0; i < cpus->len; i++) {
        unsigned int apic_id = cpus->cpus[i].arch_id;
        assert(apic_id < pcms->apic_id_limit);
        numa_fw_cfg[apic_id + 1] = cpu_to_le64(cpus->cpus[i].props.node_id);
    }
    for (i = 0; i < nb_numa_nodes; i++) {
        numa_fw_cfg[pcms->apic_id_limit + 1 + i] =
            cpu_to_le64(numa_info[i].node_mem);
    }
    fw_cfg_add_bytes(fw_cfg, FW_CFG_NUMA, numa_fw_cfg,
                     (1 + pcms->apic_id_limit + nb_numa_nodes) *
                     sizeof(*numa_fw_cfg));

    return fw_cfg;
}

static long get_file_size(FILE *f)
{
    long where, size;

    /* XXX: on Unix systems, using fstat() probably makes more sense */

    where = ftell(f);
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, where, SEEK_SET);

    return size;
}

/* setup_data types */
#define SETUP_NONE     0
#define SETUP_E820_EXT 1
#define SETUP_DTB      2
#define SETUP_PCI      3
#define SETUP_EFI      4

struct setup_data {
    uint64_t next;
    uint32_t type;
    uint32_t len;
    uint8_t data[0];
} __attribute__((packed));

static void load_linux(PCMachineState *pcms,
                       FWCfgState *fw_cfg)
{
    uint16_t protocol=0;
    int setup_size, kernel_size, initrd_size = 0, cmdline_size;
    int dtb_size, setup_data_offset;
    uint32_t initrd_max;
    uint8_t header[8192], *setup, *kernel, *initrd_data;
    hwaddr real_addr, prot_addr, cmdline_addr, initrd_addr = 0;
    FILE *f;
    char *vmode;
    MachineState *machine = MACHINE(pcms);
    PCMachineClass *pcmc = PC_MACHINE_GET_CLASS(pcms);
    struct setup_data *setup_data;
    const char *kernel_filename = machine->kernel_filename;
    const char *initrd_filename = machine->initrd_filename;
    const char *dtb_filename = machine->dtb;
    const char *kernel_cmdline = machine->kernel_cmdline;

    /* Align to 16 bytes as a paranoia measure */
    cmdline_size = (strlen(kernel_cmdline)+16) & ~15;

    /* load the kernel header */
    f = fopen(kernel_filename, "rb");
    if (!f || !(kernel_size = get_file_size(f)) ||
        fread(header, 1, MIN(ARRAY_SIZE(header), kernel_size), f) !=
        MIN(ARRAY_SIZE(header), kernel_size)) {
        fprintf(stderr, "qemu: could not load kernel '%s': %s\n",
                kernel_filename, strerror(errno));
        exit(1);
    }

    /* kernel protocol version */
#if 0
    fprintf(stderr, "header magic: %#x\n", ldl_p(header+0x202));
#endif
    if (ldl_p(header+0x202) == 0x53726448) {
        protocol = lduw_p(header+0x206);
    }

    if (protocol < 0x200 || !(header[0x211] & 0x01)) {
        /* Low kernel */
        real_addr    = 0x90000;
        cmdline_addr = 0x9a000 - cmdline_size;
        prot_addr    = 0x10000;
    } else if (protocol < 0x202) {
        /* High but ancient kernel */
        real_addr    = 0x90000;
        cmdline_addr = 0x9a000 - cmdline_size;
        prot_addr    = 0x100000;
    } else {
        /* High and recent kernel */
        real_addr    = 0x10000;
        cmdline_addr = 0x20000;
        prot_addr    = 0x100000;
    }

#if 0
    fprintf(stderr,
            "qemu: real_addr     = 0x" TARGET_FMT_plx "\n"
            "qemu: cmdline_addr  = 0x" TARGET_FMT_plx "\n"
            "qemu: prot_addr     = 0x" TARGET_FMT_plx "\n",
            real_addr,
            cmdline_addr,
            prot_addr);
#endif

    /* highest address for loading the initrd */
    if (protocol >= 0x203) {
        initrd_max = ldl_p(header+0x22c);
    } else {
        initrd_max = 0x37ffffff;
    }

    if (initrd_max >= pcms->below_4g_mem_size - pcmc->acpi_data_size) {
        initrd_max = pcms->below_4g_mem_size - pcmc->acpi_data_size - 1;
    }

    fw_cfg_add_i32(fw_cfg, FW_CFG_CMDLINE_ADDR, cmdline_addr);
    fw_cfg_add_i32(fw_cfg, FW_CFG_CMDLINE_SIZE, strlen(kernel_cmdline)+1);
    fw_cfg_add_string(fw_cfg, FW_CFG_CMDLINE_DATA, kernel_cmdline);

    if (protocol >= 0x202) {
        stl_p(header+0x228, cmdline_addr);
    } else {
        stw_p(header+0x20, 0xA33F);
        stw_p(header+0x22, cmdline_addr-real_addr);
    }

    /* handle vga= parameter */
    vmode = strstr(kernel_cmdline, "vga=");
    if (vmode) {
        unsigned int video_mode;
        /* skip "vga=" */
        vmode += 4;
        if (!strncmp(vmode, "normal", 6)) {
            video_mode = 0xffff;
        } else if (!strncmp(vmode, "ext", 3)) {
            video_mode = 0xfffe;
        } else if (!strncmp(vmode, "ask", 3)) {
            video_mode = 0xfffd;
        } else {
            video_mode = strtol(vmode, NULL, 0);
        }
        stw_p(header+0x1fa, video_mode);
    }

    /* loader type */
    /* High nybble = B reserved for QEMU; low nybble is revision number.
       If this code is substantially changed, you may want to consider
       incrementing the revision. */
    if (protocol >= 0x200) {
        header[0x210] = 0xB0;
    }
    /* heap */
    if (protocol >= 0x201) {
        header[0x211] |= 0x80;	/* CAN_USE_HEAP */
        stw_p(header+0x224, cmdline_addr-real_addr-0x200);
    }

    /* load initrd */
    if (initrd_filename) {
        if (protocol < 0x200) {
            fprintf(stderr, "qemu: linux kernel too old to load a ram disk\n");
            exit(1);
        }

        initrd_size = get_image_size(initrd_filename);
        if (initrd_size < 0) {
            fprintf(stderr, "qemu: error reading initrd %s: %s\n",
                    initrd_filename, strerror(errno));
            exit(1);
        }

        initrd_addr = (initrd_max-initrd_size) & ~4095;

        initrd_data = g_malloc(initrd_size);
        load_image(initrd_filename, initrd_data);

        fw_cfg_add_i32(fw_cfg, FW_CFG_INITRD_ADDR, initrd_addr);
        fw_cfg_add_i32(fw_cfg, FW_CFG_INITRD_SIZE, initrd_size);
        fw_cfg_add_bytes(fw_cfg, FW_CFG_INITRD_DATA, initrd_data, initrd_size);

        stl_p(header+0x218, initrd_addr);
        stl_p(header+0x21c, initrd_size);
    }

    /* load kernel and setup */
    setup_size = header[0x1f1];
    if (setup_size == 0) {
        setup_size = 4;
    }
    setup_size = (setup_size+1)*512;
    if (setup_size > kernel_size) {
        fprintf(stderr, "qemu: invalid kernel header\n");
        exit(1);
    }
    kernel_size -= setup_size;

    setup  = g_malloc(setup_size);
    kernel = g_malloc(kernel_size);
    fseek(f, 0, SEEK_SET);
    if (fread(setup, 1, setup_size, f) != setup_size) {
        fprintf(stderr, "fread() failed\n");
        exit(1);
    }
    if (fread(kernel, 1, kernel_size, f) != kernel_size) {
        fprintf(stderr, "fread() failed\n");
        exit(1);
    }
    fclose(f);

    /* append dtb to kernel */
    if (dtb_filename) {
        if (protocol < 0x209) {
            fprintf(stderr, "qemu: Linux kernel too old to load a dtb\n");
            exit(1);
        }

        dtb_size = get_image_size(dtb_filename);
        if (dtb_size <= 0) {
            fprintf(stderr, "qemu: error reading dtb %s: %s\n",
                    dtb_filename, strerror(errno));
            exit(1);
        }

        setup_data_offset = QEMU_ALIGN_UP(kernel_size, 16);
        kernel_size = setup_data_offset + sizeof(struct setup_data) + dtb_size;
        kernel = g_realloc(kernel, kernel_size);

        stq_p(header+0x250, prot_addr + setup_data_offset);

        setup_data = (struct setup_data *)(kernel + setup_data_offset);
        setup_data->next = 0;
        setup_data->type = cpu_to_le32(SETUP_DTB);
        setup_data->len = cpu_to_le32(dtb_size);

        load_image_size(dtb_filename, setup_data->data, dtb_size);
    }

    memcpy(setup, header, MIN(sizeof(header), setup_size));

    fw_cfg_add_i32(fw_cfg, FW_CFG_KERNEL_ADDR, prot_addr);
    fw_cfg_add_i32(fw_cfg, FW_CFG_KERNEL_SIZE, kernel_size);
    fw_cfg_add_bytes(fw_cfg, FW_CFG_KERNEL_DATA, kernel, kernel_size);

    fw_cfg_add_i32(fw_cfg, FW_CFG_SETUP_ADDR, real_addr);
    fw_cfg_add_i32(fw_cfg, FW_CFG_SETUP_SIZE, setup_size);
    fw_cfg_add_bytes(fw_cfg, FW_CFG_SETUP_DATA, setup, setup_size);

    option_rom[nb_option_roms].bootindex = 0;
    option_rom[nb_option_roms].name = "linuxboot.bin";
    if (pcmc->linuxboot_dma_enabled && fw_cfg_dma_enabled(fw_cfg)) {
        option_rom[nb_option_roms].name = "linuxboot_dma.bin";
    }
    nb_option_roms++;
}

DeviceState *cpu_get_current_apic(void)
{
    if (current_cpu) {
        X86CPU *cpu = X86_CPU(current_cpu);
        return cpu->apic_state;
    } else {
        return NULL;
    }
}

static void pc_new_cpu(const char *typename, int64_t apic_id, Error **errp)
{
    Object *cpu = NULL;
    Error *local_err = NULL;

    cpu = object_new(typename);

    object_property_set_uint(cpu, apic_id, "apic-id", &local_err);
    object_property_set_bool(cpu, true, "realized", &local_err);

    object_unref(cpu);
    error_propagate(errp, local_err);
}

void pc_hot_add_cpu(const int64_t id, Error **errp)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    int64_t apic_id = x86_cpu_apic_id_from_index(id);
    Error *local_err = NULL;

    if (id < 0) {
        error_setg(errp, "Invalid CPU id: %" PRIi64, id);
        return;
    }

    if (apic_id >= ACPI_CPU_HOTPLUG_ID_LIMIT) {
        error_setg(errp, "Unable to add CPU: %" PRIi64
                   ", resulting APIC ID (%" PRIi64 ") is too large",
                   id, apic_id);
        return;
    }

    pc_new_cpu(ms->cpu_type, apic_id, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }
}

void pc_cpus_init(PCMachineState *pcms)
{
    int i;
    const CPUArchIdList *possible_cpus;
    MachineState *ms = MACHINE(pcms);
    MachineClass *mc = MACHINE_GET_CLASS(pcms);

    /* Calculates the limit to CPU APIC ID values
     *
     * Limit for the APIC ID value, so that all
     * CPU APIC IDs are < pcms->apic_id_limit.
     *
     * This is used for FW_CFG_MAX_CPUS. See comments on bochs_bios_init().
     */
    pcms->apic_id_limit = x86_cpu_apic_id_from_index(max_cpus - 1) + 1;
    possible_cpus = mc->possible_cpu_arch_ids(ms);
    for (i = 0; i < smp_cpus; i++) {
        pc_new_cpu(possible_cpus->cpus[i].type, possible_cpus->cpus[i].arch_id,
                   &error_fatal);
    }
}

static void pc_build_feature_control_file(PCMachineState *pcms)
{
    MachineState *ms = MACHINE(pcms);
    X86CPU *cpu = X86_CPU(ms->possible_cpus->cpus[0].cpu);
    CPUX86State *env = &cpu->env;
    uint32_t unused, ecx, edx;
    uint64_t feature_control_bits = 0;
    uint64_t *val;

    cpu_x86_cpuid(env, 1, 0, &unused, &unused, &ecx, &edx);
    if (ecx & CPUID_EXT_VMX) {
        feature_control_bits |= FEATURE_CONTROL_VMXON_ENABLED_OUTSIDE_SMX;
    }

    if ((edx & (CPUID_EXT2_MCE | CPUID_EXT2_MCA)) ==
        (CPUID_EXT2_MCE | CPUID_EXT2_MCA) &&
        (env->mcg_cap & MCG_LMCE_P)) {
        feature_control_bits |= FEATURE_CONTROL_LMCE;
    }

    if (!feature_control_bits) {
        return;
    }

    val = g_malloc(sizeof(*val));
    *val = cpu_to_le64(feature_control_bits | FEATURE_CONTROL_LOCKED);
    fw_cfg_add_file(pcms->fw_cfg, "etc/msr_feature_control", val, sizeof(*val));
}

static void rtc_set_cpus_count(ISADevice *rtc, uint16_t cpus_count)
{
    if (cpus_count > 0xff) {
        /* If the number of CPUs can't be represented in 8 bits, the
         * BIOS must use "FW_CFG_NB_CPUS". Set RTC field to 0 just
         * to make old BIOSes fail more predictably.
         */
        rtc_set_memory(rtc, 0x5f, 0);
    } else {
        rtc_set_memory(rtc, 0x5f, cpus_count - 1);
    }
}

static
void pc_machine_done(Notifier *notifier, void *data)
{
    PCMachineState *pcms = container_of(notifier,
                                        PCMachineState, machine_done);
    PCIBus *bus = pcms->bus;

    /* set the number of CPUs */
    rtc_set_cpus_count(pcms->rtc, pcms->boot_cpus);

    if (bus) {
        int extra_hosts = 0;

        QLIST_FOREACH(bus, &bus->child, sibling) {
            /* look for expander root buses */
            if (pci_bus_is_root(bus)) {
                extra_hosts++;
            }
        }
        if (extra_hosts && pcms->fw_cfg) {
            uint64_t *val = g_malloc(sizeof(*val));
            *val = cpu_to_le64(extra_hosts);
            fw_cfg_add_file(pcms->fw_cfg,
                    "etc/extra-pci-roots", val, sizeof(*val));
        }
    }

    acpi_setup();
    if (pcms->fw_cfg) {
        pc_build_smbios(pcms);
        pc_build_feature_control_file(pcms);
        /* update FW_CFG_NB_CPUS to account for -device added CPUs */
        fw_cfg_modify_i16(pcms->fw_cfg, FW_CFG_NB_CPUS, pcms->boot_cpus);
    }

    if (pcms->apic_id_limit > 255) {
        IntelIOMMUState *iommu = INTEL_IOMMU_DEVICE(x86_iommu_get_default());

        if (!iommu || !iommu->x86_iommu.intr_supported ||
            iommu->intr_eim != ON_OFF_AUTO_ON) {
            error_report("current -smp configuration requires "
                         "Extended Interrupt Mode enabled. "
                         "You can add an IOMMU using: "
                         "-device intel-iommu,intremap=on,eim=on");
            exit(EXIT_FAILURE);
        }
    }
}

void pc_guest_info_init(PCMachineState *pcms)
{
    int i;

    pcms->apic_xrupt_override = kvm_allows_irq0_override();
    pcms->numa_nodes = nb_numa_nodes;
    pcms->node_mem = g_malloc0(pcms->numa_nodes *
                                    sizeof *pcms->node_mem);
    for (i = 0; i < nb_numa_nodes; i++) {
        pcms->node_mem[i] = numa_info[i].node_mem;
    }

    pcms->machine_done.notify = pc_machine_done;
    qemu_add_machine_init_done_notifier(&pcms->machine_done);
}

/* setup pci memory address space mapping into system address space */
void pc_pci_as_mapping_init(Object *owner, MemoryRegion *system_memory,
                            MemoryRegion *pci_address_space)
{
    /* Set to lower priority than RAM */
    memory_region_add_subregion_overlap(system_memory, 0x0,
                                        pci_address_space, -1);
}

void pc_memory_init(PCMachineState *pcms,
                    MemoryRegion *system_memory,
                    MemoryRegion *rom_memory,
                    MemoryRegion **ram_memory)
{
    int linux_boot, i;
    MemoryRegion *ram, *option_rom_mr;
    MemoryRegion *ram_below_4g, *ram_above_4g;
    FWCfgState *fw_cfg;
    MachineState *machine = MACHINE(pcms);
    PCMachineClass *pcmc = PC_MACHINE_GET_CLASS(pcms);

    assert(machine->ram_size == pcms->below_4g_mem_size +
                                pcms->above_4g_mem_size);

    linux_boot = (machine->kernel_filename != NULL);

    /* Allocate RAM.  We allocate it as a single memory region and use
     * aliases to address portions of it, mostly for backwards compatibility
     * with older qemus that used qemu_ram_alloc().
     */
    ram = g_malloc(sizeof(*ram));
    memory_region_allocate_system_memory(ram, NULL, "pc.ram",
                                         machine->ram_size);
    *ram_memory = ram;
    ram_below_4g = g_malloc(sizeof(*ram_below_4g));
    memory_region_init_alias(ram_below_4g, NULL, "ram-below-4g", ram,
                             0, pcms->below_4g_mem_size);
    memory_region_add_subregion(system_memory, 0, ram_below_4g);
    e820_add_entry(0, pcms->below_4g_mem_size, E820_RAM);
    if (pcms->above_4g_mem_size > 0) {
        ram_above_4g = g_malloc(sizeof(*ram_above_4g));
        memory_region_init_alias(ram_above_4g, NULL, "ram-above-4g", ram,
                                 pcms->below_4g_mem_size,
                                 pcms->above_4g_mem_size);
        memory_region_add_subregion(system_memory, 0x100000000ULL,
                                    ram_above_4g);
        e820_add_entry(0x100000000ULL, pcms->above_4g_mem_size, E820_RAM);
    }

    if (!pcmc->has_reserved_memory &&
        (machine->ram_slots ||
         (machine->maxram_size > machine->ram_size))) {
        MachineClass *mc = MACHINE_GET_CLASS(machine);

        error_report("\"-memory 'slots|maxmem'\" is not supported by: %s",
                     mc->name);
        exit(EXIT_FAILURE);
    }

    /* initialize hotplug memory address space */
    if (pcmc->has_reserved_memory &&
        (machine->ram_size < machine->maxram_size)) {
        ram_addr_t hotplug_mem_size =
            machine->maxram_size - machine->ram_size;

        if (machine->ram_slots > ACPI_MAX_RAM_SLOTS) {
            error_report("unsupported amount of memory slots: %"PRIu64,
                         machine->ram_slots);
            exit(EXIT_FAILURE);
        }

        if (QEMU_ALIGN_UP(machine->maxram_size,
                          TARGET_PAGE_SIZE) != machine->maxram_size) {
            error_report("maximum memory size must by aligned to multiple of "
                         "%d bytes", TARGET_PAGE_SIZE);
            exit(EXIT_FAILURE);
        }

        pcms->hotplug_memory.base =
            ROUND_UP(0x100000000ULL + pcms->above_4g_mem_size, 1ULL << 30);

        if (pcmc->enforce_aligned_dimm) {
            /* size hotplug region assuming 1G page max alignment per slot */
            hotplug_mem_size += (1ULL << 30) * machine->ram_slots;
        }

        if ((pcms->hotplug_memory.base + hotplug_mem_size) <
            hotplug_mem_size) {
            error_report("unsupported amount of maximum memory: " RAM_ADDR_FMT,
                         machine->maxram_size);
            exit(EXIT_FAILURE);
        }

        memory_region_init(&pcms->hotplug_memory.mr, OBJECT(pcms),
                           "hotplug-memory", hotplug_mem_size);
        memory_region_add_subregion(system_memory, pcms->hotplug_memory.base,
                                    &pcms->hotplug_memory.mr);
    }

    /* Initialize PC system firmware */
    pc_system_firmware_init(rom_memory, !pcmc->pci_enabled);

    option_rom_mr = g_malloc(sizeof(*option_rom_mr));
    memory_region_init_ram(option_rom_mr, NULL, "pc.rom", PC_ROM_SIZE,
                           &error_fatal);
    if (pcmc->pci_enabled) {
        memory_region_set_readonly(option_rom_mr, true);
    }
    memory_region_add_subregion_overlap(rom_memory,
                                        PC_ROM_MIN_VGA,
                                        option_rom_mr,
                                        1);

    fw_cfg = bochs_bios_init(&address_space_memory, pcms);

    rom_set_fw(fw_cfg);

    if (pcmc->has_reserved_memory && pcms->hotplug_memory.base) {
        uint64_t *val = g_malloc(sizeof(*val));
        PCMachineClass *pcmc = PC_MACHINE_GET_CLASS(pcms);
        uint64_t res_mem_end = pcms->hotplug_memory.base;

        if (!pcmc->broken_reserved_end) {
            res_mem_end += memory_region_size(&pcms->hotplug_memory.mr);
        }
        *val = cpu_to_le64(ROUND_UP(res_mem_end, 0x1ULL << 30));
        fw_cfg_add_file(fw_cfg, "etc/reserved-memory-end", val, sizeof(*val));
    }

    if (linux_boot) {
        load_linux(pcms, fw_cfg);
    }

    for (i = 0; i < nb_option_roms; i++) {
        rom_add_option(option_rom[i].name, option_rom[i].bootindex);
    }
    pcms->fw_cfg = fw_cfg;

    /* Init default IOAPIC address space */
    pcms->ioapic_as = &address_space_memory;
}

/*
 * The 64bit pci hole starts after "above 4G RAM" and
 * potentially the space reserved for memory hotplug.
 */
uint64_t pc_pci_hole64_start(void)
{
    PCMachineState *pcms = PC_MACHINE(qdev_get_machine());
    PCMachineClass *pcmc = PC_MACHINE_GET_CLASS(pcms);
    uint64_t hole64_start = 0;

    if (pcmc->has_reserved_memory && pcms->hotplug_memory.base) {
        hole64_start = pcms->hotplug_memory.base;
        if (!pcmc->broken_reserved_end) {
            hole64_start += memory_region_size(&pcms->hotplug_memory.mr);
        }
    } else {
        hole64_start = 0x100000000ULL + pcms->above_4g_mem_size;
    }

    return ROUND_UP(hole64_start, 1ULL << 30);
}

qemu_irq pc_allocate_cpu_irq(void)
{
    return qemu_allocate_irq(pic_irq_request, NULL, 0);
}

static const MemoryRegionOps ioport80_io_ops = {
    .write = ioport80_write,
    .read = ioport80_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static const MemoryRegionOps ioportF0_io_ops = {
    .write = ioportF0_write,
    .read = ioportF0_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

void pc_basic_device_init(ISABus *isa_bus, qemu_irq *gsi,
                          ISADevice **rtc_state,
                          bool create_fdctrl,
                          bool has_pit)
{
    int pit_isa_irq = 0;
    qemu_irq pit_alt_irq = NULL;
    qemu_irq rtc_irq = NULL;
    MemoryRegion *ioport80_io = g_new(MemoryRegion, 1);
    MemoryRegion *ioportF0_io = g_new(MemoryRegion, 1);

    memory_region_init_io(ioport80_io, NULL, &ioport80_io_ops, NULL, "ioport80", 1);
    memory_region_add_subregion(isa_bus->address_space_io, 0x80, ioport80_io);

    memory_region_init_io(ioportF0_io, NULL, &ioportF0_io_ops, NULL, "ioportF0", 1);
    memory_region_add_subregion(isa_bus->address_space_io, 0xf0, ioportF0_io);

    *rtc_state = mc146818_rtc_init(isa_bus, 2000, rtc_irq);

    qemu_register_boot_set(pc_boot_set, *rtc_state);

    if (has_pit) {
        if (kvm_pit_in_kernel()) {
            kvm_pit_init(isa_bus, 0x40);
        } else {
            i8254_pit_init(isa_bus, 0x40, pit_isa_irq, pit_alt_irq);
        }
    }
}

void ioapic_init_gsi(GSIState *gsi_state, const char *parent_name)
{
    DeviceState *dev;
    SysBusDevice *d;
    unsigned int i;

    if (kvm_ioapic_in_kernel()) {
        dev = qdev_create(NULL, "kvm-ioapic");
    } else {
        dev = qdev_create(NULL, "ioapic");
    }
    if (parent_name) {
        object_property_add_child(object_resolve_path(parent_name, NULL),
                                  "ioapic", OBJECT(dev), NULL);
    }
    qdev_init_nofail(dev);
    d = SYS_BUS_DEVICE(dev);
    sysbus_mmio_map(d, 0, IO_APIC_DEFAULT_ADDRESS);

    for (i = 0; i < IOAPIC_NUM_PINS; i++) {
        gsi_state->ioapic_irq[i] = qdev_get_gpio_in(dev, i);
    }
}

static void pc_dimm_plug(HotplugHandler *hotplug_dev,
                         DeviceState *dev, Error **errp)
{
    HotplugHandlerClass *hhc;
    Error *local_err = NULL;
    PCMachineState *pcms = PC_MACHINE(hotplug_dev);
    PCMachineClass *pcmc = PC_MACHINE_GET_CLASS(pcms);
    PCDIMMDevice *dimm = PC_DIMM(dev);
    PCDIMMDeviceClass *ddc = PC_DIMM_GET_CLASS(dimm);
    MemoryRegion *mr;
    uint64_t align = TARGET_PAGE_SIZE;
    bool is_nvdimm = object_dynamic_cast(OBJECT(dev), TYPE_NVDIMM);

    mr = ddc->get_memory_region(dimm, &local_err);
    if (local_err) {
        goto out;
    }

    if (memory_region_get_alignment(mr) && pcmc->enforce_aligned_dimm) {
        align = memory_region_get_alignment(mr);
    }

    /*
     * When -no-acpi is used with Q35 machine type, no ACPI is built,
     * but pcms->acpi_dev is still created. Check !acpi_enabled in
     * addition to cover this case.
     */
    if (!pcms->acpi_dev || !acpi_enabled) {
        error_setg(&local_err,
                   "memory hotplug is not enabled: missing acpi device or acpi disabled");
        goto out;
    }

    if (is_nvdimm && !pcms->acpi_nvdimm_state.is_enabled) {
        error_setg(&local_err,
                   "nvdimm is not enabled: missing 'nvdimm' in '-M'");
        goto out;
    }

    pc_dimm_memory_plug(dev, &pcms->hotplug_memory, mr, align, &local_err);
    if (local_err) {
        goto out;
    }

    if (is_nvdimm) {
        nvdimm_plug(&pcms->acpi_nvdimm_state);
    }

    hhc = HOTPLUG_HANDLER_GET_CLASS(pcms->acpi_dev);
    hhc->plug(HOTPLUG_HANDLER(pcms->acpi_dev), dev, &error_abort);
out:
    error_propagate(errp, local_err);
}

static void pc_dimm_unplug_request(HotplugHandler *hotplug_dev,
                                   DeviceState *dev, Error **errp)
{
    HotplugHandlerClass *hhc;
    Error *local_err = NULL;
    PCMachineState *pcms = PC_MACHINE(hotplug_dev);

    /*
     * When -no-acpi is used with Q35 machine type, no ACPI is built,
     * but pcms->acpi_dev is still created. Check !acpi_enabled in
     * addition to cover this case.
     */
    if (!pcms->acpi_dev || !acpi_enabled) {
        error_setg(&local_err,
                   "memory hotplug is not enabled: missing acpi device or acpi disabled");
        goto out;
    }

    if (object_dynamic_cast(OBJECT(dev), TYPE_NVDIMM)) {
        error_setg(&local_err,
                   "nvdimm device hot unplug is not supported yet.");
        goto out;
    }

    hhc = HOTPLUG_HANDLER_GET_CLASS(pcms->acpi_dev);
    hhc->unplug_request(HOTPLUG_HANDLER(pcms->acpi_dev), dev, &local_err);

out:
    error_propagate(errp, local_err);
}

static void pc_dimm_unplug(HotplugHandler *hotplug_dev,
                           DeviceState *dev, Error **errp)
{
    PCMachineState *pcms = PC_MACHINE(hotplug_dev);
    PCDIMMDevice *dimm = PC_DIMM(dev);
    PCDIMMDeviceClass *ddc = PC_DIMM_GET_CLASS(dimm);
    MemoryRegion *mr;
    HotplugHandlerClass *hhc;
    Error *local_err = NULL;

    mr = ddc->get_memory_region(dimm, &local_err);
    if (local_err) {
        goto out;
    }

    hhc = HOTPLUG_HANDLER_GET_CLASS(pcms->acpi_dev);
    hhc->unplug(HOTPLUG_HANDLER(pcms->acpi_dev), dev, &local_err);

    if (local_err) {
        goto out;
    }

    pc_dimm_memory_unplug(dev, &pcms->hotplug_memory, mr);
    object_unparent(OBJECT(dev));

 out:
    error_propagate(errp, local_err);
}

static int pc_apic_cmp(const void *a, const void *b)
{
   CPUArchId *apic_a = (CPUArchId *)a;
   CPUArchId *apic_b = (CPUArchId *)b;

   return apic_a->arch_id - apic_b->arch_id;
}

/* returns pointer to CPUArchId descriptor that matches CPU's apic_id
 * in ms->possible_cpus->cpus, if ms->possible_cpus->cpus has no
 * entry corresponding to CPU's apic_id returns NULL.
 */
static CPUArchId *pc_find_cpu_slot(MachineState *ms, uint32_t id, int *idx)
{
    CPUArchId apic_id, *found_cpu;

    apic_id.arch_id = id;
    found_cpu = bsearch(&apic_id, ms->possible_cpus->cpus,
        ms->possible_cpus->len, sizeof(*ms->possible_cpus->cpus),
        pc_apic_cmp);
    if (found_cpu && idx) {
        *idx = found_cpu - ms->possible_cpus->cpus;
    }
    return found_cpu;
}

static void pc_cpu_plug(HotplugHandler *hotplug_dev,
                        DeviceState *dev, Error **errp)
{
    CPUArchId *found_cpu;
    HotplugHandlerClass *hhc;
    Error *local_err = NULL;
    X86CPU *cpu = X86_CPU(dev);
    PCMachineState *pcms = PC_MACHINE(hotplug_dev);

    if (pcms->acpi_dev) {
        hhc = HOTPLUG_HANDLER_GET_CLASS(pcms->acpi_dev);
        hhc->plug(HOTPLUG_HANDLER(pcms->acpi_dev), dev, &local_err);
        if (local_err) {
            goto out;
        }
    }

    /* increment the number of CPUs */
    pcms->boot_cpus++;
    if (pcms->rtc) {
        rtc_set_cpus_count(pcms->rtc, pcms->boot_cpus);
    }
    if (pcms->fw_cfg) {
        fw_cfg_modify_i16(pcms->fw_cfg, FW_CFG_NB_CPUS, pcms->boot_cpus);
    }

    found_cpu = pc_find_cpu_slot(MACHINE(pcms), cpu->apic_id, NULL);
    found_cpu->cpu = OBJECT(dev);
out:
    error_propagate(errp, local_err);
}
static void pc_cpu_unplug_request_cb(HotplugHandler *hotplug_dev,
                                     DeviceState *dev, Error **errp)
{
    int idx = -1;
    HotplugHandlerClass *hhc;
    Error *local_err = NULL;
    X86CPU *cpu = X86_CPU(dev);
    PCMachineState *pcms = PC_MACHINE(hotplug_dev);

    if (!pcms->acpi_dev) {
        error_setg(&local_err, "CPU hot unplug not supported without ACPI");
        goto out;
    }

    pc_find_cpu_slot(MACHINE(pcms), cpu->apic_id, &idx);
    assert(idx != -1);
    if (idx == 0) {
        error_setg(&local_err, "Boot CPU is unpluggable");
        goto out;
    }

    hhc = HOTPLUG_HANDLER_GET_CLASS(pcms->acpi_dev);
    hhc->unplug_request(HOTPLUG_HANDLER(pcms->acpi_dev), dev, &local_err);

    if (local_err) {
        goto out;
    }

 out:
    error_propagate(errp, local_err);

}

static void pc_cpu_unplug_cb(HotplugHandler *hotplug_dev,
                             DeviceState *dev, Error **errp)
{
    CPUArchId *found_cpu;
    HotplugHandlerClass *hhc;
    Error *local_err = NULL;
    X86CPU *cpu = X86_CPU(dev);
    PCMachineState *pcms = PC_MACHINE(hotplug_dev);

    hhc = HOTPLUG_HANDLER_GET_CLASS(pcms->acpi_dev);
    hhc->unplug(HOTPLUG_HANDLER(pcms->acpi_dev), dev, &local_err);

    if (local_err) {
        goto out;
    }

    found_cpu = pc_find_cpu_slot(MACHINE(pcms), cpu->apic_id, NULL);
    found_cpu->cpu = NULL;
    object_unparent(OBJECT(dev));

    /* decrement the number of CPUs */
    pcms->boot_cpus--;
    /* Update the number of CPUs in CMOS */
    rtc_set_cpus_count(pcms->rtc, pcms->boot_cpus);
    fw_cfg_modify_i16(pcms->fw_cfg, FW_CFG_NB_CPUS, pcms->boot_cpus);
 out:
    error_propagate(errp, local_err);
}

static void pc_cpu_pre_plug(HotplugHandler *hotplug_dev,
                            DeviceState *dev, Error **errp)
{
    int idx;
    CPUState *cs;
    CPUArchId *cpu_slot;
    X86CPUTopoInfo topo;
    X86CPU *cpu = X86_CPU(dev);
    MachineState *ms = MACHINE(hotplug_dev);
    PCMachineState *pcms = PC_MACHINE(hotplug_dev);

    if(!object_dynamic_cast(OBJECT(cpu), ms->cpu_type)) {
        error_setg(errp, "Invalid CPU type, expected cpu type: '%s'",
                   ms->cpu_type);
        return;
    }

    /* if APIC ID is not set, set it based on socket/core/thread properties */
    if (cpu->apic_id == UNASSIGNED_APIC_ID) {
        int max_socket = (max_cpus - 1) / smp_threads / smp_cores;

        if (cpu->socket_id < 0) {
            error_setg(errp, "CPU socket-id is not set");
            return;
        } else if (cpu->socket_id > max_socket) {
            error_setg(errp, "Invalid CPU socket-id: %u must be in range 0:%u",
                       cpu->socket_id, max_socket);
            return;
        }
        if (cpu->core_id < 0) {
            error_setg(errp, "CPU core-id is not set");
            return;
        } else if (cpu->core_id > (smp_cores - 1)) {
            error_setg(errp, "Invalid CPU core-id: %u must be in range 0:%u",
                       cpu->core_id, smp_cores - 1);
            return;
        }
        if (cpu->thread_id < 0) {
            error_setg(errp, "CPU thread-id is not set");
            return;
        } else if (cpu->thread_id > (smp_threads - 1)) {
            error_setg(errp, "Invalid CPU thread-id: %u must be in range 0:%u",
                       cpu->thread_id, smp_threads - 1);
            return;
        }

        topo.pkg_id = cpu->socket_id;
        topo.core_id = cpu->core_id;
        topo.smt_id = cpu->thread_id;
        cpu->apic_id = apicid_from_topo_ids(smp_cores, smp_threads, &topo);
    }

    cpu_slot = pc_find_cpu_slot(MACHINE(pcms), cpu->apic_id, &idx);
    if (!cpu_slot) {
        MachineState *ms = MACHINE(pcms);

        x86_topo_ids_from_apicid(cpu->apic_id, smp_cores, smp_threads, &topo);
        error_setg(errp, "Invalid CPU [socket: %u, core: %u, thread: %u] with"
                  " APIC ID %" PRIu32 ", valid index range 0:%d",
                   topo.pkg_id, topo.core_id, topo.smt_id, cpu->apic_id,
                   ms->possible_cpus->len - 1);
        return;
    }

    if (cpu_slot->cpu) {
        error_setg(errp, "CPU[%d] with APIC ID %" PRIu32 " exists",
                   idx, cpu->apic_id);
        return;
    }

    /* if 'address' properties socket-id/core-id/thread-id are not set, set them
     * so that machine_query_hotpluggable_cpus would show correct values
     */
    /* TODO: move socket_id/core_id/thread_id checks into x86_cpu_realizefn()
     * once -smp refactoring is complete and there will be CPU private
     * CPUState::nr_cores and CPUState::nr_threads fields instead of globals */
    x86_topo_ids_from_apicid(cpu->apic_id, smp_cores, smp_threads, &topo);
    if (cpu->socket_id != -1 && cpu->socket_id != topo.pkg_id) {
        error_setg(errp, "property socket-id: %u doesn't match set apic-id:"
            " 0x%x (socket-id: %u)", cpu->socket_id, cpu->apic_id, topo.pkg_id);
        return;
    }
    cpu->socket_id = topo.pkg_id;

    if (cpu->core_id != -1 && cpu->core_id != topo.core_id) {
        error_setg(errp, "property core-id: %u doesn't match set apic-id:"
            " 0x%x (core-id: %u)", cpu->core_id, cpu->apic_id, topo.core_id);
        return;
    }
    cpu->core_id = topo.core_id;

    if (cpu->thread_id != -1 && cpu->thread_id != topo.smt_id) {
        error_setg(errp, "property thread-id: %u doesn't match set apic-id:"
            " 0x%x (thread-id: %u)", cpu->thread_id, cpu->apic_id, topo.smt_id);
        return;
    }
    cpu->thread_id = topo.smt_id;

    cs = CPU(cpu);
    cs->cpu_index = idx;

    numa_cpu_pre_plug(cpu_slot, dev, errp);
}

static void pc_machine_device_pre_plug_cb(HotplugHandler *hotplug_dev,
                                          DeviceState *dev, Error **errp)
{
    if (object_dynamic_cast(OBJECT(dev), TYPE_CPU)) {
        pc_cpu_pre_plug(hotplug_dev, dev, errp);
    }
}

static void pc_machine_device_plug_cb(HotplugHandler *hotplug_dev,
                                      DeviceState *dev, Error **errp)
{
    if (object_dynamic_cast(OBJECT(dev), TYPE_PC_DIMM)) {
        pc_dimm_plug(hotplug_dev, dev, errp);
    } else if (object_dynamic_cast(OBJECT(dev), TYPE_CPU)) {
        pc_cpu_plug(hotplug_dev, dev, errp);
    }
}

static void pc_machine_device_unplug_request_cb(HotplugHandler *hotplug_dev,
                                                DeviceState *dev, Error **errp)
{
    if (object_dynamic_cast(OBJECT(dev), TYPE_PC_DIMM)) {
        pc_dimm_unplug_request(hotplug_dev, dev, errp);
    } else if (object_dynamic_cast(OBJECT(dev), TYPE_CPU)) {
        pc_cpu_unplug_request_cb(hotplug_dev, dev, errp);
    } else {
        error_setg(errp, "acpi: device unplug request for not supported device"
                   " type: %s", object_get_typename(OBJECT(dev)));
    }
}

static void pc_machine_device_unplug_cb(HotplugHandler *hotplug_dev,
                                        DeviceState *dev, Error **errp)
{
    if (object_dynamic_cast(OBJECT(dev), TYPE_PC_DIMM)) {
        pc_dimm_unplug(hotplug_dev, dev, errp);
    } else if (object_dynamic_cast(OBJECT(dev), TYPE_CPU)) {
        pc_cpu_unplug_cb(hotplug_dev, dev, errp);
    } else {
        error_setg(errp, "acpi: device unplug for not supported device"
                   " type: %s", object_get_typename(OBJECT(dev)));
    }
}

static HotplugHandler *pc_get_hotpug_handler(MachineState *machine,
                                             DeviceState *dev)
{
    PCMachineClass *pcmc = PC_MACHINE_GET_CLASS(machine);

    if (object_dynamic_cast(OBJECT(dev), TYPE_PC_DIMM) ||
        object_dynamic_cast(OBJECT(dev), TYPE_CPU)) {
        return HOTPLUG_HANDLER(machine);
    }

    return pcmc->get_hotplug_handler ?
        pcmc->get_hotplug_handler(machine, dev) : NULL;
}

static void
pc_machine_get_hotplug_memory_region_size(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    PCMachineState *pcms = PC_MACHINE(obj);
    int64_t value = memory_region_size(&pcms->hotplug_memory.mr);

    visit_type_int(v, name, &value, errp);
}

static void pc_machine_get_max_ram_below_4g(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    PCMachineState *pcms = PC_MACHINE(obj);
    uint64_t value = pcms->max_ram_below_4g;

    visit_type_size(v, name, &value, errp);
}

static void pc_machine_set_max_ram_below_4g(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    PCMachineState *pcms = PC_MACHINE(obj);
    Error *error = NULL;
    uint64_t value;

    visit_type_size(v, name, &value, &error);
    if (error) {
        error_propagate(errp, error);
        return;
    }
    if (value > (1ULL << 32)) {
        error_setg(&error,
                   "Machine option 'max-ram-below-4g=%"PRIu64
                   "' expects size less than or equal to 4G", value);
        error_propagate(errp, error);
        return;
    }

    if (value < (1ULL << 20)) {
        warn_report("Only %" PRIu64 " bytes of RAM below the 4GiB boundary,"
                    "BIOS may not work with less than 1MiB", value);
    }

    pcms->max_ram_below_4g = value;
}


static bool pc_machine_get_nvdimm(Object *obj, Error **errp)
{
    PCMachineState *pcms = PC_MACHINE(obj);

    return pcms->acpi_nvdimm_state.is_enabled;
}

static void pc_machine_set_nvdimm(Object *obj, bool value, Error **errp)
{
    PCMachineState *pcms = PC_MACHINE(obj);

    pcms->acpi_nvdimm_state.is_enabled = value;
}

static bool pc_machine_get_smbus(Object *obj, Error **errp)
{
    PCMachineState *pcms = PC_MACHINE(obj);

    return pcms->smbus;
}

static void pc_machine_set_smbus(Object *obj, bool value, Error **errp)
{
    PCMachineState *pcms = PC_MACHINE(obj);

    pcms->smbus = value;
}

static bool pc_machine_get_pit(Object *obj, Error **errp)
{
    PCMachineState *pcms = PC_MACHINE(obj);

    return pcms->pit;
}

static void pc_machine_set_pit(Object *obj, bool value, Error **errp)
{
    PCMachineState *pcms = PC_MACHINE(obj);

    pcms->pit = value;
}

static void pc_machine_initfn(Object *obj)
{
    PCMachineState *pcms = PC_MACHINE(obj);

    pcms->max_ram_below_4g = 0; /* use default */
    /* nvdimm is disabled on default. */
    pcms->acpi_nvdimm_state.is_enabled = false;
    /* acpi build is enabled by default if machine supports it */
    pcms->acpi_build_enabled = PC_MACHINE_GET_CLASS(pcms)->has_acpi_build;
    pcms->smbus = true;
    pcms->pit = true;
}

static void pc_machine_reset(void)
{
    CPUState *cs;
    X86CPU *cpu;

    qemu_devices_reset();

    /* Reset APIC after devices have been reset to cancel
     * any changes that qemu_devices_reset() might have done.
     */
    CPU_FOREACH(cs) {
        cpu = X86_CPU(cs);

        if (cpu->apic_state) {
            device_reset(cpu->apic_state);
        }
    }
}

static CpuInstanceProperties
pc_cpu_index_to_props(MachineState *ms, unsigned cpu_index)
{
    MachineClass *mc = MACHINE_GET_CLASS(ms);
    const CPUArchIdList *possible_cpus = mc->possible_cpu_arch_ids(ms);

    assert(cpu_index < possible_cpus->len);
    return possible_cpus->cpus[cpu_index].props;
}

static int64_t pc_get_default_cpu_node_id(const MachineState *ms, int idx)
{
   X86CPUTopoInfo topo;

   assert(idx < ms->possible_cpus->len);
   x86_topo_ids_from_apicid(ms->possible_cpus->cpus[idx].arch_id,
                            smp_cores, smp_threads, &topo);
   return topo.pkg_id % nb_numa_nodes;
}

static const CPUArchIdList *pc_possible_cpu_arch_ids(MachineState *ms)
{
    int i;

    if (ms->possible_cpus) {
        /*
         * make sure that max_cpus hasn't changed since the first use, i.e.
         * -smp hasn't been parsed after it
        */
        assert(ms->possible_cpus->len == max_cpus);
        return ms->possible_cpus;
    }

    ms->possible_cpus = g_malloc0(sizeof(CPUArchIdList) +
                                  sizeof(CPUArchId) * max_cpus);
    ms->possible_cpus->len = max_cpus;
    for (i = 0; i < ms->possible_cpus->len; i++) {
        X86CPUTopoInfo topo;

        ms->possible_cpus->cpus[i].type = ms->cpu_type;
        ms->possible_cpus->cpus[i].vcpus_count = 1;
        ms->possible_cpus->cpus[i].arch_id = x86_cpu_apic_id_from_index(i);
        x86_topo_ids_from_apicid(ms->possible_cpus->cpus[i].arch_id,
                                 smp_cores, smp_threads, &topo);
        ms->possible_cpus->cpus[i].props.has_socket_id = true;
        ms->possible_cpus->cpus[i].props.socket_id = topo.pkg_id;
        ms->possible_cpus->cpus[i].props.has_core_id = true;
        ms->possible_cpus->cpus[i].props.core_id = topo.core_id;
        ms->possible_cpus->cpus[i].props.has_thread_id = true;
        ms->possible_cpus->cpus[i].props.thread_id = topo.smt_id;
    }
    return ms->possible_cpus;
}

static void x86_nmi(NMIState *n, int cpu_index, Error **errp)
{
    /* cpu index isn't used */
    CPUState *cs;

    CPU_FOREACH(cs) {
        X86CPU *cpu = X86_CPU(cs);

        if (!cpu->apic_state) {
            cpu_interrupt(cs, CPU_INTERRUPT_NMI);
        } else {
            apic_deliver_nmi(cpu->apic_state);
        }
    }
}

static void pc_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    PCMachineClass *pcmc = PC_MACHINE_CLASS(oc);
    HotplugHandlerClass *hc = HOTPLUG_HANDLER_CLASS(oc);
    NMIClass *nc = NMI_CLASS(oc);

    pcmc->get_hotplug_handler = mc->get_hotplug_handler;
    pcmc->pci_enabled = true;
    pcmc->has_acpi_build = true;
    pcmc->rsdp_in_ram = true;
    pcmc->smbios_defaults = true;
    pcmc->smbios_uuid_encoded = true;
    pcmc->gigabyte_align = true;
    pcmc->has_reserved_memory = true;
    pcmc->kvmclock_enabled = true;
    pcmc->enforce_aligned_dimm = true;
    /* BIOS ACPI tables: 128K. Other BIOS datastructures: less than 4K reported
     * to be used at the moment, 32K should be enough for a while.  */
    pcmc->acpi_data_size = 0x20000 + 0x8000;
    pcmc->save_tsc_khz = true;
    pcmc->linuxboot_dma_enabled = true;
    mc->get_hotplug_handler = pc_get_hotpug_handler;
    mc->cpu_index_to_instance_props = pc_cpu_index_to_props;
    mc->get_default_cpu_node_id = pc_get_default_cpu_node_id;
    mc->possible_cpu_arch_ids = pc_possible_cpu_arch_ids;
    mc->auto_enable_numa_with_memhp = true;
    mc->has_hotpluggable_cpus = true;
    mc->default_boot_order = "cad";
    mc->hot_add_cpu = pc_hot_add_cpu;
    mc->block_default_type = IF_VIRTIO;
    mc->max_cpus = 255;
    mc->reset = pc_machine_reset;
    hc->pre_plug = pc_machine_device_pre_plug_cb;
    hc->plug = pc_machine_device_plug_cb;
    hc->unplug_request = pc_machine_device_unplug_request_cb;
    hc->unplug = pc_machine_device_unplug_cb;
    nc->nmi_monitor_handler = x86_nmi;
    mc->default_cpu_type = TARGET_DEFAULT_CPU_TYPE;

    object_class_property_add(oc, PC_MACHINE_MEMHP_REGION_SIZE, "int",
        pc_machine_get_hotplug_memory_region_size, NULL,
        NULL, NULL, &error_abort);

    object_class_property_add(oc, PC_MACHINE_MAX_RAM_BELOW_4G, "size",
        pc_machine_get_max_ram_below_4g, pc_machine_set_max_ram_below_4g,
        NULL, NULL, &error_abort);

    object_class_property_set_description(oc, PC_MACHINE_MAX_RAM_BELOW_4G,
        "Maximum ram below the 4G boundary (32bit boundary)", &error_abort);


    object_class_property_add_bool(oc, PC_MACHINE_NVDIMM,
        pc_machine_get_nvdimm, pc_machine_set_nvdimm, &error_abort);

    object_class_property_add_bool(oc, PC_MACHINE_SMBUS,
        pc_machine_get_smbus, pc_machine_set_smbus, &error_abort);

    object_class_property_add_bool(oc, PC_MACHINE_PIT,
        pc_machine_get_pit, pc_machine_set_pit, &error_abort);
}

static const TypeInfo pc_machine_info = {
    .name = TYPE_PC_MACHINE,
    .parent = TYPE_MACHINE,
    .abstract = true,
    .instance_size = sizeof(PCMachineState),
    .instance_init = pc_machine_initfn,
    .class_size = sizeof(PCMachineClass),
    .class_init = pc_machine_class_init,
    .interfaces = (InterfaceInfo[]) {
         { TYPE_HOTPLUG_HANDLER },
         { TYPE_NMI },
         { }
    },
};

static void pc_machine_register_types(void)
{
    type_register_static(&pc_machine_info);
}

type_init(pc_machine_register_types)
