/* Support for generating ACPI tables and passing them to Guests
 *
 * Copyright (C) 2015 Red Hat Inc
 *
 * Author: Michael S. Tsirkin <mst@redhat.com>
 * Author: Igor Mammedov <imammedo@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include <glib/gprintf.h>
#include "hw/acpi/aml-build.h"
#include "hw/mem/memory-device.h"
#include "qemu/bswap.h"
#include "qemu/bitops.h"
#include "sysemu/numa.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_bus.h"
#include "qemu/range.h"
#include "hw/pci/pci_bridge.h"
#include "hw/i386/pc.h"
#include "sysemu/tpm.h"
#include "hw/acpi/tpm.h"
#include "qom/qom-qobject.h"
#include "qapi/qmp/qnum.h"
#include "hw/acpi/pcihp.h"
#include "hw/i386/apic.h"

#define PCI_HOST_BRIDGE_CONFIG_ADDR        0xcf8
#define PCI_HOST_BRIDGE_IO_0_MIN_ADDR      0x0000
#define PCI_HOST_BRIDGE_IO_0_MAX_ADDR      0x0cf7
#define PCI_HOST_BRIDGE_IO_1_MIN_ADDR      0x0d00
#define PCI_HOST_BRIDGE_IO_1_MAX_ADDR      0xffff
#define PCI_VGA_MEM_BASE_ADDR              0x000a0000
#define PCI_VGA_MEM_MAX_ADDR               0x000bffff
#define IO_0_LEN                           0xcf8
#define VGA_MEM_LEN                        0x20000

static const char *pci_hosts[] = {
   "/machine/i440fx",
   "/machine/q35",
   "/machine/pcilite",
   NULL,
};

static GArray *build_alloc_array(void)
{
    return g_array_new(false, true /* clear */, 1);
}

static void build_free_array(GArray *array)
{
    g_array_free(array, true);
}

static void build_prepend_byte(GArray *array, uint8_t val)
{
    g_array_prepend_val(array, val);
}

static void build_append_byte(GArray *array, uint8_t val)
{
    g_array_append_val(array, val);
}

static void build_append_array(GArray *array, GArray *val)
{
    g_array_append_vals(array, val->data, val->len);
}

#define ACPI_NAMESEG_LEN 4

static void
build_append_nameseg(GArray *array, const char *seg)
{
    int len;

    len = strlen(seg);
    assert(len <= ACPI_NAMESEG_LEN);

    g_array_append_vals(array, seg, len);
    /* Pad up to ACPI_NAMESEG_LEN characters if necessary. */
    g_array_append_vals(array, "____", ACPI_NAMESEG_LEN - len);
}

static void GCC_FMT_ATTR(2, 0)
build_append_namestringv(GArray *array, const char *format, va_list ap)
{
    char *s;
    char **segs;
    char **segs_iter;
    int seg_count = 0;

    s = g_strdup_vprintf(format, ap);
    segs = g_strsplit(s, ".", 0);
    g_free(s);

    /* count segments */
    segs_iter = segs;
    while (*segs_iter) {
        ++segs_iter;
        ++seg_count;
    }
    /*
     * ACPI 5.0 spec: 20.2.2 Name Objects Encoding:
     * "SegCount can be from 1 to 255"
     */
    assert(seg_count > 0 && seg_count <= 255);

    /* handle RootPath || PrefixPath */
    s = *segs;
    while (*s == '\\' || *s == '^') {
        build_append_byte(array, *s);
        ++s;
    }

    switch (seg_count) {
    case 1:
        if (!*s) {
            build_append_byte(array, 0x00); /* NullName */
        } else {
            build_append_nameseg(array, s);
        }
        break;

    case 2:
        build_append_byte(array, 0x2E); /* DualNamePrefix */
        build_append_nameseg(array, s);
        build_append_nameseg(array, segs[1]);
        break;
    default:
        build_append_byte(array, 0x2F); /* MultiNamePrefix */
        build_append_byte(array, seg_count);

        /* handle the 1st segment manually due to prefix/root path */
        build_append_nameseg(array, s);

        /* add the rest of segments */
        segs_iter = segs + 1;
        while (*segs_iter) {
            build_append_nameseg(array, *segs_iter);
            ++segs_iter;
        }
        break;
    }
    g_strfreev(segs);
}

GCC_FMT_ATTR(2, 3)
static void build_append_namestring(GArray *array, const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    build_append_namestringv(array, format, ap);
    va_end(ap);
}

/* 5.4 Definition Block Encoding */
enum {
    PACKAGE_LENGTH_1BYTE_SHIFT = 6, /* Up to 63 - use extra 2 bits. */
    PACKAGE_LENGTH_2BYTE_SHIFT = 4,
    PACKAGE_LENGTH_3BYTE_SHIFT = 12,
    PACKAGE_LENGTH_4BYTE_SHIFT = 20,
};

static void
build_prepend_package_length(GArray *package, unsigned length, bool incl_self)
{
    uint8_t byte;
    unsigned length_bytes;

    if (length + 1 < (1 << PACKAGE_LENGTH_1BYTE_SHIFT)) {
        length_bytes = 1;
    } else if (length + 2 < (1 << PACKAGE_LENGTH_3BYTE_SHIFT)) {
        length_bytes = 2;
    } else if (length + 3 < (1 << PACKAGE_LENGTH_4BYTE_SHIFT)) {
        length_bytes = 3;
    } else {
        length_bytes = 4;
    }

    /*
     * NamedField uses PkgLength encoding but it doesn't include length
     * of PkgLength itself.
     */
    if (incl_self) {
        /*
         * PkgLength is the length of the inclusive length of the data
         * and PkgLength's length itself when used for terms with
         * explitit length.
         */
        length += length_bytes;
    }

    switch (length_bytes) {
    case 1:
        byte = length;
        build_prepend_byte(package, byte);
        return;
    case 4:
        byte = length >> PACKAGE_LENGTH_4BYTE_SHIFT;
        build_prepend_byte(package, byte);
        length &= (1 << PACKAGE_LENGTH_4BYTE_SHIFT) - 1;
        /* fall through */
    case 3:
        byte = length >> PACKAGE_LENGTH_3BYTE_SHIFT;
        build_prepend_byte(package, byte);
        length &= (1 << PACKAGE_LENGTH_3BYTE_SHIFT) - 1;
        /* fall through */
    case 2:
        byte = length >> PACKAGE_LENGTH_2BYTE_SHIFT;
        build_prepend_byte(package, byte);
        length &= (1 << PACKAGE_LENGTH_2BYTE_SHIFT) - 1;
        /* fall through */
    }
    /*
     * Most significant two bits of byte zero indicate how many following bytes
     * are in PkgLength encoding.
     */
    byte = ((length_bytes - 1) << PACKAGE_LENGTH_1BYTE_SHIFT) | length;
    build_prepend_byte(package, byte);
}

static void
build_append_pkg_length(GArray *array, unsigned length, bool incl_self)
{
    GArray *tmp = build_alloc_array();

    build_prepend_package_length(tmp, length, incl_self);
    build_append_array(array, tmp);
    build_free_array(tmp);
}

static void build_package(GArray *package, uint8_t op)
{
    build_prepend_package_length(package, package->len, true);
    build_prepend_byte(package, op);
}

static void build_extop_package(GArray *package, uint8_t op)
{
    build_package(package, op);
    build_prepend_byte(package, 0x5B); /* ExtOpPrefix */
}

void build_append_int_noprefix(GArray *table, uint64_t value, int size)
{
    int i;

    for (i = 0; i < size; ++i) {
        build_append_byte(table, value & 0xFF);
        value = value >> 8;
    }
}

static void build_append_int(GArray *table, uint64_t value)
{
    if (value == 0x00) {
        build_append_byte(table, 0x00); /* ZeroOp */
    } else if (value == 0x01) {
        build_append_byte(table, 0x01); /* OneOp */
    } else if (value <= 0xFF) {
        build_append_byte(table, 0x0A); /* BytePrefix */
        build_append_int_noprefix(table, value, 1);
    } else if (value <= 0xFFFF) {
        build_append_byte(table, 0x0B); /* WordPrefix */
        build_append_int_noprefix(table, value, 2);
    } else if (value <= 0xFFFFFFFF) {
        build_append_byte(table, 0x0C); /* DWordPrefix */
        build_append_int_noprefix(table, value, 4);
    } else {
        build_append_byte(table, 0x0E); /* QWordPrefix */
        build_append_int_noprefix(table, value, 8);
    }
}

/* Generic Address Structure (GAS)
 * ACPI 2.0/3.0: 5.2.3.1 Generic Address Structure
 * 2.0 compat note:
 *    @access_width must be 0, see ACPI 2.0:Table 5-1
 */
void build_append_gas(GArray *table, AmlAddressSpace as,
                      uint8_t bit_width, uint8_t bit_offset,
                      uint8_t access_width, uint64_t address)
{
    build_append_int_noprefix(table, as, 1);
    build_append_int_noprefix(table, bit_width, 1);
    build_append_int_noprefix(table, bit_offset, 1);
    build_append_int_noprefix(table, access_width, 1);
    build_append_int_noprefix(table, address, 8);
}

/*
 * Build NAME(XXXX, 0x00000000) where 0x00000000 is encoded as a dword,
 * and return the offset to 0x00000000 for runtime patching.
 *
 * Warning: runtime patching is best avoided. Only use this as
 * a replacement for DataTableRegion (for guests that don't
 * support it).
 */
int
build_append_named_dword(GArray *array, const char *name_format, ...)
{
    int offset;
    va_list ap;

    build_append_byte(array, 0x08); /* NameOp */
    va_start(ap, name_format);
    build_append_namestringv(array, name_format, ap);
    va_end(ap);

    build_append_byte(array, 0x0C); /* DWordPrefix */

    offset = array->len;
    build_append_int_noprefix(array, 0x00000000, 4);
    assert(array->len == offset + 4);

    return offset;
}

static GPtrArray *alloc_list;

static Aml *aml_alloc(void)
{
    Aml *var = g_new0(typeof(*var), 1);

    g_ptr_array_add(alloc_list, var);
    var->block_flags = AML_NO_OPCODE;
    var->buf = build_alloc_array();
    return var;
}

static Aml *aml_opcode(uint8_t op)
{
    Aml *var = aml_alloc();

    var->op  = op;
    var->block_flags = AML_OPCODE;
    return var;
}

static Aml *aml_bundle(uint8_t op, AmlBlockFlags flags)
{
    Aml *var = aml_alloc();

    var->op  = op;
    var->block_flags = flags;
    return var;
}

static void aml_free(gpointer data, gpointer user_data)
{
    Aml *var = data;
    build_free_array(var->buf);
    g_free(var);
}

Aml *init_aml_allocator(void)
{
    assert(!alloc_list);
    alloc_list = g_ptr_array_new();
    return aml_alloc();
}

void free_aml_allocator(void)
{
    g_ptr_array_foreach(alloc_list, aml_free, NULL);
    g_ptr_array_free(alloc_list, true);
    alloc_list = 0;
}

/* pack data with DefBuffer encoding */
static void build_buffer(GArray *array, uint8_t op)
{
    GArray *data = build_alloc_array();

    build_append_int(data, array->len);
    g_array_prepend_vals(array, data->data, data->len);
    build_free_array(data);
    build_package(array, op);
}

void aml_append(Aml *parent_ctx, Aml *child)
{
    GArray *buf = build_alloc_array();
    build_append_array(buf, child->buf);

    switch (child->block_flags) {
    case AML_OPCODE:
        build_append_byte(parent_ctx->buf, child->op);
        break;
    case AML_EXT_PACKAGE:
        build_extop_package(buf, child->op);
        break;
    case AML_PACKAGE:
        build_package(buf, child->op);
        break;
    case AML_RES_TEMPLATE:
        build_append_byte(buf, 0x79); /* EndTag */
        /*
         * checksum operations are treated as succeeded if checksum
         * field is zero. [ACPI Spec 1.0b, 6.4.2.8 End Tag]
         */
        build_append_byte(buf, 0);
        /* fall through, to pack resources in buffer */
    case AML_BUFFER:
        build_buffer(buf, child->op);
        break;
    case AML_NO_OPCODE:
        break;
    default:
        assert(0);
        break;
    }
    build_append_array(parent_ctx->buf, buf);
    build_free_array(buf);
}

/* ACPI 1.0b: 16.2.5.1 Namespace Modifier Objects Encoding: DefScope */
Aml *aml_scope(const char *name_format, ...)
{
    va_list ap;
    Aml *var = aml_bundle(0x10 /* ScopeOp */, AML_PACKAGE);
    va_start(ap, name_format);
    build_append_namestringv(var->buf, name_format, ap);
    va_end(ap);
    return var;
}

/* ACPI 1.0b: 16.2.5.3 Type 1 Opcodes Encoding: DefReturn */
Aml *aml_return(Aml *val)
{
    Aml *var = aml_opcode(0xA4 /* ReturnOp */);
    aml_append(var, val);
    return var;
}

/* ACPI 1.0b: 16.2.6.3 Debug Objects Encoding: DebugObj */
Aml *aml_debug(void)
{
    Aml *var = aml_alloc();
    build_append_byte(var->buf, 0x5B); /* ExtOpPrefix */
    build_append_byte(var->buf, 0x31); /* DebugOp */
    return var;
}

/*
 * ACPI 1.0b: 16.2.3 Data Objects Encoding:
 * encodes: ByteConst, WordConst, DWordConst, QWordConst, ZeroOp, OneOp
 */
Aml *aml_int(const uint64_t val)
{
    Aml *var = aml_alloc();
    build_append_int(var->buf, val);
    return var;
}

/*
 * helper to construct NameString, which returns Aml object
 * for using with aml_append or other aml_* terms
 */
Aml *aml_name(const char *name_format, ...)
{
    va_list ap;
    Aml *var = aml_alloc();
    va_start(ap, name_format);
    build_append_namestringv(var->buf, name_format, ap);
    va_end(ap);
    return var;
}

/* ACPI 1.0b: 16.2.5.1 Namespace Modifier Objects Encoding: DefName */
Aml *aml_name_decl(const char *name, Aml *val)
{
    Aml *var = aml_opcode(0x08 /* NameOp */);
    build_append_namestring(var->buf, "%s", name);
    aml_append(var, val);
    return var;
}

/* ACPI 1.0b: 16.2.6.1 Arg Objects Encoding */
Aml *aml_arg(int pos)
{
    uint8_t op = 0x68 /* ARG0 op */ + pos;

    assert(pos <= 6);
    return aml_opcode(op);
}

/* ACPI 2.0a: 17.2.4.4 Type 2 Opcodes Encoding: DefToInteger */
Aml *aml_to_integer(Aml *arg)
{
    Aml *var = aml_opcode(0x99 /* ToIntegerOp */);
    aml_append(var, arg);
    build_append_byte(var->buf, 0x00 /* NullNameOp */);
    return var;
}

