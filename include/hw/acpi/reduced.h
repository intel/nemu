#ifndef HW_ACPI_REDUCED_H
#define HW_ACPI_REDUCED_H

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
