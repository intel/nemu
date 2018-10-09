#ifndef HW_PC_H
#define HW_PC_H

#include "qemu-common.h"
#include "exec/memory.h"
#include "hw/boards.h"
#include "hw/isa/isa.h"
#include "hw/block/fdc.h"
#include "net/net.h"
#include "hw/i386/ioapic.h"

#include "qemu/range.h"
#include "qemu/bitmap.h"
#include "sysemu/sysemu.h"
#include "hw/acpi/acpi.h"
#include "hw/pci/pci.h"
#include "hw/compat.h"
#include "hw/mem/pc-dimm.h"
#include "hw/mem/nvdimm.h"
#include "hw/acpi/acpi_dev_interface.h"

#define HPET_INTCAP "hpet-intcap"

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
    Notifier machine_done;

    /* Pointers to devices and objects: */
    HotplugHandler *acpi_dev;
    ISADevice *rtc;
    PCIBus *bus;
    FWCfgState *fw_cfg;
    qemu_irq *gsi;

    /* Configuration options: */
    uint64_t max_ram_below_4g;
    OnOffAuto vmport;
    OnOffAuto smm;

    AcpiNVDIMMState acpi_nvdimm_state;

    bool acpi_build_enabled;
    bool smbus;
    bool sata;
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

    /* ACPI configuration */
    AcpiConfiguration *acpi_configuration;
};

#define PC_MACHINE_ACPI_DEVICE_PROP "acpi-device"
#define PC_MACHINE_MAX_RAM_BELOW_4G "max-ram-below-4g"
#define PC_MACHINE_VMPORT           "vmport"
#define PC_MACHINE_SMM              "smm"
#define PC_MACHINE_NVDIMM           "nvdimm"
#define PC_MACHINE_NVDIMM_PERSIST   "nvdimm-persistence"
#define PC_MACHINE_SMBUS            "smbus"
#define PC_MACHINE_SATA             "sata"
#define PC_MACHINE_PIT              "pit"

/**
 * PCMachineClass:
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

qemu_irq *i8259_init(ISABus *bus, qemu_irq parent_irq);
qemu_irq *kvm_i8259_init(ISABus *bus);

/* ioapic.c */

/* Global System Interrupts */

#define GSI_NUM_PINS IOAPIC_NUM_PINS

typedef struct GSIState {
    qemu_irq i8259_irq[ISA_NUM_IRQS];
    qemu_irq ioapic_irq[IOAPIC_NUM_PINS];
} GSIState;

void gsi_handler(void *opaque, int n, int level);

/* vmport.c */
#define TYPE_VMPORT "vmport"
typedef uint32_t (VMPortReadFunc)(void *opaque, uint32_t address);

static inline void vmport_init(ISABus *bus)
{
    isa_create_simple(bus, TYPE_VMPORT);
}

void vmport_register(unsigned char command, VMPortReadFunc *func, void *opaque);
void vmmouse_get_data(uint32_t *data);
void vmmouse_set_data(const uint32_t *data);

/* pc.c */
extern int fd_bootchk;
extern bool compat_apic_id_mode;

void pc_acpi_smi_interrupt(void *opaque, int irq, int level);

void pc_cpus_init(PCMachineState *pcms);
void pc_hot_add_cpu(const int64_t id, Error **errp);
void pc_acpi_init(const char *default_dsdt);

void pc_guest_info_init(PCMachineState *pcms);

void xen_load_linux(PCMachineState *pcms);
void pc_memory_init(PCMachineState *pcms,
                    MemoryRegion *system_memory,
                    MemoryRegion *rom_memory,
                    MemoryRegion **ram_memory);
uint64_t pc_pci_hole64_start(void);
qemu_irq pc_allocate_cpu_irq(void);
DeviceState *pc_vga_init(ISABus *isa_bus, PCIBus *pci_bus);
void pc_basic_device_init(ISABus *isa_bus, qemu_irq *gsi,
                          ISADevice **rtc_state,
                          bool create_fdctrl,
                          bool no_vmport,
                          bool has_pit,
                          uint32_t hpet_irqs);
void pc_init_ne2k_isa(ISABus *bus, NICInfo *nd);
void pc_cmos_init(PCMachineState *pcms,
                  BusState *ide0, BusState *ide1,
                  ISADevice *s);
void pc_nic_init(PCMachineClass *pcmc, ISABus *isa_bus, PCIBus *pci_bus);
void pc_pci_device_init(PCIBus *pci_bus);

