#ifndef HW_ACPI_REDUCED_H
#define HW_ACPI_REDUCED_H

#define ACPI_REDUCED_SLEEP_LEVEL	  5
#define ACPI_REDUCED_SLEEP_ENABLE	  (1<<5) /* SLP_EN */
#define ACPI_REDUCED_SLEEP_CONTROL_IOPORT 0x3B0
#define ACPI_REDUCED_RESET_IOPORT	  0x3C0
#define ACPI_REDUCED_RESET_VALUE	  4

void acpi_reduced_setup(MachineState *machine, AcpiConfiguration *conf);

#endif