/* ACPI 2.0a: 17.2.4.4 Type 2 Opcodes Encoding: DefToHexString */
Aml *aml_to_hexstring(Aml *src, Aml *dst)
{
    Aml *var = aml_opcode(0x98 /* ToHexStringOp */);
    aml_append(var, src);
    if (dst) {
        aml_append(var, dst);
    } else {
        build_append_byte(var->buf, 0x00 /* NullNameOp */);
    }
    return var;
}

/* ACPI 2.0a: 17.2.4.4 Type 2 Opcodes Encoding: DefToBuffer */
Aml *aml_to_buffer(Aml *src, Aml *dst)
{
    Aml *var = aml_opcode(0x96 /* ToBufferOp */);
    aml_append(var, src);
    if (dst) {
        aml_append(var, dst);
    } else {
        build_append_byte(var->buf, 0x00 /* NullNameOp */);
    }
    return var;
}

/* ACPI 1.0b: 16.2.5.4 Type 2 Opcodes Encoding: DefStore */
Aml *aml_store(Aml *val, Aml *target)
{
    Aml *var = aml_opcode(0x70 /* StoreOp */);
    aml_append(var, val);
    aml_append(var, target);
    return var;
}

/**
 * build_opcode_2arg_dst:
 * @op: 1-byte opcode
 * @arg1: 1st operand
 * @arg2: 2nd operand
 * @dst: optional target to store to, set to NULL if it's not required
 *
 * An internal helper to compose AML terms that have
 *   "Op Operand Operand Target"
 * pattern.
 *
 * Returns: The newly allocated and composed according to patter Aml object.
 */
static Aml *
build_opcode_2arg_dst(uint8_t op, Aml *arg1, Aml *arg2, Aml *dst)
{
    Aml *var = aml_opcode(op);
    aml_append(var, arg1);
    aml_append(var, arg2);
    if (dst) {
        aml_append(var, dst);
    } else {
        build_append_byte(var->buf, 0x00 /* NullNameOp */);
    }
    return var;
}

/* ACPI 1.0b: 16.2.5.4 Type 2 Opcodes Encoding: DefAnd */
Aml *aml_and(Aml *arg1, Aml *arg2, Aml *dst)
{
    return build_opcode_2arg_dst(0x7B /* AndOp */, arg1, arg2, dst);
}

/* ACPI 1.0b: 16.2.5.4 Type 2 Opcodes Encoding: DefOr */
Aml *aml_or(Aml *arg1, Aml *arg2, Aml *dst)
{
    return build_opcode_2arg_dst(0x7D /* OrOp */, arg1, arg2, dst);
}

/* ACPI 1.0b: 16.2.5.4 Type 2 Opcodes Encoding: DefLOr */
Aml *aml_lor(Aml *arg1, Aml *arg2)
{
    Aml *var = aml_opcode(0x91 /* LOrOp */);
    aml_append(var, arg1);
    aml_append(var, arg2);
    return var;
}

/* ACPI 1.0b: 16.2.5.4 Type 2 Opcodes Encoding: DefShiftLeft */
Aml *aml_shiftleft(Aml *arg1, Aml *count)
{
    return build_opcode_2arg_dst(0x79 /* ShiftLeftOp */, arg1, count, NULL);
}

/* ACPI 1.0b: 16.2.5.4 Type 2 Opcodes Encoding: DefShiftRight */
Aml *aml_shiftright(Aml *arg1, Aml *count, Aml *dst)
{
    return build_opcode_2arg_dst(0x7A /* ShiftRightOp */, arg1, count, dst);
}

/* ACPI 1.0b: 16.2.5.4 Type 2 Opcodes Encoding: DefLLess */
Aml *aml_lless(Aml *arg1, Aml *arg2)
{
    Aml *var = aml_opcode(0x95 /* LLessOp */);
    aml_append(var, arg1);
    aml_append(var, arg2);
    return var;
}

/* ACPI 1.0b: 16.2.5.4 Type 2 Opcodes Encoding: DefAdd */
Aml *aml_add(Aml *arg1, Aml *arg2, Aml *dst)
{
    return build_opcode_2arg_dst(0x72 /* AddOp */, arg1, arg2, dst);
}

/* ACPI 1.0b: 16.2.5.4 Type 2 Opcodes Encoding: DefSubtract */
Aml *aml_subtract(Aml *arg1, Aml *arg2, Aml *dst)
{
    return build_opcode_2arg_dst(0x74 /* SubtractOp */, arg1, arg2, dst);
}

/* ACPI 1.0b: 16.2.5.4 Type 2 Opcodes Encoding: DefIncrement */
Aml *aml_increment(Aml *arg)
{
    Aml *var = aml_opcode(0x75 /* IncrementOp */);
    aml_append(var, arg);
    return var;
}

/* ACPI 1.0b: 16.2.5.4 Type 2 Opcodes Encoding: DefDecrement */
Aml *aml_decrement(Aml *arg)
{
    Aml *var = aml_opcode(0x76 /* DecrementOp */);
    aml_append(var, arg);
    return var;
}

/* ACPI 1.0b: 16.2.5.4 Type 2 Opcodes Encoding: DefIndex */
Aml *aml_index(Aml *arg1, Aml *idx)
{
    return build_opcode_2arg_dst(0x88 /* IndexOp */, arg1, idx, NULL);
}

/* ACPI 1.0b: 16.2.5.3 Type 1 Opcodes Encoding: DefNotify */
Aml *aml_notify(Aml *arg1, Aml *arg2)
{
    Aml *var = aml_opcode(0x86 /* NotifyOp */);
    aml_append(var, arg1);
    aml_append(var, arg2);
    return var;
}

/* helper to call method without argument */
Aml *aml_call0(const char *method)
{
    Aml *var = aml_alloc();
    build_append_namestring(var->buf, "%s", method);
    return var;
}

/* helper to call method with 1 argument */
Aml *aml_call1(const char *method, Aml *arg1)
{
    Aml *var = aml_alloc();
    build_append_namestring(var->buf, "%s", method);
    aml_append(var, arg1);
    return var;
}

/* helper to call method with 2 arguments */
Aml *aml_call2(const char *method, Aml *arg1, Aml *arg2)
{
    Aml *var = aml_alloc();
    build_append_namestring(var->buf, "%s", method);
    aml_append(var, arg1);
    aml_append(var, arg2);
    return var;
}

/* helper to call method with 3 arguments */
Aml *aml_call3(const char *method, Aml *arg1, Aml *arg2, Aml *arg3)
{
    Aml *var = aml_alloc();
    build_append_namestring(var->buf, "%s", method);
    aml_append(var, arg1);
    aml_append(var, arg2);
    aml_append(var, arg3);
    return var;
}

/* helper to call method with 4 arguments */
Aml *aml_call4(const char *method, Aml *arg1, Aml *arg2, Aml *arg3, Aml *arg4)
{
    Aml *var = aml_alloc();
    build_append_namestring(var->buf, "%s", method);
    aml_append(var, arg1);
    aml_append(var, arg2);
    aml_append(var, arg3);
    aml_append(var, arg4);
    return var;
}

/* helper to call method with 5 arguments */
Aml *aml_call5(const char *method, Aml *arg1, Aml *arg2, Aml *arg3, Aml *arg4,
               Aml *arg5)
{
    Aml *var = aml_alloc();
    build_append_namestring(var->buf, "%s", method);
    aml_append(var, arg1);
    aml_append(var, arg2);
    aml_append(var, arg3);
    aml_append(var, arg4);
    aml_append(var, arg5);
    return var;
}

/*
 * ACPI 5.0: 6.4.3.8.1 GPIO Connection Descriptor
 * Type 1, Large Item Name 0xC
 */

static Aml *aml_gpio_connection(AmlGpioConnectionType type,
                                AmlConsumerAndProducer con_and_pro,
                                uint8_t flags, AmlPinConfig pin_config,
                                uint16_t output_drive,
                                uint16_t debounce_timeout,
                                const uint32_t pin_list[], uint32_t pin_count,
                                const char *resource_source_name,
                                const uint8_t *vendor_data,
                                uint16_t vendor_data_len)
{
    Aml *var = aml_alloc();
    const uint16_t min_desc_len = 0x16;
    uint16_t resource_source_name_len, length;
    uint16_t pin_table_offset, resource_source_name_offset, vendor_data_offset;
    uint32_t i;

    assert(resource_source_name);
    resource_source_name_len = strlen(resource_source_name) + 1;
    length = min_desc_len + resource_source_name_len + vendor_data_len;
    pin_table_offset = min_desc_len + 1;
    resource_source_name_offset = pin_table_offset + pin_count * 2;
    vendor_data_offset = resource_source_name_offset + resource_source_name_len;

    build_append_byte(var->buf, 0x8C);  /* GPIO Connection Descriptor */
    build_append_int_noprefix(var->buf, length, 2); /* Length */
    build_append_byte(var->buf, 1);     /* Revision ID */
    build_append_byte(var->buf, type);  /* GPIO Connection Type */
    /* General Flags (2 bytes) */
    build_append_int_noprefix(var->buf, con_and_pro, 2);
    /* Interrupt and IO Flags (2 bytes) */
    build_append_int_noprefix(var->buf, flags, 2);
    /* Pin Configuration 0 = Default 1 = Pull-up 2 = Pull-down 3 = No Pull */
    build_append_byte(var->buf, pin_config);
    /* Output Drive Strength (2 bytes) */
    build_append_int_noprefix(var->buf, output_drive, 2);
    /* Debounce Timeout (2 bytes) */
    build_append_int_noprefix(var->buf, debounce_timeout, 2);
    /* Pin Table Offset (2 bytes) */
    build_append_int_noprefix(var->buf, pin_table_offset, 2);
    build_append_byte(var->buf, 0);     /* Resource Source Index */
    /* Resource Source Name Offset (2 bytes) */
    build_append_int_noprefix(var->buf, resource_source_name_offset, 2);
    /* Vendor Data Offset (2 bytes) */
    build_append_int_noprefix(var->buf, vendor_data_offset, 2);
    /* Vendor Data Length (2 bytes) */
    build_append_int_noprefix(var->buf, vendor_data_len, 2);
    /* Pin Number (2n bytes)*/
    for (i = 0; i < pin_count; i++) {
        build_append_int_noprefix(var->buf, pin_list[i], 2);
    }

    /* Resource Source Name */
    build_append_namestring(var->buf, "%s", resource_source_name);
    build_append_byte(var->buf, '\0');

    /* Vendor-defined Data */
    if (vendor_data != NULL) {
        g_array_append_vals(var->buf, vendor_data, vendor_data_len);
    }

    return var;
}

/*
 * ACPI 5.0: 19.5.53
 * GpioInt(GPIO Interrupt Connection Resource Descriptor Macro)
 */
Aml *aml_gpio_int(AmlConsumerAndProducer con_and_pro,
                  AmlLevelAndEdge edge_level,
                  AmlActiveHighAndLow active_level, AmlShared shared,
                  AmlPinConfig pin_config, uint16_t debounce_timeout,
                  const uint32_t pin_list[], uint32_t pin_count,
                  const char *resource_source_name,
                  const uint8_t *vendor_data, uint16_t vendor_data_len)
{
    uint8_t flags = edge_level | (active_level << 1) | (shared << 3);

    return aml_gpio_connection(AML_INTERRUPT_CONNECTION, con_and_pro, flags,
                               pin_config, 0, debounce_timeout, pin_list,
                               pin_count, resource_source_name, vendor_data,
                               vendor_data_len);
}

/*
 * ACPI 1.0b: 6.4.3.4 32-Bit Fixed Location Memory Range Descriptor
 * (Type 1, Large Item Name 0x6)
 */
Aml *aml_memory32_fixed(uint32_t addr, uint32_t size,
                        AmlReadAndWrite read_and_write)
{
    Aml *var = aml_alloc();
    build_append_byte(var->buf, 0x86); /* Memory32Fixed Resource Descriptor */
    build_append_byte(var->buf, 9);    /* Length, bits[7:0] value = 9 */
    build_append_byte(var->buf, 0);    /* Length, bits[15:8] value = 0 */
    build_append_byte(var->buf, read_and_write); /* Write status, 1 rw 0 ro */

    /* Range base address */
    build_append_byte(var->buf, extract32(addr, 0, 8));  /* bits[7:0] */
    build_append_byte(var->buf, extract32(addr, 8, 8));  /* bits[15:8] */
    build_append_byte(var->buf, extract32(addr, 16, 8)); /* bits[23:16] */
    build_append_byte(var->buf, extract32(addr, 24, 8)); /* bits[31:24] */

    /* Range length */
    build_append_byte(var->buf, extract32(size, 0, 8));  /* bits[7:0] */
    build_append_byte(var->buf, extract32(size, 8, 8));  /* bits[15:8] */
    build_append_byte(var->buf, extract32(size, 16, 8)); /* bits[23:16] */
    build_append_byte(var->buf, extract32(size, 24, 8)); /* bits[31:24] */
    return var;
}

/*
 * ACPI 5.0: 6.4.3.6 Extended Interrupt Descriptor
 * Type 1, Large Item Name 0x9
 */
Aml *aml_interrupt(AmlConsumerAndProducer con_and_pro,
                   AmlLevelAndEdge level_and_edge,
                   AmlActiveHighAndLow high_and_low, AmlShared shared,
                   uint32_t *irq_list, uint8_t irq_count)
{
    int i;
    Aml *var = aml_alloc();
    uint8_t irq_flags = con_and_pro | (level_and_edge << 1)
                        | (high_and_low << 2) | (shared << 3);
    const int header_bytes_in_len = 2;
    uint16_t len = header_bytes_in_len + irq_count * sizeof(uint32_t);

    assert(irq_count > 0);

    build_append_byte(var->buf, 0x89); /* Extended irq descriptor */
    build_append_byte(var->buf, len & 0xFF); /* Length, bits[7:0] */
    build_append_byte(var->buf, len >> 8); /* Length, bits[15:8] */
    build_append_byte(var->buf, irq_flags); /* Interrupt Vector Information. */
    build_append_byte(var->buf, irq_count);   /* Interrupt table length */

    /* Interrupt Number List */
    for (i = 0; i < irq_count; i++) {
        build_append_int_noprefix(var->buf, irq_list[i], 4);
    }
    return var;
}

/* ACPI 1.0b: 6.4.2.5 I/O Port Descriptor */
Aml *aml_io(AmlIODecode dec, uint16_t min_base, uint16_t max_base,
            uint8_t aln, uint8_t len)
{
    Aml *var = aml_alloc();
    build_append_byte(var->buf, 0x47); /* IO port descriptor */
    build_append_byte(var->buf, dec);
    build_append_byte(var->buf, min_base & 0xff);
    build_append_byte(var->buf, (min_base >> 8) & 0xff);
    build_append_byte(var->buf, max_base & 0xff);
    build_append_byte(var->buf, (max_base >> 8) & 0xff);
    build_append_byte(var->buf, aln);
    build_append_byte(var->buf, len);
    return var;
}

/*
 * ACPI 1.0b: 6.4.2.1.1 ASL Macro for IRQ Descriptor
 *
 * More verbose description at:
 * ACPI 5.0: 19.5.64 IRQNoFlags (Interrupt Resource Descriptor Macro)
 *           6.4.2.1 IRQ Descriptor
 */
