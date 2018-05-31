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
#include "hw/i386/pc_piix.h"
#include "hw/i386/pc.h"
#include "hw/acpi/aml-build.h"

void build_append_pcihp_notify_entry(Aml *method, int slot)
{
    Aml *if_ctx;
    int32_t devfn = PCI_DEVFN(slot, 0);

    if_ctx = aml_if(aml_and(aml_arg(0), aml_int(0x1U << slot), NULL));
    aml_append(if_ctx, aml_notify(aml_name("S%.02X", devfn), aml_arg(1)));
    aml_append(method, if_ctx);
}

/* _CRS method - get current settings */
static Aml *build_iqcr_method_piix(void)
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
    
    if_ctx = aml_if(aml_lless(aml_arg(0), aml_int(0x80)));
    aml_append(if_ctx, aml_store(aml_arg(0), aml_name("PRRI")));
    aml_append(method, if_ctx);

    aml_append(method, aml_return(aml_name("PRR0")));
    return method;
}

void build_piix4_pci0_int(Aml *table)
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
    aml_append(sb_scope, build_iqcr_method_piix());

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

void build_piix4_pm(Aml *table)
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

void build_piix4_isa_bridge(Aml *table)
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

void build_piix4_pci_hotplug(Aml *table)
{
    Aml *scope;
    Aml *field;
    Aml *method;

    scope =  aml_scope("_SB.PCI0");

    aml_append(scope,
        aml_operation_region("PCST", AML_SYSTEM_IO, aml_int(0xae00), 0x08));
    field = aml_field("PCST", AML_DWORD_ACC, AML_NOLOCK, AML_WRITE_AS_ZEROS);
    aml_append(field, aml_named_field("PCIU", 32));
    aml_append(field, aml_named_field("PCID", 32));
    aml_append(scope, field);

    aml_append(scope,
        aml_operation_region("SEJ", AML_SYSTEM_IO, aml_int(0xae08), 0x04));
    field = aml_field("SEJ", AML_DWORD_ACC, AML_NOLOCK, AML_WRITE_AS_ZEROS);
    aml_append(field, aml_named_field("B0EJ", 32));
    aml_append(scope, field);

    aml_append(scope,
        aml_operation_region("BNMR", AML_SYSTEM_IO, aml_int(0xae10), 0x04));
    field = aml_field("BNMR", AML_DWORD_ACC, AML_NOLOCK, AML_WRITE_AS_ZEROS);
    aml_append(field, aml_named_field("BNUM", 32));
    aml_append(scope, field);

    aml_append(scope, aml_mutex("BLCK", 0));

    method = aml_method("PCEJ", 2, AML_NOTSERIALIZED);
    aml_append(method, aml_acquire(aml_name("BLCK"), 0xFFFF));
    aml_append(method, aml_store(aml_arg(0), aml_name("BNUM")));
    aml_append(method,
        aml_store(aml_shiftleft(aml_int(1), aml_arg(1)), aml_name("B0EJ")));
    aml_append(method, aml_release(aml_name("BLCK")));
    aml_append(method, aml_return(aml_int(0)));
    aml_append(scope, method);

    aml_append(table, scope);
}

