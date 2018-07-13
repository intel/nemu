#ifndef HW_ACPI_REDUCED_H
#define HW_ACPI_REDUCED_H

typedef struct MemMapEntry {
    hwaddr base;
    hwaddr size;
} MemMapEntry;

void acpi_reduced_setup(MachineState *machine, AcpiConfiguration *conf);

#endif
