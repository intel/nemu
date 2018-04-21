/*
 * Machine 'none' tests.
 *
 * Copyright (c) 2018 Red Hat Inc.
 *
 * Authors:
 *  Igor Mammedov <imammedo@redhat.com>,
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"

#include "qemu-common.h"
#include "qemu/cutils.h"
#include "libqtest.h"
#include "qapi/qmp/qdict.h"


struct arch2cpu {
    const char *arch;
    const char *cpu_model;
};

static struct arch2cpu cpus_map[] = {
    /* tested targets list */
    { "arm", "cortex-a15" },
    { "aarch64", "cortex-a57" },
    { "x86_64", "qemu64,apic-id=0" },
    { "i386", "qemu32,apic-id=0" },
};

static const char *get_cpu_model_by_arch(const char *arch)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(cpus_map); i++) {
        if (!strcmp(arch, cpus_map[i].arch)) {
            return cpus_map[i].cpu_model;
        }
    }
    return NULL;
}

static void test_machine_cpu_cli(void)
{
    QDict *response;
    const char *arch = qtest_get_arch();
    const char *cpu_model = get_cpu_model_by_arch(arch);

    global_qtest = qtest_startf("-machine none -cpu '%s'", cpu_model);

    response = qmp("{ 'execute': 'quit' }");
    g_assert(qdict_haskey(response, "return"));
    QDECREF(response);

    qtest_quit(global_qtest);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("machine/none/cpu_option", test_machine_cpu_cli);

    return g_test_run();
}
