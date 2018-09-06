/* Support for generating ACPI tables and passing them to Guests
 *
 * Copyright (C) 2008-2010  Kevin O'Connor <kevin@koconnor.net>
 * Copyright (C) 2006 Fabrice Bellard
 * Copyright (C) 2013 Red Hat Inc
 *
 * Author: Michael S. Tsirkin <mst@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qmp/qnum.h"
#include "acpi-build.h"
#include "qemu-common.h"
#include "qemu/bitmap.h"
#include "qemu/error-report.h"
#include "hw/pci/pci.h"
#include "qom/cpu.h"
#include "target/i386/cpu.h"
#include "hw/misc/pvpanic.h"
#include "hw/timer/hpet.h"
#include "hw/acpi/acpi-defs.h"
#include "hw/acpi/acpi.h"
#include "hw/acpi/cpu.h"
#include "hw/nvram/fw_cfg.h"
#include "hw/acpi/bios-linker-loader.h"
#include "hw/loader.h"
#include "hw/isa/isa.h"
#include "hw/block/fdc.h"
#include "hw/acpi/memory_hotplug.h"
#include "sysemu/tpm.h"
#include "hw/acpi/tpm.h"
#include "hw/acpi/vmgenid.h"
#include "sysemu/tpm_backend.h"
#include "hw/timer/mc146818rtc_regs.h"
#include "hw/mem/memory-device.h"
#include "sysemu/numa.h"

/* Supported chipsets: */
#include "hw/acpi/piix4.h"
#include "hw/acpi/pcihp.h"
#include "hw/i386/ich9.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci-host/q35.h"
#include "hw/i386/x86-iommu.h"

#include "hw/acpi/aml-build.h"

#include "qom/qom-qobject.h"
#include "hw/i386/amd_iommu.h"
#include "hw/i386/intel_iommu.h"

#include "hw/acpi/ipmi.h"

/* These are used to size the ACPI tables for -M pc-i440fx-1.7 and
 * -M pc-i440fx-2.0.  Even if the actual amount of AML generated grows
 * a little bit, there should be plenty of free space since the DSDT
 * shrunk by ~1.5k between QEMU 2.0 and QEMU 2.1.
 */
#define ACPI_BUILD_LEGACY_CPU_AML_SIZE    97
#define ACPI_BUILD_ALIGN_SIZE             0x1000

#define ACPI_BUILD_TABLE_SIZE             0x20000

