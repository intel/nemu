/*
 * Basic CMOS device for virtual platform
 *
 * Copyright (c) Intel Corporation; author: Robert Bradford
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
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/i386/virt.h"

#define TYPE_VIRT_CMOS "virt-cmos"
#define VIRT_CMOS_DEVICE(obj) \
     OBJECT_CHECK(VirtCmosState, (obj), TYPE_VIRT_CMOS)

typedef struct VirtCmosState {
    SysBusDevice parent_obj;
    MemoryRegion io;
    
    uint8_t cmos_data[128];
    uint8_t cmos_index;
} VirtCmosState;

static void virt_cmos_ioport_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned width)
{
    VirtCmosState *s = VIRT_CMOS_DEVICE(opaque);

    s->cmos_index = val & 0x7f;
}

static uint64_t virt_cmos_ioport_read(void *opaque, hwaddr addr, unsigned width)
{
    VirtCmosState *s = VIRT_CMOS_DEVICE(opaque);

    return s->cmos_data[s->cmos_index];
}

static const MemoryRegionOps virt_cmos_ops = {
    .read = virt_cmos_ioport_read,
    .write = virt_cmos_ioport_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void virt_cmos_realizefn(DeviceState *dev, Error **errp)
{
    SysBusDevice *sys = SYS_BUS_DEVICE(dev);
    VirtCmosState *d = VIRT_CMOS_DEVICE(dev);
    
    memory_region_init_io(&d->io, OBJECT(dev), &virt_cmos_ops, d,
                          TYPE_VIRT_CMOS, 2);
    sysbus_add_io(sys, 0x70, &d->io);
}


static void virt_cmos_class_initfn(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = virt_cmos_realizefn;
    dc->user_creatable = false;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo virt_cmos = {
    .name          = TYPE_VIRT_CMOS,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(VirtCmosState),
    .class_init    = virt_cmos_class_initfn,
};


void virt_cmos_set(DeviceState *dev, uint8_t field, uint8_t value)
{
    VirtCmosState *s = VIRT_CMOS_DEVICE(dev);

    s->cmos_data[field] = value;
}

DeviceState *virt_cmos_init(void) {
    return sysbus_create_simple(TYPE_VIRT_CMOS, -1, NULL);
}

static void virt_cmos_register_types(void)
{
    type_register_static(&virt_cmos);
}


type_init(virt_cmos_register_types)
