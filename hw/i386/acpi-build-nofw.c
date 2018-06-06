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
    hwaddr      start;
    hwaddr      offset;
    uint64_t    size;
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
    e820_add_entry(start, size, E820_RESERVED);

    zone->start = start;
    zone->size = size;
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
    uint64_t max_size = zone->size;
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
    AcpiZone     *zone;
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
                                  AcpiZone *zone, hwaddr offset)
{
    PCLiteAcpiFileEntry file = { g_strdup(name), zone, offset };
    assert(!acpi_file_search(name));
    g_array_append_val(acpi_files->file_list, file);
}

static hwaddr acpi_file_get_addr(PCLiteAcpiFileEntry *file)
{
    return file->zone->start + file->offset;
}

static void acpi_patch_allocate(const BiosLinkerLoaderEntry *cmd,
                                        const BiosLinkerFileEntry *file,
                                        Error **errp)
{
    AcpiZone *zone = acpi_get_zone(cmd->alloc.zone);
    GArray *data = file->blob;
    unsigned size = acpi_data_len(data);
    hwaddr offset;
    hwaddr dest;
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

    dest = zone->start + offset;
    acpi_dprintf(" ACPI allocate, name %s, offset  %lx, size %x\n", file->name,
offset, size);
    cpu_physical_memory_write(dest, data->data, size);

    acpi_file_add(cmd->alloc.file, zone, offset);

 out:
    error_propagate(errp, local_err);
}

static void acpi_patch_add_pointer(const BiosLinkerLoaderEntry *cmd,
                                           Error **errp)
{
    PCLiteAcpiFileEntry *dest_file, *src_file;
    hwaddr dest;
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

    dest = acpi_file_get_addr(dest_file);
    cpu_physical_memory_read(dest + offset, &pointer, size);
    pointer += acpi_file_get_addr(src_file);
    cpu_physical_memory_write(dest + offset, &pointer, size);

 out:
    error_propagate(errp, local_err);
}

static void acpi_patch_add_checksum(const BiosLinkerLoaderEntry *cmd,
                                            Error **errp)
{
    PCLiteAcpiFileEntry *file = acpi_file_search(cmd->cksum.file);
    uint32_t start = cmd->cksum.start;
    uint32_t offset = cmd->cksum.offset;
    uint32_t length = cmd->cksum.length;
    hwaddr dest;
    uint8_t cksum, *buffer;
    Error *local_err = NULL;

    if (!file) {
        error_setg(&local_err, "Not found file %s", cmd->cksum.file);
        goto out;
    }
    buffer = g_malloc0(length);
    dest = acpi_file_get_addr(file);
    cpu_physical_memory_read(dest + start, buffer, length);
    cksum = acpi_checksum(buffer, length);
    cpu_physical_memory_write(dest + offset, &cksum, 1);
    g_free(buffer);

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
