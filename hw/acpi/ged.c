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

void build_ged_aml(Aml *table, const char *name,
                   GedEvent *events, uint8_t events_size)
{
    Aml *crs = aml_resource_template();
    Aml *evt;
    Aml *zero = aml_int(0);
    Aml *one = aml_int(1);
    Aml *dev = aml_device("%s", name);
    Aml *has_irq = aml_local(0);
    Aml *while_ctx;
    uint8_t i;

    /*
     * For each GED event we:
     * - Add an interrupt to the CRS section.
     * - Add a conditional block for each event, inside a while loop.
     *   This is semantically equivalent to a switch/case implementation.
     */
    evt = aml_method("_EVT", 1, AML_SERIALIZED);
    {
        Aml *irq = aml_arg(0);
        Aml *ged_aml;
        Aml *if_ctx, *else_ctx;

        /* Local0 = One */
        aml_append(evt, aml_store(one, has_irq));


        /*
         * Here we want to call a method for each supported GED event type.
         * The resulting ASL code looks like:
         *
         * Local0 = One
         * While ((Local0 == One))
         * {
         *    Local0 = Zero
         *    If (Arg0 == irq0)
         *    {
         *        MethodEvent0()
         *        Local0 = Zero
         *    }
         *    ElseIf (Arg0 == irq1)
         *    {
         *        MethodEvent1()
         *        Local0 = Zero
         *    }
         *    ElseIf (Arg0 == irq2)
         *    {
         *        MethodEvent2()
         *        Local0 = Zero
         *    }
         * }
         */

        /* While ((Local0 == One)) */
        while_ctx = aml_while(aml_equal(has_irq, one));
        {
            else_ctx = NULL;

            /*
             * Clear loop condition, we don't want to enter an infinite loop.
             * Local0 = Zero
             */
            aml_append(while_ctx, aml_store(zero, has_irq));
            for (i = 0; i < events_size; i++) {
                ged_aml = ged_event_aml(&events[i]);
                if (!ged_aml) {
                    continue;
                }

                /* _CRS interrupt */
                aml_append(crs, aml_interrupt(AML_CONSUMER, AML_LEVEL, AML_ACTIVE_HIGH,
                                              AML_EXCLUSIVE, &events[i].irq, 1));

                /* If ((Arg0 == irq))*/
                if_ctx = aml_if(aml_equal(irq, aml_int(events[i].irq)));
                {
                    /* AML for this specific type of event */
                    aml_append(if_ctx, ged_aml);
                }

                /*
                 * We append the first if to the while context.
                 * Other ifs will be elseifs.
                 */
                if (!else_ctx) {
                    aml_append(while_ctx, if_ctx);
                } else {
                    aml_append(else_ctx, if_ctx);
                    aml_append(while_ctx, else_ctx);
                }

                if (i != events_size - 1) {
                    else_ctx = aml_else();
                }
            }
        }

        aml_append(evt, while_ctx);
    }

    aml_append(dev, aml_name_decl("_HID", aml_string("ACPI0013")));
    aml_append(dev, aml_name_decl("_UID", zero));
    aml_append(dev, aml_name_decl("_CRS", crs));
    aml_append(dev, evt);

    aml_append(table, dev);
}