Aml *aml_irq_no_flags(uint8_t irq)
{
    uint16_t irq_mask;
    Aml *var = aml_alloc();

    assert(irq < 16);
    build_append_byte(var->buf, 0x22); /* IRQ descriptor 2 byte form */

    irq_mask = 1U << irq;
    build_append_byte(var->buf, irq_mask & 0xFF); /* IRQ mask bits[7:0] */
    build_append_byte(var->buf, irq_mask >> 8); /* IRQ mask bits[15:8] */
    return var;
}

/* ACPI 1.0b: 16.2.5.4 Type 2 Opcodes Encoding: DefLNot */
Aml *aml_lnot(Aml *arg)
{
    Aml *var = aml_opcode(0x92 /* LNotOp */);
    aml_append(var, arg);
    return var;
}

/* ACPI 1.0b: 16.2.5.4 Type 2 Opcodes Encoding: DefLEqual */
Aml *aml_equal(Aml *arg1, Aml *arg2)
{
    Aml *var = aml_opcode(0x93 /* LequalOp */);
    aml_append(var, arg1);
    aml_append(var, arg2);
    return var;
}

/* ACPI 1.0b: 16.2.5.4 Type 2 Opcodes Encoding: DefLGreater */
Aml *aml_lgreater(Aml *arg1, Aml *arg2)
{
    Aml *var = aml_opcode(0x94 /* LGreaterOp */);
    aml_append(var, arg1);
    aml_append(var, arg2);
    return var;
}

/* ACPI 1.0b: 16.2.5.4 Type 2 Opcodes Encoding: DefLGreaterEqual */
Aml *aml_lgreater_equal(Aml *arg1, Aml *arg2)
{
    /* LGreaterEqualOp := LNotOp LLessOp */
    Aml *var = aml_opcode(0x92 /* LNotOp */);
    build_append_byte(var->buf, 0x95 /* LLessOp */);
    aml_append(var, arg1);
    aml_append(var, arg2);
    return var;
}

/* ACPI 1.0b: 16.2.5.3 Type 1 Opcodes Encoding: DefIfElse */
Aml *aml_if(Aml *predicate)
{
    Aml *var = aml_bundle(0xA0 /* IfOp */, AML_PACKAGE);
    aml_append(var, predicate);
    return var;
}

/* ACPI 1.0b: 16.2.5.3 Type 1 Opcodes Encoding: DefElse */
Aml *aml_else(void)
{
    Aml *var = aml_bundle(0xA1 /* ElseOp */, AML_PACKAGE);
    return var;
}

/* ACPI 1.0b: 16.2.5.3 Type 1 Opcodes Encoding: DefWhile */
Aml *aml_while(Aml *predicate)
{
    Aml *var = aml_bundle(0xA2 /* WhileOp */, AML_PACKAGE);
    aml_append(var, predicate);
    return var;
}

/* ACPI 1.0b: 16.2.5.2 Named Objects Encoding: DefMethod */
Aml *aml_method(const char *name, int arg_count, AmlSerializeFlag sflag)
{
    Aml *var = aml_bundle(0x14 /* MethodOp */, AML_PACKAGE);
    int methodflags;

    /*
     * MethodFlags:
     *   bit 0-2: ArgCount (0-7)
     *   bit 3: SerializeFlag
     *     0: NotSerialized
     *     1: Serialized
     *   bit 4-7: reserved (must be 0)
     */
    assert(arg_count < 8);
    methodflags = arg_count | (sflag << 3);

    build_append_namestring(var->buf, "%s", name);
    build_append_byte(var->buf, methodflags); /* MethodFlags: ArgCount */
    return var;
}

/* ACPI 1.0b: 16.2.5.2 Named Objects Encoding: DefDevice */
Aml *aml_device(const char *name_format, ...)
{
    va_list ap;
    Aml *var = aml_bundle(0x82 /* DeviceOp */, AML_EXT_PACKAGE);
    va_start(ap, name_format);
    build_append_namestringv(var->buf, name_format, ap);
    va_end(ap);
    return var;
}

/* ACPI 1.0b: 6.4.1 ASL Macros for Resource Descriptors */
Aml *aml_resource_template(void)
{
    /* ResourceTemplate is a buffer of Resources with EndTag at the end */
    Aml *var = aml_bundle(0x11 /* BufferOp */, AML_RES_TEMPLATE);
    return var;
}

/* ACPI 1.0b: 16.2.5.4 Type 2 Opcodes Encoding: DefBuffer
 * Pass byte_list as NULL to request uninitialized buffer to reserve space.
 */
Aml *aml_buffer(int buffer_size, uint8_t *byte_list)
{
    int i;
    Aml *var = aml_bundle(0x11 /* BufferOp */, AML_BUFFER);

    for (i = 0; i < buffer_size; i++) {
        if (byte_list == NULL) {
            build_append_byte(var->buf, 0x0);
        } else {
            build_append_byte(var->buf, byte_list[i]);
        }
    }

    return var;
}

/* ACPI 1.0b: 16.2.5.4 Type 2 Opcodes Encoding: DefPackage */
Aml *aml_package(uint8_t num_elements)
{
    Aml *var = aml_bundle(0x12 /* PackageOp */, AML_PACKAGE);
    build_append_byte(var->buf, num_elements);
    return var;
}

/* ACPI 1.0b: 16.2.5.2 Named Objects Encoding: DefOpRegion */
Aml *aml_operation_region(const char *name, AmlRegionSpace rs,
                          Aml *offset, uint32_t len)
{
    Aml *var = aml_alloc();
    build_append_byte(var->buf, 0x5B); /* ExtOpPrefix */
    build_append_byte(var->buf, 0x80); /* OpRegionOp */
    build_append_namestring(var->buf, "%s", name);
    build_append_byte(var->buf, rs);
    aml_append(var, offset);
    build_append_int(var->buf, len);
    return var;
}

/* ACPI 1.0b: 16.2.5.2 Named Objects Encoding: NamedField */
Aml *aml_named_field(const char *name, unsigned length)
{
    Aml *var = aml_alloc();
    build_append_nameseg(var->buf, name);
    build_append_pkg_length(var->buf, length, false);
    return var;
}

/* ACPI 1.0b: 16.2.5.2 Named Objects Encoding: ReservedField */
Aml *aml_reserved_field(unsigned length)
{
    Aml *var = aml_alloc();
    /* ReservedField  := 0x00 PkgLength */
    build_append_byte(var->buf, 0x00);
    build_append_pkg_length(var->buf, length, false);
    return var;
}

/* ACPI 1.0b: 16.2.5.2 Named Objects Encoding: DefField */
Aml *aml_field(const char *name, AmlAccessType type, AmlLockRule lock,
               AmlUpdateRule rule)
{
    Aml *var = aml_bundle(0x81 /* FieldOp */, AML_EXT_PACKAGE);
    uint8_t flags = rule << 5 | type;

    flags |= lock << 4; /* LockRule at 4 bit offset */

    build_append_namestring(var->buf, "%s", name);
    build_append_byte(var->buf, flags);
    return var;
}

static
Aml *create_field_common(int opcode, Aml *srcbuf, Aml *index, const char *name)
{
    Aml *var = aml_opcode(opcode);
    aml_append(var, srcbuf);
    aml_append(var, index);
    build_append_namestring(var->buf, "%s", name);
    return var;
}

/* ACPI 1.0b: 16.2.5.2 Named Objects Encoding: DefCreateField */
Aml *aml_create_field(Aml *srcbuf, Aml *bit_index, Aml *num_bits,
                      const char *name)
{
    Aml *var = aml_alloc();
    build_append_byte(var->buf, 0x5B); /* ExtOpPrefix */
    build_append_byte(var->buf, 0x13); /* CreateFieldOp */
    aml_append(var, srcbuf);
    aml_append(var, bit_index);
    aml_append(var, num_bits);
    build_append_namestring(var->buf, "%s", name);
    return var;
}

/* ACPI 1.0b: 16.2.5.2 Named Objects Encoding: DefCreateDWordField */
Aml *aml_create_dword_field(Aml *srcbuf, Aml *index, const char *name)
{
    return create_field_common(0x8A /* CreateDWordFieldOp */,
                               srcbuf, index, name);
}

/* ACPI 2.0a: 17.2.4.2 Named Objects Encoding: DefCreateQWordField */
Aml *aml_create_qword_field(Aml *srcbuf, Aml *index, const char *name)
{
    return create_field_common(0x8F /* CreateQWordFieldOp */,
                               srcbuf, index, name);
}

/* ACPI 1.0b: 16.2.3 Data Objects Encoding: String */
Aml *aml_string(const char *name_format, ...)
{
    Aml *var = aml_opcode(0x0D /* StringPrefix */);
    va_list ap;
    char *s;
    int len;

    va_start(ap, name_format);
    len = g_vasprintf(&s, name_format, ap);
    va_end(ap);

    g_array_append_vals(var->buf, s, len + 1);
    g_free(s);

    return var;
}

/* ACPI 1.0b: 16.2.6.2 Local Objects Encoding */
Aml *aml_local(int num)
{
    uint8_t op = 0x60 /* Local0Op */ + num;

    assert(num <= 7);
    return aml_opcode(op);
}

/* ACPI 2.0a: 17.2.2 Data Objects Encoding: DefVarPackage */
Aml *aml_varpackage(uint32_t num_elements)
{
    Aml *var = aml_bundle(0x13 /* VarPackageOp */, AML_PACKAGE);
    build_append_int(var->buf, num_elements);
    return var;
}

/* ACPI 1.0b: 16.2.5.2 Named Objects Encoding: DefProcessor */
Aml *aml_processor(uint8_t proc_id, uint32_t pblk_addr, uint8_t pblk_len,
                   const char *name_format, ...)
{
    va_list ap;
    Aml *var = aml_bundle(0x83 /* ProcessorOp */, AML_EXT_PACKAGE);
    va_start(ap, name_format);
    build_append_namestringv(var->buf, name_format, ap);
    va_end(ap);
    build_append_byte(var->buf, proc_id); /* ProcID */
    build_append_int_noprefix(var->buf, pblk_addr, sizeof(pblk_addr));
    build_append_byte(var->buf, pblk_len); /* PblkLen */
    return var;
}

static uint8_t Hex2Digit(char c)
{
    if (c >= 'A') {
        return c - 'A' + 10;
    }

    return c - '0';
}

/* ACPI 1.0b: 15.2.3.6.4.1 EISAID Macro - Convert EISA ID String To Integer */
Aml *aml_eisaid(const char *str)
{
    Aml *var = aml_alloc();
    uint32_t id;

    g_assert(strlen(str) == 7);
    id = (str[0] - 0x40) << 26 |
    (str[1] - 0x40) << 21 |
    (str[2] - 0x40) << 16 |
    Hex2Digit(str[3]) << 12 |
    Hex2Digit(str[4]) << 8 |
    Hex2Digit(str[5]) << 4 |
    Hex2Digit(str[6]);

    build_append_byte(var->buf, 0x0C); /* DWordPrefix */
    build_append_int_noprefix(var->buf, bswap32(id), sizeof(id));
    return var;
}

/* ACPI 1.0b: 6.4.3.5.5 Word Address Space Descriptor: bytes 3-5 */
static Aml *aml_as_desc_header(AmlResourceType type, AmlMinFixed min_fixed,
                               AmlMaxFixed max_fixed, AmlDecode dec,
                               uint8_t type_flags)
{
    uint8_t flags = max_fixed | min_fixed | dec;
    Aml *var = aml_alloc();

    build_append_byte(var->buf, type);
    build_append_byte(var->buf, flags);
    build_append_byte(var->buf, type_flags); /* Type Specific Flags */
    return var;
}

/* ACPI 1.0b: 6.4.3.5.5 Word Address Space Descriptor */
static Aml *aml_word_as_desc(AmlResourceType type, AmlMinFixed min_fixed,
                             AmlMaxFixed max_fixed, AmlDecode dec,
                             uint16_t addr_gran, uint16_t addr_min,
                             uint16_t addr_max, uint16_t addr_trans,
                             uint16_t len, uint8_t type_flags)
{
    Aml *var = aml_alloc();

    build_append_byte(var->buf, 0x88); /* Word Address Space Descriptor */
    /* minimum length since we do not encode optional fields */
    build_append_byte(var->buf, 0x0D);
    build_append_byte(var->buf, 0x0);

    aml_append(var,
        aml_as_desc_header(type, min_fixed, max_fixed, dec, type_flags));
    build_append_int_noprefix(var->buf, addr_gran, sizeof(addr_gran));
    build_append_int_noprefix(var->buf, addr_min, sizeof(addr_min));
    build_append_int_noprefix(var->buf, addr_max, sizeof(addr_max));
    build_append_int_noprefix(var->buf, addr_trans, sizeof(addr_trans));
    build_append_int_noprefix(var->buf, len, sizeof(len));
    return var;
}

/* ACPI 1.0b: 6.4.3.5.3 DWord Address Space Descriptor */
static Aml *aml_dword_as_desc(AmlResourceType type, AmlMinFixed min_fixed,
                              AmlMaxFixed max_fixed, AmlDecode dec,
                              uint32_t addr_gran, uint32_t addr_min,
                              uint32_t addr_max, uint32_t addr_trans,
                              uint32_t len, uint8_t type_flags)
{
    Aml *var = aml_alloc();

    build_append_byte(var->buf, 0x87); /* DWord Address Space Descriptor */
    /* minimum length since we do not encode optional fields */
    build_append_byte(var->buf, 23);
    build_append_byte(var->buf, 0x0);


    aml_append(var,
        aml_as_desc_header(type, min_fixed, max_fixed, dec, type_flags));
    build_append_int_noprefix(var->buf, addr_gran, sizeof(addr_gran));
    build_append_int_noprefix(var->buf, addr_min, sizeof(addr_min));
    build_append_int_noprefix(var->buf, addr_max, sizeof(addr_max));
    build_append_int_noprefix(var->buf, addr_trans, sizeof(addr_trans));
    build_append_int_noprefix(var->buf, len, sizeof(len));
    return var;
}

/* ACPI 1.0b: 6.4.3.5.1 QWord Address Space Descriptor */
static Aml *aml_qword_as_desc(AmlResourceType type, AmlMinFixed min_fixed,
                              AmlMaxFixed max_fixed, AmlDecode dec,
                              uint64_t addr_gran, uint64_t addr_min,
                              uint64_t addr_max, uint64_t addr_trans,
                              uint64_t len, uint8_t type_flags)
{
    Aml *var = aml_alloc();

    build_append_byte(var->buf, 0x8A); /* QWord Address Space Descriptor */
    /* minimum length since we do not encode optional fields */
    build_append_byte(var->buf, 0x2B);
    build_append_byte(var->buf, 0x0);

    aml_append(var,
        aml_as_desc_header(type, min_fixed, max_fixed, dec, type_flags));
    build_append_int_noprefix(var->buf, addr_gran, sizeof(addr_gran));
    build_append_int_noprefix(var->buf, addr_min, sizeof(addr_min));
    build_append_int_noprefix(var->buf, addr_max, sizeof(addr_max));
    build_append_int_noprefix(var->buf, addr_trans, sizeof(addr_trans));
    build_append_int_noprefix(var->buf, len, sizeof(len));
    return var;
}