/* #define DEBUG_ACPI_BUILD */
#ifdef DEBUG_ACPI_BUILD
#define ACPI_BUILD_DPRINTF(fmt, ...)        \
    do {printf("ACPI_BUILD: " fmt, ## __VA_ARGS__); } while (0)
#else
#define ACPI_BUILD_DPRINTF(fmt, ...)
#endif

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
    bool is_piix4;
    bool has_hpet;
    TPMVersion tpm_version;
    const unsigned char *dsdt_code;
    unsigned dsdt_size;
    uint16_t pvpanic_port;
    uint16_t applesmc_io_base;
} AcpiMiscInfo;

typedef struct AcpiBuildPciBusHotplugState {
    GArray *device_table;
    GArray *notify_table;
    struct AcpiBuildPciBusHotplugState *parent;
    bool pcihp_bridge_en;
} AcpiBuildPciBusHotplugState;

static void init_common_fadt_data(Object *o, AcpiFadtData *data)
{
    uint32_t io = object_property_get_uint(o, ACPI_PM_PROP_PM_IO_BASE, NULL);
    AmlAddressSpace as = AML_AS_SYSTEM_IO;
    AcpiFadtData fadt = {
        .rev = 3,
        .flags =
            (1 << ACPI_FADT_F_WBINVD) |
            (1 << ACPI_FADT_F_PROC_C1) |
            (1 << ACPI_FADT_F_SLP_BUTTON) |
            (1 << ACPI_FADT_F_RTC_S4) |
            (1 << ACPI_FADT_F_USE_PLATFORM_CLOCK) |
            /* APIC destination mode ("Flat Logical") has an upper limit of 8
             * CPUs for more than 8 CPUs, "Clustered Logical" mode has to be
             * used
             */
            ((max_cpus > 8) ? (1 << ACPI_FADT_F_FORCE_APIC_CLUSTER_MODEL) : 0),
        .int_model = 1 /* Multiple APIC */,
        .rtc_century = RTC_CENTURY,
        .plvl2_lat = 0xfff /* C2 state not supported */,
        .plvl3_lat = 0xfff /* C3 state not supported */,
        .smi_cmd = ACPI_PORT_SMI_CMD,
        .sci_int = object_property_get_uint(o, ACPI_PM_PROP_SCI_INT, NULL),
        .acpi_enable_cmd =
            object_property_get_uint(o, ACPI_PM_PROP_ACPI_ENABLE_CMD, NULL),
        .acpi_disable_cmd =
            object_property_get_uint(o, ACPI_PM_PROP_ACPI_DISABLE_CMD, NULL),
        .pm1a_evt = { .space_id = as, .bit_width = 4 * 8, .address = io },
        .pm1a_cnt = { .space_id = as, .bit_width = 2 * 8,
                      .address = io + 0x04 },
        .pm_tmr = { .space_id = as, .bit_width = 4 * 8, .address = io + 0x08 },
        .gpe0_blk = { .space_id = as, .bit_width =
            object_property_get_uint(o, ACPI_PM_PROP_GPE0_BLK_LEN, NULL) * 8,
            .address = object_property_get_uint(o, ACPI_PM_PROP_GPE0_BLK, NULL)
        },
    };
    *data = fadt;
}

static void acpi_get_pm_info(AcpiPmInfo *pm)
{
    Object *piix = piix4_pm_find();
    Object *lpc = ich9_lpc_find();
    Object *obj = piix ? piix : lpc;
    QObject *o;
    pm->cpu_hp_io_base = 0;
    pm->pcihp_io_base = 0;
    pm->pcihp_io_len = 0;

    init_common_fadt_data(obj, &pm->fadt);
    if (piix) {
        /* w2k requires FADT(rev1) or it won't boot, keep PC compatible */
        pm->fadt.rev = 1;
        pm->cpu_hp_io_base = PIIX4_CPU_HOTPLUG_IO_BASE;
        pm->pcihp_io_base =
            object_property_get_uint(obj, ACPI_PCIHP_IO_BASE_PROP, NULL);
        pm->pcihp_io_len =
            object_property_get_uint(obj, ACPI_PCIHP_IO_LEN_PROP, NULL);
    }
    if (lpc) {
        struct AcpiGenericAddress r = { .space_id = AML_AS_SYSTEM_IO,
            .bit_width = 8, .address = ICH9_RST_CNT_IOPORT };
        pm->fadt.reset_reg = r;
        pm->fadt.reset_val = 0xf;
        pm->fadt.flags |= 1 << ACPI_FADT_F_RESET_REG_SUP;
        pm->cpu_hp_io_base = ICH9_CPU_HOTPLUG_IO_BASE;
    }
    assert(obj);

    /* The above need not be conditional on machine type because the reset port
     * happens to be the same on PIIX (pc) and ICH9 (q35). */
    QEMU_BUILD_BUG_ON(ICH9_RST_CNT_IOPORT != RCR_IOPORT);

    /* Fill in optional s3/s4 related properties */
    o = object_property_get_qobject(obj, ACPI_PM_PROP_S3_DISABLED, NULL);
    if (o) {
        pm->s3_disabled = qnum_get_uint(qobject_to(QNum, o));
    } else {
        pm->s3_disabled = false;
    }
    qobject_unref(o);
    o = object_property_get_qobject(obj, ACPI_PM_PROP_S4_DISABLED, NULL);
    if (o) {
        pm->s4_disabled = qnum_get_uint(qobject_to(QNum, o));
    } else {
        pm->s4_disabled = false;
    }
    qobject_unref(o);
    o = object_property_get_qobject(obj, ACPI_PM_PROP_S4_VAL, NULL);
    if (o) {
        pm->s4_val = qnum_get_uint(qobject_to(QNum, o));
    } else {
        pm->s4_val = false;
    }
    qobject_unref(o);

    pm->pcihp_bridge_en =
        object_property_get_bool(obj, "acpi-pci-hotplug-with-bridge-support",
                                 NULL);
}

static void acpi_get_misc_info(AcpiMiscInfo *info)
{
    Object *piix = piix4_pm_find();
    Object *lpc = ich9_lpc_find();
    assert(!!piix != !!lpc);

    if (piix) {
        info->is_piix4 = true;
    }
    if (lpc) {
        info->is_piix4 = false;
    }

    info->has_hpet = hpet_find();
    info->tpm_version = tpm_get_version(tpm_find());
    info->pvpanic_port = pvpanic_port();
    info->applesmc_io_base = applesmc_port();
}

/* FACS */
static void
build_facs(GArray *table_data, BIOSLinker *linker)
{
    AcpiFacsDescriptorRev1 *facs = acpi_data_push(table_data, sizeof *facs);
    memcpy(&facs->signature, "FACS", 4);
    facs->length = cpu_to_le32(sizeof(*facs));
}

void pc_madt_cpu_entry(AcpiDeviceIf *adev, int uid,
                       const CPUArchIdList *apic_ids, GArray *entry)
{
    uint32_t apic_id = apic_ids->cpus[uid].arch_id;

    /* ACPI spec says that LAPIC entry for non present
     * CPU may be omitted from MADT or it must be marked
     * as disabled. However omitting non present CPU from
     * MADT breaks hotplug on linux. So possible CPUs
     * should be put in MADT but kept disabled.
     */
    if (apic_id < 255) {
        AcpiMadtProcessorApic *apic = acpi_data_push(entry, sizeof *apic);

        apic->type = ACPI_APIC_PROCESSOR;
        apic->length = sizeof(*apic);
        apic->processor_id = uid;
        apic->local_apic_id = apic_id;
        if (apic_ids->cpus[uid].cpu != NULL) {
            apic->flags = cpu_to_le32(1);
        } else {
            apic->flags = cpu_to_le32(0);
        }
    } else {
        AcpiMadtProcessorX2Apic *apic = acpi_data_push(entry, sizeof *apic);

        apic->type = ACPI_APIC_LOCAL_X2APIC;
        apic->length = sizeof(*apic);
        apic->uid = cpu_to_le32(uid);
        apic->x2apic_id = cpu_to_le32(apic_id);
        if (apic_ids->cpus[uid].cpu != NULL) {
            apic->flags = cpu_to_le32(1);
        } else {
            apic->flags = cpu_to_le32(0);
        }
    }
}

static void build_hpet_aml(Aml *table)
{
    Aml *crs;
    Aml *field;
    Aml *method;
    Aml *if_ctx;
    Aml *scope = aml_scope("_SB");
    Aml *dev = aml_device("HPET");
    Aml *zero = aml_int(0);
    Aml *id = aml_local(0);
    Aml *period = aml_local(1);

    aml_append(dev, aml_name_decl("_HID", aml_eisaid("PNP0103")));
    aml_append(dev, aml_name_decl("_UID", zero));

    aml_append(dev,
        aml_operation_region("HPTM", AML_SYSTEM_MEMORY, aml_int(HPET_BASE),
                             HPET_LEN));
    field = aml_field("HPTM", AML_DWORD_ACC, AML_LOCK, AML_PRESERVE);
    aml_append(field, aml_named_field("VEND", 32));
    aml_append(field, aml_named_field("PRD", 32));
    aml_append(dev, field);

    method = aml_method("_STA", 0, AML_NOTSERIALIZED);
    aml_append(method, aml_store(aml_name("VEND"), id));
    aml_append(method, aml_store(aml_name("PRD"), period));
    aml_append(method, aml_shiftright(id, aml_int(16), id));
    if_ctx = aml_if(aml_lor(aml_equal(id, zero),
                            aml_equal(id, aml_int(0xffff))));
    {
        aml_append(if_ctx, aml_return(zero));
    }
    aml_append(method, if_ctx);

    if_ctx = aml_if(aml_lor(aml_equal(period, zero),
                            aml_lgreater(period, aml_int(100000000))));
    {
        aml_append(if_ctx, aml_return(zero));
    }
    aml_append(method, if_ctx);

    aml_append(method, aml_return(aml_int(0x0F)));
    aml_append(dev, method);

    crs = aml_resource_template();
    aml_append(crs, aml_memory32_fixed(HPET_BASE, HPET_LEN, AML_READ_ONLY));
    aml_append(dev, aml_name_decl("_CRS", crs));

    aml_append(scope, dev);
    aml_append(table, scope);
}

static Aml *build_fdinfo_aml(int idx, FloppyDriveType type)
{
    Aml *dev, *fdi;
    uint8_t maxc, maxh, maxs;

    isa_fdc_get_drive_max_chs(type, &maxc, &maxh, &maxs);

    dev = aml_device("FLP%c", 'A' + idx);

    aml_append(dev, aml_name_decl("_ADR", aml_int(idx)));

    fdi = aml_package(16);
    aml_append(fdi, aml_int(idx));  /* Drive Number */
    aml_append(fdi,
        aml_int(cmos_get_fd_drive_type(type)));  /* Device Type */
    /*
     * the values below are the limits of the drive, and are thus independent
     * of the inserted media
     */
    aml_append(fdi, aml_int(maxc));  /* Maximum Cylinder Number */
    aml_append(fdi, aml_int(maxs));  /* Maximum Sector Number */
    aml_append(fdi, aml_int(maxh));  /* Maximum Head Number */
    /*
     * SeaBIOS returns the below values for int 0x13 func 0x08 regardless of
     * the drive type, so shall we
     */
    aml_append(fdi, aml_int(0xAF));  /* disk_specify_1 */
    aml_append(fdi, aml_int(0x02));  /* disk_specify_2 */
    aml_append(fdi, aml_int(0x25));  /* disk_motor_wait */
    aml_append(fdi, aml_int(0x02));  /* disk_sector_siz */
    aml_append(fdi, aml_int(0x12));  /* disk_eot */
    aml_append(fdi, aml_int(0x1B));  /* disk_rw_gap */
    aml_append(fdi, aml_int(0xFF));  /* disk_dtl */
    aml_append(fdi, aml_int(0x6C));  /* disk_formt_gap */
    aml_append(fdi, aml_int(0xF6));  /* disk_fill */
    aml_append(fdi, aml_int(0x0F));  /* disk_head_sttl */
    aml_append(fdi, aml_int(0x08));  /* disk_motor_strt */

    aml_append(dev, aml_name_decl("_FDI", fdi));
    return dev;
}

static Aml *build_fdc_device_aml(ISADevice *fdc)
{
    int i;
    Aml *dev;
    Aml *crs;

#define ACPI_FDE_MAX_FD 4
    uint32_t fde_buf[5] = {
        0, 0, 0, 0,     /* presence of floppy drives #0 - #3 */
        cpu_to_le32(2)  /* tape presence (2 == never present) */
    };

    dev = aml_device("FDC0");
    aml_append(dev, aml_name_decl("_HID", aml_eisaid("PNP0700")));

    crs = aml_resource_template();
    aml_append(crs, aml_io(AML_DECODE16, 0x03F2, 0x03F2, 0x00, 0x04));
    aml_append(crs, aml_io(AML_DECODE16, 0x03F7, 0x03F7, 0x00, 0x01));
    aml_append(crs, aml_irq_no_flags(6));
    aml_append(crs,
        aml_dma(AML_COMPATIBILITY, AML_NOTBUSMASTER, AML_TRANSFER8, 2));
    aml_append(dev, aml_name_decl("_CRS", crs));

    for (i = 0; i < MIN(MAX_FD, ACPI_FDE_MAX_FD); i++) {
        FloppyDriveType type = isa_fdc_get_drive_type(fdc, i);

        if (type < FLOPPY_DRIVE_TYPE_NONE) {
            fde_buf[i] = cpu_to_le32(1);  /* drive present */
            aml_append(dev, build_fdinfo_aml(i, type));
        }
    }
    aml_append(dev, aml_name_decl("_FDE",
               aml_buffer(sizeof(fde_buf), (uint8_t *)fde_buf)));

    return dev;
}

static Aml *build_rtc_device_aml(void)
{
    Aml *dev;
    Aml *crs;

    dev = aml_device("RTC");
    aml_append(dev, aml_name_decl("_HID", aml_eisaid("PNP0B00")));
    crs = aml_resource_template();
    aml_append(crs, aml_io(AML_DECODE16, 0x0070, 0x0070, 0x10, 0x02));
    aml_append(crs, aml_irq_no_flags(8));
    aml_append(crs, aml_io(AML_DECODE16, 0x0072, 0x0072, 0x02, 0x06));
    aml_append(dev, aml_name_decl("_CRS", crs));

    return dev;
}

static Aml *build_kbd_device_aml(void)
{
    Aml *dev;
    Aml *crs;
    Aml *method;

    dev = aml_device("KBD");
    aml_append(dev, aml_name_decl("_HID", aml_eisaid("PNP0303")));

    method = aml_method("_STA", 0, AML_NOTSERIALIZED);
    aml_append(method, aml_return(aml_int(0x0f)));
    aml_append(dev, method);

    crs = aml_resource_template();
    aml_append(crs, aml_io(AML_DECODE16, 0x0060, 0x0060, 0x01, 0x01));
    aml_append(crs, aml_io(AML_DECODE16, 0x0064, 0x0064, 0x01, 0x01));
    aml_append(crs, aml_irq_no_flags(1));
    aml_append(dev, aml_name_decl("_CRS", crs));

    return dev;
}

static Aml *build_mouse_device_aml(void)
{
    Aml *dev;
    Aml *crs;
    Aml *method;

    dev = aml_device("MOU");
    aml_append(dev, aml_name_decl("_HID", aml_eisaid("PNP0F13")));

    method = aml_method("_STA", 0, AML_NOTSERIALIZED);
    aml_append(method, aml_return(aml_int(0x0f)));
    aml_append(dev, method);

    crs = aml_resource_template();
    aml_append(crs, aml_irq_no_flags(12));
    aml_append(dev, aml_name_decl("_CRS", crs));

    return dev;
}

static Aml *build_lpt_device_aml(void)
{
    Aml *dev;
    Aml *crs;
    Aml *method;
    Aml *if_ctx;
    Aml *else_ctx;
    Aml *zero = aml_int(0);
    Aml *is_present = aml_local(0);

    dev = aml_device("LPT");
    aml_append(dev, aml_name_decl("_HID", aml_eisaid("PNP0400")));

    method = aml_method("_STA", 0, AML_NOTSERIALIZED);
    aml_append(method, aml_store(aml_name("LPEN"), is_present));
    if_ctx = aml_if(aml_equal(is_present, zero));
    {
        aml_append(if_ctx, aml_return(aml_int(0x00)));
    }
    aml_append(method, if_ctx);
    else_ctx = aml_else();
    {
        aml_append(else_ctx, aml_return(aml_int(0x0f)));
    }
    aml_append(method, else_ctx);
    aml_append(dev, method);

    crs = aml_resource_template();
    aml_append(crs, aml_io(AML_DECODE16, 0x0378, 0x0378, 0x08, 0x08));
    aml_append(crs, aml_irq_no_flags(7));
    aml_append(dev, aml_name_decl("_CRS", crs));

    return dev;
}

static Aml *build_com_device_aml(uint8_t uid)
{
    Aml *dev;
    Aml *crs;
    Aml *method;
    Aml *if_ctx;
    Aml *else_ctx;
    Aml *zero = aml_int(0);
    Aml *is_present = aml_local(0);
    const char *enabled_field = "CAEN";
    uint8_t irq = 4;
    uint16_t io_port = 0x03F8;

    assert(uid == 1 || uid == 2);
    if (uid == 2) {
        enabled_field = "CBEN";
        irq = 3;
        io_port = 0x02F8;
    }

    dev = aml_device("COM%d", uid);
    aml_append(dev, aml_name_decl("_HID", aml_eisaid("PNP0501")));
    aml_append(dev, aml_name_decl("_UID", aml_int(uid)));

    method = aml_method("_STA", 0, AML_NOTSERIALIZED);
    aml_append(method, aml_store(aml_name("%s", enabled_field), is_present));
    if_ctx = aml_if(aml_equal(is_present, zero));
    {
        aml_append(if_ctx, aml_return(aml_int(0x00)));
    }
    aml_append(method, if_ctx);
    else_ctx = aml_else();
    {
        aml_append(else_ctx, aml_return(aml_int(0x0f)));
    }
    aml_append(method, else_ctx);
    aml_append(dev, method);

    crs = aml_resource_template();
    aml_append(crs, aml_io(AML_DECODE16, io_port, io_port, 0x00, 0x08));
    aml_append(crs, aml_irq_no_flags(irq));
    aml_append(dev, aml_name_decl("_CRS", crs));

    return dev;
}

static void build_isa_devices_aml(Aml *table)
{
    ISADevice *fdc = pc_find_fdc0();
    bool ambiguous;

    Aml *scope = aml_scope("_SB.PCI0.ISA");
    Object *obj = object_resolve_path_type("", TYPE_ISA_BUS, &ambiguous);

    aml_append(scope, build_rtc_device_aml());
    aml_append(scope, build_kbd_device_aml());
    aml_append(scope, build_mouse_device_aml());
    if (fdc) {
        aml_append(scope, build_fdc_device_aml(fdc));
    }
    aml_append(scope, build_lpt_device_aml());
    aml_append(scope, build_com_device_aml(1));
    aml_append(scope, build_com_device_aml(2));

    if (ambiguous) {
        error_report("Multiple ISA busses, unable to define IPMI ACPI data");
    } else if (!obj) {
        error_report("No ISA bus, unable to define IPMI ACPI data");
    } else {
        build_acpi_ipmi_devices(scope, BUS(obj));
    }

    aml_append(table, scope);
}

static void build_dbg_aml(Aml *table)
{
    Aml *field;
    Aml *method;
    Aml *while_ctx;
    Aml *scope = aml_scope("\\");
    Aml *buf = aml_local(0);
    Aml *len = aml_local(1);
    Aml *idx = aml_local(2);

    aml_append(scope,
       aml_operation_region("DBG", AML_SYSTEM_IO, aml_int(0x0402), 0x01));
    field = aml_field("DBG", AML_BYTE_ACC, AML_NOLOCK, AML_PRESERVE);
    aml_append(field, aml_named_field("DBGB", 8));
    aml_append(scope, field);

    method = aml_method("DBUG", 1, AML_NOTSERIALIZED);

    aml_append(method, aml_to_hexstring(aml_arg(0), buf));
    aml_append(method, aml_to_buffer(buf, buf));
    aml_append(method, aml_subtract(aml_sizeof(buf), aml_int(1), len));
    aml_append(method, aml_store(aml_int(0), idx));

    while_ctx = aml_while(aml_lless(idx, len));
    aml_append(while_ctx,
        aml_store(aml_derefof(aml_index(buf, idx)), aml_name("DBGB")));
    aml_append(while_ctx, aml_increment(idx));
    aml_append(method, while_ctx);

    aml_append(method, aml_store(aml_int(0x0A), aml_name("DBGB")));
    aml_append(scope, method);

    aml_append(table, scope);
}

static Aml *build_link_dev(const char *name, uint8_t uid, Aml *reg)
{
    Aml *dev;
    Aml *crs;
    Aml *method;
    uint32_t irqs[] = {5, 10, 11};

    dev = aml_device("%s", name);
    aml_append(dev, aml_name_decl("_HID", aml_eisaid("PNP0C0F")));
    aml_append(dev, aml_name_decl("_UID", aml_int(uid)));

    crs = aml_resource_template();
    aml_append(crs, aml_interrupt(AML_CONSUMER, AML_LEVEL, AML_ACTIVE_HIGH,
                                  AML_SHARED, irqs, ARRAY_SIZE(irqs)));
    aml_append(dev, aml_name_decl("_PRS", crs));

    method = aml_method("_STA", 0, AML_NOTSERIALIZED);
    aml_append(method, aml_return(aml_call1("IQST", reg)));
    aml_append(dev, method);

    method = aml_method("_DIS", 0, AML_NOTSERIALIZED);
    aml_append(method, aml_or(reg, aml_int(0x80), reg));
    aml_append(dev, method);

    method = aml_method("_CRS", 0, AML_NOTSERIALIZED);
    aml_append(method, aml_return(aml_call1("IQCR", reg)));
    aml_append(dev, method);

    method = aml_method("_SRS", 1, AML_NOTSERIALIZED);
    aml_append(method, aml_create_dword_field(aml_arg(0), aml_int(5), "PRRI"));
    aml_append(method, aml_store(aml_name("PRRI"), reg));
    aml_append(dev, method);

    return dev;
 }

/* _CRS method - get current settings */
static Aml *build_iqcr_method(bool is_piix4)
{
    Aml *if_ctx;
    uint32_t irqs;
    Aml *method = aml_method("IQCR", 1, AML_SERIALIZED);
    Aml *crs = aml_resource_template();

    irqs = 0;
    aml_append(crs, aml_interrupt(AML_CONSUMER, AML_LEVEL,
                                  AML_ACTIVE_HIGH, AML_SHARED, &irqs, 1));
    aml_append(method, aml_name_decl("PRR0", crs));

    aml_append(method,
        aml_create_dword_field(aml_name("PRR0"), aml_int(5), "PRRI"));

    if (is_piix4) {
        if_ctx = aml_if(aml_lless(aml_arg(0), aml_int(0x80)));
        aml_append(if_ctx, aml_store(aml_arg(0), aml_name("PRRI")));
        aml_append(method, if_ctx);
    } else {
        aml_append(method,
            aml_store(aml_and(aml_arg(0), aml_int(0xF), NULL),
                      aml_name("PRRI")));
    }

    aml_append(method, aml_return(aml_name("PRR0")));
    return method;
}

/* _STA method - get status */
static Aml *build_irq_status_method(void)
{
    Aml *if_ctx;
    Aml *method = aml_method("IQST", 1, AML_NOTSERIALIZED);

    if_ctx = aml_if(aml_and(aml_int(0x80), aml_arg(0), NULL));
    aml_append(if_ctx, aml_return(aml_int(0x09)));
    aml_append(method, if_ctx);
    aml_append(method, aml_return(aml_int(0x0B)));
    return method;
}

static void build_piix4_pci0_int(Aml *table)
{
    Aml *dev;
    Aml *crs;
    Aml *field;
    Aml *method;
    uint32_t irqs;
    Aml *sb_scope = aml_scope("_SB");
    Aml *pci0_scope = aml_scope("PCI0");

    aml_append(pci0_scope, build_prt(true));
    aml_append(sb_scope, pci0_scope);

    field = aml_field("PCI0.ISA.P40C", AML_BYTE_ACC, AML_NOLOCK, AML_PRESERVE);
    aml_append(field, aml_named_field("PRQ0", 8));
    aml_append(field, aml_named_field("PRQ1", 8));
    aml_append(field, aml_named_field("PRQ2", 8));
    aml_append(field, aml_named_field("PRQ3", 8));
    aml_append(sb_scope, field);

    aml_append(sb_scope, build_irq_status_method());
    aml_append(sb_scope, build_iqcr_method(true));

    aml_append(sb_scope, build_link_dev("LNKA", 0, aml_name("PRQ0")));
    aml_append(sb_scope, build_link_dev("LNKB", 1, aml_name("PRQ1")));
    aml_append(sb_scope, build_link_dev("LNKC", 2, aml_name("PRQ2")));
    aml_append(sb_scope, build_link_dev("LNKD", 3, aml_name("PRQ3")));

    dev = aml_device("LNKS");
    {
        aml_append(dev, aml_name_decl("_HID", aml_eisaid("PNP0C0F")));
        aml_append(dev, aml_name_decl("_UID", aml_int(4)));

        crs = aml_resource_template();
        irqs = 9;
        aml_append(crs, aml_interrupt(AML_CONSUMER, AML_LEVEL,
                                      AML_ACTIVE_HIGH, AML_SHARED,
                                      &irqs, 1));
        aml_append(dev, aml_name_decl("_PRS", crs));

        /* The SCI cannot be disabled and is always attached to GSI 9,
         * so these are no-ops.  We only need this link to override the
         * polarity to active high and match the content of the MADT.
         */
        method = aml_method("_STA", 0, AML_NOTSERIALIZED);
        aml_append(method, aml_return(aml_int(0x0b)));
        aml_append(dev, method);

        method = aml_method("_DIS", 0, AML_NOTSERIALIZED);
        aml_append(dev, method);

        method = aml_method("_CRS", 0, AML_NOTSERIALIZED);
        aml_append(method, aml_return(aml_name("_PRS")));
        aml_append(dev, method);

        method = aml_method("_SRS", 1, AML_NOTSERIALIZED);
        aml_append(dev, method);
    }
    aml_append(sb_scope, dev);

    aml_append(table, sb_scope);
}

static void append_q35_prt_entry(Aml *ctx, uint32_t nr, const char *name)
{
    int i;
    int head;
    Aml *pkg;
    char base = name[3] < 'E' ? 'A' : 'E';
    char *s = g_strdup(name);
    Aml *a_nr = aml_int((nr << 16) | 0xffff);

    assert(strlen(s) == 4);

    head = name[3] - base;
    for (i = 0; i < 4; i++) {
        if (head + i > 3) {
            head = i * -1;
        }
        s[3] = base + head + i;
        pkg = aml_package(4);
        aml_append(pkg, a_nr);
        aml_append(pkg, aml_int(i));
        aml_append(pkg, aml_name("%s", s));
        aml_append(pkg, aml_int(0));
        aml_append(ctx, pkg);
    }
    g_free(s);
}

static Aml *build_q35_routing_table(const char *str)
{
    int i;
    Aml *pkg;
    char *name = g_strdup_printf("%s ", str);

    pkg = aml_package(128);
    for (i = 0; i < 0x18; i++) {
            name[3] = 'E' + (i & 0x3);
            append_q35_prt_entry(pkg, i, name);
    }

    name[3] = 'E';
    append_q35_prt_entry(pkg, 0x18, name);

    /* INTA -> PIRQA for slot 25 - 31, see the default value of D<N>IR */
    for (i = 0x0019; i < 0x1e; i++) {
        name[3] = 'A';
        append_q35_prt_entry(pkg, i, name);
    }

    /* PCIe->PCI bridge. use PIRQ[E-H] */
    name[3] = 'E';
    append_q35_prt_entry(pkg, 0x1e, name);
    name[3] = 'A';
    append_q35_prt_entry(pkg, 0x1f, name);

    g_free(name);
    return pkg;
}

static void build_q35_pci0_int(Aml *table)
{
    Aml *field;
    Aml *method;
    Aml *sb_scope = aml_scope("_SB");
    Aml *pci0_scope = aml_scope("PCI0");

    /* Zero => PIC mode, One => APIC Mode */
    aml_append(table, aml_name_decl("PICF", aml_int(0)));
    method = aml_method("_PIC", 1, AML_NOTSERIALIZED);
    {
        aml_append(method, aml_store(aml_arg(0), aml_name("PICF")));
    }
    aml_append(table, method);

    aml_append(pci0_scope,
        aml_name_decl("PRTP", build_q35_routing_table("LNK")));
    aml_append(pci0_scope,
        aml_name_decl("PRTA", build_q35_routing_table("GSI")));

    method = aml_method("_PRT", 0, AML_NOTSERIALIZED);
    {
        Aml *if_ctx;
        Aml *else_ctx;

        /* PCI IRQ routing table, example from ACPI 2.0a specification,
           section 6.2.8.1 */
        /* Note: we provide the same info as the PCI routing
           table of the Bochs BIOS */
        if_ctx = aml_if(aml_equal(aml_name("PICF"), aml_int(0)));
        aml_append(if_ctx, aml_return(aml_name("PRTP")));
        aml_append(method, if_ctx);
        else_ctx = aml_else();
        aml_append(else_ctx, aml_return(aml_name("PRTA")));
        aml_append(method, else_ctx);
    }
    aml_append(pci0_scope, method);
    aml_append(sb_scope, pci0_scope);

    field = aml_field("PCI0.ISA.PIRQ", AML_BYTE_ACC, AML_NOLOCK, AML_PRESERVE);
    aml_append(field, aml_named_field("PRQA", 8));
    aml_append(field, aml_named_field("PRQB", 8));
    aml_append(field, aml_named_field("PRQC", 8));
    aml_append(field, aml_named_field("PRQD", 8));
    aml_append(field, aml_reserved_field(0x20));
    aml_append(field, aml_named_field("PRQE", 8));
    aml_append(field, aml_named_field("PRQF", 8));
    aml_append(field, aml_named_field("PRQG", 8));
    aml_append(field, aml_named_field("PRQH", 8));
    aml_append(sb_scope, field);

    aml_append(sb_scope, build_irq_status_method());
    aml_append(sb_scope, build_iqcr_method(false));

    aml_append(sb_scope, build_link_dev("LNKA", 0, aml_name("PRQA")));
    aml_append(sb_scope, build_link_dev("LNKB", 1, aml_name("PRQB")));
    aml_append(sb_scope, build_link_dev("LNKC", 2, aml_name("PRQC")));
    aml_append(sb_scope, build_link_dev("LNKD", 3, aml_name("PRQD")));
    aml_append(sb_scope, build_link_dev("LNKE", 4, aml_name("PRQE")));
    aml_append(sb_scope, build_link_dev("LNKF", 5, aml_name("PRQF")));
    aml_append(sb_scope, build_link_dev("LNKG", 6, aml_name("PRQG")));
    aml_append(sb_scope, build_link_dev("LNKH", 7, aml_name("PRQH")));

    aml_append(sb_scope, build_gsi_link_dev("GSIA", 0x10, 0x10));
    aml_append(sb_scope, build_gsi_link_dev("GSIB", 0x11, 0x11));
    aml_append(sb_scope, build_gsi_link_dev("GSIC", 0x12, 0x12));
    aml_append(sb_scope, build_gsi_link_dev("GSID", 0x13, 0x13));
    aml_append(sb_scope, build_gsi_link_dev("GSIE", 0x14, 0x14));
    aml_append(sb_scope, build_gsi_link_dev("GSIF", 0x15, 0x15));
    aml_append(sb_scope, build_gsi_link_dev("GSIG", 0x16, 0x16));
    aml_append(sb_scope, build_gsi_link_dev("GSIH", 0x17, 0x17));

    aml_append(table, sb_scope);
}

static void build_q35_isa_bridge(Aml *table)
{
    Aml *dev;
    Aml *scope;
    Aml *field;

    scope =  aml_scope("_SB.PCI0");
    dev = aml_device("ISA");
    aml_append(dev, aml_name_decl("_ADR", aml_int(0x001F0000)));

    /* ICH9 PCI to ISA irq remapping */
    aml_append(dev, aml_operation_region("PIRQ", AML_PCI_CONFIG,
                                         aml_int(0x60), 0x0C));

    aml_append(dev, aml_operation_region("LPCD", AML_PCI_CONFIG,
                                         aml_int(0x80), 0x02));
    field = aml_field("LPCD", AML_ANY_ACC, AML_NOLOCK, AML_PRESERVE);
    aml_append(field, aml_named_field("COMA", 3));
    aml_append(field, aml_reserved_field(1));
    aml_append(field, aml_named_field("COMB", 3));
    aml_append(field, aml_reserved_field(1));
    aml_append(field, aml_named_field("LPTD", 2));
    aml_append(dev, field);

    aml_append(dev, aml_operation_region("LPCE", AML_PCI_CONFIG,
                                         aml_int(0x82), 0x02));
    /* enable bits */
    field = aml_field("LPCE", AML_ANY_ACC, AML_NOLOCK, AML_PRESERVE);
    aml_append(field, aml_named_field("CAEN", 1));
    aml_append(field, aml_named_field("CBEN", 1));
    aml_append(field, aml_named_field("LPEN", 1));
    aml_append(dev, field);

    aml_append(scope, dev);
    aml_append(table, scope);
}

static void build_piix4_pm(Aml *table)
{
    Aml *dev;
    Aml *scope;

    scope =  aml_scope("_SB.PCI0");
    dev = aml_device("PX13");
    aml_append(dev, aml_name_decl("_ADR", aml_int(0x00010003)));

    aml_append(dev, aml_operation_region("P13C", AML_PCI_CONFIG,
                                         aml_int(0x00), 0xff));
    aml_append(scope, dev);
    aml_append(table, scope);
}

static void build_piix4_isa_bridge(Aml *table)
{
    Aml *dev;
    Aml *scope;
    Aml *field;

    scope =  aml_scope("_SB.PCI0");
    dev = aml_device("ISA");
    aml_append(dev, aml_name_decl("_ADR", aml_int(0x00010000)));

    /* PIIX PCI to ISA irq remapping */
    aml_append(dev, aml_operation_region("P40C", AML_PCI_CONFIG,
                                         aml_int(0x60), 0x04));
    /* enable bits */
    field = aml_field("^PX13.P13C", AML_ANY_ACC, AML_NOLOCK, AML_PRESERVE);
    /* Offset(0x5f),, 7, */
    aml_append(field, aml_reserved_field(0x2f8));
    aml_append(field, aml_reserved_field(7));
    aml_append(field, aml_named_field("LPEN", 1));
    /* Offset(0x67),, 3, */
    aml_append(field, aml_reserved_field(0x38));
    aml_append(field, aml_reserved_field(3));
    aml_append(field, aml_named_field("CAEN", 1));
    aml_append(field, aml_reserved_field(3));
    aml_append(field, aml_named_field("CBEN", 1));
    aml_append(dev, field);

    aml_append(scope, dev);
    aml_append(table, scope);
}

static void build_piix4_pci_hotplug(Aml *table)
{
    Aml *scope;

    scope = aml_scope("_SB.PCI0");

    build_acpi_pci_hotplug(scope);
    aml_append(table, scope);
}

static void
build_dsdt(GArray *table_data, BIOSLinker *linker,
           AcpiPmInfo *pm, AcpiMiscInfo *misc,
           AcpiPciBus *pci_host, MachineState *machine, AcpiConfiguration *conf)
{
    Aml *dsdt, *sb_scope, *scope, *dev, *method, *field, *pkg, *crs;
    uint32_t nr_mem = machine->ram_slots;

    dsdt = init_aml_allocator();

    /* Reserve space for header */
    acpi_data_push(dsdt->buf, sizeof(AcpiTableHeader));

    build_dbg_aml(dsdt);
    if (misc->is_piix4) {
        sb_scope = aml_scope("_SB");
        dev = aml_device("PCI0");
        aml_append(dev, aml_name_decl("_HID", aml_eisaid("PNP0A03")));
        aml_append(dev, aml_name_decl("_ADR", aml_int(0)));
        aml_append(dev, aml_name_decl("_UID", aml_int(1)));
        aml_append(sb_scope, dev);
        aml_append(dsdt, sb_scope);

        build_hpet_aml(dsdt);
        build_piix4_pm(dsdt);
        build_piix4_isa_bridge(dsdt);
        build_isa_devices_aml(dsdt);
        build_piix4_pci_hotplug(dsdt);
        build_piix4_pci0_int(dsdt);
    } else {
        sb_scope = aml_scope("_SB");
        dev = aml_device("PCI0");
        aml_append(dev, aml_name_decl("_HID", aml_eisaid("PNP0A08")));
        aml_append(dev, aml_name_decl("_CID", aml_eisaid("PNP0A03")));
        aml_append(dev, aml_name_decl("_ADR", aml_int(0)));
        aml_append(dev, aml_name_decl("_UID", aml_int(1)));
        aml_append(dev, aml_name_decl("SUPP", aml_int(0)));
        aml_append(dev, aml_name_decl("CTRL", aml_int(0)));
        aml_append(dev, build_osc_method(0x1F));
        aml_append(sb_scope, dev);
        aml_append(dsdt, sb_scope);

        build_hpet_aml(dsdt);
        build_q35_isa_bridge(dsdt);
        build_isa_devices_aml(dsdt);
        build_q35_pci0_int(dsdt);
    }

    if (conf->legacy_cpu_hotplug) {
        build_legacy_cpu_hotplug_aml(dsdt, machine, pm->cpu_hp_io_base);
    } else {
        CPUHotplugFeatures opts = {
            .apci_1_compatible = true, .has_legacy_cphp = true
        };
        build_cpus_aml(dsdt, machine, opts, pm->cpu_hp_io_base,
                       "\\_SB.PCI0", "\\_GPE._E02");
    }
    build_memory_hotplug_aml(dsdt, nr_mem, "\\_SB.PCI0", "\\_GPE._E03");

    scope =  aml_scope("_GPE");
    {
        aml_append(scope, aml_name_decl("_HID", aml_string("ACPI0006")));

        if (misc->is_piix4) {
            method = aml_method("_E01", 0, AML_NOTSERIALIZED);
            aml_append(method,
                aml_acquire(aml_name("\\_SB.PCI0.BLCK"), 0xFFFF));
            aml_append(method, aml_call0("\\_SB.PCI0.PCNT"));
            aml_append(method, aml_release(aml_name("\\_SB.PCI0.BLCK")));
            aml_append(scope, method);
        }

        if (conf->acpi_nvdimm_state.is_enabled) {
            method = aml_method("_E04", 0, AML_NOTSERIALIZED);
            aml_append(method, aml_notify(aml_name("\\_SB.NVDR"),
                                          aml_int(0x80)));
            aml_append(scope, method);
        }
    }
    aml_append(dsdt, scope);

    scope = build_pci_host_bridge(dsdt, pci_host);

    /* reserve GPE0 block resources */
    dev = aml_device("GPE0");
    aml_append(dev, aml_name_decl("_HID", aml_string("PNP0A06")));
    aml_append(dev, aml_name_decl("_UID", aml_string("GPE0 resources")));
    /* device present, functioning, decoding, not shown in UI */
    aml_append(dev, aml_name_decl("_STA", aml_int(0xB)));
    crs = aml_resource_template();
    aml_append(crs,
        aml_io(
               AML_DECODE16,
               pm->fadt.gpe0_blk.address,
               pm->fadt.gpe0_blk.address,
               1,
               pm->fadt.gpe0_blk.bit_width / 8)
    );
    aml_append(dev, aml_name_decl("_CRS", crs));
    aml_append(scope, dev);

    /* reserve PCIHP resources */
    if (pm->pcihp_io_len) {
        dev = aml_device("PHPR");
        aml_append(dev, aml_name_decl("_HID", aml_string("PNP0A06")));
        aml_append(dev,
            aml_name_decl("_UID", aml_string("PCI Hotplug resources")));
        /* device present, functioning, decoding, not shown in UI */
        aml_append(dev, aml_name_decl("_STA", aml_int(0xB)));
        crs = aml_resource_template();
        aml_append(crs,
            aml_io(AML_DECODE16, pm->pcihp_io_base, pm->pcihp_io_base, 1,
                   pm->pcihp_io_len)
        );
        aml_append(dev, aml_name_decl("_CRS", crs));
        aml_append(scope, dev);
    }
    aml_append(dsdt, scope);

    /*  create S3_ / S4_ / S5_ packages if necessary */
    scope = aml_scope("\\");
    if (!pm->s3_disabled) {
        pkg = aml_package(4);
        aml_append(pkg, aml_int(1)); /* PM1a_CNT.SLP_TYP */
        aml_append(pkg, aml_int(1)); /* PM1b_CNT.SLP_TYP, FIXME: not impl. */
        aml_append(pkg, aml_int(0)); /* reserved */
        aml_append(pkg, aml_int(0)); /* reserved */
        aml_append(scope, aml_name_decl("_S3", pkg));
    }

    if (!pm->s4_disabled) {
        pkg = aml_package(4);
        aml_append(pkg, aml_int(pm->s4_val)); /* PM1a_CNT.SLP_TYP */
        /* PM1b_CNT.SLP_TYP, FIXME: not impl. */
        aml_append(pkg, aml_int(pm->s4_val));
        aml_append(pkg, aml_int(0)); /* reserved */
        aml_append(pkg, aml_int(0)); /* reserved */
        aml_append(scope, aml_name_decl("_S4", pkg));
    }

    pkg = aml_package(4);
    aml_append(pkg, aml_int(0)); /* PM1a_CNT.SLP_TYP */
    aml_append(pkg, aml_int(0)); /* PM1b_CNT.SLP_TYP not impl. */
    aml_append(pkg, aml_int(0)); /* reserved */
    aml_append(pkg, aml_int(0)); /* reserved */
    aml_append(scope, aml_name_decl("_S5", pkg));
    aml_append(dsdt, scope);

    /* create fw_cfg node, unconditionally */
    {
        /* when using port i/o, the 8-bit data register *always* overlaps
         * with half of the 16-bit control register. Hence, the total size
         * of the i/o region used is FW_CFG_CTL_SIZE; when using DMA, the
         * DMA control register is located at FW_CFG_DMA_IO_BASE + 4 */
        uint8_t io_size = object_property_get_bool(OBJECT(conf->fw_cfg),
                                                   "dma_enabled", NULL) ?
                          ROUND_UP(FW_CFG_CTL_SIZE, 4) + sizeof(dma_addr_t) :
                          FW_CFG_CTL_SIZE;

        scope = aml_scope("\\_SB.PCI0");
        dev = aml_device("FWCF");

        aml_append(dev, aml_name_decl("_HID", aml_string("QEMU0002")));

        /* device present, functioning, decoding, not shown in UI */
        aml_append(dev, aml_name_decl("_STA", aml_int(0xB)));

        crs = aml_resource_template();
        aml_append(crs,
            aml_io(AML_DECODE16, FW_CFG_IO_BASE, FW_CFG_IO_BASE, 0x01, io_size)
        );
        aml_append(dev, aml_name_decl("_CRS", crs));

        aml_append(scope, dev);
        aml_append(dsdt, scope);
    }

    if (misc->applesmc_io_base) {
        scope = aml_scope("\\_SB.PCI0.ISA");
        dev = aml_device("SMC");

        aml_append(dev, aml_name_decl("_HID", aml_eisaid("APP0001")));
        /* device present, functioning, decoding, not shown in UI */
        aml_append(dev, aml_name_decl("_STA", aml_int(0xB)));

        crs = aml_resource_template();
        aml_append(crs,
            aml_io(AML_DECODE16, misc->applesmc_io_base, misc->applesmc_io_base,
                   0x01, APPLESMC_MAX_DATA_LENGTH)
        );
        aml_append(crs, aml_irq_no_flags(6));
        aml_append(dev, aml_name_decl("_CRS", crs));

        aml_append(scope, dev);
        aml_append(dsdt, scope);
    }

    if (misc->pvpanic_port) {
        scope = aml_scope("\\_SB.PCI0.ISA");

        dev = aml_device("PEVT");
        aml_append(dev, aml_name_decl("_HID", aml_string("QEMU0001")));

        crs = aml_resource_template();
        aml_append(crs,
            aml_io(AML_DECODE16, misc->pvpanic_port, misc->pvpanic_port, 1, 1)
        );
        aml_append(dev, aml_name_decl("_CRS", crs));

        aml_append(dev, aml_operation_region("PEOR", AML_SYSTEM_IO,
                                              aml_int(misc->pvpanic_port), 1));
        field = aml_field("PEOR", AML_BYTE_ACC, AML_NOLOCK, AML_PRESERVE);
        aml_append(field, aml_named_field("PEPT", 8));
        aml_append(dev, field);

        /* device present, functioning, decoding, shown in UI */
        aml_append(dev, aml_name_decl("_STA", aml_int(0xF)));

        method = aml_method("RDPT", 0, AML_NOTSERIALIZED);
        aml_append(method, aml_store(aml_name("PEPT"), aml_local(0)));
        aml_append(method, aml_return(aml_local(0)));
        aml_append(dev, method);

        method = aml_method("WRPT", 1, AML_NOTSERIALIZED);
        aml_append(method, aml_store(aml_arg(0), aml_name("PEPT")));
        aml_append(dev, method);

        aml_append(scope, dev);
        aml_append(dsdt, scope);
    }

    sb_scope = aml_scope("\\_SB");
    {
        Object *pci_host;
        PCIBus *bus = NULL;

        pci_host = acpi_get_pci_host();
        if (pci_host) {
            bus = PCI_HOST_BRIDGE(pci_host)->bus;
        }

        if (bus) {
            Aml *scope = aml_scope("PCI0");
            /* Scan all PCI buses. Generate tables to support hotplug. */
            build_append_pci_bus_devices(scope, bus, pm->pcihp_bridge_en);

            if (TPM_IS_TIS(tpm_find())) {
                dev = aml_device("ISA.TPM");
                aml_append(dev, aml_name_decl("_HID", aml_eisaid("PNP0C31")));
                aml_append(dev, aml_name_decl("_STA", aml_int(0xF)));
                crs = aml_resource_template();
                aml_append(crs, aml_memory32_fixed(TPM_TIS_ADDR_BASE,
                           TPM_TIS_ADDR_SIZE, AML_READ_WRITE));
                /*
                    FIXME: TPM_TIS_IRQ=5 conflicts with PNP0C0F irqs,
                    Rewrite to take IRQ from TPM device model and
                    fix default IRQ value there to use some unused IRQ
                 */
                /* aml_append(crs, aml_irq_no_flags(TPM_TIS_IRQ)); */
                aml_append(dev, aml_name_decl("_CRS", crs));
                aml_append(scope, dev);
            }

            aml_append(sb_scope, scope);
        }
    }

    if (TPM_IS_CRB(tpm_find())) {
        dev = aml_device("TPM");
        aml_append(dev, aml_name_decl("_HID", aml_string("MSFT0101")));
        crs = aml_resource_template();
        aml_append(crs, aml_memory32_fixed(TPM_CRB_ADDR_BASE,
                                           TPM_CRB_ADDR_SIZE, AML_READ_WRITE));
        aml_append(dev, aml_name_decl("_CRS", crs));

        method = aml_method("_STA", 0, AML_NOTSERIALIZED);
        aml_append(method, aml_return(aml_int(0x0f)));
        aml_append(dev, method);

        aml_append(sb_scope, dev);
    }

    aml_append(dsdt, sb_scope);

    /* copy AML table into ACPI tables blob and patch header there */
    g_array_append_vals(table_data, dsdt->buf->data, dsdt->buf->len);
    build_header(linker, table_data,
        (void *)(table_data->data + table_data->len - dsdt->buf->len),
        "DSDT", dsdt->buf->len, 1, NULL, NULL);
    free_aml_allocator();
}

static void
build_hpet(GArray *table_data, BIOSLinker *linker)
{
    Acpi20Hpet *hpet;

    hpet = acpi_data_push(table_data, sizeof(*hpet));
    /* Note timer_block_id value must be kept in sync with value advertised by
     * emulated hpet
     */
    hpet->timer_block_id = cpu_to_le32(0x8086a201);
    hpet->addr.address = cpu_to_le64(HPET_BASE);
    build_header(linker, table_data,
                 (void *)hpet, "HPET", sizeof(*hpet), 1, NULL, NULL);
}

static void
build_tpm_tcpa(GArray *table_data, BIOSLinker *linker, GArray *tcpalog)
{
    Acpi20Tcpa *tcpa = acpi_data_push(table_data, sizeof *tcpa);
    unsigned log_addr_size = sizeof(tcpa->log_area_start_address);
    unsigned log_addr_offset =
        (char *)&tcpa->log_area_start_address - table_data->data;

    tcpa->platform_class = cpu_to_le16(TPM_TCPA_ACPI_CLASS_CLIENT);
    tcpa->log_area_minimum_length = cpu_to_le32(TPM_LOG_AREA_MINIMUM_SIZE);
    acpi_data_push(tcpalog, le32_to_cpu(tcpa->log_area_minimum_length));

    bios_linker_loader_alloc(linker, ACPI_BUILD_TPMLOG_FILE, tcpalog, 1,
                             false /* high memory */);

    /* log area start address to be filled by Guest linker */
    bios_linker_loader_add_pointer(linker,
        ACPI_BUILD_TABLE_FILE, log_addr_offset, log_addr_size,
        ACPI_BUILD_TPMLOG_FILE, 0);

    build_header(linker, table_data,
                 (void *)tcpa, "TCPA", sizeof(*tcpa), 2, NULL, NULL);
}

static void
build_tpm2(GArray *table_data, BIOSLinker *linker, GArray *tcpalog)
{
    Acpi20TPM2 *tpm2_ptr = acpi_data_push(table_data, sizeof *tpm2_ptr);
    unsigned log_addr_size = sizeof(tpm2_ptr->log_area_start_address);
    unsigned log_addr_offset =
        (char *)&tpm2_ptr->log_area_start_address - table_data->data;

    tpm2_ptr->platform_class = cpu_to_le16(TPM2_ACPI_CLASS_CLIENT);
    if (TPM_IS_TIS(tpm_find())) {
        tpm2_ptr->control_area_address = cpu_to_le64(0);
        tpm2_ptr->start_method = cpu_to_le32(TPM2_START_METHOD_MMIO);
    } else if (TPM_IS_CRB(tpm_find())) {
        tpm2_ptr->control_area_address = cpu_to_le64(TPM_CRB_ADDR_CTRL);
        tpm2_ptr->start_method = cpu_to_le32(TPM2_START_METHOD_CRB);
    } else {
        g_warn_if_reached();
    }

    tpm2_ptr->log_area_minimum_length =
        cpu_to_le32(TPM_LOG_AREA_MINIMUM_SIZE);

    /* log area start address to be filled by Guest linker */
    bios_linker_loader_add_pointer(linker, ACPI_BUILD_TABLE_FILE,
                                   log_addr_offset, log_addr_size,
                                   ACPI_BUILD_TPMLOG_FILE, 0);
    build_header(linker, table_data,
                 (void *)tpm2_ptr, "TPM2", sizeof(*tpm2_ptr), 4, NULL, NULL);
}

/*
 * VT-d spec 8.1 DMA Remapping Reporting Structure
 * (version Oct. 2014 or later)
 */
static void
build_dmar_q35(GArray *table_data, BIOSLinker *linker)
{
    int dmar_start = table_data->len;

    AcpiTableDmar *dmar;
    AcpiDmarHardwareUnit *drhd;
    AcpiDmarRootPortATS *atsr;
    uint8_t dmar_flags = 0;
    X86IOMMUState *iommu = x86_iommu_get_default();
    AcpiDmarDeviceScope *scope = NULL;
    /* Root complex IOAPIC use one path[0] only */
    size_t ioapic_scope_size = sizeof(*scope) + sizeof(scope->path[0]);
    IntelIOMMUState *intel_iommu = INTEL_IOMMU_DEVICE(iommu);

    assert(iommu);
    if (iommu->intr_supported) {
        dmar_flags |= 0x1;      /* Flags: 0x1: INT_REMAP */
    }

    dmar = acpi_data_push(table_data, sizeof(*dmar));
    dmar->host_address_width = intel_iommu->aw_bits - 1;
    dmar->flags = dmar_flags;

    /* DMAR Remapping Hardware Unit Definition structure */
    drhd = acpi_data_push(table_data, sizeof(*drhd) + ioapic_scope_size);
    drhd->type = cpu_to_le16(ACPI_DMAR_TYPE_HARDWARE_UNIT);
    drhd->length = cpu_to_le16(sizeof(*drhd) + ioapic_scope_size);
    drhd->flags = ACPI_DMAR_INCLUDE_PCI_ALL;
    drhd->pci_segment = cpu_to_le16(0);
    drhd->address = cpu_to_le64(Q35_HOST_BRIDGE_IOMMU_ADDR);

    /* Scope definition for the root-complex IOAPIC. See VT-d spec
     * 8.3.1 (version Oct. 2014 or later). */
    scope = &drhd->scope[0];
    scope->entry_type = 0x03;   /* Type: 0x03 for IOAPIC */
    scope->length = ioapic_scope_size;
    scope->enumeration_id = ACPI_BUILD_IOAPIC_ID;
    scope->bus = Q35_PSEUDO_BUS_PLATFORM;
    scope->path[0].device = PCI_SLOT(Q35_PSEUDO_DEVFN_IOAPIC);
    scope->path[0].function = PCI_FUNC(Q35_PSEUDO_DEVFN_IOAPIC);

    if (iommu->dt_supported) {
        atsr = acpi_data_push(table_data, sizeof(*atsr));
        atsr->type = cpu_to_le16(ACPI_DMAR_TYPE_ATSR);
        atsr->length = cpu_to_le16(sizeof(*atsr));
        atsr->flags = ACPI_DMAR_ATSR_ALL_PORTS;
        atsr->pci_segment = cpu_to_le16(0);
    }

    build_header(linker, table_data, (void *)(table_data->data + dmar_start),
                 "DMAR", table_data->len - dmar_start, 1, NULL, NULL);
}
/*
 *   IVRS table as specified in AMD IOMMU Specification v2.62, Section 5.2
 *   accessible here http://support.amd.com/TechDocs/48882_IOMMU.pdf
 */
static void
build_amd_iommu(GArray *table_data, BIOSLinker *linker)
{
    int iommu_start = table_data->len;
    AMDVIState *s = AMD_IOMMU_DEVICE(x86_iommu_get_default());

    /* IVRS header */
    acpi_data_push(table_data, sizeof(AcpiTableHeader));
    /* IVinfo - IO virtualization information common to all
     * IOMMU units in a system
     */
    build_append_int_noprefix(table_data, 40UL << 8/* PASize */, 4);
    /* reserved */
    build_append_int_noprefix(table_data, 0, 8);

    /* IVHD definition - type 10h */
    build_append_int_noprefix(table_data, 0x10, 1);
    /* virtualization flags */
    build_append_int_noprefix(table_data,
                             (1UL << 0) | /* HtTunEn      */
                             (1UL << 4) | /* iotblSup     */
                             (1UL << 6) | /* PrefSup      */
                             (1UL << 7),  /* PPRSup       */
                             1);
    /* IVHD length */
    build_append_int_noprefix(table_data, 28, 2);
    /* DeviceID */
    build_append_int_noprefix(table_data, s->devid, 2);
    /* Capability offset */
    build_append_int_noprefix(table_data, s->capab_offset, 2);
    /* IOMMU base address */
    build_append_int_noprefix(table_data, s->mmio.addr, 8);
    /* PCI Segment Group */
    build_append_int_noprefix(table_data, 0, 2);
    /* IOMMU info */
    build_append_int_noprefix(table_data, 0, 2);
    /* IOMMU Feature Reporting */
    build_append_int_noprefix(table_data,
                             (48UL << 30) | /* HATS   */
                             (48UL << 28) | /* GATS   */
                             (1UL << 2),    /* GTSup  */
                             4);
    /*
     *   Type 1 device entry reporting all devices
     *   These are 4-byte device entries currently reporting the range of
     *   Refer to Spec - Table 95:IVHD Device Entry Type Codes(4-byte)
     */
    build_append_int_noprefix(table_data, 0x0000001, 4);

    build_header(linker, table_data, (void *)(table_data->data + iommu_start),
                 "IVRS", table_data->len - iommu_start, 1, NULL, NULL);
}

static
void acpi_build(AcpiBuildTables *tables, MachineState *machine, AcpiConfiguration *conf)
{
    GArray *table_offsets;
    unsigned facs, dsdt, rsdt, fadt;
    AcpiPmInfo pm;
    AcpiMiscInfo misc;
    AcpiMcfgInfo mcfg;
    Range pci_hole, pci_hole64;
    uint8_t *u;
    size_t aml_len = 0;
    GArray *tables_blob = tables->table_data;
    AcpiSlicOem slic_oem = { .id = NULL, .table_id = NULL };
    Object *vmgenid_dev;

    acpi_get_pm_info(&pm);
    acpi_get_misc_info(&misc);
    acpi_get_pci_holes(&pci_hole, &pci_hole64);
    acpi_get_slic_oem(&slic_oem);

    table_offsets = g_array_new(false, true /* clear */,
                                        sizeof(uint32_t));
    ACPI_BUILD_DPRINTF("init ACPI tables\n");

    bios_linker_loader_alloc(tables->linker,
                             ACPI_BUILD_TABLE_FILE, tables_blob,
                             64 /* Ensure FACS is aligned */,
                             false /* high memory */);

    AcpiPciBus pci_host = {
        .pci_bus    = PC_MACHINE(machine)->bus,
        .pci_hole   = &pci_hole,
        .pci_hole64 = &pci_hole64,
    };
    /*
     * FACS is pointed to by FADT.
     * We place it first since it's the only table that has alignment
     * requirements.
     */
    facs = tables_blob->len;
    build_facs(tables_blob, tables->linker);

    /* DSDT is pointed to by FADT */
    dsdt = tables_blob->len;
    build_dsdt(tables_blob, tables->linker, &pm, &misc,
               &pci_host, machine, conf);

    /* Count the size of the DSDT and SSDT, we will need it for legacy
     * sizing of ACPI tables.
     */
    aml_len += tables_blob->len - dsdt;

    /* ACPI tables pointed to by RSDT */
    fadt = tables_blob->len;
    acpi_add_table(table_offsets, tables_blob);
    pm.fadt.facs_tbl_offset = &facs;
    pm.fadt.dsdt_tbl_offset = &dsdt;
    pm.fadt.xdsdt_tbl_offset = &dsdt;
    build_fadt(tables_blob, tables->linker, &pm.fadt,
               slic_oem.id, slic_oem.table_id);
    aml_len += tables_blob->len - fadt;

    acpi_add_table(table_offsets, tables_blob);
    acpi_build_madt(tables_blob, tables->linker, machine, conf);

    vmgenid_dev = find_vmgenid_dev();
    if (vmgenid_dev) {
        acpi_add_table(table_offsets, tables_blob);
        vmgenid_build_acpi(VMGENID(vmgenid_dev), tables_blob,
                           tables->vmgenid, tables->linker);
    }

    if (misc.has_hpet) {
        acpi_add_table(table_offsets, tables_blob);
        build_hpet(tables_blob, tables->linker);
    }
    if (misc.tpm_version != TPM_VERSION_UNSPEC) {
        acpi_add_table(table_offsets, tables_blob);
        build_tpm_tcpa(tables_blob, tables->linker, tables->tcpalog);

        if (misc.tpm_version == TPM_VERSION_2_0) {
            acpi_add_table(table_offsets, tables_blob);
            build_tpm2(tables_blob, tables->linker, tables->tcpalog);
        }
    }
    if (conf->numa_nodes) {
        acpi_add_table(table_offsets, tables_blob);
        acpi_build_srat(tables_blob, tables->linker, machine, conf);
        if (have_numa_distance) {
            acpi_add_table(table_offsets, tables_blob);
            build_slit(tables_blob, tables->linker);
        }
    }
    if (acpi_get_mcfg(&mcfg)) {
        acpi_add_table(table_offsets, tables_blob);
        acpi_build_mcfg(tables_blob, tables->linker, &mcfg);
    }
    if (x86_iommu_get_default()) {
        IommuType IOMMUType = x86_iommu_get_type();
        if (IOMMUType == TYPE_AMD) {
            acpi_add_table(table_offsets, tables_blob);
            build_amd_iommu(tables_blob, tables->linker);
        } else if (IOMMUType == TYPE_INTEL) {
            acpi_add_table(table_offsets, tables_blob);
            build_dmar_q35(tables_blob, tables->linker);
        }
    }
    if (conf->acpi_nvdimm_state.is_enabled) {
        nvdimm_build_acpi(table_offsets, tables_blob, tables->linker,
                          &conf->acpi_nvdimm_state, machine->ram_slots);
    }

    /* Add tables supplied by user (if any) */
    for (u = acpi_table_first(); u; u = acpi_table_next(u)) {
        unsigned len = acpi_table_len(u);

        acpi_add_table(table_offsets, tables_blob);
        g_array_append_vals(tables_blob, u, len);
    }

    /* RSDT is pointed to by RSDP */
    rsdt = tables_blob->len;
    build_rsdt(tables_blob, tables->linker, table_offsets,
               slic_oem.id, slic_oem.table_id);

    /* RSDP is in FSEG memory, so allocate it separately */
    build_rsdp_rsdt(tables->rsdp, tables->linker, rsdt);

    /* We'll expose it all to Guest so we want to reduce
     * chance of size changes.
     *
     * We used to align the tables to 4k, but of course this would
     * too simple to be enough.  4k turned out to be too small an
     * alignment very soon, and in fact it is almost impossible to
     * keep the table size stable for all (max_cpus, max_memory_slots)
     * combinations.  So the table size is always 64k for pc-i440fx-2.1
     * and we give an error if the table grows beyond that limit.
     *
     * We still have the problem of migrating from "-M pc-i440fx-2.0".  For
     * that, we exploit the fact that QEMU 2.1 generates _smaller_ tables
     * than 2.0 and we can always pad the smaller tables with zeros.  We can
     * then use the exact size of the 2.0 tables.
     *
     * All this is for PIIX4, since QEMU 2.0 didn't support Q35 migration.
     */
    if (conf->legacy_acpi_table_size) {
        /* Subtracting aml_len gives the size of fixed tables.  Then add the
         * size of the PIIX4 DSDT/SSDT in QEMU 2.0.
         */
        int legacy_aml_len =
            conf->legacy_acpi_table_size +
            ACPI_BUILD_LEGACY_CPU_AML_SIZE * conf->apic_id_limit;
        int legacy_table_size =
            ROUND_UP(tables_blob->len - aml_len + legacy_aml_len,
                     ACPI_BUILD_ALIGN_SIZE);
        if (tables_blob->len > legacy_table_size) {
            /* Should happen only with PCI bridges and -M pc-i440fx-2.0.  */
            warn_report("ACPI table size %u exceeds %d bytes,"
                        " migration may not work",
                        tables_blob->len, legacy_table_size);
            error_printf("Try removing CPUs, NUMA nodes, memory slots"
                         " or PCI bridges.");
        }
        g_array_set_size(tables_blob, legacy_table_size);
    } else {
        /* Make sure we have a buffer in case we need to resize the tables. */
        if (tables_blob->len > ACPI_BUILD_TABLE_SIZE / 2) {
            /* As of QEMU 2.1, this fires with 160 VCPUs and 255 memory slots.  */
            warn_report("ACPI table size %u exceeds %d bytes,"
                        " migration may not work",
                        tables_blob->len, ACPI_BUILD_TABLE_SIZE / 2);
            error_printf("Try removing CPUs, NUMA nodes, memory slots"
                         " or PCI bridges.");
        }
        acpi_align_size(tables_blob, ACPI_BUILD_TABLE_SIZE);
    }

    acpi_align_size(tables->linker->cmd_blob, ACPI_BUILD_ALIGN_SIZE);

    /* Cleanup memory that's no longer used. */
    g_array_free(table_offsets, true);
}

static void acpi_ram_update(MemoryRegion *mr, GArray *data)
{
    uint32_t size = acpi_data_len(data);

    /* Make sure RAM size is correct - in case it got changed e.g. by migration */
    memory_region_ram_resize(mr, size, &error_abort);

    memcpy(memory_region_get_ram_ptr(mr), data->data, size);
    memory_region_set_dirty(mr, 0, size);
}

static void acpi_build_update(void *build_opaque)
{
    AcpiConfiguration *conf = build_opaque;
    AcpiBuildState *build_state;
    AcpiBuildTables tables;

    /* No ACPI configuration? Nothing to do. */
    if (!conf) {
        return;
    }

    build_state = conf->build_state;

    /* No state to update or already patched? Nothing to do. */
    if (!build_state || build_state->patched) {
        return;
    }
    build_state->patched = 1;

    acpi_build_tables_init(&tables);

    acpi_build(&tables, MACHINE(qdev_get_machine()), conf);

    acpi_ram_update(build_state->table_mr, tables.table_data);

    if (build_state->rsdp) {
        memcpy(build_state->rsdp, tables.rsdp->data, acpi_data_len(tables.rsdp));
    } else {
        acpi_ram_update(build_state->rsdp_mr, tables.rsdp);
    }

    acpi_ram_update(build_state->linker_mr, tables.linker->cmd_blob);
    acpi_build_tables_cleanup(&tables, true);
}

static void acpi_build_reset(void *build_opaque)
{
    AcpiBuildState *build_state = build_opaque;
    build_state->patched = 0;
}

static MemoryRegion *acpi_add_rom_blob(AcpiConfiguration *conf,
                                       GArray *blob, const char *name,
                                       uint64_t max_size)
{
    return rom_add_blob(name, blob->data, acpi_data_len(blob), max_size, -1,
                        name, acpi_build_update, conf, NULL, true);
}

static const VMStateDescription vmstate_acpi_build = {
    .name = "acpi_build",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_BOOL(patched, AcpiBuildState),
        VMSTATE_END_OF_LIST()
    },
};

