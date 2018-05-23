/*
 * QEMU DNS resolver
 *
 * Copyright (c) 2016 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "qemu/osdep.h"
#include "io/dns-resolver.h"
#include "qapi/clone-visitor.h"
#include "qapi/qapi-visit-sockets.h"
#include "qemu/sockets.h"
#include "qapi/error.h"
#include "qemu/cutils.h"

#ifndef AI_NUMERICSERV
# define AI_NUMERICSERV 0
#endif

static QIODNSResolver *instance;
static GOnce instance_init = G_ONCE_INIT;

static gpointer qio_dns_resolve_init_instance(gpointer unused G_GNUC_UNUSED)
{
    instance = QIO_DNS_RESOLVER(object_new(TYPE_QIO_DNS_RESOLVER));
    return NULL;
}

QIODNSResolver *qio_dns_resolver_get_instance(void)
{
    g_once(&instance_init, qio_dns_resolve_init_instance, NULL);
    return instance;
}

static int qio_dns_resolver_lookup_sync_inet(QIODNSResolver *resolver,
                                             SocketAddress *addr,
                                             size_t *naddrs,
                                             SocketAddress ***addrs,
                                             Error **errp)
{
    struct addrinfo ai, *res, *e;
    InetSocketAddress *iaddr = &addr->u.inet;
    char port[33];
    char uaddr[INET6_ADDRSTRLEN + 1];
    char uport[33];
    int rc;
    Error *err = NULL;
    size_t i;

    *naddrs = 0;
    *addrs = NULL;

    memset(&ai, 0, sizeof(ai));
    ai.ai_flags = AI_PASSIVE;
    if (iaddr->has_numeric && iaddr->numeric) {
        ai.ai_flags |= AI_NUMERICHOST | AI_NUMERICSERV;
    }
    ai.ai_family = inet_ai_family_from_address(iaddr, &err);
    ai.ai_socktype = SOCK_STREAM;

    if (err) {
        error_propagate(errp, err);
        return -1;
    }

    if (iaddr->host == NULL) {
        error_setg(errp, "host not specified");
        return -1;
    }
    if (iaddr->port != NULL) {
        pstrcpy(port, sizeof(port), iaddr->port);
    } else {
        port[0] = '\0';
    }

    rc = getaddrinfo(strlen(iaddr->host) ? iaddr->host : NULL,
                     strlen(port) ? port : NULL, &ai, &res);
    if (rc != 0) {
        error_setg(errp, "address resolution failed for %s:%s: %s",
                   iaddr->host, port, gai_strerror(rc));
        return -1;
    }

    for (e = res; e != NULL; e = e->ai_next) {
        (*naddrs)++;
    }

    *addrs = g_new0(SocketAddress *, *naddrs);

    /* create socket + bind */
    for (i = 0, e = res; e != NULL; i++, e = e->ai_next) {
        SocketAddress *newaddr = g_new0(SocketAddress, 1);

        newaddr->type = SOCKET_ADDRESS_TYPE_INET;

        getnameinfo((struct sockaddr *)e->ai_addr, e->ai_addrlen,
                    uaddr, INET6_ADDRSTRLEN, uport, 32,
                    NI_NUMERICHOST | NI_NUMERICSERV);

        newaddr->u.inet = (InetSocketAddress){
            .host = g_strdup(uaddr),
            .port = g_strdup(uport),
            .has_numeric = true,
            .numeric = true,
            .has_to = iaddr->has_to,
            .to = iaddr->to,
            .has_ipv4 = iaddr->has_ipv4,
            .ipv4 = iaddr->ipv4,
            .has_ipv6 = iaddr->has_ipv6,
            .ipv6 = iaddr->ipv6,
        };

        (*addrs)[i] = newaddr;
    }
    freeaddrinfo(res);
    return 0;
}


static int qio_dns_resolver_lookup_sync_nop(QIODNSResolver *resolver,
                                            SocketAddress *addr,
                                            size_t *naddrs,
                                            SocketAddress ***addrs,
                                            Error **errp)
{
    *naddrs = 1;
    *addrs = g_new0(SocketAddress *, 1);
    (*addrs)[0] = QAPI_CLONE(SocketAddress, addr);

    return 0;
}


int qio_dns_resolver_lookup_sync(QIODNSResolver *resolver,
                                 SocketAddress *addr,
                                 size_t *naddrs,
                                 SocketAddress ***addrs,
                                 Error **errp)
{
    switch (addr->type) {
    case SOCKET_ADDRESS_TYPE_INET:
        return qio_dns_resolver_lookup_sync_inet(resolver,
                                                 addr,
                                                 naddrs,
                                                 addrs,
                                                 errp);

    case SOCKET_ADDRESS_TYPE_UNIX:
    case SOCKET_ADDRESS_TYPE_VSOCK:
    case SOCKET_ADDRESS_TYPE_FD:
        return qio_dns_resolver_lookup_sync_nop(resolver,
                                                addr,
                                                naddrs,
                                                addrs,
                                                errp);

    default:
        abort();
    }
}


struct QIODNSResolverLookupData {
    SocketAddress *addr;
    SocketAddress **addrs;
    size_t naddrs;
};


static const TypeInfo qio_dns_resolver_info = {
    .parent = TYPE_OBJECT,
    .name = TYPE_QIO_DNS_RESOLVER,
    .instance_size = sizeof(QIODNSResolver),
    .class_size = sizeof(QIODNSResolverClass),
};


static void qio_dns_resolver_register_types(void)
{
    type_register_static(&qio_dns_resolver_info);
}


type_init(qio_dns_resolver_register_types);