/*
 * ACPI 1.0b: 6.4.3.5.6 ASL Macros for WORD Address Descriptor
 *
 * More verbose description at:
 * ACPI 5.0: 19.5.141 WordBusNumber (Word Bus Number Resource Descriptor Macro)
 */
Aml *aml_word_bus_number(AmlMinFixed min_fixed, AmlMaxFixed max_fixed,
                         AmlDecode dec, uint16_t addr_gran,
                         uint16_t addr_min, uint16_t addr_max,
                         uint16_t addr_trans, uint16_t len)

{
    return aml_word_as_desc(AML_BUS_NUMBER_RANGE, min_fixed, max_fixed, dec,
                            addr_gran, addr_min, addr_max, addr_trans, len, 0);
}

/*
 * ACPI 1.0b: 6.4.3.5.6 ASL Macros for WORD Address Descriptor
 *
 * More verbose description at:
 * ACPI 5.0: 19.5.142 WordIO (Word IO Resource Descriptor Macro)
 */
Aml *aml_word_io(AmlMinFixed min_fixed, AmlMaxFixed max_fixed,
                 AmlDecode dec, AmlISARanges isa_ranges,
                 uint16_t addr_gran, uint16_t addr_min,
                 uint16_t addr_max, uint16_t addr_trans,
                 uint16_t len)

{
    return aml_word_as_desc(AML_IO_RANGE, min_fixed, max_fixed, dec,
                            addr_gran, addr_min, addr_max, addr_trans, len,
                            isa_ranges);
}

/*
 * ACPI 1.0b: 6.4.3.5.4 ASL Macros for DWORD Address Descriptor
 *
 * More verbose description at:
 * ACPI 5.0: 19.5.33 DWordIO (DWord IO Resource Descriptor Macro)
 */
Aml *aml_dword_io(AmlMinFixed min_fixed, AmlMaxFixed max_fixed,
                 AmlDecode dec, AmlISARanges isa_ranges,
                 uint32_t addr_gran, uint32_t addr_min,
                 uint32_t addr_max, uint32_t addr_trans,
                 uint32_t len)

{
    return aml_dword_as_desc(AML_IO_RANGE, min_fixed, max_fixed, dec,
                            addr_gran, addr_min, addr_max, addr_trans, len,
                            isa_ranges);
}

/*
 * ACPI 1.0b: 6.4.3.5.4 ASL Macros for DWORD Address Space Descriptor
 *
 * More verbose description at:
 * ACPI 5.0: 19.5.34 DWordMemory (DWord Memory Resource Descriptor Macro)
 */
Aml *aml_dword_memory(AmlDecode dec, AmlMinFixed min_fixed,
                      AmlMaxFixed max_fixed, AmlCacheable cacheable,
                      AmlReadAndWrite read_and_write,
                      uint32_t addr_gran, uint32_t addr_min,
                      uint32_t addr_max, uint32_t addr_trans,
                      uint32_t len)
{
    uint8_t flags = read_and_write | (cacheable << 1);

    return aml_dword_as_desc(AML_MEMORY_RANGE, min_fixed, max_fixed,
                             dec, addr_gran, addr_min, addr_max,
                             addr_trans, len, flags);
}

/*
 * ACPI 1.0b: 6.4.3.5.2 ASL Macros for QWORD Address Space Descriptor
 *
 * More verbose description at:
 * ACPI 5.0: 19.5.102 QWordMemory (QWord Memory Resource Descriptor Macro)
 */
Aml *aml_qword_memory(AmlDecode dec, AmlMinFixed min_fixed,
                      AmlMaxFixed max_fixed, AmlCacheable cacheable,
                      AmlReadAndWrite read_and_write,
                      uint64_t addr_gran, uint64_t addr_min,
                      uint64_t addr_max, uint64_t addr_trans,
                      uint64_t len)
{
    uint8_t flags = read_and_write | (cacheable << 1);

    return aml_qword_as_desc(AML_MEMORY_RANGE, min_fixed, max_fixed,
                             dec, addr_gran, addr_min, addr_max,
                             addr_trans, len, flags);
}

/* ACPI 1.0b: 6.4.2.2 DMA Format/6.4.2.2.1 ASL Macro for DMA Descriptor */
Aml *aml_dma(AmlDmaType typ, AmlDmaBusMaster bm, AmlTransferSize sz,
             uint8_t channel)
{
    Aml *var = aml_alloc();
    uint8_t flags = sz | bm << 2 | typ << 5;

    assert(channel < 8);
    build_append_byte(var->buf, 0x2A);    /* Byte 0: DMA Descriptor */
    build_append_byte(var->buf, 1U << channel); /* Byte 1: _DMA - DmaChannel */
    build_append_byte(var->buf, flags);   /* Byte 2 */
    return var;
}

/* ACPI 1.0b: 16.2.5.3 Type 1 Opcodes Encoding: DefSleep */
Aml *aml_sleep(uint64_t msec)
{
    Aml *var = aml_alloc();
    build_append_byte(var->buf, 0x5B); /* ExtOpPrefix */
    build_append_byte(var->buf, 0x22); /* SleepOp */
    aml_append(var, aml_int(msec));
    return var;
}

static uint8_t Hex2Byte(const char *src)
{
    int hi, lo;

    hi = Hex2Digit(src[0]);
    assert(hi >= 0);
    assert(hi <= 15);

    lo = Hex2Digit(src[1]);
    assert(lo >= 0);
    assert(lo <= 15);
    return (hi << 4) | lo;
}

/*
 * ACPI 3.0: 17.5.124 ToUUID (Convert String to UUID Macro)
 * e.g. UUID: aabbccdd-eeff-gghh-iijj-kkllmmnnoopp
 * call aml_touuid("aabbccdd-eeff-gghh-iijj-kkllmmnnoopp");
 */
Aml *aml_touuid(const char *uuid)
{
    Aml *var = aml_bundle(0x11 /* BufferOp */, AML_BUFFER);

    assert(strlen(uuid) == 36);
    assert(uuid[8] == '-');
    assert(uuid[13] == '-');
    assert(uuid[18] == '-');
    assert(uuid[23] == '-');

    build_append_byte(var->buf, Hex2Byte(uuid + 6));  /* dd - at offset 00 */
    build_append_byte(var->buf, Hex2Byte(uuid + 4));  /* cc - at offset 01 */
    build_append_byte(var->buf, Hex2Byte(uuid + 2));  /* bb - at offset 02 */
    build_append_byte(var->buf, Hex2Byte(uuid + 0));  /* aa - at offset 03 */

    build_append_byte(var->buf, Hex2Byte(uuid + 11)); /* ff - at offset 04 */
    build_append_byte(var->buf, Hex2Byte(uuid + 9));  /* ee - at offset 05 */

    build_append_byte(var->buf, Hex2Byte(uuid + 16)); /* hh - at offset 06 */
    build_append_byte(var->buf, Hex2Byte(uuid + 14)); /* gg - at offset 07 */

    build_append_byte(var->buf, Hex2Byte(uuid + 19)); /* ii - at offset 08 */
    build_append_byte(var->buf, Hex2Byte(uuid + 21)); /* jj - at offset 09 */

    build_append_byte(var->buf, Hex2Byte(uuid + 24)); /* kk - at offset 10 */
    build_append_byte(var->buf, Hex2Byte(uuid + 26)); /* ll - at offset 11 */
    build_append_byte(var->buf, Hex2Byte(uuid + 28)); /* mm - at offset 12 */
    build_append_byte(var->buf, Hex2Byte(uuid + 30)); /* nn - at offset 13 */
    build_append_byte(var->buf, Hex2Byte(uuid + 32)); /* oo - at offset 14 */
    build_append_byte(var->buf, Hex2Byte(uuid + 34)); /* pp - at offset 15 */

    return var;
}

/*
 * ACPI 2.0b: 16.2.3.6.4.3  Unicode Macro (Convert Ascii String To Unicode)
 */
Aml *aml_unicode(const char *str)
{
    int i = 0;
    Aml *var = aml_bundle(0x11 /* BufferOp */, AML_BUFFER);

    do {
        build_append_byte(var->buf, str[i]);
        build_append_byte(var->buf, 0);
        i++;
    } while (i <= strlen(str));

    return var;
}

/* ACPI 1.0b: 16.2.5.4 Type 2 Opcodes Encoding: DefRefOf */
Aml *aml_refof(Aml *arg)
{
    Aml *var = aml_opcode(0x71 /* RefOfOp */);
    aml_append(var, arg);
    return var;
}

/* ACPI 1.0b: 16.2.5.4 Type 2 Opcodes Encoding: DefDerefOf */
Aml *aml_derefof(Aml *arg)
{
    Aml *var = aml_opcode(0x83 /* DerefOfOp */);
    aml_append(var, arg);
    return var;
}

/* ACPI 1.0b: 16.2.5.4 Type 2 Opcodes Encoding: DefSizeOf */
Aml *aml_sizeof(Aml *arg)
{
    Aml *var = aml_opcode(0x87 /* SizeOfOp */);
    aml_append(var, arg);
    return var;
}

/* ACPI 1.0b: 16.2.5.2 Named Objects Encoding: DefMutex */
Aml *aml_mutex(const char *name, uint8_t sync_level)
{
    Aml *var = aml_alloc();
    build_append_byte(var->buf, 0x5B); /* ExtOpPrefix */
    build_append_byte(var->buf, 0x01); /* MutexOp */
    build_append_namestring(var->buf, "%s", name);
    assert(!(sync_level & 0xF0));
    build_append_byte(var->buf, sync_level);
    return var;
}

/* ACPI 1.0b: 16.2.5.4 Type 2 Opcodes Encoding: DefAcquire */
Aml *aml_acquire(Aml *mutex, uint16_t timeout)
{
    Aml *var = aml_alloc();
    build_append_byte(var->buf, 0x5B); /* ExtOpPrefix */
    build_append_byte(var->buf, 0x23); /* AcquireOp */
    aml_append(var, mutex);
    build_append_int_noprefix(var->buf, timeout, sizeof(timeout));
    return var;
}

/* ACPI 1.0b: 16.2.5.3 Type 1 Opcodes Encoding: DefRelease */
Aml *aml_release(Aml *mutex)
{
    Aml *var = aml_alloc();
    build_append_byte(var->buf, 0x5B); /* ExtOpPrefix */
    build_append_byte(var->buf, 0x27); /* ReleaseOp */
    aml_append(var, mutex);
    return var;
}

/* ACPI 1.0b: 16.2.5.1 Name Space Modifier Objects Encoding: DefAlias */
Aml *aml_alias(const char *source_object, const char *alias_object)
{
    Aml *var = aml_opcode(0x06 /* AliasOp */);
    aml_append(var, aml_name("%s", source_object));
    aml_append(var, aml_name("%s", alias_object));
    return var;
}

/* ACPI 1.0b: 16.2.5.4 Type 2 Opcodes Encoding: DefConcat */
Aml *aml_concatenate(Aml *source1, Aml *source2, Aml *target)
{
    return build_opcode_2arg_dst(0x73 /* ConcatOp */, source1, source2,
                                 target);
}

/* ACPI 1.0b: 16.2.5.4 Type 2 Opcodes Encoding: DefObjectType */
Aml *aml_object_type(Aml *object)
{
    Aml *var = aml_opcode(0x8E /* ObjectTypeOp */);
    aml_append(var, object);
    return var;
}

void
build_header(BIOSLinker *linker, GArray *table_data,
             AcpiTableHeader *h, const char *sig, int len, uint8_t rev,
             const char *oem_id, const char *oem_table_id)
{
    unsigned tbl_offset = (char *)h - table_data->data;
    unsigned checksum_offset = (char *)&h->checksum - table_data->data;
    memcpy(&h->signature, sig, 4);
    h->length = cpu_to_le32(len);
    h->revision = rev;

    if (oem_id) {
        strncpy((char *)h->oem_id, oem_id, sizeof h->oem_id);
    } else {
        memcpy(h->oem_id, ACPI_BUILD_APPNAME6, 6);
    }

    if (oem_table_id) {
        strncpy((char *)h->oem_table_id, oem_table_id, sizeof(h->oem_table_id));
    } else {
        memcpy(h->oem_table_id, ACPI_BUILD_APPNAME4, 4);
        memcpy(h->oem_table_id + 4, sig, 4);
    }

    h->oem_revision = cpu_to_le32(1);
    memcpy(h->asl_compiler_id, ACPI_BUILD_APPNAME4, 4);
    h->asl_compiler_revision = cpu_to_le32(1);
    /* Checksum to be filled in by Guest linker */
    bios_linker_loader_add_checksum(linker, ACPI_BUILD_TABLE_FILE,
        tbl_offset, len, checksum_offset);
}

void *acpi_data_push(GArray *table_data, unsigned size)
{
    unsigned off = table_data->len;
    g_array_set_size(table_data, off + size);
    return table_data->data + off;
}

unsigned acpi_data_len(GArray *table)
{
    assert(g_array_get_element_size(table) == 1);
    return table->len;
}

void acpi_align_size(GArray *blob, unsigned align)
{
    /* Align size to multiple of given size. This reduces the chance
     * we need to change size in the future (breaking cross version migration).
     */
    g_array_set_size(blob, ROUND_UP(acpi_data_len(blob), align));
}


void acpi_add_table(GArray *table_offsets, GArray *table_data)
{
    uint32_t offset = table_data->len;
    g_array_append_val(table_offsets, offset);
}

void acpi_build_tables_init(AcpiBuildTables *tables)
{
    tables->rsdp = g_array_new(false, true /* clear */, 1);
    tables->table_data = g_array_new(false, true /* clear */, 1);
    tables->tcpalog = g_array_new(false, true /* clear */, 1);
    tables->vmgenid = g_array_new(false, true /* clear */, 1);
    tables->linker = bios_linker_loader_init();
}

void acpi_build_tables_cleanup(AcpiBuildTables *tables, bool mfre)
{
    bios_linker_loader_cleanup(tables->linker);
    g_array_free(tables->rsdp, true);
    g_array_free(tables->table_data, true);
    g_array_free(tables->tcpalog, mfre);
    g_array_free(tables->vmgenid, mfre);
}

/*
 * Because of the PXB hosts we cannot simply query TYPE_PCI_HOST_BRIDGE.
 * On i386 arch we only have two pci hosts, so we can look only for them.
 */
Object *acpi_get_pci_host(void)
{
    PCIHostState *host;
    int i = 0;

    while (pci_hosts[i]) {
        host = OBJECT_CHECK(PCIHostState,
                            object_resolve_path(pci_hosts[i], NULL),
                            TYPE_PCI_HOST_BRIDGE);
        if (host) {
            return OBJECT(host);
        }

        i++;
    }

    return NULL;
}

