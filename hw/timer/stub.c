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
#include "qemu/timer.h"
#include "qapi/qapi-commands-misc.h"
#include "qapi/qapi-events-misc.h"

#ifdef TARGET_I386
void qmp_rtc_reset_reinjection(Error **errp)
{
}
#endif