typedef void (*cpu_set_smm_t)(int smm, void *arg);

void ioapic_init_gsi(GSIState *gsi_state, const char *parent_name);

ISADevice *pc_find_fdc0(void);
int cmos_get_fd_drive_type(FloppyDriveType fd0);

#define FW_CFG_IO_BASE     0x510

#define PORT92_A20_LINE "a20"

/* acpi_piix.c */

I2CBus *piix4_pm_init(PCIBus *bus, int devfn, uint32_t smb_io_base,
                      qemu_irq sci_irq, qemu_irq smi_irq,
                      int smm_enabled, DeviceState **piix4_pm);

/* hpet.c */
extern int no_hpet;

/* piix_pci.c */
struct PCII440FXState;
typedef struct PCII440FXState PCII440FXState;

#define TYPE_I440FX_PCI_HOST_BRIDGE "i440FX-pcihost"
#define TYPE_I440FX_PCI_DEVICE "i440FX"

#define TYPE_IGD_PASSTHROUGH_I440FX_PCI_DEVICE "igd-passthrough-i440FX"

/*
 * Reset Control Register: PCI-accessible ISA-Compatible Register at address
 * 0xcf9, provided by the PCI/ISA bridge (PIIX3 PCI function 0, 8086:7000).
 */
#define RCR_IOPORT 0xcf9

PCIBus *i440fx_init(const char *host_type, const char *pci_type,
                    PCII440FXState **pi440fx_state, int *piix_devfn,
                    ISABus **isa_bus, qemu_irq *pic,
                    MemoryRegion *address_space_mem,
                    MemoryRegion *address_space_io,
                    ram_addr_t ram_size,
                    ram_addr_t below_4g_mem_size,
                    ram_addr_t above_4g_mem_size,
                    MemoryRegion *pci_memory,
                    MemoryRegion *ram_memory);

/* piix4.c */
extern PCIDevice *piix4_dev;
int piix4_init(PCIBus *bus, ISABus **isa_bus, int devfn);

/* acpi-build.c */
void pc_madt_cpu_entry(AcpiDeviceIf *adev, int uid,
                       const CPUArchIdList *apic_ids, GArray *entry);

#define PC_COMPAT_2_12 \
    HW_COMPAT_2_12 \
    {\
        .driver   = TYPE_X86_CPU,\
        .property = "legacy-cache",\
        .value    = "on",\
    },{\
        .driver   = TYPE_X86_CPU,\
        .property = "topoext",\
        .value    = "off",\
    },{\
        .driver   = "EPYC-" TYPE_X86_CPU,\
        .property = "xlevel",\
        .value    = stringify(0x8000000a),\
    },{\
        .driver   = "EPYC-IBPB-" TYPE_X86_CPU,\
        .property = "xlevel",\
        .value    = stringify(0x8000000a),\
    },


#define PC_COMPAT_2_11 \
    HW_COMPAT_2_11 \
    {\
        .driver   = TYPE_X86_CPU,\
        .property = "x-migrate-smi-count",\
        .value    = "off",\
    },{\
        .driver   = "Skylake-Server" "-" TYPE_X86_CPU,\
        .property = "clflushopt",\
        .value    = "off",\
    },

#define PC_COMPAT_2_10 \
    HW_COMPAT_2_10 \
    {\
        .driver   = TYPE_X86_CPU,\
        .property = "x-hv-max-vps",\
        .value    = "0x40",\
    },{\
        .driver   = "i440FX-pcihost",\
        .property = "x-pci-hole64-fix",\
        .value    = "off",\
    },{\
        .driver   = "q35-pcihost",\
        .property = "x-pci-hole64-fix",\
        .value    = "off",\
    },

#define PC_COMPAT_2_9 \
    HW_COMPAT_2_9 \
    {\
        .driver   = "mch",\
        .property = "extended-tseg-mbytes",\
        .value    = stringify(0),\
    },\

#define PC_COMPAT_2_8 \
    HW_COMPAT_2_8 \
    {\
        .driver   = TYPE_X86_CPU,\
        .property = "tcg-cpuid",\
        .value    = "off",\
    },\
    {\
        .driver   = "kvmclock",\
        .property = "x-mach-use-reliable-get-clock",\
        .value    = "off",\
    },\
    {\
        .driver   = "ICH9-LPC",\
        .property = "x-smi-broadcast",\
        .value    = "off",\
    },\
    {\
        .driver   = TYPE_X86_CPU,\
        .property = "vmware-cpuid-freq",\
        .value    = "off",\
    },\
    {\
        .driver   = "Haswell-" TYPE_X86_CPU,\
        .property = "stepping",\
        .value    = "1",\
    },

