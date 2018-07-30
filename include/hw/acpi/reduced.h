#ifndef HW_ACPI_REDUCED_H
#define HW_ACPI_REDUCED_H

#define ACPI_REDUCED_SLEEP_LEVEL	  5
#define ACPI_REDUCED_SLEEP_ENABLE	  (1<<5) /* SLP_EN */
#define ACPI_REDUCED_SLEEP_CONTROL_IOPORT 0x3B0

typedef struct Aml Aml;

typedef enum {
    GED_CPU_HOTPLUG    = 1,
    GED_MEMORY_HOTPLUG = 2,
    GED_PCI_HOTPLUG    = 3,
    GED_NVDIMM_HOTPLUG = 4,
} GedEventType;

typedef struct GedEvent {
    uint32_t     irq;
    GedEventType event;
} GedEvent;

void acpi_reduced_setup(MachineState *machine, AcpiConfiguration *conf);
void build_ged_aml(Aml *table, const char* name,
                   GedEvent *events, uint8_t events_size);

#endif
