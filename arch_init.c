/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "qemu/osdep.h"
#include "qemu-common.h"
#include "cpu.h"
#include "sysemu/sysemu.h"
#include "sysemu/arch_init.h"
#include "hw/pci/pci.h"
#include "qapi/qapi-commands-misc.h"
#include "qemu/config-file.h"
#include "qemu/error-report.h"
#include "hw/acpi/acpi.h"
#include "qemu/help_option.h"

int graphic_width = 800;
int graphic_height = 600;
int graphic_depth = 32;


#if defined(TARGET_ARM)
#define QEMU_ARCH QEMU_ARCH_ARM
#elif defined(TARGET_I386)
#define QEMU_ARCH QEMU_ARCH_I386
#endif

const uint32_t arch_type = QEMU_ARCH;

int kvm_available(void)
{
    return 1;
}


TargetInfo *qmp_query_target(Error **errp)
{
    TargetInfo *info = g_malloc0(sizeof(*info));

    info->arch = g_strdup(TARGET_NAME);

    return info;
}
