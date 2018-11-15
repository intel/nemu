/*
 * QEMU Light weight PCI Expander Host Bridge Emulation
 *
 * Copyright (C) 2018 Intel Corporation.
 *
 * Author:
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

#ifndef HW_PCI_VIRT_H
#define HW_PCI_VIRT_H

#include "hw/hw.h"
#include "qemu/range.h"
#include "hw/pci/pcie_host.h"

#define TYPE_PCI_VIRT_HOST      "pci-virt"
#define TYPE_PCI_VIRT_DEVICE    "pci-virt-device"

#define PCI_VIRT_HOST(obj) \
    OBJECT_CHECK(PCIVirtHost, (obj), TYPE_PCI_VIRT_HOST)

typedef struct PCIVirtHost {
    /*< private >*/
    PCIExpressHost parent_obj;
    /*< public >*/

    Range pci_hole;
    Range pci_hole64;
    uint64_t pci_hole64_size;

    char bus_path[8];
    uint16_t segment_nr;
} PCIVirtHost;

#endif
