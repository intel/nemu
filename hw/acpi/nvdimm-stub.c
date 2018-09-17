/*
 * QEMU acpi nvdimm stub
 *
 * Copyright Intel Corp. 2018
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "hw/mem/nvdimm.h"

void nvdimm_plug(AcpiNVDIMMState *state)
{
}

void nvdimm_init_acpi_state(AcpiNVDIMMState *state, MemoryRegion *io,
                            FWCfgState *fw_cfg, Object *owner)
{
}

void nvdimm_acpi_plug_cb(HotplugHandler *hotplug_dev, DeviceState *dev)
{
}

void nvdimm_build_acpi(GArray *table_offsets, GArray *table_data,
                       BIOSLinker *linker, AcpiNVDIMMState *state,
                       uint32_t ram_slots)
{
}