#define PC_COMPAT_2_7 \
    HW_COMPAT_2_7 \
    {\
        .driver   = TYPE_X86_CPU,\
        .property = "l3-cache",\
        .value    = "off",\
    },\
    {\
        .driver   = TYPE_X86_CPU,\
        .property = "full-cpuid-auto-level",\
        .value    = "off",\
    },\
    {\
        .driver   = "Opteron_G3" "-" TYPE_X86_CPU,\
        .property = "family",\
        .value    = "15",\
    },\
    {\
        .driver   = "Opteron_G3" "-" TYPE_X86_CPU,\
        .property = "model",\
        .value    = "6",\
    },\
    {\
        .driver   = "Opteron_G3" "-" TYPE_X86_CPU,\
        .property = "stepping",\
        .value    = "1",\
    },\
    {\
        .driver   = "isa-pcspk",\
        .property = "migrate",\
        .value    = "off",\
    },

#define PC_COMPAT_2_6 \
    HW_COMPAT_2_6 \
    {\
        .driver   = TYPE_X86_CPU,\
        .property = "cpuid-0xb",\
        .value    = "off",\
    },{\
        .driver   = "vmxnet3",\
        .property = "romfile",\
        .value    = "",\
    },\
    {\
        .driver = TYPE_X86_CPU,\
        .property = "fill-mtrr-mask",\
        .value = "off",\
    },\
    {\
        .driver   = "apic-common",\
        .property = "legacy-instance-id",\
        .value    = "on",\
    },

#define PC_COMPAT_2_5 \
    HW_COMPAT_2_5

/* Helper for setting model-id for CPU models that changed model-id
 * depending on QEMU versions up to QEMU 2.4.
 */
#define PC_CPU_MODEL_IDS(v) \
    {\
        .driver   = "qemu32-" TYPE_X86_CPU,\
        .property = "model-id",\
        .value    = "QEMU Virtual CPU version " v,\
    },\
    {\
        .driver   = "qemu64-" TYPE_X86_CPU,\
        .property = "model-id",\
        .value    = "QEMU Virtual CPU version " v,\
    },\
    {\
        .driver   = "athlon-" TYPE_X86_CPU,\
        .property = "model-id",\
        .value    = "QEMU Virtual CPU version " v,\
    },

#define PC_COMPAT_2_4 \
    HW_COMPAT_2_4 \
    PC_CPU_MODEL_IDS("2.4.0") \
    {\
        .driver   = "Haswell-" TYPE_X86_CPU,\
        .property = "abm",\
        .value    = "off",\
    },\
    {\
        .driver   = "Haswell-noTSX-" TYPE_X86_CPU,\
        .property = "abm",\
        .value    = "off",\
    },\
    {\
        .driver   = "Broadwell-" TYPE_X86_CPU,\
        .property = "abm",\
        .value    = "off",\
    },\
    {\
        .driver   = "Broadwell-noTSX-" TYPE_X86_CPU,\
        .property = "abm",\
        .value    = "off",\
    },\
    {\
        .driver   = "host" "-" TYPE_X86_CPU,\
        .property = "host-cache-info",\
        .value    = "on",\
    },\
    {\
        .driver   = TYPE_X86_CPU,\
        .property = "check",\
        .value    = "off",\
    },\
    {\
        .driver   = "qemu64" "-" TYPE_X86_CPU,\
        .property = "sse4a",\
        .value    = "on",\
    },\
    {\
        .driver   = "qemu64" "-" TYPE_X86_CPU,\
        .property = "abm",\
        .value    = "on",\
    },\
    {\
        .driver   = "qemu64" "-" TYPE_X86_CPU,\
        .property = "popcnt",\
        .value    = "on",\
    },\
    {\
        .driver   = "qemu32" "-" TYPE_X86_CPU,\
        .property = "popcnt",\
        .value    = "on",\
    },{\
        .driver   = "Opteron_G2" "-" TYPE_X86_CPU,\
        .property = "rdtscp",\
        .value    = "on",\
    },{\
        .driver   = "Opteron_G3" "-" TYPE_X86_CPU,\
        .property = "rdtscp",\
        .value    = "on",\
    },{\
        .driver   = "Opteron_G4" "-" TYPE_X86_CPU,\
        .property = "rdtscp",\
        .value    = "on",\
    },{\
        .driver   = "Opteron_G5" "-" TYPE_X86_CPU,\
        .property = "rdtscp",\
        .value    = "on",\
    },


