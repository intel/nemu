#include "qemu/osdep.h"
#include <glib.h>
#include "qemu-common.h"
#include "qemu/mmap-alloc.h"
#include "acpi-build.h"
#include "hw/i386/pc.h"
#include "hw/acpi/acpi.h"
#include "hw/acpi/aml-build.h"
#include "hw/acpi/bios-linker-loader.h"
#include "exec/memory.h"
#include "qapi/error.h"

/* #define DEBUG_ACPI */
#ifdef DEBUG_ACPI
#define acpi_dprintf(fmt, ...)                  \
    do {                                                \
        printf("ACPI: "fmt, ##__VA_ARGS__);     \
    } while (0)
#else
#define acpi_dprintf(fmt, ...)
#endif


typedef
struct AcpiZone {
    MemoryRegion *mr;
    hwaddr       start;
    hwaddr       offset;
} AcpiZone;
static AcpiZone acpi_himem_zone;
static AcpiZone acpi_fseg_zone;

#define ACPI_HIMEM_SIZE (256 * 1024)
#define ACPI_FSEG_SIZE  (0x100000 - 0xe0000)

static AcpiZone *acpi_get_zone(uint8_t zone)
{
    if (zone == BIOS_LINKER_LOADER_ALLOC_ZONE_HIGH) {
        return &acpi_himem_zone;
    } else if (zone == BIOS_LINKER_LOADER_ALLOC_ZONE_FSEG) {
        return &acpi_fseg_zone;
    } else {
        return NULL;
    }
}

static int acpi_zone_init(AcpiZone *zone, const char *name,
                                  hwaddr start, uint64_t size)
{
    void *buf;
    MemoryRegion *mr;

    buf = qemu_ram_mmap(-1, size, 0x1000, true);
    if (buf == MAP_FAILED) {
        return -1;
    }

    mr = g_malloc(sizeof(*mr));
    memory_region_init_ram_ptr(mr, NULL, name, size, buf);
    memory_region_add_subregion_overlap(get_system_memory(), start, mr, 0);
    e820_add_entry(start, size, E820_RESERVED);

    zone->mr = mr;
    zone->start = start;
    zone->offset = 0;

    return 0;
}

static void acpi_zones_init(PCMachineState *pcms)
{
    uint64_t start;

    assert(pcms->below_4g_mem_size >= ACPI_HIMEM_SIZE);
    start = pcms->below_4g_mem_size - ACPI_HIMEM_SIZE;
    acpi_zone_init(&acpi_himem_zone, "acpi_himem",
                           start, ACPI_HIMEM_SIZE);
    acpi_zone_init(&acpi_fseg_zone, "acpi_fseg",
                           0xe0000, ACPI_FSEG_SIZE);
}

/* return the offset within the corresponding zone, or ~0 for failure */
static hwaddr acpi_zone_alloc(AcpiZone *zone,
                                      uint64_t size, uint64_t align,
                                      Error **errp)
{
    hwaddr start = zone->start;
    hwaddr offset = zone->offset;
    uint64_t max_size = memory_region_size(zone->mr);
    uint64_t addr;
    Error *local_err = NULL;

    addr = ROUND_UP(start + offset, align);
    offset = addr - start;
    if (size > max_size || max_size - size < offset) {
        error_setg(&local_err, "Not enough space");
        goto out;
    }
    zone->offset = offset + size;

 out:
    error_propagate(errp, local_err);
    return offset;
}


typedef
struct PCLiteAcpiFileEntry {
    char         *name;
    MemoryRegion *mr;
    hwaddr       offset;
} PCLiteAcpiFileEntry;

typedef
struct PCLiteAcpiFiles {
    GArray *file_list;
} PCLiteAcpiFiles;

static PCLiteAcpiFiles *acpi_files;

static void acpi_files_init(void)
{
    acpi_files = g_new(PCLiteAcpiFiles, 1);
    acpi_files->file_list = g_array_new(false, true /* clear */,
                                                sizeof(PCLiteAcpiFileEntry));
}

static PCLiteAcpiFileEntry *acpi_file_search(const char *name)
{
    int i;
    GArray *file_list = acpi_files->file_list;
    PCLiteAcpiFileEntry *file;

    for (i = 0; i < file_list->len; i++) {
        file = &g_array_index(file_list, PCLiteAcpiFileEntry, i);
        if (!strcmp(file->name, name)) {
            return file;
        }
    }
    return NULL;
}

static void acpi_file_add(const char *name,
                                  MemoryRegion *mr, hwaddr offset)
{
    PCLiteAcpiFileEntry file = { g_strdup(name), mr, offset };
    assert(!acpi_file_search(name));
    g_array_append_val(acpi_files->file_list, file);
}

static void *acpi_file_get_ptr(PCLiteAcpiFileEntry *file)
{
    void *ptr = memory_region_get_ram_ptr(file->mr);
    return ptr + file->offset;
}

static hwaddr acpi_file_get_addr(PCLiteAcpiFileEntry *file)
{
    return file->mr->addr + file->offset;
}

static void acpi_patch_allocate(const BiosLinkerLoaderEntry *cmd,
                                        const BiosLinkerFileEntry *file,
                                        Error **errp)
{
    AcpiZone *zone = acpi_get_zone(cmd->alloc.zone);
    MemoryRegion *zone_mr = zone->mr;
    GArray *data = file->blob;
    unsigned size = acpi_data_len(data);
    hwaddr offset;
    void *dest;
    Error *local_err = NULL;

    assert(!strncmp(cmd->alloc.file, file->name, BIOS_LINKER_LOADER_FILESZ));

    if (!zone) {
        error_setg(&local_err, "Unknown zone type %d of file %s",
                   cmd->alloc.zone, cmd->alloc.file);
        goto out;
    }

    offset = acpi_zone_alloc(zone, size, cmd->alloc.align, &local_err);
    if (local_err) {
        goto out;
    }

    dest = memory_region_get_ram_ptr(zone_mr);
    memcpy(dest + offset, data->data, size);
    memory_region_set_dirty(zone_mr, offset, size);

    acpi_file_add(cmd->alloc.file, zone_mr, offset);

 out:
    error_propagate(errp, local_err);
}

static void acpi_patch_add_pointer(const BiosLinkerLoaderEntry *cmd,
                                           Error **errp)
{
    PCLiteAcpiFileEntry *dest_file, *src_file;
    void *dest;
    uint64_t pointer = 0;
    uint32_t offset = cmd->pointer.offset;
    uint32_t size = cmd->pointer.size;
    Error *local_err = NULL;

    dest_file = acpi_file_search(cmd->pointer.dest_file);
    if (!dest_file) {
        error_setg(&local_err, "Not found dest_file %s",
                   cmd->pointer.dest_file);
        goto out;
    }
    src_file = acpi_file_search(cmd->pointer.src_file);
    if (!src_file) {
        error_setg(&local_err, "Not found src_file %s",
                   cmd->pointer.src_file);
        goto out;
    }

    dest = acpi_file_get_ptr(dest_file);
    memcpy(&pointer, dest + offset, size);
    pointer += acpi_file_get_addr(src_file);
    memcpy(dest + offset, &pointer, size);
    memory_region_set_dirty(dest_file->mr, dest_file->offset + offset, size);

 out:
    error_propagate(errp, local_err);
}

static void acpi_patch_add_checksum(const BiosLinkerLoaderEntry *cmd,
                                            Error **errp)
{
    PCLiteAcpiFileEntry *file = acpi_file_search(cmd->cksum.file);
    uint32_t offset = cmd->cksum.offset;
    uint8_t *dest, *cksum;
    Error *local_err = NULL;

    if (!file) {
        error_setg(&local_err, "Not found file %s", cmd->cksum.file);
        goto out;
    }

    dest = acpi_file_get_ptr(file);
    cksum = dest + offset;
    *cksum = acpi_checksum(dest + cmd->cksum.start, cmd->cksum.length);
    memory_region_set_dirty(file->mr, file->offset + offset, sizeof(*cksum));

 out:
    error_propagate(errp, local_err);
}

static void acpi_patch(BIOSLinker *linker, Error **errp)
{
    void *cmd_blob_data = linker->cmd_blob->data;
    unsigned cmd_blob_len = linker->cmd_blob->len;
    uint64_t offset;
    const BiosLinkerLoaderEntry *cmd;
    const BiosLinkerFileEntry *file;
    Error *local_err = NULL;

    for (offset = 0; offset < cmd_blob_len; offset += sizeof(*cmd)) {
        cmd = cmd_blob_data + offset;

        switch (cmd->command) {
        case BIOS_LINKER_LOADER_COMMAND_ALLOCATE:
            file = bios_linker_find_file(linker, cmd->alloc.file);
            acpi_patch_allocate(cmd, file, &local_err);
            break;
        case BIOS_LINKER_LOADER_COMMAND_ADD_POINTER:
            acpi_patch_add_pointer(cmd, &local_err);
            break;
        case BIOS_LINKER_LOADER_COMMAND_ADD_CHECKSUM:
            acpi_patch_add_checksum(cmd, &local_err);
            break;
        default:
            acpi_dprintf("Ignore unknown command 0x%x\n", cmd->command);
            continue;
        }

        if (local_err) {
            goto out;
        }
    }

 out:
    error_propagate(errp, local_err);
}


void acpi_build_nofw(PCMachineState *pcms, BIOSLinker *linker, Error **errp)
{
    acpi_zones_init(pcms);
    acpi_files_init();
    acpi_patch(linker, errp);
}
