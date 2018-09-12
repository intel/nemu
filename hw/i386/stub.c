/*
 * QEMU hw stub
 *
 * Copyright Intel Corp. 2018
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "multiboot.h"
#include "hw/i386/apic_internal.h"

int load_multiboot(FWCfgState *fw_cfg,
                   FILE *f,
                   const char *kernel_filename,
                   const char *initrd_filename,
                   const char *kernel_cmdline,
                   int kernel_file_size,
                   uint8_t *header)
{
    return 0;
}

void vapic_report_tpr_access(DeviceState *dev, CPUState *cs, target_ulong ip,
                             TPRAccess access)
{
}
