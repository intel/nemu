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
#include "qapi/qmp/qnum.h"
#include "hw/acpi/acpi-defs.h"
#include "hw/acpi/acpi.h"
#include "hw/acpi/cpu.h"

#include "hw/i386/ich9.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci-host/q35.h"

#include "hw/acpi/aml-build.h"
#include "qom/qom-qobject.h"

/* _CRS method - get current settings */
static Aml *build_iqcr_method_q35(void)
{
    uint32_t irqs;
    Aml *method = aml_method("IQCR", 1, AML_SERIALIZED);
    Aml *crs = aml_resource_template();

    irqs = 0;
    aml_append(crs, aml_interrupt(AML_CONSUMER, AML_LEVEL,
                                  AML_ACTIVE_HIGH, AML_SHARED, &irqs, 1));
    aml_append(method, aml_name_decl("PRR0", crs));

    aml_append(method,
        aml_create_dword_field(aml_name("PRR0"), aml_int(5), "PRRI"));

    aml_append(method,
        aml_store(aml_and(aml_arg(0), aml_int(0xF), NULL),
                  aml_name("PRRI")));

    aml_append(method, aml_return(aml_name("PRR0")));
    return method;
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

void build_q35_pci0_int(Aml *table)
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
    aml_append(sb_scope, build_iqcr_method_q35());

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

void build_q35_isa_bridge(Aml *table)
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

Aml *build_q35_osc_method(void)
{
    Aml *if_ctx;
    Aml *if_ctx2;
    Aml *else_ctx;
    Aml *method;
    Aml *a_cwd1 = aml_name("CDW1");
    Aml *a_ctrl = aml_local(0);

    method = aml_method("_OSC", 4, AML_NOTSERIALIZED);
    aml_append(method, aml_create_dword_field(aml_arg(3), aml_int(0), "CDW1"));

    if_ctx = aml_if(aml_equal(
        aml_arg(0), aml_touuid("33DB4D5B-1FF7-401C-9657-7441C03DD766")));
    aml_append(if_ctx, aml_create_dword_field(aml_arg(3), aml_int(4), "CDW2"));
    aml_append(if_ctx, aml_create_dword_field(aml_arg(3), aml_int(8), "CDW3"));

    aml_append(if_ctx, aml_store(aml_name("CDW3"), a_ctrl));

    /*
     * Always allow native PME, AER (no dependencies)
     * Allow SHPC (PCI bridges can have SHPC controller)
     */
    aml_append(if_ctx, aml_and(a_ctrl, aml_int(0x1F), a_ctrl));

    if_ctx2 = aml_if(aml_lnot(aml_equal(aml_arg(1), aml_int(1))));
    /* Unknown revision */
    aml_append(if_ctx2, aml_or(a_cwd1, aml_int(0x08), a_cwd1));
    aml_append(if_ctx, if_ctx2);

    if_ctx2 = aml_if(aml_lnot(aml_equal(aml_name("CDW3"), a_ctrl)));
    /* Capabilities bits were masked */
    aml_append(if_ctx2, aml_or(a_cwd1, aml_int(0x10), a_cwd1));
    aml_append(if_ctx, if_ctx2);

    /* Update DWORD3 in the buffer */
    aml_append(if_ctx, aml_store(a_ctrl, aml_name("CDW3")));
    aml_append(method, if_ctx);

    else_ctx = aml_else();
    /* Unrecognized UUID */
    aml_append(else_ctx, aml_or(a_cwd1, aml_int(4), a_cwd1));
    aml_append(method, else_ctx);

    aml_append(method, aml_return(aml_arg(3)));
    return method;
}

void build_mcfg_q35(GArray *table_data, BIOSLinker *linker, AcpiMcfgInfo *info)
{
    AcpiTableMcfg *mcfg;
    const char *sig;
    int len = sizeof(*mcfg) + 1 * sizeof(mcfg->allocation[0]);

    mcfg = acpi_data_push(table_data, len);
    mcfg->allocation[0].address = cpu_to_le64(info->mcfg_base);
    /* Only a single allocation so no need to play with segments */
    mcfg->allocation[0].pci_segment = cpu_to_le16(0);
    mcfg->allocation[0].start_bus_number = 0;
    mcfg->allocation[0].end_bus_number = PCIE_MMCFG_BUS(info->mcfg_size - 1);

    /* MCFG is used for ECAM which can be enabled or disabled by guest.
     * To avoid table size changes (which create migration issues),
     * always create the table even if there are no allocations,
     * but set the signature to a reserved value in this case.
     * ACPI spec requires OSPMs to ignore such tables.
     */
    if (info->mcfg_base == PCIE_BASE_ADDR_UNMAPPED) {
        /* Reserved signature: ignored by OSPM */
        sig = "QEMU";
    } else {
        sig = "MCFG";
    }
    build_header(linker, table_data, (void *)mcfg, sig, len, 1, NULL, NULL);
}

/*
 * VT-d spec 8.1 DMA Remapping Reporting Structure
 * (version Oct. 2014 or later)
 */
void build_dmar_q35(GArray *table_data, BIOSLinker *linker)
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

bool acpi_get_mcfg(AcpiMcfgInfo *mcfg)
{
    Object *pci_host;
    QObject *o;

    pci_host = acpi_get_i386_pci_host();
    g_assert(pci_host);

    o = object_property_get_qobject(pci_host, PCIE_HOST_MCFG_BASE, NULL);
    if (!o) {
        return false;
    }
    mcfg->mcfg_base = qnum_get_uint(qobject_to(QNum, o));
    qobject_decref(o);

    o = object_property_get_qobject(pci_host, PCIE_HOST_MCFG_SIZE, NULL);
    assert(o);
    mcfg->mcfg_size = qnum_get_uint(qobject_to(QNum, o));
    qobject_decref(o);
    return true;
}

