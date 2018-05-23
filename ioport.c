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
/*
 * splitted out ioport related stuffs from vl.c.
 */

#include "qemu/osdep.h"
#include "qemu-common.h"
#include "cpu.h"
#include "exec/ioport.h"
#include "trace-root.h"
#include "exec/memory.h"
#include "exec/address-spaces.h"

typedef struct MemoryRegionPortioList {
    MemoryRegion mr;
    void *portio_opaque;
    MemoryRegionPortio ports[];
} MemoryRegionPortioList;

static uint64_t unassigned_io_read(void *opaque, hwaddr addr, unsigned size)
{
    return -1ULL;
}

static void unassigned_io_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
}

const MemoryRegionOps unassigned_io_ops = {
    .read = unassigned_io_read,
    .write = unassigned_io_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

void cpu_outb(uint32_t addr, uint8_t val)
{
    trace_cpu_out(addr, 'b', val);
    address_space_write(&address_space_io, addr, MEMTXATTRS_UNSPECIFIED,
                        &val, 1);
}

void cpu_outw(uint32_t addr, uint16_t val)
{
    uint8_t buf[2];

    trace_cpu_out(addr, 'w', val);
    stw_p(buf, val);
    address_space_write(&address_space_io, addr, MEMTXATTRS_UNSPECIFIED,
                        buf, 2);
}

void cpu_outl(uint32_t addr, uint32_t val)
{
    uint8_t buf[4];

    trace_cpu_out(addr, 'l', val);
    stl_p(buf, val);
    address_space_write(&address_space_io, addr, MEMTXATTRS_UNSPECIFIED,
                        buf, 4);
}

uint8_t cpu_inb(uint32_t addr)
{
    uint8_t val;

    address_space_read(&address_space_io, addr, MEMTXATTRS_UNSPECIFIED,
                       &val, 1);
    trace_cpu_in(addr, 'b', val);
    return val;
}

uint16_t cpu_inw(uint32_t addr)
{
    uint8_t buf[2];
    uint16_t val;

    address_space_read(&address_space_io, addr, MEMTXATTRS_UNSPECIFIED, buf, 2);
    val = lduw_p(buf);
    trace_cpu_in(addr, 'w', val);
    return val;
}

uint32_t cpu_inl(uint32_t addr)
{
    uint8_t buf[4];
    uint32_t val;

    address_space_read(&address_space_io, addr, MEMTXATTRS_UNSPECIFIED, buf, 4);
    val = ldl_p(buf);
    trace_cpu_in(addr, 'l', val);
    return val;
}