#define PC_COMPAT_2_3 \
    HW_COMPAT_2_3 \
    PC_CPU_MODEL_IDS("2.3.0") \
    {\
        .driver   = TYPE_X86_CPU,\
        .property = "arat",\
        .value    = "off",\
    },{\
        .driver   = "qemu64" "-" TYPE_X86_CPU,\
        .property = "min-level",\
        .value    = stringify(4),\
    },{\
        .driver   = "kvm64" "-" TYPE_X86_CPU,\
        .property = "min-level",\
        .value    = stringify(5),\
    },{\
        .driver   = "pentium3" "-" TYPE_X86_CPU,\
        .property = "min-level",\
        .value    = stringify(2),\
    },{\
        .driver   = "n270" "-" TYPE_X86_CPU,\
        .property = "min-level",\
        .value    = stringify(5),\
    },{\
        .driver   = "Conroe" "-" TYPE_X86_CPU,\
        .property = "min-level",\
        .value    = stringify(4),\
    },{\
        .driver   = "Penryn" "-" TYPE_X86_CPU,\
        .property = "min-level",\
        .value    = stringify(4),\
    },{\
        .driver   = "Nehalem" "-" TYPE_X86_CPU,\
        .property = "min-level",\
        .value    = stringify(4),\
    },{\
        .driver   = "n270" "-" TYPE_X86_CPU,\
        .property = "min-xlevel",\
        .value    = stringify(0x8000000a),\
    },{\
        .driver   = "Penryn" "-" TYPE_X86_CPU,\
        .property = "min-xlevel",\
        .value    = stringify(0x8000000a),\
    },{\
        .driver   = "Conroe" "-" TYPE_X86_CPU,\
        .property = "min-xlevel",\
        .value    = stringify(0x8000000a),\
    },{\
        .driver   = "Nehalem" "-" TYPE_X86_CPU,\
        .property = "min-xlevel",\
        .value    = stringify(0x8000000a),\
    },{\
        .driver   = "Westmere" "-" TYPE_X86_CPU,\
        .property = "min-xlevel",\
        .value    = stringify(0x8000000a),\
    },{\
        .driver   = "SandyBridge" "-" TYPE_X86_CPU,\
        .property = "min-xlevel",\
        .value    = stringify(0x8000000a),\
    },{\
        .driver   = "IvyBridge" "-" TYPE_X86_CPU,\
        .property = "min-xlevel",\
        .value    = stringify(0x8000000a),\
    },{\
        .driver   = "Haswell" "-" TYPE_X86_CPU,\
        .property = "min-xlevel",\
        .value    = stringify(0x8000000a),\
    },{\
        .driver   = "Haswell-noTSX" "-" TYPE_X86_CPU,\
        .property = "min-xlevel",\
        .value    = stringify(0x8000000a),\
    },{\
        .driver   = "Broadwell" "-" TYPE_X86_CPU,\
        .property = "min-xlevel",\
        .value    = stringify(0x8000000a),\
    },{\
        .driver   = "Broadwell-noTSX" "-" TYPE_X86_CPU,\
        .property = "min-xlevel",\
        .value    = stringify(0x8000000a),\
    },{\
        .driver = TYPE_X86_CPU,\
        .property = "kvm-no-smi-migration",\
        .value    = "on",\
    },

