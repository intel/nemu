/*
 * PXE test cases.
 *
 * Copyright (c) 2016, 2017 Red Hat Inc.
 *
 * Authors:
 *  Michael S. Tsirkin <mst@redhat.com>,
 *  Victor Kaplansky <victork@redhat.com>
 *  Thomas Huth <thuth@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "qemu-common.h"
#include "libqtest.h"
#include "boot-sector.h"

#define NETNAME "net0"

static char disk[] = "tests/pxe-test-disk-XXXXXX";

typedef struct testdef {
    const char *machine;    /* Machine type */
    const char *model;      /* NIC device model */
} testdef_t;

static testdef_t x86_tests[] = {
    { "pc", "virtio-net-pci" },
    { "q35", "virtio-net-pci", },
    { NULL },
};

static void test_pxe_one(const testdef_t *test, bool ipv6)
{
    char *args;

    /* TODO: This test will not pass as it needs TCG
     * We may need to delete this whole PXE test
     */
    args = g_strdup_printf(
        "-machine %s,accel=kvm:tcg -nodefaults -boot order=n "
        "-netdev user,id=" NETNAME ",tftp=./,bootfile=%s,ipv4=%s,ipv6=%s "
        "-device %s,bootindex=1,netdev=" NETNAME,
        test->machine, disk, ipv6 ? "off" : "on", ipv6 ? "on" : "off",
        test->model);

    qtest_start(args);
    boot_sector_test(global_qtest);
    qtest_quit(global_qtest);
    g_free(args);
}

static void test_pxe_ipv4(gconstpointer data)
{
    const testdef_t *test = data;

    test_pxe_one(test, false);
}

static void test_pxe_ipv6(gconstpointer data)
{
    const testdef_t *test = data;

    test_pxe_one(test, true);
}

static void test_batch(const testdef_t *tests, bool ipv6)
{
    int i;

    for (i = 0; tests[i].machine; i++) {
        const testdef_t *test = &tests[i];
        char *testname;

        testname = g_strdup_printf("pxe/ipv4/%s/%s",
                                   test->machine, test->model);
        qtest_add_data_func(testname, test, test_pxe_ipv4);
        g_free(testname);

        if (ipv6) {
            testname = g_strdup_printf("pxe/ipv6/%s/%s",
                                       test->machine, test->model);
            qtest_add_data_func(testname, test, test_pxe_ipv6);
            g_free(testname);
        }
    }
}

int main(int argc, char *argv[])
{
    int ret;
    const char *arch = qtest_get_arch();

    ret = boot_sector_init(disk);
    if(ret)
        return ret;

    g_test_init(&argc, &argv, NULL);

    if (strcmp(arch, "i386") == 0 || strcmp(arch, "x86_64") == 0) {
        test_batch(x86_tests, false);
    }

    ret = g_test_run();
    boot_sector_cleanup(disk);
    return ret;
}