void acpi_get_pci_holes(Range *hole, Range *hole64)
{
    Object *pci_host;

    pci_host = acpi_get_pci_host();
    g_assert(pci_host);

    range_set_bounds1(hole,
                      object_property_get_uint(pci_host,
                                               PCI_HOST_PROP_PCI_HOLE_START,
                                               NULL),
                      object_property_get_uint(pci_host,
                                               PCI_HOST_PROP_PCI_HOLE_END,
                                               NULL));
    range_set_bounds1(hole64,
                      object_property_get_uint(pci_host,
                                               PCI_HOST_PROP_PCI_HOLE64_START,
                                               NULL),
                      object_property_get_uint(pci_host,
                                               PCI_HOST_PROP_PCI_HOLE64_END,
                                               NULL));
}


bool acpi_get_mcfg(AcpiMcfgInfo *mcfg)
{
    Object *pci_host;
    QObject *o;

    pci_host = acpi_get_pci_host();
    g_assert(pci_host);

    o = object_property_get_qobject(pci_host, PCIE_HOST_MCFG_BASE, NULL);
    if (!o) {
        return false;
    }
    mcfg->mcfg_base = qnum_get_uint(qobject_to(QNum, o));
    qobject_unref(o);

    o = object_property_get_qobject(pci_host, PCIE_HOST_MCFG_SIZE, NULL);
    assert(o);
    mcfg->mcfg_size = qnum_get_uint(qobject_to(QNum, o));
    qobject_unref(o);
    return true;
}

static void crs_range_insert(GPtrArray *ranges, uint64_t base, uint64_t limit)
{
    CrsRangeEntry *entry;

    entry = g_malloc(sizeof(*entry));
    entry->base = base;
    entry->limit = limit;

    g_ptr_array_add(ranges, entry);
}

static void crs_range_free(gpointer data)
{
    CrsRangeEntry *entry = (CrsRangeEntry *)data;
    g_free(entry);
}

void crs_range_set_init(CrsRangeSet *range_set)
{
    range_set->io_ranges = g_ptr_array_new_with_free_func(crs_range_free);
    range_set->mem_ranges = g_ptr_array_new_with_free_func(crs_range_free);
    range_set->mem_64bit_ranges =
            g_ptr_array_new_with_free_func(crs_range_free);
}

void crs_range_set_free(CrsRangeSet *range_set)
{
    g_ptr_array_free(range_set->io_ranges, true);
    g_ptr_array_free(range_set->mem_ranges, true);
    g_ptr_array_free(range_set->mem_64bit_ranges, true);
}

static gint crs_range_compare(gconstpointer a, gconstpointer b)
{
     CrsRangeEntry *entry_a = *(CrsRangeEntry **)a;
     CrsRangeEntry *entry_b = *(CrsRangeEntry **)b;

     return (int64_t)entry_a->base - (int64_t)entry_b->base;
}

/*
 * crs_replace_with_free_ranges - given the 'used' ranges within [start - end]
 * interval, computes the 'free' ranges from the same interval.
 * Example: If the input array is { [a1 - a2],[b1 - b2] }, the function
 * will return { [base - a1], [a2 - b1], [b2 - limit] }.
 */
void crs_replace_with_free_ranges(GPtrArray *ranges,
                                         uint64_t start, uint64_t end)
{
    GPtrArray *free_ranges = g_ptr_array_new();
    uint64_t free_base = start;
    int i;

    g_ptr_array_sort(ranges, crs_range_compare);
    for (i = 0; i < ranges->len; i++) {
        CrsRangeEntry *used = g_ptr_array_index(ranges, i);

        if (free_base < used->base) {
            crs_range_insert(free_ranges, free_base, used->base - 1);
        }

        free_base = used->limit + 1;
    }

    if (free_base < end) {
        crs_range_insert(free_ranges, free_base, end);
    }

    g_ptr_array_set_size(ranges, 0);
    for (i = 0; i < free_ranges->len; i++) {
        g_ptr_array_add(ranges, g_ptr_array_index(free_ranges, i));
    }

    g_ptr_array_free(free_ranges, true);
}

/*
 * crs_range_merge - merges adjacent ranges in the given array.
 * Array elements are deleted and replaced with the merged ranges.
 */
static void crs_range_merge(GPtrArray *range)
{
    GPtrArray *tmp =  g_ptr_array_new_with_free_func(crs_range_free);
    CrsRangeEntry *entry;
    uint64_t range_base, range_limit;
    int i;

    if (!range->len) {
        return;
    }

    g_ptr_array_sort(range, crs_range_compare);

    entry = g_ptr_array_index(range, 0);
    range_base = entry->base;
    range_limit = entry->limit;
    for (i = 1; i < range->len; i++) {
        entry = g_ptr_array_index(range, i);
        if (entry->base - 1 == range_limit) {
            range_limit = entry->limit;
        } else {
            crs_range_insert(tmp, range_base, range_limit);
            range_base = entry->base;
            range_limit = entry->limit;
        }
    }
    crs_range_insert(tmp, range_base, range_limit);

    g_ptr_array_set_size(range, 0);
    for (i = 0; i < tmp->len; i++) {
        entry = g_ptr_array_index(tmp, i);
        crs_range_insert(range, entry->base, entry->limit);
    }
    g_ptr_array_free(tmp, true);
}

Aml *build_crs(PCIHostState *host, CrsRangeSet *range_set)
{
    Aml *crs = aml_resource_template();
    CrsRangeSet temp_range_set;
    CrsRangeEntry *entry;
    uint8_t max_bus = pci_bus_num(host->bus);
    uint8_t type;
    int devfn;
    int i;

    crs_range_set_init(&temp_range_set);
    for (devfn = 0; devfn < ARRAY_SIZE(host->bus->devices); devfn++) {
        uint64_t range_base, range_limit;
        PCIDevice *dev = host->bus->devices[devfn];

        if (!dev) {
            continue;
        }

        for (i = 0; i < PCI_NUM_REGIONS; i++) {
            PCIIORegion *r = &dev->io_regions[i];

            range_base = r->addr;
            range_limit = r->addr + r->size - 1;

            /*
             * Work-around for old bioses
             * that do not support multiple root buses
             */
            if (!range_base || range_base > range_limit) {
                continue;
            }

            if (r->type & PCI_BASE_ADDRESS_SPACE_IO) {
                crs_range_insert(temp_range_set.io_ranges,
                                 range_base, range_limit);
            } else { /* "memory" */
                crs_range_insert(temp_range_set.mem_ranges,
                                 range_base, range_limit);
            }
        }

        type = dev->config[PCI_HEADER_TYPE] & ~PCI_HEADER_TYPE_MULTI_FUNCTION;
        if (type == PCI_HEADER_TYPE_BRIDGE) {
            uint8_t subordinate = dev->config[PCI_SUBORDINATE_BUS];
            if (subordinate > max_bus) {
                max_bus = subordinate;
            }

            range_base = pci_bridge_get_base(dev, PCI_BASE_ADDRESS_SPACE_IO);
            range_limit = pci_bridge_get_limit(dev, PCI_BASE_ADDRESS_SPACE_IO);

            /*
             * Work-around for old bioses
             * that do not support multiple root buses
             */
            if (range_base && range_base <= range_limit) {
                crs_range_insert(temp_range_set.io_ranges,
                                 range_base, range_limit);
            }

            range_base =
                pci_bridge_get_base(dev, PCI_BASE_ADDRESS_SPACE_MEMORY);
            range_limit =
                pci_bridge_get_limit(dev, PCI_BASE_ADDRESS_SPACE_MEMORY);

            /*
             * Work-around for old bioses
             * that do not support multiple root buses
             */
            if (range_base && range_base <= range_limit) {
                uint64_t length = range_limit - range_base + 1;
                if (range_limit <= UINT32_MAX && length <= UINT32_MAX)  {
                    crs_range_insert(temp_range_set.mem_ranges,
                                     range_base, range_limit);
                } else {
                    crs_range_insert(temp_range_set.mem_64bit_ranges,
                                     range_base, range_limit);
                }
            }

            range_base =
                pci_bridge_get_base(dev, PCI_BASE_ADDRESS_MEM_PREFETCH);
            range_limit =
                pci_bridge_get_limit(dev, PCI_BASE_ADDRESS_MEM_PREFETCH);

            /*
             * Work-around for old bioses
             * that do not support multiple root buses
             */
            if (range_base && range_base <= range_limit) {
                uint64_t length = range_limit - range_base + 1;
                if (range_limit <= UINT32_MAX && length <= UINT32_MAX) {
                    crs_range_insert(temp_range_set.mem_ranges,
                                     range_base, range_limit);
                } else {
                    crs_range_insert(temp_range_set.mem_64bit_ranges,
                                     range_base, range_limit);
                }
            }
        }
    }

    crs_range_merge(temp_range_set.io_ranges);
    for (i = 0; i < temp_range_set.io_ranges->len; i++) {
        entry = g_ptr_array_index(temp_range_set.io_ranges, i);
        aml_append(crs,
                   aml_word_io(AML_MIN_FIXED, AML_MAX_FIXED,
                               AML_POS_DECODE, AML_ENTIRE_RANGE,
                               0, entry->base, entry->limit, 0,
                               entry->limit - entry->base + 1));
        crs_range_insert(range_set->io_ranges, entry->base, entry->limit);
    }

    crs_range_merge(temp_range_set.mem_ranges);
    for (i = 0; i < temp_range_set.mem_ranges->len; i++) {
        entry = g_ptr_array_index(temp_range_set.mem_ranges, i);
        aml_append(crs,
                   aml_dword_memory(AML_POS_DECODE, AML_MIN_FIXED,
                                    AML_MAX_FIXED, AML_NON_CACHEABLE,
                                    AML_READ_WRITE,
                                    0, entry->base, entry->limit, 0,
                                    entry->limit - entry->base + 1));
        crs_range_insert(range_set->mem_ranges, entry->base, entry->limit);
    }

    crs_range_merge(temp_range_set.mem_64bit_ranges);
    for (i = 0; i < temp_range_set.mem_64bit_ranges->len; i++) {
        entry = g_ptr_array_index(temp_range_set.mem_64bit_ranges, i);
        aml_append(crs,
                   aml_qword_memory(AML_POS_DECODE, AML_MIN_FIXED,
                                    AML_MAX_FIXED, AML_NON_CACHEABLE,
                                    AML_READ_WRITE,
                                    0, entry->base, entry->limit, 0,
                                    entry->limit - entry->base + 1));
        crs_range_insert(range_set->mem_64bit_ranges,
                         entry->base, entry->limit);
    }

    crs_range_set_free(&temp_range_set);

    aml_append(crs,
        aml_word_bus_number(AML_MIN_FIXED, AML_MAX_FIXED, AML_POS_DECODE,
                            0,
                            pci_bus_num(host->bus),
                            max_bus,
                            0,
                            max_bus - pci_bus_num(host->bus) + 1));

    return crs;
}

Aml *build_osc_method(uint32_t value)
{
    Aml *ifctx, *ifctx1, *elsectx, *method, *UUID;

    method = aml_method("_OSC", 4, AML_NOTSERIALIZED);
    aml_append(method,
        aml_create_dword_field(aml_arg(3), aml_int(0), "CDW1"));

    /* PCI Firmware Specification 3.0
     * 4.5.1. _OSC Interface for PCI Host Bridge Devices
     * The _OSC interface for a PCI/PCI-X/PCI Express hierarchy is
     * identified by the Universal Unique IDentifier (UUID)
     * 33DB4D5B-1FF7-401C-9657-7441C03DD766
     */
    UUID = aml_touuid("33DB4D5B-1FF7-401C-9657-7441C03DD766");
    ifctx = aml_if(aml_equal(aml_arg(0), UUID));
    aml_append(ifctx,
        aml_create_dword_field(aml_arg(3), aml_int(4), "CDW2"));
    aml_append(ifctx,
        aml_create_dword_field(aml_arg(3), aml_int(8), "CDW3"));
    aml_append(ifctx, aml_store(aml_name("CDW2"), aml_name("SUPP")));
    aml_append(ifctx, aml_store(aml_name("CDW3"), aml_name("CTRL")));
    aml_append(ifctx, aml_store(aml_and(aml_name("CTRL"), aml_int(value), NULL),
                                aml_name("CTRL")));

    ifctx1 = aml_if(aml_lnot(aml_equal(aml_arg(1), aml_int(0x1))));
    aml_append(ifctx1, aml_store(aml_or(aml_name("CDW1"), aml_int(0x08), NULL),
                                 aml_name("CDW1")));
    aml_append(ifctx, ifctx1);

    ifctx1 = aml_if(aml_lnot(aml_equal(aml_name("CDW3"), aml_name("CTRL"))));
    aml_append(ifctx1, aml_store(aml_or(aml_name("CDW1"), aml_int(0x10), NULL),
                                 aml_name("CDW1")));
    aml_append(ifctx, ifctx1);

    aml_append(ifctx, aml_store(aml_name("CTRL"), aml_name("CDW3")));
    aml_append(ifctx, aml_return(aml_arg(3)));
    aml_append(method, ifctx);

    elsectx = aml_else();
    aml_append(elsectx, aml_store(aml_or(aml_name("CDW1"), aml_int(4), NULL),
                                  aml_name("CDW1")));
    aml_append(elsectx, aml_return(aml_arg(3)));
    aml_append(method, elsectx);

    return method;
}

void
acpi_build_mcfg(GArray *table_data, BIOSLinker *linker, AcpiMcfgInfo *info)
{
    AcpiTableMcfg *mcfg;
    const char *sig;
    int len = sizeof(*mcfg) + 1 * sizeof(mcfg->allocation[0]);

    mcfg = acpi_data_push(table_data, len);
    mcfg->allocation[0].address = cpu_to_le64(info->mcfg_base);
    /* Only a single allocation so no need to play with segments */
    mcfg->allocation[0].pci_segment = cpu_to_le16(0);
    mcfg->allocation[0].start_bus_number = 0;
    mcfg->allocation[0].end_bus_number = PCIE_MMCFG_BUS(info->mcfg_size - 1);

    /* MCFG is used for ECAM which can be enabled or disabled by guest.
     * To avoid table size changes (which create migration issues),
     * always create the table even if there are no allocations,
     * but set the signature to a reserved value in this case.
     * ACPI spec requires OSPMs to ignore such tables.
     */
    if (info->mcfg_base == PCIE_BASE_ADDR_UNMAPPED) {
        /* Reserved signature: ignored by OSPM */
        sig = "QEMU";
    } else {
        sig = "MCFG";
    }
    build_header(linker, table_data, (void *)mcfg, sig, len, 1, NULL, NULL);
}

Aml *build_gsi_link_dev(const char *name, uint8_t uid, uint8_t gsi)
{
    Aml *dev;
    Aml *crs;
    Aml *method;
    uint32_t irqs;

    dev = aml_device("%s", name);
    aml_append(dev, aml_name_decl("_HID", aml_eisaid("PNP0C0F")));
    aml_append(dev, aml_name_decl("_UID", aml_int(uid)));

    crs = aml_resource_template();
    irqs = gsi;
    aml_append(crs, aml_interrupt(AML_CONSUMER, AML_LEVEL, AML_ACTIVE_HIGH,
                                  AML_SHARED, &irqs, 1));
    aml_append(dev, aml_name_decl("_PRS", crs));

    aml_append(dev, aml_name_decl("_CRS", crs));

    /*
     * _DIS can be no-op because the interrupt cannot be disabled.
     */
    method = aml_method("_DIS", 0, AML_NOTSERIALIZED);
    aml_append(dev, method);

    method = aml_method("_SRS", 1, AML_NOTSERIALIZED);
    aml_append(dev, method);

    return dev;
}