#define PC_COMPAT_2_2 \
    HW_COMPAT_2_2 \
    PC_CPU_MODEL_IDS("2.2.0") \
    {\
        .driver = "kvm64" "-" TYPE_X86_CPU,\
        .property = "vme",\
        .value = "off",\
    },\
    {\
        .driver = "kvm32" "-" TYPE_X86_CPU,\
        .property = "vme",\
        .value = "off",\
    },\
    {\
        .driver = "Conroe" "-" TYPE_X86_CPU,\
        .property = "vme",\
        .value = "off",\
    },\
    {\
        .driver = "Penryn" "-" TYPE_X86_CPU,\
        .property = "vme",\
        .value = "off",\
    },\
    {\
        .driver = "Nehalem" "-" TYPE_X86_CPU,\
        .property = "vme",\
        .value = "off",\
    },\
    {\
        .driver = "Westmere" "-" TYPE_X86_CPU,\
        .property = "vme",\
        .value = "off",\
    },\
    {\
        .driver = "SandyBridge" "-" TYPE_X86_CPU,\
        .property = "vme",\
        .value = "off",\
    },\
    {\
        .driver = "Haswell" "-" TYPE_X86_CPU,\
        .property = "vme",\
        .value = "off",\
    },\
    {\
        .driver = "Broadwell" "-" TYPE_X86_CPU,\
        .property = "vme",\
        .value = "off",\
    },\
    {\
        .driver = "Opteron_G1" "-" TYPE_X86_CPU,\
        .property = "vme",\
        .value = "off",\
    },\
    {\
        .driver = "Opteron_G2" "-" TYPE_X86_CPU,\
        .property = "vme",\
        .value = "off",\
    },\
    {\
        .driver = "Opteron_G3" "-" TYPE_X86_CPU,\
        .property = "vme",\
        .value = "off",\
    },\
    {\
        .driver = "Opteron_G4" "-" TYPE_X86_CPU,\
        .property = "vme",\
        .value = "off",\
    },\
    {\
        .driver = "Opteron_G5" "-" TYPE_X86_CPU,\
        .property = "vme",\
        .value = "off",\
    },\
    {\
        .driver = "Haswell" "-" TYPE_X86_CPU,\
        .property = "f16c",\
        .value = "off",\
    },\
    {\
        .driver = "Haswell" "-" TYPE_X86_CPU,\
        .property = "rdrand",\
        .value = "off",\
    },\
    {\
        .driver = "Broadwell" "-" TYPE_X86_CPU,\
        .property = "f16c",\
        .value = "off",\
    },\
    {\
        .driver = "Broadwell" "-" TYPE_X86_CPU,\
        .property = "rdrand",\
        .value = "off",\
    },

#define PC_COMPAT_2_1 \
    HW_COMPAT_2_1 \
    PC_CPU_MODEL_IDS("2.1.0") \
    {\
        .driver = "coreduo" "-" TYPE_X86_CPU,\
        .property = "vmx",\
        .value = "on",\
    },\
    {\
        .driver = "core2duo" "-" TYPE_X86_CPU,\
        .property = "vmx",\
        .value = "on",\
    },

#define PC_COMPAT_2_0 \
    PC_CPU_MODEL_IDS("2.0.0") \
    {\
        .driver   = "virtio-scsi-pci",\
        .property = "any_layout",\
        .value    = "off",\
    },{\
        .driver   = "PIIX4_PM",\
        .property = "memory-hotplug-support",\
        .value    = "off",\
    },\
    {\
        .driver   = "apic",\
        .property = "version",\
        .value    = stringify(0x11),\
    },\
    {\
        .driver   = "nec-usb-xhci",\
        .property = "superspeed-ports-first",\
        .value    = "off",\
    },\
    {\
        .driver   = "nec-usb-xhci",\
        .property = "force-pcie-endcap",\
        .value    = "on",\
    },\
    {\
        .driver   = "pci-serial",\
        .property = "prog_if",\
        .value    = stringify(0),\
    },\
    {\
        .driver   = "pci-serial-2x",\
        .property = "prog_if",\
        .value    = stringify(0),\
    },\
    {\
        .driver   = "pci-serial-4x",\
        .property = "prog_if",\
        .value    = stringify(0),\
    },\
    {\
        .driver   = "virtio-net-pci",\
        .property = "guest_announce",\
        .value    = "off",\
    },\
    {\
        .driver   = "ICH9-LPC",\
        .property = "memory-hotplug-support",\
        .value    = "off",\
    },{\
        .driver   = "xio3130-downstream",\
        .property = COMPAT_PROP_PCP,\
        .value    = "off",\
    },{\
        .driver   = "ioh3420",\
        .property = COMPAT_PROP_PCP,\
        .value    = "off",\
    },

#define PC_COMPAT_1_7 \
    PC_CPU_MODEL_IDS("1.7.0") \
    {\
        .driver   = TYPE_USB_DEVICE,\
        .property = "msos-desc",\
        .value    = "no",\
    },\
    {\
        .driver   = "PIIX4_PM",\
        .property = "acpi-pci-hotplug-with-bridge-support",\
        .value    = "off",\
    },\
    {\
        .driver   = "hpet",\
        .property = HPET_INTCAP,\
        .value    = stringify(4),\
    },

