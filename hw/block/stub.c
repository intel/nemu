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
#include "hw/scsi/scsi.h"

int cdrom_read_toc(int nb_sectors, uint8_t *buf, int msf, int start_track)
{
    return -1;
}

int cdrom_read_toc_raw(int nb_sectors, uint8_t *buf, int msf, int session_num)
{
    return -1;
}
