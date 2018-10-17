/*
 * QEMU ACPI hotplug utilities
 *
 * Copyright (C) 2016 Red Hat Inc
 *
 * Authors:
 *   Igor Mammedov <imammedo@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#ifndef ACPI_CPU_H
#define ACPI_CPU_H

#include "hw/qdev-core.h"
#include "hw/acpi/acpi.h"
#include "hw/acpi/aml-build.h"
#include "hw/hotplug.h"

#define CPU_NAME_FMT      "C%.03X"
#define CPUHP_RES_DEVICE  "PRES"
#define CPU_LOCK          "CPLK"
#define CPU_STS_METHOD    "CSTA"
#define CPU_SCAN_METHOD   "CSCN"
#define CPU_NOTIFY_METHOD "CTFY"
#define CPU_EJECT_METHOD  "CEJ0"
#define CPU_OST_METHOD    "COST"

#define CPU_ENABLED       "CPEN"
#define CPU_SELECTOR      "CSEL"
#define CPU_COMMAND       "CCMD"
#define CPU_DATA          "CDAT"
#define CPU_INSERT_EVENT  "CINS"
#define CPU_REMOVE_EVENT  "CRMV"
#define CPU_EJECT_EVENT   "CEJ0"

typedef struct AcpiCpuStatus {
    struct CPUState *cpu;
    uint64_t arch_id;
    bool is_inserting;
    bool is_removing;
    uint32_t ost_event;
    uint32_t ost_status;
} AcpiCpuStatus;

typedef struct CPUHotplugState {
    MemoryRegion ctrl_reg;
    uint32_t selector;
    uint8_t command;
    uint32_t dev_count;
    AcpiCpuStatus *devs;
} CPUHotplugState;

void acpi_cpu_plug_cb(HotplugHandler *hotplug_dev,
                      CPUHotplugState *cpu_st, DeviceState *dev, Error **errp);

void acpi_cpu_unplug_request_cb(HotplugHandler *hotplug_dev,
                                CPUHotplugState *cpu_st,
                                DeviceState *dev, Error **errp);

void acpi_cpu_unplug_cb(CPUHotplugState *cpu_st,
                        DeviceState *dev, Error **errp);

void cpu_hotplug_hw_init(MemoryRegion *as, Object *owner,
                         CPUHotplugState *state, hwaddr base_addr);

typedef struct CPUHotplugFeatures {
    bool acpi_1_compatible;
    bool has_legacy_cphp;
} CPUHotplugFeatures;

void build_cpus_aml(Aml *table, MachineState *machine, CPUHotplugFeatures opts,
                    hwaddr io_base,
                    const char *res_root,
                    const char *event_handler_method);

void acpi_cpu_ospm_status(CPUHotplugState *cpu_st, ACPIOSTInfoList ***list);

extern const VMStateDescription vmstate_cpu_hotplug;
#define VMSTATE_CPU_HOTPLUG(cpuhp, state) \
    VMSTATE_STRUCT(cpuhp, state, 1, \
                   vmstate_cpu_hotplug, CPUHotplugState)

#endif