#define PC_COMPAT_1_6 \
    PC_CPU_MODEL_IDS("1.6.0") \
    {\
        .driver   = "e1000",\
        .property = "mitigation",\
        .value    = "off",\
    },{\
        .driver   = "qemu64-" TYPE_X86_CPU,\
        .property = "model",\
        .value    = stringify(2),\
    },{\
        .driver   = "qemu32-" TYPE_X86_CPU,\
        .property = "model",\
        .value    = stringify(3),\
    },{\
        .driver   = "i440FX-pcihost",\
        .property = "short_root_bus",\
        .value    = stringify(1),\
    },{\
        .driver   = "q35-pcihost",\
        .property = "short_root_bus",\
        .value    = stringify(1),\
    },

#define PC_COMPAT_1_5 \
    PC_CPU_MODEL_IDS("1.5.0") \
    {\
        .driver   = "Conroe-" TYPE_X86_CPU,\
        .property = "model",\
        .value    = stringify(2),\
    },{\
        .driver   = "Conroe-" TYPE_X86_CPU,\
        .property = "min-level",\
        .value    = stringify(2),\
    },{\
        .driver   = "Penryn-" TYPE_X86_CPU,\
        .property = "model",\
        .value    = stringify(2),\
    },{\
        .driver   = "Penryn-" TYPE_X86_CPU,\
        .property = "min-level",\
        .value    = stringify(2),\
    },{\
        .driver   = "Nehalem-" TYPE_X86_CPU,\
        .property = "model",\
        .value    = stringify(2),\
    },{\
        .driver   = "Nehalem-" TYPE_X86_CPU,\
        .property = "min-level",\
        .value    = stringify(2),\
    },{\
        .driver   = "virtio-net-pci",\
        .property = "any_layout",\
        .value    = "off",\
    },{\
        .driver = TYPE_X86_CPU,\
        .property = "pmu",\
        .value = "on",\
    },{\
        .driver   = "i440FX-pcihost",\
        .property = "short_root_bus",\
        .value    = stringify(0),\
    },{\
        .driver   = "q35-pcihost",\
        .property = "short_root_bus",\
        .value    = stringify(0),\
    },

#define PC_COMPAT_1_4 \
    PC_CPU_MODEL_IDS("1.4.0") \
    {\
        .driver   = "scsi-hd",\
        .property = "discard_granularity",\
        .value    = stringify(0),\
    },{\
        .driver   = "scsi-cd",\
        .property = "discard_granularity",\
        .value    = stringify(0),\
    },{\
        .driver   = "scsi-disk",\
        .property = "discard_granularity",\
        .value    = stringify(0),\
    },{\
        .driver   = "ide-hd",\
        .property = "discard_granularity",\
        .value    = stringify(0),\
    },{\
        .driver   = "ide-cd",\
        .property = "discard_granularity",\
        .value    = stringify(0),\
    },{\
        .driver   = "ide-drive",\
        .property = "discard_granularity",\
        .value    = stringify(0),\
    },{\
        .driver   = "virtio-blk-pci",\
        .property = "discard_granularity",\
        .value    = stringify(0),\
    },{\
        .driver   = "virtio-serial-pci",\
        .property = "vectors",\
        /* DEV_NVECTORS_UNSPECIFIED as a uint32_t string */\
        .value    = stringify(0xFFFFFFFF),\
    },{ \
        .driver   = "virtio-net-pci", \
        .property = "ctrl_guest_offloads", \
        .value    = "off", \
    },{\
        .driver   = "e1000",\
        .property = "romfile",\
        .value    = "pxe-e1000.rom",\
    },{\
        .driver   = "ne2k_pci",\
        .property = "romfile",\
        .value    = "pxe-ne2k_pci.rom",\
    },{\
        .driver   = "pcnet",\
        .property = "romfile",\
        .value    = "pxe-pcnet.rom",\
    },{\
        .driver   = "rtl8139",\
        .property = "romfile",\
        .value    = "pxe-rtl8139.rom",\
    },{\
        .driver   = "virtio-net-pci",\
        .property = "romfile",\
        .value    = "pxe-virtio.rom",\
    },{\
        .driver   = "486-" TYPE_X86_CPU,\
        .property = "model",\
        .value    = stringify(0),\
    },\
    {\
        .driver = "n270" "-" TYPE_X86_CPU,\
        .property = "movbe",\
        .value = "off",\
    },\
    {\
        .driver = "Westmere" "-" TYPE_X86_CPU,\
        .property = "pclmulqdq",\
        .value = "off",\
    },

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

extern void igd_passthrough_isa_bridge_create(PCIBus *bus, uint16_t gpu_dev_id);
#endif
