#ifndef HW_PC_H
#define HW_PC_H

#include "qemu-common.h"
#include "exec/memory.h"
#include "hw/boards.h"
#include "hw/isa/isa.h"
#include "net/net.h"
#include "hw/i386/ioapic.h"

#include "qemu/range.h"
#include "qemu/bitmap.h"
#include "sysemu/sysemu.h"
#include "hw/pci/pci.h"
#include "hw/compat.h"
#include "hw/mem/pc-dimm.h"
#include "hw/mem/nvdimm.h"
#include "hw/acpi/acpi_dev_interface.h"
#include "hw/acpi/acpi-defs.h"
#include "hw/acpi/aml-build.h"
#include "sysemu/tpm.h"


/**
 * PCMachineState:
 * @acpi_dev: link to ACPI PM device that performs ACPI hotplug handling
 * @boot_cpus: number of present VCPUs
 */
struct PCMachineState {
    /*< private >*/
    MachineState parent_obj;

    /* <public> */

    /* State for other subsystems/APIs: */
    MemoryHotplugState hotplug_memory;
    Notifier machine_done;

    /* Pointers to devices and objects: */
    HotplugHandler *acpi_dev;
    ISADevice *rtc;
    PCIBus *bus;
    FWCfgState *fw_cfg;
    qemu_irq *gsi;

    /* Configuration options: */
    uint64_t max_ram_below_4g;

    AcpiNVDIMMState acpi_nvdimm_state;

    bool acpi_build_enabled;
    bool smbus;
    bool pit;

    /* RAM information (sizes, addresses, configuration): */
    ram_addr_t below_4g_mem_size, above_4g_mem_size;

    /* CPU and apic information: */
    bool apic_xrupt_override;
    unsigned apic_id_limit;
    uint16_t boot_cpus;

    /* NUMA information: */
    uint64_t numa_nodes;
    uint64_t *node_mem;

    /* Address space used by IOAPIC device. All IOAPIC interrupts
     * will be translated to MSI messages in the address space. */
    AddressSpace *ioapic_as;
};

#define PC_MACHINE_ACPI_DEVICE_PROP "acpi-device"
#define PC_MACHINE_MEMHP_REGION_SIZE "hotplug-memory-region-size"
#define PC_MACHINE_MAX_RAM_BELOW_4G "max-ram-below-4g"
#define PC_MACHINE_NVDIMM           "nvdimm"
#define PC_MACHINE_SMBUS            "smbus"
#define PC_MACHINE_PIT              "pit"

/**
 * PCMachineClass:
 *
 * Methods:
 *
 * @get_hotplug_handler: pointer to parent class callback @get_hotplug_handler
 *
 * Compat fields:
 *
 * @enforce_aligned_dimm: check that DIMM's address/size is aligned by
 *                        backend's alignment value if provided
 * @acpi_data_size: Size of the chunk of memory at the top of RAM
 *                  for the BIOS ACPI tables and other BIOS
 *                  datastructures.
 * @gigabyte_align: Make sure that guest addresses aligned at
 *                  1Gbyte boundaries get mapped to host
 *                  addresses aligned at 1Gbyte boundaries. This
 *                  way we can use 1GByte pages in the host.
 *
 */
struct PCMachineClass {
    /*< private >*/
    MachineClass parent_class;

    /*< public >*/

    /* Methods: */
    HotplugHandler *(*get_hotplug_handler)(MachineState *machine,
                                           DeviceState *dev);

    /* Device configuration: */
    bool pci_enabled;
    bool kvmclock_enabled;
    const char *default_nic_model;

    /* Compat options: */

    /* ACPI compat: */
    bool has_acpi_build;
    bool rsdp_in_ram;
    int legacy_acpi_table_size;
    unsigned acpi_data_size;

    /* SMBIOS compat: */
    bool smbios_defaults;
    bool smbios_legacy_mode;
    bool smbios_uuid_encoded;

    /* RAM / address space compat: */
    bool gigabyte_align;
    bool has_reserved_memory;
    bool enforce_aligned_dimm;
    bool broken_reserved_end;

    /* TSC rate migration: */
    bool save_tsc_khz;
    /* generate legacy CPU hotplug AML */
    bool legacy_cpu_hotplug;

    /* use DMA capable linuxboot option rom */
    bool linuxboot_dma_enabled;
};

#define TYPE_PC_MACHINE "generic-pc-machine"
#define PC_MACHINE(obj) \
    OBJECT_CHECK(PCMachineState, (obj), TYPE_PC_MACHINE)
#define PC_MACHINE_GET_CLASS(obj) \
    OBJECT_GET_CLASS(PCMachineClass, (obj), TYPE_PC_MACHINE)
#define PC_MACHINE_CLASS(klass) \
    OBJECT_CLASS_CHECK(PCMachineClass, (klass), TYPE_PC_MACHINE)

/* i8259.c */

extern DeviceState *isa_pic;
qemu_irq *i8259_init(ISABus *bus, qemu_irq parent_irq);
qemu_irq *kvm_i8259_init(ISABus *bus);
int pic_read_irq(DeviceState *d);
int pic_get_output(DeviceState *d);

/* ioapic.c */

void kvm_ioapic_dump_state(Monitor *mon, const QDict *qdict);
void ioapic_dump_state(Monitor *mon, const QDict *qdict);

/* Global System Interrupts */

#define GSI_NUM_PINS IOAPIC_NUM_PINS

