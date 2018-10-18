/*
 * debug exit port emulation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/sysbus.h"

#define TYPE_SYS_BUS_DEBUG_EXIT_DEVICE "sysbus-debugexit"
#define      SYS_BUS_DEBUG_EXIT_DEVICE(obj) \
     OBJECT_CHECK(SysBusDebugExitState, (obj), TYPE_SYS_BUS_DEBUG_EXIT_DEVICE)

typedef struct SysBusDebugExitState {
    SysBusDevice parent_obj;

    uint32_t iobase;
    uint32_t iosize;
    MemoryRegion io;
} SysBusDebugExitState;

static uint64_t debug_exit_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static void debug_exit_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned width)
{
    exit((val << 1) | 1);
}

static const MemoryRegionOps debug_exit_ops = {
    .read = debug_exit_read,
    .write = debug_exit_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void debug_exit_realizefn(DeviceState *d, Error **errp)
{
    SysBusDevice *dev = SYS_BUS_DEVICE(d);
    SysBusDebugExitState *sysbus_state = SYS_BUS_DEBUG_EXIT_DEVICE(d);

    memory_region_init_io(&sysbus_state->io, OBJECT(dev), &debug_exit_ops, sysbus_state,
                          TYPE_SYS_BUS_DEBUG_EXIT_DEVICE, sysbus_state->iosize);
    sysbus_add_io(dev, sysbus_state->iobase, &sysbus_state->io);
}

static Property debug_exit_properties[] = {
    DEFINE_PROP_UINT32("iobase", SysBusDebugExitState, iobase, 0x501),
    DEFINE_PROP_UINT32("iosize", SysBusDebugExitState, iosize, 0x02),
    DEFINE_PROP_END_OF_LIST(),
};

static void debug_exit_class_initfn(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = debug_exit_realizefn;
    dc->props = debug_exit_properties;
    dc->user_creatable = true;
    dc->hotpluggable = false;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo debug_exit_info = {
    .name          = TYPE_SYS_BUS_DEBUG_EXIT_DEVICE,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SysBusDebugExitState),
    .class_init    = debug_exit_class_initfn,
};

static void debug_exit_register_types(void)
{
    type_register_static(&debug_exit_info);
}

type_init(debug_exit_register_types)
