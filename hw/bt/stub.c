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
#include "hw/bt.h"
#include "sysemu/bt.h"

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

void bt_vhci_init(struct HCIInfo *info)
{
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
