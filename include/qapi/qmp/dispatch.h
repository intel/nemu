/*
 * Core Definitions for QAPI/QMP Dispatch
 *
 * Copyright IBM, Corp. 2011
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.1 or later.
 * See the COPYING.LIB file in the top-level directory.
 *
 */

#ifndef QAPI_QMP_DISPATCH_H
#define QAPI_QMP_DISPATCH_H

#include "qemu/queue.h"

typedef void (QmpCommandFunc)(QDict *, QObject **, Error **);

typedef enum QmpCommandOptions
{
    QCO_NO_OPTIONS            =  0x0,
    QCO_NO_SUCCESS_RESP       =  (1U << 0),
    QCO_ALLOW_OOB             =  (1U << 1),
} QmpCommandOptions;

typedef struct QmpCommand
{
    const char *name;
    QmpCommandFunc *fn;
    QmpCommandOptions options;
    QTAILQ_ENTRY(QmpCommand) node;
    bool enabled;
} QmpCommand;

typedef QTAILQ_HEAD(QmpCommandList, QmpCommand) QmpCommandList;

void qmp_register_command(QmpCommandList *cmds, const char *name,
                          QmpCommandFunc *fn, QmpCommandOptions options);
void qmp_unregister_command(QmpCommandList *cmds, const char *name);
QmpCommand *qmp_find_command(QmpCommandList *cmds, const char *name);
QObject *qmp_dispatch(QmpCommandList *cmds, QObject *request);

QObject *qmp_build_error_object(Error *err);
QDict *qmp_dispatch_check_obj(const QObject *request, Error **errp);
bool qmp_is_oob(QDict *dict);

typedef void (*qmp_cmd_callback_fn)(QmpCommand *cmd, void *opaque);

void qmp_for_each_command(QmpCommandList *cmds, qmp_cmd_callback_fn fn,
                          void *opaque);

#endif