typedef struct GSIState {
    qemu_irq i8259_irq[ISA_NUM_IRQS];
    qemu_irq ioapic_irq[IOAPIC_NUM_PINS];
} GSIState;


/* pc.c */
extern int fd_bootchk;

void pc_register_ferr_irq(qemu_irq irq);
void pc_acpi_smi_interrupt(void *opaque, int irq, int level);

void pc_cpus_init(PCMachineState *pcms);
void pc_hot_add_cpu(const int64_t id, Error **errp);

void pc_guest_info_init(PCMachineState *pcms);

#define PCI_HOST_PROP_PCI_HOLE_START   "pci-hole-start"
#define PCI_HOST_PROP_PCI_HOLE_END     "pci-hole-end"
#define PCI_HOST_PROP_PCI_HOLE64_START "pci-hole64-start"
#define PCI_HOST_PROP_PCI_HOLE64_END   "pci-hole64-end"
#define PCI_HOST_PROP_PCI_HOLE64_SIZE  "pci-hole64-size"
#define PCI_HOST_BELOW_4G_MEM_SIZE     "below-4g-mem-size"
#define PCI_HOST_ABOVE_4G_MEM_SIZE     "above-4g-mem-size"


void pc_pci_as_mapping_init(Object *owner, MemoryRegion *system_memory,
                            MemoryRegion *pci_address_space);

void pc_memory_init(PCMachineState *pcms,
                    MemoryRegion *system_memory,
                    MemoryRegion *rom_memory,
                    MemoryRegion **ram_memory);
uint64_t pc_pci_hole64_start(void);
void pc_basic_device_init(ISABus *isa_bus, qemu_irq *gsi,
                          ISADevice **rtc_state,
                          bool create_fdctrl,
                          bool has_pit);
void pc_cmos_init(PCMachineState *pcms,
                  ISADevice *s);
void pc_pci_device_init(PCIBus *pci_bus);

typedef void (*cpu_set_smm_t)(int smm, void *arg);

void ioapic_init_gsi(GSIState *gsi_state, const char *parent_name);

#define FW_CFG_IO_BASE     0x510

#define PORT92_A20_LINE "a20"

/*
 * Reset Control Register: PCI-accessible ISA-Compatible Register at address
 * 0xcf9, provided by the PCI/ISA bridge (PIIX3 PCI function 0, 8086:7000).
 */
#define RCR_IOPORT 0xcf9

/* Default IOAPIC ID */
#define ACPI_BUILD_IOAPIC_ID 0x0
typedef struct AcpiMcfgInfo {
    uint64_t mcfg_base;
    uint32_t mcfg_size;
} AcpiMcfgInfo;

typedef struct AcpiPmInfo {
    bool s3_disabled;
    bool s4_disabled;
    bool pcihp_bridge_en;
    uint8_t s4_val;
    AcpiFadtData fadt;
    uint16_t cpu_hp_io_base;
    uint16_t pcihp_io_base;
    uint16_t pcihp_io_len;
} AcpiPmInfo;

typedef struct AcpiMiscInfo {
    TPMVersion tpm_version;
    const unsigned char *dsdt_code;
    unsigned dsdt_size;
    uint16_t pvpanic_port;
    uint16_t applesmc_io_base;
} AcpiMiscInfo;

/*acpi-build-q35.c*/
void build_q35_pci0_int(Aml *table);
void build_q35_isa_bridge(Aml *table);
Aml *build_q35_osc_method(void);
void build_mcfg_q35(GArray *table_data, BIOSLinker *linker, AcpiMcfgInfo *info);
void build_dmar_q35(GArray *table_data, BIOSLinker *linker);
bool acpi_get_mcfg(AcpiMcfgInfo *mcfg);
Object *acpi_get_i386_pci_host(void);


/*acpi-build-core.c*/
Aml *build_irq_status_method(void);
Aml *build_link_dev(const char *name, uint8_t uid, Aml *reg);
Aml *build_gsi_link_dev(const char *name, uint8_t uid, uint8_t gsi);
Aml *build_prt(bool is_pci0_prt);
void pc_madt_cpu_entry(AcpiDeviceIf *adev, int uid,
                       const CPUArchIdList *apic_ids, GArray *entry);

/* pc_sysfw.c */
void pc_system_firmware_init(MemoryRegion *rom_memory,
                             bool isapc_ram_fw);

/* e820 types */
#define E820_RAM        1
#define E820_RESERVED   2
#define E820_ACPI       3
#define E820_NVS        4
#define E820_UNUSABLE   5

int e820_add_entry(uint64_t, uint64_t, uint32_t);
int e820_get_num_entries(void);
bool e820_get_entry(int, uint32_t, uint64_t *, uint64_t *);

#define DEFINE_PC_MACHINE(suffix, namestr, initfn, optsfn) \
    static void pc_machine_##suffix##_class_init(ObjectClass *oc, void *data) \
    { \
        MachineClass *mc = MACHINE_CLASS(oc); \
        optsfn(mc); \
        mc->init = initfn; \
    } \
    static const TypeInfo pc_machine_type_##suffix = { \
        .name       = namestr TYPE_MACHINE_SUFFIX, \
        .parent     = TYPE_PC_MACHINE, \
        .class_init = pc_machine_##suffix##_class_init, \
    }; \
    static void pc_machine_init_##suffix(void) \
    { \
        type_register(&pc_machine_type_##suffix); \
    } \
    type_init(pc_machine_init_##suffix)

#endif
