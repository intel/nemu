#ifndef QEMU_HW_ACPI_MEMORY_HOTPLUG_H
#define QEMU_HW_ACPI_MEMORY_HOTPLUG_H

#include "hw/qdev-core.h"
#include "hw/acpi/acpi.h"
#include "hw/acpi/aml-build.h"

#define MEMORY_SLOTS_NUMBER          "MDNR"
#define MEMORY_HOTPLUG_IO_REGION     "HPMR"
#define MEMORY_SLOT_ADDR_LOW         "MRBL"
#define MEMORY_SLOT_ADDR_HIGH        "MRBH"
#define MEMORY_SLOT_SIZE_LOW         "MRLL"
#define MEMORY_SLOT_SIZE_HIGH        "MRLH"
#define MEMORY_SLOT_PROXIMITY        "MPX"
#define MEMORY_SLOT_ENABLED          "MES"
#define MEMORY_SLOT_INSERT_EVENT     "MINS"
#define MEMORY_SLOT_REMOVE_EVENT     "MRMV"
#define MEMORY_SLOT_EJECT            "MEJ"
#define MEMORY_SLOT_SLECTOR          "MSEL"
#define MEMORY_SLOT_OST_EVENT        "MOEV"
#define MEMORY_SLOT_OST_STATUS       "MOSC"
#define MEMORY_SLOT_LOCK             "MLCK"
#define MEMORY_SLOT_STATUS_METHOD    "MRST"
#define MEMORY_SLOT_CRS_METHOD       "MCRS"
#define MEMORY_SLOT_OST_METHOD       "MOST"
#define MEMORY_SLOT_PROXIMITY_METHOD "MPXM"
#define MEMORY_SLOT_EJECT_METHOD     "MEJ0"
#define MEMORY_SLOT_NOTIFY_METHOD    "MTFY"
#define MEMORY_SLOT_SCAN_METHOD      "MSCN"
#define MEMORY_HOTPLUG_DEVICE        "MHPD"
#define MEMORY_HOTPLUG_IO_LEN         24
#define MEMORY_DEVICES_CONTAINER     "\\_SB.MHPC"

/**
 * MemStatus:
 * @is_removing: the memory device in slot has been requested to be ejected.
 *
 * This structure stores memory device's status.
 */
typedef struct MemStatus {
    DeviceState *dimm;
    bool is_enabled;
    bool is_inserting;
    bool is_removing;
    uint32_t ost_event;
    uint32_t ost_status;
} MemStatus;

typedef struct MemHotplugState {
    bool is_enabled; /* true if memory hotplug is supported */
    MemoryRegion io;
    uint32_t selector;
    uint32_t dev_count;
    MemStatus *devs;
} MemHotplugState;

void acpi_memory_hotplug_init(MemoryRegion *as, Object *owner,
                              MemHotplugState *state, uint16_t io_base);

void acpi_memory_plug_cb(HotplugHandler *hotplug_dev, MemHotplugState *mem_st,
                         DeviceState *dev, Error **errp);
void acpi_memory_unplug_request_cb(HotplugHandler *hotplug_dev,
                                   MemHotplugState *mem_st,
                                   DeviceState *dev, Error **errp);
void acpi_memory_unplug_cb(MemHotplugState *mem_st,
                           DeviceState *dev, Error **errp);

extern const VMStateDescription vmstate_memory_hotplug;
#define VMSTATE_MEMORY_HOTPLUG(memhp, state) \
    VMSTATE_STRUCT(memhp, state, 1, \
                   vmstate_memory_hotplug, MemHotplugState)

void acpi_memory_ospm_status(MemHotplugState *mem_st, ACPIOSTInfoList ***list);

void build_memory_hotplug_aml(Aml *table, uint32_t nr_mem,
                              const char *res_root,
                              const char *event_handler_method);
#endif