void acpi_setup(MachineState *machine, AcpiConfiguration *conf)
{
    AcpiBuildTables tables;
    AcpiBuildState *build_state;
    Object *vmgenid_dev;

    if (!conf) {
        ACPI_BUILD_DPRINTF("No ACPI config. Bailing out.\n");
        return;
    }

    build_state = g_malloc0(sizeof *build_state);
    conf->build_state = build_state;

    acpi_build_tables_init(&tables);
    acpi_build(&tables, machine, conf);

    /* Now expose it all to Guest */
    build_state->table_mr = acpi_add_rom_blob(conf, tables.table_data,
                                               ACPI_BUILD_TABLE_FILE,
                                               ACPI_BUILD_TABLE_MAX_SIZE);
    assert(build_state->table_mr != NULL);

    build_state->linker_mr =
        acpi_add_rom_blob(conf, tables.linker->cmd_blob,
                          "etc/table-loader", 0);

    fw_cfg_add_file(conf->fw_cfg, ACPI_BUILD_TPMLOG_FILE,
                    tables.tcpalog->data, acpi_data_len(tables.tcpalog));

    vmgenid_dev = find_vmgenid_dev();
    if (vmgenid_dev) {
        vmgenid_add_fw_cfg(VMGENID(vmgenid_dev), conf->fw_cfg,
                           tables.vmgenid);
    }

    if (!conf->rsdp_in_ram) {
        /*
         * Keep for compatibility with old machine types.
         * Though RSDP is small, its contents isn't immutable, so
         * we'll update it along with the rest of tables on guest access.
         */
        uint32_t rsdp_size = acpi_data_len(tables.rsdp);

        build_state->rsdp = g_memdup(tables.rsdp->data, rsdp_size);
        fw_cfg_add_file_callback(conf->fw_cfg, ACPI_BUILD_RSDP_FILE,
                                 acpi_build_update, NULL, conf,
                                 build_state->rsdp, rsdp_size, true);
        build_state->rsdp_mr = NULL;
    } else {
        build_state->rsdp = NULL;
        build_state->rsdp_mr = acpi_add_rom_blob(conf, tables.rsdp,
                                                  ACPI_BUILD_RSDP_FILE, 0);
    }

    qemu_register_reset(acpi_build_reset, build_state);
    acpi_build_reset(build_state);
    vmstate_register(NULL, 0, &vmstate_acpi_build, build_state);

    /* Cleanup tables but don't free the memory: we track it
     * in build_state.
     */
    acpi_build_tables_cleanup(&tables, false);
}
