
#ifndef HW_I386_ACPI_BUILD_H
#define HW_I386_ACPI_BUILD_H

#include "hw/acpi/acpi.h"

/* Build methods */
GArray *build_madt(GArray *table_data, BIOSLinker *linker, MachineState *ms, AcpiConfiguration *conf);

/* ACPI setup */
void acpi_setup(MachineState *machine, AcpiConfiguration *conf);

#endif
