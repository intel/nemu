/*
 * QEMU i386 virt Platform stub
 *
 * Copyright Intel Corp. 2018
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
//#include "qemu-common.h"
#include "sysemu/watchdog.h"
#include "hw/qdev-core.h"
#include "hw/i386/pc.h"
#include "./../../vfio/pci.h"
#include "hw/i386/apic_internal.h"
#include "hw/bt.h"
#include "sysemu/bt.h"
#include "hw/audio/soundhw.h"
#include "hw/scsi/scsi.h"

DeviceState *isa_pic=NULL;

int select_watchdog_action(const char *p)
{
    return -1;
}

int pic_get_output(DeviceState *d)
{
    return false;
}

void vfio_display_finalize(VFIOPCIDevice *vdev)
{
}

void vfio_display_reset(VFIOPCIDevice *vdev)
{
}

int vfio_display_probe(VFIOPCIDevice *vdev, Error **errp)
{
    return 0;
}

void vapic_report_tpr_access(DeviceState *dev, CPUState *cpu, target_ulong ip,
                                     TPRAccess access)
{
}

int pic_read_irq(DeviceState *d)
{
    return 0;
}

struct HCIInfo *hci_init(const char *str)
{
    return NULL;
}

struct bt_scatternet_s *qemu_find_bt_vlan(int id)
{
    return NULL;
}

struct bt_device_s *bt_keyboard_init(struct bt_scatternet_s *net)
{
    return NULL;
}

static void null_hci_send(struct HCIInfo *hci, const uint8_t *data, int len)
{
}

static int null_hci_addr_set(struct HCIInfo *hci, const uint8_t *bd_addr)
{
    return -ENOTSUP;
}

struct HCIInfo null_hci = {
    .cmd_send = null_hci_send,
    .sco_send = null_hci_send,
    .acl_send = null_hci_send,
    .bdaddr_set = null_hci_addr_set,
};

struct HCIInfo *bt_new_hci(struct bt_scatternet_s *net)
{
    return NULL;
}

void select_soundhw(const char *optarg)
{
}

int select_watchdog(const char *p)
{
    return 0;
}

void soundhw_init(void)
{
}

int cdrom_read_toc(int nb_sectors, uint8_t *buf, int msf, int start_track)
{
    return -1;
}

int cdrom_read_toc_raw(int nb_sectors, uint8_t *buf, int msf, int session_num)
{
    return -1;
}
