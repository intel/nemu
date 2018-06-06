
#ifndef HW_I386_ACPI_BUILD_H
#define HW_I386_ACPI_BUILD_H

#include "hw/i386/pc.h"
#include "hw/acpi/bios-linker-loader.h"
#include "qapi/error.h"

void acpi_setup(void);
void acpi_build_nofw(PCMachineState *pcms, BIOSLinker *linker, Error **errp);

#endif
