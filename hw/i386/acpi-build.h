
#ifndef HW_I386_ACPI_BUILD_H
#define HW_I386_ACPI_BUILD_H

#include "hw/acpi/acpi.h"
#include "hw/acpi/bios-linker-loader.h"
#include "qapi/error.h"

/* Build methods */
GArray *build_madt(GArray *table_data, BIOSLinker *linker, MachineState *ms, AcpiConfiguration *conf);

/* ACPI setup */
void acpi_setup(MachineState *machine, AcpiConfiguration *conf);

void build_append_pci_bus_devices(Aml *parent_scope, PCIBus *bus,
                                         bool pcihp_bridge_en);
#endif