/**
 * build_prt_entry:
 * @link_name: link name for PCI route entry
 *
 * build AML package containing a PCI route entry for @link_name
 */
static Aml *build_prt_entry(const char *link_name)
{
    Aml *a_zero = aml_int(0);
    Aml *pkg = aml_package(4);
    aml_append(pkg, a_zero);
    aml_append(pkg, a_zero);
    aml_append(pkg, aml_name("%s", link_name));
    aml_append(pkg, a_zero);
    return pkg;
}

GArray *acpi_build_madt(GArray *table_data, BIOSLinker *linker, MachineState *ms, AcpiConfiguration *conf)
{
    MachineClass *mc = MACHINE_GET_CLASS(ms);
    const CPUArchIdList *apic_ids = mc->possible_cpu_arch_ids(ms);
    int madt_start = table_data->len;
    AcpiDeviceIfClass *adevc = ACPI_DEVICE_IF_GET_CLASS(conf->acpi_dev);
    AcpiDeviceIf *adev = ACPI_DEVICE_IF(conf->acpi_dev);
    bool x2apic_mode = false;

    AcpiMultipleApicTable *madt;
    AcpiMadtIoApic *io_apic;
    AcpiMadtIntsrcovr *intsrcovr;
    int i;

    madt = acpi_data_push(table_data, sizeof *madt);
    madt->local_apic_address = cpu_to_le32(APIC_DEFAULT_ADDRESS);
    madt->flags = cpu_to_le32(1);

    for (i = 0; i < apic_ids->len; i++) {
        adevc->madt_cpu(adev, i, apic_ids, table_data);
        if (apic_ids->cpus[i].arch_id > 254) {
            x2apic_mode = true;
        }
    }

    io_apic = acpi_data_push(table_data, sizeof *io_apic);
    io_apic->type = ACPI_APIC_IO;
    io_apic->length = sizeof(*io_apic);
    io_apic->io_apic_id = ACPI_BUILD_IOAPIC_ID;
    io_apic->address = cpu_to_le32(IO_APIC_DEFAULT_ADDRESS);
    io_apic->interrupt = cpu_to_le32(0);

    if (conf->apic_xrupt_override) {
        intsrcovr = acpi_data_push(table_data, sizeof *intsrcovr);
        intsrcovr->type   = ACPI_APIC_XRUPT_OVERRIDE;
        intsrcovr->length = sizeof(*intsrcovr);
        intsrcovr->source = 0;
        intsrcovr->gsi    = cpu_to_le32(2);
        intsrcovr->flags  = cpu_to_le16(0); /* conforms to bus specifications */
    }
    for (i = 1; i < 16; i++) {
#define ACPI_BUILD_PCI_IRQS ((1<<5) | (1<<9) | (1<<10) | (1<<11))
        if (!(ACPI_BUILD_PCI_IRQS & (1 << i))) {
            /* No need for a INT source override structure. */
            continue;
        }
        intsrcovr = acpi_data_push(table_data, sizeof *intsrcovr);
        intsrcovr->type   = ACPI_APIC_XRUPT_OVERRIDE;
        intsrcovr->length = sizeof(*intsrcovr);
        intsrcovr->source = i;
        intsrcovr->gsi    = cpu_to_le32(i);
        intsrcovr->flags  = cpu_to_le16(0xd); /* active high, level triggered */
    }

    if (x2apic_mode) {
        AcpiMadtLocalX2ApicNmi *local_nmi;

        local_nmi = acpi_data_push(table_data, sizeof *local_nmi);
        local_nmi->type   = ACPI_APIC_LOCAL_X2APIC_NMI;
        local_nmi->length = sizeof(*local_nmi);
        local_nmi->uid    = 0xFFFFFFFF; /* all processors */
        local_nmi->flags  = cpu_to_le16(0);
        local_nmi->lint   = 1; /* ACPI_LINT1 */
    } else {
        AcpiMadtLocalNmi *local_nmi;

        local_nmi = acpi_data_push(table_data, sizeof *local_nmi);
        local_nmi->type         = ACPI_APIC_LOCAL_NMI;
        local_nmi->length       = sizeof(*local_nmi);
        local_nmi->processor_id = 0xff; /* all processors */
        local_nmi->flags        = cpu_to_le16(0);
        local_nmi->lint         = 1; /* ACPI_LINT1 */
    }

    build_header(linker, table_data,
                 (void *)(table_data->data + madt_start), "APIC",
                 table_data->len - madt_start, 1, NULL, NULL);

    return table_data;
}

/*
 * initialize_route - Initialize the interrupt routing rule
 * through a specific LINK:
 *  if (lnk_idx == idx)
 *      route using link 'link_name'
 */
static Aml *initialize_route(Aml *route, const char *link_name,
                             Aml *lnk_idx, int idx)
{
    Aml *if_ctx = aml_if(aml_equal(lnk_idx, aml_int(idx)));
    Aml *pkg = build_prt_entry(link_name);

    aml_append(if_ctx, aml_store(pkg, route));

    return if_ctx;
}

/*
 * build_prt - Define interrupt rounting rules
 *
 * Returns an array of 128 routes, one for each device,
 * based on device location.
 * The main goal is to equaly distribute the interrupts
 * over the 4 existing ACPI links (works only for i440fx).
 * The hash function is  (slot + pin) & 3 -> "LNK[D|A|B|C]".
 *
 */
Aml *build_prt(bool is_pci0_prt)
{
    Aml *method, *while_ctx, *pin, *res;

    method = aml_method("_PRT", 0, AML_NOTSERIALIZED);
    res = aml_local(0);
    pin = aml_local(1);
    aml_append(method, aml_store(aml_package(128), res));
    aml_append(method, aml_store(aml_int(0), pin));

    /* while (pin < 128) */
    while_ctx = aml_while(aml_lless(pin, aml_int(128)));
    {
        Aml *slot = aml_local(2);
        Aml *lnk_idx = aml_local(3);
        Aml *route = aml_local(4);

        /* slot = pin >> 2 */
        aml_append(while_ctx,
                   aml_store(aml_shiftright(pin, aml_int(2), NULL), slot));
        /* lnk_idx = (slot + pin) & 3 */
        aml_append(while_ctx,
            aml_store(aml_and(aml_add(pin, slot, NULL), aml_int(3), NULL),
                      lnk_idx));

        /* route[2] = "LNK[D|A|B|C]", selection based on pin % 3  */
        aml_append(while_ctx, initialize_route(route, "LNKD", lnk_idx, 0));
        if (is_pci0_prt) {
            Aml *if_device_1, *if_pin_4, *else_pin_4;

            /* device 1 is the power-management device, needs SCI */
            if_device_1 = aml_if(aml_equal(lnk_idx, aml_int(1)));
            {
                if_pin_4 = aml_if(aml_equal(pin, aml_int(4)));
                {
                    aml_append(if_pin_4,
                        aml_store(build_prt_entry("LNKS"), route));
                }
                aml_append(if_device_1, if_pin_4);
                else_pin_4 = aml_else();
                {
                    aml_append(else_pin_4,
                        aml_store(build_prt_entry("LNKA"), route));
                }
                aml_append(if_device_1, else_pin_4);
            }
            aml_append(while_ctx, if_device_1);
        } else {
            aml_append(while_ctx, initialize_route(route, "LNKA", lnk_idx, 1));
        }
        aml_append(while_ctx, initialize_route(route, "LNKB", lnk_idx, 2));
        aml_append(while_ctx, initialize_route(route, "LNKC", lnk_idx, 3));

        /* route[0] = 0x[slot]FFFF */
        aml_append(while_ctx,
            aml_store(aml_or(aml_shiftleft(slot, aml_int(16)), aml_int(0xFFFF),
                             NULL),
                      aml_index(route, aml_int(0))));
        /* route[1] = pin & 3 */
        aml_append(while_ctx,
            aml_store(aml_and(pin, aml_int(3), NULL),
                      aml_index(route, aml_int(1))));
        /* res[pin] = route */
        aml_append(while_ctx, aml_store(route, aml_index(res, pin)));
        /* pin++ */
        aml_append(while_ctx, aml_increment(pin));
    }
    aml_append(method, while_ctx);
    /* return res*/
    aml_append(method, aml_return(res));

    return method;
}

Aml *build_pci_host_bridge(Aml *table, AcpiPciBus *pci_host)
{
    CrsRangeEntry *entry;
    Aml *scope, *dev, *crs;
    CrsRangeSet crs_range_set;
    Range *pci_hole = NULL;
    Range *pci_hole64 = NULL;
    PCIBus *bus = NULL;
    int root_bus_limit = 0xFF;
    int i;

    bus = pci_host->pci_bus;
    assert(bus);
    pci_hole = pci_host->pci_hole;
    pci_hole64 = pci_host->pci_hole64;

    crs_range_set_init(&crs_range_set);
    QLIST_FOREACH(bus, &bus->child, sibling) {
        uint8_t bus_num = pci_bus_num(bus);
        uint8_t numa_node = pci_bus_numa_node(bus);

        /* look only for expander root buses */
        if (!pci_bus_is_root(bus)) {
            continue;
        }

        if (bus_num < root_bus_limit) {
            root_bus_limit = bus_num - 1;
        }

        scope = aml_scope("\\_SB");
        dev = aml_device("PC%.02X", bus_num);
        aml_append(dev, aml_name_decl("_UID", aml_int(bus_num)));
        aml_append(dev, aml_name_decl("_HID", aml_eisaid("PNP0A03")));
        aml_append(dev, aml_name_decl("_BBN", aml_int(bus_num)));
        if (pci_bus_is_express(bus)) {
            aml_append(dev, aml_name_decl("SUPP", aml_int(0)));
            aml_append(dev, aml_name_decl("CTRL", aml_int(0)));
            aml_append(dev, build_osc_method(0x1F));
        }
        if (numa_node != NUMA_NODE_UNASSIGNED) {
            aml_append(dev, aml_name_decl("_PXM", aml_int(numa_node)));
        }

        aml_append(dev, build_prt(false));
        crs = build_crs(PCI_HOST_BRIDGE(BUS(bus)->parent), &crs_range_set);
        aml_append(dev, aml_name_decl("_CRS", crs));
        aml_append(scope, dev);
        aml_append(table, scope);
    }
    scope = aml_scope("\\_SB.PCI0");
    /* build PCI0._CRS */
    crs = aml_resource_template();
    /* set the pcie bus num */
    aml_append(crs,
        aml_word_bus_number(AML_MIN_FIXED, AML_MAX_FIXED, AML_POS_DECODE,
                            0x0000, 0x0, root_bus_limit,
                            0x0000, root_bus_limit + 1));
    aml_append(crs, aml_io(AML_DECODE16, PCI_HOST_BRIDGE_CONFIG_ADDR,
                           PCI_HOST_BRIDGE_CONFIG_ADDR, 0x01, 0x08));
    /* set the io region 0 in pci host bridge */
    aml_append(crs,
        aml_word_io(AML_MIN_FIXED, AML_MAX_FIXED,
                    AML_POS_DECODE, AML_ENTIRE_RANGE,
                    0x0000, PCI_HOST_BRIDGE_IO_0_MIN_ADDR,
                    PCI_HOST_BRIDGE_IO_0_MAX_ADDR, 0x0000, IO_0_LEN));

    /* set the io region 1 in pci host bridge */
    crs_replace_with_free_ranges(crs_range_set.io_ranges,
                                 PCI_HOST_BRIDGE_IO_1_MIN_ADDR,
                                 PCI_HOST_BRIDGE_IO_1_MAX_ADDR);
    for (i = 0; i < crs_range_set.io_ranges->len; i++) {
        entry = g_ptr_array_index(crs_range_set.io_ranges, i);
        aml_append(crs,
            aml_word_io(AML_MIN_FIXED, AML_MAX_FIXED,
                        AML_POS_DECODE, AML_ENTIRE_RANGE,
                        0x0000, entry->base, entry->limit,
                        0x0000, entry->limit - entry->base + 1));
    }

    /* set the vga mem region(0) in pci host bridge */
    aml_append(crs,
        aml_dword_memory(AML_POS_DECODE, AML_MIN_FIXED, AML_MAX_FIXED,
                         AML_CACHEABLE, AML_READ_WRITE,
                         0, PCI_VGA_MEM_BASE_ADDR, PCI_VGA_MEM_MAX_ADDR,
                         0, VGA_MEM_LEN));

    /* set the mem region 1 in pci host bridge */
    crs_replace_with_free_ranges(crs_range_set.mem_ranges,
                                 range_lob(pci_hole),
                                 range_upb(pci_hole));
    for (i = 0; i < crs_range_set.mem_ranges->len; i++) {
        entry = g_ptr_array_index(crs_range_set.mem_ranges, i);
        aml_append(crs,
            aml_dword_memory(AML_POS_DECODE, AML_MIN_FIXED, AML_MAX_FIXED,
                             AML_NON_CACHEABLE, AML_READ_WRITE,
                             0, entry->base, entry->limit,
                             0, entry->limit - entry->base + 1));
    }

    /* set the mem region 2 in pci host bridge */
    if (!range_is_empty(pci_hole64)) {
        crs_replace_with_free_ranges(crs_range_set.mem_64bit_ranges,
                                     range_lob(pci_hole64),
                                     range_upb(pci_hole64));
        for (i = 0; i < crs_range_set.mem_64bit_ranges->len; i++) {
            entry = g_ptr_array_index(crs_range_set.mem_64bit_ranges, i);
            aml_append(crs,
                       aml_qword_memory(AML_POS_DECODE, AML_MIN_FIXED,
                                        AML_MAX_FIXED,
                                        AML_CACHEABLE, AML_READ_WRITE,
                                        0, entry->base, entry->limit,
                                        0, entry->limit - entry->base + 1));
        }
    }

    if (TPM_IS_TIS(tpm_find())) {
        aml_append(crs, aml_memory32_fixed(TPM_TIS_ADDR_BASE,
                   TPM_TIS_ADDR_SIZE, AML_READ_WRITE));
    }

    aml_append(scope, aml_name_decl("_CRS", crs));
    crs_range_set_free(&crs_range_set);
    return scope;
}

