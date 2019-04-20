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
#include "sysemu/watchdog.h"
#include "hw/qdev-core.h"

int select_watchdog_action(const char *p)
{
    return -1;
}

int select_watchdog(const char *p)
{
    return 0;
}
