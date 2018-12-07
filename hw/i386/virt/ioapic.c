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


#include "qemu/osdep.h"
#include "hw/i386/virt.h"
#include "sysemu/kvm.h"
#include "hw/i386/ioapic.h"
#include "kvm_i386.h"
#include "ioapic.h"

void virt_gsi_handler(void *opaque, int n, int level)
{
    qemu_irq *ioapic_irq = opaque;
     qemu_set_irq(ioapic_irq[n], level);
}
void virt_ioapic_init(VirtMachineState *vms)
{
    qemu_irq *ioapic_irq;
    DeviceState *ioapic_dev;
    SysBusDevice *d;
    unsigned int i;
     /* KVM IRQ chip */
    assert(kvm_irqchip_in_kernel());
    ioapic_irq = g_new0(qemu_irq, IOAPIC_NUM_PINS);
    kvm_pc_setup_irq_routing(true);
     /* KVM IOAPIC */
    assert(kvm_ioapic_in_kernel());
    ioapic_dev = qdev_create(NULL, "kvm-ioapic");
     object_property_add_child(qdev_get_machine(), "ioapic", OBJECT(ioapic_dev), NULL);
     qdev_init_nofail(ioapic_dev);
    d = SYS_BUS_DEVICE(ioapic_dev);
    sysbus_mmio_map(d, 0, IO_APIC_DEFAULT_ADDRESS);
     for (i = 0; i < IOAPIC_NUM_PINS; i++) {
        ioapic_irq[i] = qdev_get_gpio_in(ioapic_dev, i);
    }
     vms->gsi = qemu_allocate_irqs(virt_gsi_handler, ioapic_irq, IOAPIC_NUM_PINS);
}