void build_acpi_pci_hotplug(Aml *scope)
{
    Aml *field;
    Aml *method;

    aml_append(scope,
        aml_operation_region("PCST", AML_SYSTEM_IO, aml_int(0xae00), 0x08));
    field = aml_field("PCST", AML_DWORD_ACC, AML_NOLOCK, AML_WRITE_AS_ZEROS);
    aml_append(field, aml_named_field("PCIU", 32));
    aml_append(field, aml_named_field("PCID", 32));
    aml_append(scope, field);

    aml_append(scope,
        aml_operation_region("SEJ", AML_SYSTEM_IO, aml_int(0xae08), 0x04));
    field = aml_field("SEJ", AML_DWORD_ACC, AML_NOLOCK, AML_WRITE_AS_ZEROS);
    aml_append(field, aml_named_field("B0EJ", 32));
    aml_append(scope, field);

    aml_append(scope,
        aml_operation_region("BNMR", AML_SYSTEM_IO, aml_int(0xae10), 0x04));
    field = aml_field("BNMR", AML_DWORD_ACC, AML_NOLOCK, AML_WRITE_AS_ZEROS);
    aml_append(field, aml_named_field("BNUM", 32));
    aml_append(scope, field);

    aml_append(scope, aml_mutex("BLCK", 0));

    method = aml_method("PCEJ", 2, AML_NOTSERIALIZED);
    aml_append(method, aml_acquire(aml_name("BLCK"), 0xFFFF));
    aml_append(method, aml_store(aml_arg(0), aml_name("BNUM")));
    aml_append(method,
        aml_store(aml_shiftleft(aml_int(1), aml_arg(1)), aml_name("B0EJ")));
    aml_append(method, aml_release(aml_name("BLCK")));
    aml_append(method, aml_return(aml_int(0)));
    aml_append(scope, method);
}

static void build_append_pcihp_notify_entry(Aml *method, int slot)
{
    Aml *if_ctx;
    int32_t devfn = PCI_DEVFN(slot, 0);

    if_ctx = aml_if(aml_and(aml_arg(0), aml_int(0x1U << slot), NULL));
    aml_append(if_ctx, aml_notify(aml_name("S%.02X", devfn), aml_arg(1)));
    aml_append(method, if_ctx);
}

void build_append_pci_bus_devices(Aml *parent_scope, PCIBus *bus,
                                         bool pcihp_bridge_en)
{
    Aml *dev, *notify_method = NULL, *method;
    QObject *bsel;
    PCIBus *sec;
    int i;

    bsel = object_property_get_qobject(OBJECT(bus), ACPI_PCIHP_PROP_BSEL, NULL);
    if (bsel) {
        uint64_t bsel_val = qnum_get_uint(qobject_to(QNum, bsel));

        aml_append(parent_scope, aml_name_decl("BSEL", aml_int(bsel_val)));
        notify_method = aml_method("DVNT", 2, AML_NOTSERIALIZED);
    }

    for (i = 0; i < ARRAY_SIZE(bus->devices); i += PCI_FUNC_MAX) {
        DeviceClass *dc;
        PCIDeviceClass *pc;
        PCIDevice *pdev = bus->devices[i];
        int slot = PCI_SLOT(i);
        bool hotplug_enabled_dev;
        bool bridge_in_acpi;

        if (!pdev) {
            if (bsel) { /* add hotplug slots for non present devices */
                dev = aml_device("S%.02X", PCI_DEVFN(slot, 0));
                aml_append(dev, aml_name_decl("_SUN", aml_int(slot)));
                aml_append(dev, aml_name_decl("_ADR", aml_int(slot << 16)));
                method = aml_method("_EJ0", 1, AML_NOTSERIALIZED);
                aml_append(method,
                    aml_call2("PCEJ", aml_name("BSEL"), aml_name("_SUN"))
                );
                aml_append(dev, method);
                aml_append(parent_scope, dev);

                build_append_pcihp_notify_entry(notify_method, slot);
            }
            continue;
        }

        pc = PCI_DEVICE_GET_CLASS(pdev);
        dc = DEVICE_GET_CLASS(pdev);

        /* When hotplug for bridges is enabled, bridges are
         * described in ACPI separately (see build_pci_bus_end).
         * In this case they aren't themselves hot-pluggable.
         * Hotplugged bridges *are* hot-pluggable.
         */
        bridge_in_acpi = pc->is_bridge && pcihp_bridge_en &&
            !DEVICE(pdev)->hotplugged;

        hotplug_enabled_dev = bsel && dc->hotpluggable && !bridge_in_acpi;

        if (pc->class_id == PCI_CLASS_BRIDGE_ISA) {
            continue;
        }

        /* start to compose PCI slot descriptor */
        dev = aml_device("S%.02X", PCI_DEVFN(slot, 0));
        aml_append(dev, aml_name_decl("_ADR", aml_int(slot << 16)));

        if (pc->class_id == PCI_CLASS_DISPLAY_VGA) {
            /* add VGA specific AML methods */
            int s3d;

            if (object_dynamic_cast(OBJECT(pdev), "qxl-vga")) {
                s3d = 3;
            } else {
                s3d = 0;
            }

            method = aml_method("_S1D", 0, AML_NOTSERIALIZED);
            aml_append(method, aml_return(aml_int(0)));
            aml_append(dev, method);

            method = aml_method("_S2D", 0, AML_NOTSERIALIZED);
            aml_append(method, aml_return(aml_int(0)));
            aml_append(dev, method);

            method = aml_method("_S3D", 0, AML_NOTSERIALIZED);
            aml_append(method, aml_return(aml_int(s3d)));
            aml_append(dev, method);
        } else if (hotplug_enabled_dev) {
            /* add _SUN/_EJ0 to make slot hotpluggable  */
            aml_append(dev, aml_name_decl("_SUN", aml_int(slot)));

            method = aml_method("_EJ0", 1, AML_NOTSERIALIZED);
            aml_append(method,
                aml_call2("PCEJ", aml_name("BSEL"), aml_name("_SUN"))
            );
            aml_append(dev, method);

            if (bsel) {
                build_append_pcihp_notify_entry(notify_method, slot);
            }
        } else if (bridge_in_acpi) {
            /*
             * device is coldplugged bridge,
             * add child device descriptions into its scope
             */
            PCIBus *sec_bus = pci_bridge_get_sec_bus(PCI_BRIDGE(pdev));

            build_append_pci_bus_devices(dev, sec_bus, pcihp_bridge_en);
        }
        /* slot descriptor has been composed, add it into parent context */
        aml_append(parent_scope, dev);
    }

    if (bsel) {
        aml_append(parent_scope, notify_method);
    }

    /* Append PCNT method to notify about events on local and child buses.
     * Add unconditionally for root since DSDT expects it.
     */
    method = aml_method("PCNT", 0, AML_NOTSERIALIZED);

    /* If bus supports hotplug select it and notify about local events */
    if (bsel) {
        uint64_t bsel_val = qnum_get_uint(qobject_to(QNum, bsel));

        aml_append(method, aml_store(aml_int(bsel_val), aml_name("BNUM")));
        aml_append(method,
            aml_call2("DVNT", aml_name("PCIU"), aml_int(1) /* Device Check */)
        );
        aml_append(method,
            aml_call2("DVNT", aml_name("PCID"), aml_int(3)/* Eject Request */)
        );
    }

    /* Notify about child bus events in any case */
    if (pcihp_bridge_en) {
        QLIST_FOREACH(sec, &bus->child, sibling) {
            int32_t devfn = sec->parent_dev->devfn;

            if (pci_bus_is_root(sec) || pci_bus_is_express(sec)) {
                continue;
            }

            aml_append(method, aml_name("^S%.02X.PCNT", devfn));
        }
    }
    aml_append(parent_scope, method);
    qobject_unref(bsel);
}

void acpi_dsdt_add_pci_bus(Aml *dsdt, AcpiPciBus *pci_host)
{
    Aml *dev, *pci_scope, *hp_scope;

    dev = aml_device("\\_SB.PCI0");
    aml_append(dev, aml_name_decl("_HID", aml_eisaid("PNP0A08")));
    aml_append(dev, aml_name_decl("_CID", aml_eisaid("PNP0A03")));
    aml_append(dev, aml_name_decl("_ADR", aml_int(0)));
    aml_append(dev, aml_name_decl("_UID", aml_int(1)));
    aml_append(dev, aml_name_decl("SUPP", aml_int(0)));
    aml_append(dev, aml_name_decl("CTRL", aml_int(0)));
    aml_append(dev, build_osc_method(0x1F));
    aml_append(dsdt, dev);

    /* PCIHP */
    hp_scope =  aml_scope("\\_SB.PCI0");
    build_acpi_pci_hotplug(hp_scope);
    build_append_pci_bus_devices(hp_scope, pci_host->pci_bus, false);
    aml_append(dsdt, hp_scope);

    pci_scope = build_pci_host_bridge(dsdt, pci_host);
    aml_append(dsdt, pci_scope);
}

#define HOLE_640K_START  (640 * 1024)
#define HOLE_640K_END    (1024 * 1024)

void build_srat_hotpluggable_memory(GArray *table_data, uint64_t base,
                                    uint64_t len, int default_node)
{
    MemoryDeviceInfoList *info_list = qmp_memory_device_list();
    MemoryDeviceInfoList *info;
    MemoryDeviceInfo *mi;
    PCDIMMDeviceInfo *di;
    uint64_t end = base + len, cur, size;
    bool is_nvdimm;
    AcpiSratMemoryAffinity *numamem;
    MemoryAffinityFlags flags;

    for (cur = base, info = info_list;
         cur < end;
         cur += size, info = info->next) {
        numamem = acpi_data_push(table_data, sizeof *numamem);

        if (!info) {
            build_srat_memory(numamem, cur, end - cur, default_node,
                              MEM_AFFINITY_HOTPLUGGABLE | MEM_AFFINITY_ENABLED);
            break;
        }

        mi = info->value;
        is_nvdimm = (mi->type == MEMORY_DEVICE_INFO_KIND_NVDIMM);
        di = !is_nvdimm ? mi->u.dimm.data : mi->u.nvdimm.data;

        if (cur < di->addr) {
            build_srat_memory(numamem, cur, di->addr - cur, default_node,
                              MEM_AFFINITY_HOTPLUGGABLE | MEM_AFFINITY_ENABLED);
            numamem = acpi_data_push(table_data, sizeof *numamem);
        }

        size = di->size;

        flags = MEM_AFFINITY_ENABLED;
        if (di->hotpluggable) {
            flags |= MEM_AFFINITY_HOTPLUGGABLE;
        }
        if (is_nvdimm) {
            flags |= MEM_AFFINITY_NON_VOLATILE;
        }

        build_srat_memory(numamem, di->addr, size, di->node, flags);
    }

    qapi_free_MemoryDeviceInfoList(info_list);
}

void acpi_build_srat(GArray *table_data, BIOSLinker *linker,
                     MachineState *machine, AcpiConfiguration *conf)
{
    AcpiSystemResourceAffinityTable *srat;
    AcpiSratMemoryAffinity *numamem;

    int i;
    int srat_start, numa_start, slots;
    uint64_t mem_len, mem_base, next_base;
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    const CPUArchIdList *apic_ids = mc->possible_cpu_arch_ids(machine);
    ram_addr_t hotplugabble_address_space_size =
        object_property_get_int(OBJECT(machine), PC_MACHINE_DEVMEM_REGION_SIZE,
                                NULL);

    srat_start = table_data->len;

    srat = acpi_data_push(table_data, sizeof *srat);
    srat->reserved1 = cpu_to_le32(1);

    for (i = 0; i < apic_ids->len; i++) {
        int node_id = apic_ids->cpus[i].props.node_id;
        uint32_t apic_id = apic_ids->cpus[i].arch_id;

        if (apic_id < 255) {
            AcpiSratProcessorAffinity *core;

            core = acpi_data_push(table_data, sizeof *core);
            core->type = ACPI_SRAT_PROCESSOR_APIC;
            core->length = sizeof(*core);
            core->local_apic_id = apic_id;
            core->proximity_lo = node_id;
            memset(core->proximity_hi, 0, 3);
            core->local_sapic_eid = 0;
            core->flags = cpu_to_le32(1);
        } else {
            AcpiSratProcessorX2ApicAffinity *core;

            core = acpi_data_push(table_data, sizeof *core);
            core->type = ACPI_SRAT_PROCESSOR_x2APIC;
            core->length = sizeof(*core);
            core->x2apic_id = cpu_to_le32(apic_id);
            core->proximity_domain = cpu_to_le32(node_id);
            core->flags = cpu_to_le32(1);
        }
    }

    /* the memory map is a bit tricky, it contains at least one hole
     * from 640k-1M and possibly another one from 3.5G-4G.
     */
    next_base = 0;
    numa_start = table_data->len;

    for (i = 1; i < conf->numa_nodes + 1; ++i) {
        mem_base = next_base;
        mem_len = conf->node_mem[i - 1];
        next_base = mem_base + mem_len;

        /* Cut out the 640K hole */
        if (mem_base <= HOLE_640K_START &&
            next_base > HOLE_640K_START) {
            mem_len -= next_base - HOLE_640K_START;
            if (mem_len > 0) {
                numamem = acpi_data_push(table_data, sizeof *numamem);
                build_srat_memory(numamem, mem_base, mem_len, i - 1,
                                  MEM_AFFINITY_ENABLED);
            }

            /* Check for the rare case: 640K < RAM < 1M */
            if (next_base <= HOLE_640K_END) {
                next_base = HOLE_640K_END;
                continue;
            }
            mem_base = HOLE_640K_END;
            mem_len = next_base - HOLE_640K_END;
        }

        /* Cut out the ACPI_PCI hole */
        if (mem_base <= conf->below_4g_mem_size &&
            next_base > conf->below_4g_mem_size) {
            mem_len -= next_base - conf->below_4g_mem_size;
            if (mem_len > 0) {
                numamem = acpi_data_push(table_data, sizeof *numamem);
                build_srat_memory(numamem, mem_base, mem_len, i - 1,
                                  MEM_AFFINITY_ENABLED);
            }
            mem_base = 1ULL << 32;
            mem_len = next_base - conf->below_4g_mem_size;
            next_base = mem_base + mem_len;
        }
        numamem = acpi_data_push(table_data, sizeof *numamem);
        build_srat_memory(numamem, mem_base, mem_len, i - 1,
                          MEM_AFFINITY_ENABLED);
    }
    slots = (table_data->len - numa_start) / sizeof *numamem;
    for (; slots < conf->numa_nodes + 2; slots++) {
        numamem = acpi_data_push(table_data, sizeof *numamem);
        build_srat_memory(numamem, 0, 0, 0, MEM_AFFINITY_NOFLAGS);
    }

    /*
     * Entry is required for Windows to enable memory hotplug in OS
     * and for Linux to enable SWIOTLB when booted with less than
     * 4G of RAM. Windows works better if the entry sets proximity
     * to the highest NUMA node in the machine.
     * Memory devices may override proximity set by this entry,
     * providing _PXM method if necessary.
     */
    if (hotplugabble_address_space_size) {
        build_srat_hotpluggable_memory(table_data, machine->device_memory->base,
                                       hotplugabble_address_space_size,
                                       conf->numa_nodes - 1);
    }

    build_header(linker, table_data,
                 (void *)(table_data->data + srat_start),
                 "SRAT",
                 table_data->len - srat_start, 1, NULL, NULL);
}

