/*
 *
 * Copyright (c) 2018 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "hw/acpi/ged.h"

static uint64_t ged_read(void *opaque, hwaddr addr, unsigned size)
{
    uint64_t val = 0;
    GEDState *ged_st = opaque;

    switch (addr) {
    case ACPI_GED_IRQ_SEL_OFFSET:
        /* Read the selector value and reset it */
        qemu_mutex_lock(&ged_st->lock);
        val = ged_st->sel;
        ged_st->sel = ACPI_GED_IRQ_SEL_INIT;
        qemu_mutex_unlock(&ged_st->lock);
        break;
    default:
        break;
    }

    return val;
}

/* Nothing is expected to be written to the GED memory region */
static void ged_write(void *opaque, hwaddr addr, uint64_t data,
                      unsigned int size)
{
}

static const MemoryRegionOps ged_ops = {
    .read = ged_read,
    .write = ged_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

void acpi_ged_init(MemoryRegion *as, Object *owner, GEDState *ged_st,
                   hwaddr base_addr, uint32_t ged_irq)
{
    qemu_mutex_init(&ged_st->lock);
    ged_st->irq = ged_irq;
    memory_region_init_io(&ged_st->io, owner, &ged_ops, ged_st,
                          "acpi-ged-event", ACPI_GED_REG_LEN);
    memory_region_add_subregion(as, base_addr, &ged_st->io);
}

void acpi_ged_event(GEDState *ged_st, qemu_irq *irq, uint32_t ged_irq_sel)
{
    /* Set the GED IRQ selector to the expected device type value. This
     * way, the ACPI method will be able to trigger the right code based
     * on a unique IRQ.
     */
    qemu_mutex_lock(&ged_st->lock);
    ged_st->sel |= ged_irq_sel;
    qemu_mutex_unlock(&ged_st->lock);

    /* Trigger the event by sending an interrupt to the guest. */
    qemu_irq_pulse(irq[ged_st->irq]);
}

static Aml *ged_event_aml(GedEvent *event)
{
    Aml *method;

    if (!event) {
        return NULL;
    }

    switch (event->event) {
    case GED_CPU_HOTPLUG:
        /* We run a complete CPU SCAN when getting a CPU hotplug event */
        return aml_call0("\\_SB.CPUS." CPU_SCAN_METHOD);
    case GED_MEMORY_HOTPLUG:
        /* We run a complete memory SCAN when getting a memory hotplug event */
        return aml_call0("\\_SB.MHPC." MEMORY_SLOT_SCAN_METHOD);
    case GED_PCI_HOTPLUG:
	/* Take the PCI lock and trigger a PCI rescan */
        method = aml_acquire(aml_name("\\_SB.PCI0.BLCK"), 0xFFFF);
        aml_append(method, aml_call0("\\_SB.PCI0.PCNT"));
        aml_append(method, aml_release(aml_name("\\_SB.PCI0.BLCK")));
	return method;
    case GED_NVDIMM_HOTPLUG:
        return aml_notify(aml_name("\\_SB.NVDR"), aml_int(0x80));
    default:
        break;
    }

    return NULL;
}

void build_ged_aml(Aml *table, const char *name, uint32_t ged_irq,
                   GedEvent *events, uint32_t events_size)
{
    Aml *crs = aml_resource_template();
    Aml *evt, *field;
    Aml *zero = aml_int(0);
    Aml *dev = aml_device("%s", name);
    Aml *irq_sel = aml_local(0);
    Aml *isel = aml_name(AML_GED_IRQ_SEL);
    uint32_t i;

    /* _CRS interrupt */
    aml_append(crs, aml_interrupt(AML_CONSUMER, AML_LEVEL, AML_ACTIVE_HIGH,
                                  AML_EXCLUSIVE, &ged_irq, 1));

    /*
     * For each GED event we:
     * - Add an interrupt to the CRS section.
     * - Add a conditional block for each event, inside a while loop.
     *   This is semantically equivalent to a switch/case implementation.
     */
    evt = aml_method("_EVT", 1, AML_SERIALIZED);
    {
        Aml *ged_aml;
        Aml *if_ctx;

        /* Local0 = ISEL */
        aml_append(evt, aml_store(isel, irq_sel));

        /*
         * Here we want to call a method for each supported GED event type.
         * The resulting ASL code looks like:
         *
         * Local0 = ISEL
         * If ((Local0 & irq0) == irq0)
         * {
         *     MethodEvent0()
         * }
         *
         * If ((Local0 & irq1) == irq1)
         * {
         *     MethodEvent1()
         * }
         *
         * If ((Local0 & irq2) == irq2)
         * {
         *     MethodEvent2()
         * }
         */

        for (i = 0; i < events_size; i++) {
            ged_aml = ged_event_aml(&events[i]);
            if (!ged_aml) {
                continue;
            }

            /* If ((Local1 == irq))*/
            if_ctx = aml_if(aml_equal(aml_and(irq_sel, aml_int(events[i].selector), NULL), aml_int(events[i].selector)));
            {
                /* AML for this specific type of event */
                aml_append(if_ctx, ged_aml);
            }

            /*
             * We append the first if to the while context.
             * Other ifs will be elseifs.
             */
            aml_append(evt, if_ctx);
        }
    }

    aml_append(dev, aml_name_decl("_HID", aml_string("ACPI0013")));
    aml_append(dev, aml_name_decl("_UID", zero));
    aml_append(dev, aml_name_decl("_CRS", crs));

    /* Append IO region */
    aml_append(dev, aml_operation_region(AML_GED_IRQ_REG, AML_SYSTEM_IO,
               aml_int(ACPI_GED_EVENT_IO_BASE + ACPI_GED_IRQ_SEL_OFFSET),
               ACPI_GED_IRQ_SEL_LEN));
    field = aml_field(AML_GED_IRQ_REG, AML_DWORD_ACC, AML_NOLOCK,
                      AML_WRITE_AS_ZEROS);
    aml_append(field, aml_named_field(AML_GED_IRQ_SEL,
                                      ACPI_GED_IRQ_SEL_LEN * 8));
    aml_append(dev, field);

    /* Append _EVT method */
    aml_append(dev, evt);

    aml_append(table, dev);
}