/* Build rsdt table */
void
build_rsdt(GArray *table_data, BIOSLinker *linker, GArray *table_offsets,
           const char *oem_id, const char *oem_table_id)
{
    int i;
    unsigned rsdt_entries_offset;
    AcpiRsdtDescriptorRev1 *rsdt;
    const unsigned table_data_len = (sizeof(uint32_t) * table_offsets->len);
    const unsigned rsdt_entry_size = sizeof(rsdt->table_offset_entry[0]);
    const size_t rsdt_len = sizeof(*rsdt) + table_data_len;

    rsdt = acpi_data_push(table_data, rsdt_len);
    rsdt_entries_offset = (char *)rsdt->table_offset_entry - table_data->data;
    for (i = 0; i < table_offsets->len; ++i) {
        uint32_t ref_tbl_offset = g_array_index(table_offsets, uint32_t, i);
        uint32_t rsdt_entry_offset = rsdt_entries_offset + rsdt_entry_size * i;

        /* rsdt->table_offset_entry to be filled by Guest linker */
        bios_linker_loader_add_pointer(linker,
            ACPI_BUILD_TABLE_FILE, rsdt_entry_offset, rsdt_entry_size,
            ACPI_BUILD_TABLE_FILE, ref_tbl_offset);
    }
    build_header(linker, table_data,
                 (void *)rsdt, "RSDT", rsdt_len, 1, oem_id, oem_table_id);
}

/* Build xsdt table */
void
build_xsdt(GArray *table_data, BIOSLinker *linker, GArray *table_offsets,
           const char *oem_id, const char *oem_table_id)
{
    int i;
    unsigned xsdt_entries_offset;
    AcpiXsdtDescriptorRev2 *xsdt;
    const unsigned table_data_len = (sizeof(uint64_t) * table_offsets->len);
    const unsigned xsdt_entry_size = sizeof(xsdt->table_offset_entry[0]);
    const size_t xsdt_len = sizeof(*xsdt) + table_data_len;

    xsdt = acpi_data_push(table_data, xsdt_len);
    xsdt_entries_offset = (char *)xsdt->table_offset_entry - table_data->data;
    for (i = 0; i < table_offsets->len; ++i) {
        uint64_t ref_tbl_offset = g_array_index(table_offsets, uint32_t, i);
        uint64_t xsdt_entry_offset = xsdt_entries_offset + xsdt_entry_size * i;

        /* xsdt->table_offset_entry to be filled by Guest linker */
        bios_linker_loader_add_pointer(linker,
            ACPI_BUILD_TABLE_FILE, xsdt_entry_offset, xsdt_entry_size,
            ACPI_BUILD_TABLE_FILE, ref_tbl_offset);
    }
    build_header(linker, table_data,
                 (void *)xsdt, "XSDT", xsdt_len, 1, oem_id, oem_table_id);
}

/* Legacy RSDP pointing at an RSDT. This is deprecated */
GArray *build_rsdp_rsdt(GArray *rsdp_table, BIOSLinker *linker, unsigned rsdt_tbl_offset)
{
    AcpiRsdpDescriptor *rsdp = acpi_data_push(rsdp_table, sizeof *rsdp);
    unsigned rsdt_pa_size = sizeof(rsdp->rsdt_physical_address);
    unsigned rsdt_pa_offset =
        (char *)&rsdp->rsdt_physical_address - rsdp_table->data;

    bios_linker_loader_alloc(linker, ACPI_BUILD_RSDP_FILE, rsdp_table, 16,
                             true /* fseg memory */);

    memcpy(&rsdp->signature, "RSD PTR ", 8);
    memcpy(rsdp->oem_id, ACPI_BUILD_APPNAME6, 6);
    /* Address to be filled by Guest linker */
    bios_linker_loader_add_pointer(linker,
        ACPI_BUILD_RSDP_FILE, rsdt_pa_offset, rsdt_pa_size,
        ACPI_BUILD_TABLE_FILE, rsdt_tbl_offset);

    /* Checksum to be filled by Guest linker */
    bios_linker_loader_add_checksum(linker, ACPI_BUILD_RSDP_FILE,
        (char *)rsdp - rsdp_table->data, sizeof *rsdp,
        (char *)&rsdp->checksum - rsdp_table->data);

    return rsdp_table;
}

/* RSDP pointing at an XSDT */
GArray *build_rsdp(GArray *rsdp_table, BIOSLinker *linker, unsigned xsdt_tbl_offset)
{
    AcpiRsdpDescriptor *rsdp = acpi_data_push(rsdp_table, sizeof *rsdp);
    unsigned xsdt_pa_size = sizeof(rsdp->xsdt_physical_address);
    unsigned xsdt_pa_offset =
        (char *)&rsdp->xsdt_physical_address - rsdp_table->data;
    unsigned xsdt_offset =
        (char *)&rsdp->length - rsdp_table->data;

    bios_linker_loader_alloc(linker, ACPI_BUILD_RSDP_FILE, rsdp_table, 16,
                             true /* fseg memory */);

    memcpy(&rsdp->signature, "RSD PTR ", 8);
    memcpy(rsdp->oem_id, ACPI_BUILD_APPNAME6, 6);
    rsdp->length = cpu_to_le32(sizeof(*rsdp));
    /* version 2, we will use the XSDT pointer */
    rsdp->revision = 0x02;

    /* Address to be filled by Guest linker */
    bios_linker_loader_add_pointer(linker,
        ACPI_BUILD_RSDP_FILE, xsdt_pa_offset, xsdt_pa_size,
        ACPI_BUILD_TABLE_FILE, xsdt_tbl_offset);

    /* Legacy checksum to be filled by Guest linker */
    bios_linker_loader_add_checksum(linker, ACPI_BUILD_RSDP_FILE,
        (char *)rsdp - rsdp_table->data, xsdt_offset,
        (char *)&rsdp->checksum - rsdp_table->data);

    /* Extended checksum to be filled by Guest linker */
    bios_linker_loader_add_checksum(linker, ACPI_BUILD_RSDP_FILE,
        (char *)rsdp - rsdp_table->data, sizeof *rsdp,
        (char *)&rsdp->extended_checksum - rsdp_table->data);

    return rsdp_table;
}

void build_srat_memory(AcpiSratMemoryAffinity *numamem, uint64_t base,
                       uint64_t len, int node, MemoryAffinityFlags flags)
{
    numamem->type = ACPI_SRAT_MEMORY;
    numamem->length = sizeof(*numamem);
    numamem->proximity = cpu_to_le32(node);
    numamem->flags = cpu_to_le32(flags);
    numamem->base_addr = cpu_to_le64(base);
    numamem->range_length = cpu_to_le64(len);
}

/*
 * ACPI spec 5.2.17 System Locality Distance Information Table
 * (Revision 2.0 or later)
 */
void build_slit(GArray *table_data, BIOSLinker *linker)
{
    int slit_start, i, j;
    slit_start = table_data->len;

    acpi_data_push(table_data, sizeof(AcpiTableHeader));

    build_append_int_noprefix(table_data, nb_numa_nodes, 8);
    for (i = 0; i < nb_numa_nodes; i++) {
        for (j = 0; j < nb_numa_nodes; j++) {
            assert(numa_info[i].distance[j]);
            build_append_int_noprefix(table_data, numa_info[i].distance[j], 1);
        }
    }

    build_header(linker, table_data,
                 (void *)(table_data->data + slit_start),
                 "SLIT",
                 table_data->len - slit_start, 1, NULL, NULL);
}

/* build rev1/rev3/rev5.1 FADT */
void build_fadt(GArray *tbl, BIOSLinker *linker, const AcpiFadtData *f,
                const char *oem_id, const char *oem_table_id)
{
    int off;
    int fadt_start = tbl->len;

    acpi_data_push(tbl, sizeof(AcpiTableHeader));

    /* FACS address to be filled by Guest linker at runtime */
    off = tbl->len;
    build_append_int_noprefix(tbl, 0, 4); /* FIRMWARE_CTRL */
    if (f->facs_tbl_offset) { /* don't patch if not supported by platform */
        bios_linker_loader_add_pointer(linker,
            ACPI_BUILD_TABLE_FILE, off, 4,
            ACPI_BUILD_TABLE_FILE, *f->facs_tbl_offset);
    }

    /* DSDT address to be filled by Guest linker at runtime */
    off = tbl->len;
    build_append_int_noprefix(tbl, 0, 4); /* DSDT */
    if (f->dsdt_tbl_offset) { /* don't patch if not supported by platform */
        bios_linker_loader_add_pointer(linker,
            ACPI_BUILD_TABLE_FILE, off, 4,
            ACPI_BUILD_TABLE_FILE, *f->dsdt_tbl_offset);
    }

    /* ACPI1.0: INT_MODEL, ACPI2.0+: Reserved */
    build_append_int_noprefix(tbl, f->int_model /* Multiple APIC */, 1);
    /* Preferred_PM_Profile */
    build_append_int_noprefix(tbl, 0 /* Unspecified */, 1);
    build_append_int_noprefix(tbl, f->sci_int, 2); /* SCI_INT */
    build_append_int_noprefix(tbl, f->smi_cmd, 4); /* SMI_CMD */
    build_append_int_noprefix(tbl, f->acpi_enable_cmd, 1); /* ACPI_ENABLE */
    build_append_int_noprefix(tbl, f->acpi_disable_cmd, 1); /* ACPI_DISABLE */
    build_append_int_noprefix(tbl, 0 /* not supported */, 1); /* S4BIOS_REQ */
    /* ACPI1.0: Reserved, ACPI2.0+: PSTATE_CNT */
    build_append_int_noprefix(tbl, 0, 1);
    build_append_int_noprefix(tbl, f->pm1a_evt.address, 4); /* PM1a_EVT_BLK */
    build_append_int_noprefix(tbl, 0, 4); /* PM1b_EVT_BLK */
    build_append_int_noprefix(tbl, f->pm1a_cnt.address, 4); /* PM1a_CNT_BLK */
    build_append_int_noprefix(tbl, 0, 4); /* PM1b_CNT_BLK */
    build_append_int_noprefix(tbl, 0, 4); /* PM2_CNT_BLK */
    build_append_int_noprefix(tbl, f->pm_tmr.address, 4); /* PM_TMR_BLK */
    build_append_int_noprefix(tbl, f->gpe0_blk.address, 4); /* GPE0_BLK */
    build_append_int_noprefix(tbl, 0, 4); /* GPE1_BLK */
    /* PM1_EVT_LEN */
    build_append_int_noprefix(tbl, f->pm1a_evt.bit_width / 8, 1);
    /* PM1_CNT_LEN */
    build_append_int_noprefix(tbl, f->pm1a_cnt.bit_width / 8, 1);
    build_append_int_noprefix(tbl, 0, 1); /* PM2_CNT_LEN */
    build_append_int_noprefix(tbl, f->pm_tmr.bit_width / 8, 1); /* PM_TMR_LEN */
    /* GPE0_BLK_LEN */
    build_append_int_noprefix(tbl, f->gpe0_blk.bit_width / 8, 1);
    build_append_int_noprefix(tbl, 0, 1); /* GPE1_BLK_LEN */
    build_append_int_noprefix(tbl, 0, 1); /* GPE1_BASE */
    build_append_int_noprefix(tbl, 0, 1); /* CST_CNT */
    build_append_int_noprefix(tbl, f->plvl2_lat, 2); /* P_LVL2_LAT */
    build_append_int_noprefix(tbl, f->plvl3_lat, 2); /* P_LVL3_LAT */
    build_append_int_noprefix(tbl, 0, 2); /* FLUSH_SIZE */
    build_append_int_noprefix(tbl, 0, 2); /* FLUSH_STRIDE */
    build_append_int_noprefix(tbl, 0, 1); /* DUTY_OFFSET */
    build_append_int_noprefix(tbl, 0, 1); /* DUTY_WIDTH */
    build_append_int_noprefix(tbl, 0, 1); /* DAY_ALRM */
    build_append_int_noprefix(tbl, 0, 1); /* MON_ALRM */
    build_append_int_noprefix(tbl, f->rtc_century, 1); /* CENTURY */
    build_append_int_noprefix(tbl, 0, 2); /* IAPC_BOOT_ARCH */
    build_append_int_noprefix(tbl, 0, 1); /* Reserved */
    build_append_int_noprefix(tbl, f->flags, 4); /* Flags */

    if (f->rev == 1) {
        goto build_hdr;
    }

    build_append_gas_from_struct(tbl, &f->reset_reg); /* RESET_REG */
    build_append_int_noprefix(tbl, f->reset_val, 1); /* RESET_VALUE */
    /* Since ACPI 5.1 */
    if ((f->rev >= 6) || ((f->rev == 5) && f->minor_ver > 0)) {
        build_append_int_noprefix(tbl, f->arm_boot_arch, 2); /* ARM_BOOT_ARCH */
        /* FADT Minor Version */
        build_append_int_noprefix(tbl, f->minor_ver, 1);
    } else {
        build_append_int_noprefix(tbl, 0, 3); /* Reserved upto ACPI 5.0 */
    }
    build_append_int_noprefix(tbl, 0, 8); /* X_FIRMWARE_CTRL */

    /* XDSDT address to be filled by Guest linker at runtime */
    off = tbl->len;
    build_append_int_noprefix(tbl, 0, 8); /* X_DSDT */
    if (f->xdsdt_tbl_offset) {
        bios_linker_loader_add_pointer(linker,
            ACPI_BUILD_TABLE_FILE, off, 8,
            ACPI_BUILD_TABLE_FILE, *f->xdsdt_tbl_offset);
    }

    build_append_gas_from_struct(tbl, &f->pm1a_evt); /* X_PM1a_EVT_BLK */
    /* X_PM1b_EVT_BLK */
    build_append_gas(tbl, AML_AS_SYSTEM_MEMORY, 0 , 0, 0, 0);
    build_append_gas_from_struct(tbl, &f->pm1a_cnt); /* X_PM1a_CNT_BLK */
    /* X_PM1b_CNT_BLK */
    build_append_gas(tbl, AML_AS_SYSTEM_MEMORY, 0 , 0, 0, 0);
    /* X_PM2_CNT_BLK */
    build_append_gas(tbl, AML_AS_SYSTEM_MEMORY, 0 , 0, 0, 0);
    build_append_gas_from_struct(tbl, &f->pm_tmr); /* X_PM_TMR_BLK */
    build_append_gas_from_struct(tbl, &f->gpe0_blk); /* X_GPE0_BLK */
    build_append_gas(tbl, AML_AS_SYSTEM_MEMORY, 0 , 0, 0, 0); /* X_GPE1_BLK */

    if (f->rev <= 4) {
        goto build_hdr;
    }

    /* SLEEP_CONTROL_REG */
    build_append_gas_from_struct(tbl, &f->sleep_control_reg);
    /* SLEEP_STATUS_REG */
    build_append_gas(tbl, AML_AS_SYSTEM_MEMORY, 0 , 0, 0, 0);

    /* TODO: extra fields need to be added to support revisions above rev5 */
    assert(f->rev == 5);

build_hdr:
    build_header(linker, tbl, (void *)(tbl->data + fadt_start),
                 "FACP", tbl->len - fadt_start, f->rev, oem_id, oem_table_id);
}
