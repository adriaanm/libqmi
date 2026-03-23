/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * libqmi-glib -- GLib/GIO based library to control QMI devices
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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 * Copyright (C) 2026 BQ268 project
 *
 * AF_MSM_IPC transport endpoint for CAF/downstream Qualcomm kernels.
 * Modeled on qmi-endpoint-qrtr.c (Copyright 2019-2020 Eric Caruso,
 * Aleksander Morgado).
 */

#include <config.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#include <gmodule.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "qmi-endpoint-msmipc.h"
#include "qmi-errors.h"
#include "qmi-enum-types.h"
#include "qmi-error-types.h"
#include "qmi-message.h"

/* ── Vendored AF_MSM_IPC definitions ──────────────────────────────────── */
/* From include/uapi/linux/msm_ipc.h (CAF kernel) */

#ifndef AF_MSM_IPC
#define AF_MSM_IPC 27
#endif

#define MSM_IPC_ADDR_NAME 1
#define MSM_IPC_ADDR_ID   2

struct msm_ipc_port_addr {
    uint32_t node_id;
    uint32_t port_id;
};

struct msm_ipc_port_name {
    uint32_t service;
    uint32_t instance;
};

struct msm_ipc_addr {
    unsigned char addrtype;
    union {
        struct msm_ipc_port_addr port_addr;
        struct msm_ipc_port_name port_name;
    } addr;
};

struct sockaddr_msm_ipc {
    unsigned short family;
    struct msm_ipc_addr address;
    unsigned char reserved;
};

#define IPC_ROUTER_IOCTL_MAGIC 0xC3

#define IPC_ROUTER_IOCTL_LOOKUP_SERVER \
    _IOWR(IPC_ROUTER_IOCTL_MAGIC, 2, struct sockaddr_msm_ipc)

struct msm_ipc_server_info {
    uint32_t node_id;
    uint32_t port_id;
    uint32_t service;
    uint32_t instance;
};

struct server_lookup_args {
    struct msm_ipc_port_name port_name;
    int num_entries_in_array;
    int num_entries_found;
    uint32_t lookup_mask;
    struct msm_ipc_server_info srv_info[0];
};

/* ── CTL message constants (same as qmi-endpoint-qrtr.c) ─────────────── */

#define QMI_MESSAGE_OUTPUT_TLV_RESULT    0x02
#define QMI_MESSAGE_TLV_ALLOCATION_INFO  0x01
#define QMI_MESSAGE_INPUT_TLV_SERVICE    0x01

#define QMI_MESSAGE_CTL_GET_VERSION_INFO 0x0021
#define QMI_MESSAGE_CTL_SYNC             0x0027

/* ── GObject boilerplate ──────────────────────────────────────────────── */

G_DEFINE_TYPE (QmiEndpointMsmIpc, qmi_endpoint_msmipc, QMI_TYPE_ENDPOINT)

struct _QmiEndpointMsmIpcPrivate {
    gboolean  endpoint_open;
    GList    *clients;
};

/* ── Client management ────────────────────────────────────────────────── */

typedef struct {
    QmiService  service;
    guint       cid;
    int         fd;
    guint       fd_source_id;
} ClientInfo;

static void
client_info_free (ClientInfo *client_info)
{
    if (client_info->fd_source_id)
        g_source_remove (client_info->fd_source_id);
    if (client_info->fd >= 0)
        close (client_info->fd);
    g_slice_free (ClientInfo, client_info);
}

static void
client_info_list_free (GList *list)
{
    g_list_free_full (list, (GDestroyNotify) client_info_free);
}

static ClientInfo *
client_info_lookup (QmiEndpointMsmIpc *self,
                    QmiService         service,
                    guint              cid)
{
    GList *l;

    for (l = self->priv->clients; l; l = g_list_next (l)) {
        ClientInfo *ci = l->data;
        if (ci->service == service && ci->cid == cid)
            return ci;
    }
    return NULL;
}

static gint
client_info_cmp (const ClientInfo *a, const ClientInfo *b)
{
    if (a->service != b->service)
        return a->service - b->service;
    return a->cid - b->cid;
}

/* ── Receive callback ─────────────────────────────────────────────────── */

static void
add_qmi_message_to_buffer (QmiEndpointMsmIpc *self,
                            QmiMessage        *message)
{
    g_autoptr(GError)  error = NULL;
    const guint8      *raw_message;
    gsize              raw_message_len;

    raw_message = qmi_message_get_raw (message, &raw_message_len, &error);
    if (!raw_message)
        g_warning ("[%s] Got malformed QMI message: %s",
                   qmi_endpoint_get_name (QMI_ENDPOINT (self)), error->message);
    else
        qmi_endpoint_add_message (QMI_ENDPOINT (self), raw_message, raw_message_len);
    qmi_message_unref (message);
}

/* Wrapper carrying both endpoint and client_info for fd callback */
typedef struct {
    QmiEndpointMsmIpc *self;
    ClientInfo        *client_info;
} ClientRecvContext;

static gboolean
client_recv_cb (gint          fd,
                GIOCondition  condition,
                gpointer      user_data)
{
    ClientRecvContext *ctx = user_data;
    QmiEndpointMsmIpc *self = ctx->self;
    ClientInfo        *client_info = ctx->client_info;
    guint8             buf[8192];
    ssize_t            n;
    QmiMessage        *message;
    g_autoptr(GError)  error = NULL;
    g_autoptr(GByteArray) data = NULL;

    if (condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
        g_warning ("[%s] Error on MSMIPC socket for service %s",
                   qmi_endpoint_get_name (QMI_ENDPOINT (self)),
                   qmi_service_get_string (client_info->service));
        g_signal_emit_by_name (QMI_ENDPOINT (self), QMI_ENDPOINT_SIGNAL_HANGUP);
        return G_SOURCE_REMOVE;
    }

    n = recv (fd, buf, sizeof (buf), MSG_DONTWAIT);
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return G_SOURCE_CONTINUE;
        g_warning ("[%s] recv error on MSMIPC socket: %s",
                   qmi_endpoint_get_name (QMI_ENDPOINT (self)),
                   n == 0 ? "connection closed" : g_strerror (errno));
        return G_SOURCE_CONTINUE;
    }

    data = g_byte_array_sized_new (n);
    g_byte_array_append (data, buf, n);

    message = qmi_message_new_from_data (client_info->service,
                                          (guint8) client_info->cid,
                                          data,
                                          &error);
    if (!message) {
        g_warning ("[%s] Failed to parse QMI message from service %s: %s",
                   qmi_endpoint_get_name (QMI_ENDPOINT (self)),
                   qmi_service_get_string (client_info->service),
                   error->message);
        return G_SOURCE_CONTINUE;
    }

    add_qmi_message_to_buffer (self, message);
    return G_SOURCE_CONTINUE;
}

static void
client_recv_context_free (gpointer data)
{
    g_slice_free (ClientRecvContext, data);
}

/* ── Service lookup via IPC Router ────────────────────────────────────── */

static gboolean
lookup_service_port (guint     service_id,
                     uint32_t *out_node_id,
                     uint32_t *out_port_id,
                     GError  **error)
{
    int fd;
    int rc;
    struct {
        struct server_lookup_args args;
        struct msm_ipc_server_info info;
    } lookup;

    fd = socket (AF_MSM_IPC, SOCK_DGRAM, 0);
    if (fd < 0) {
        g_set_error (error, QMI_CORE_ERROR, QMI_CORE_ERROR_FAILED,
                     "Failed to create AF_MSM_IPC socket: %s", g_strerror (errno));
        return FALSE;
    }

    memset (&lookup, 0, sizeof (lookup));
    lookup.args.port_name.service = service_id;
    lookup.args.port_name.instance = 0;
    lookup.args.num_entries_in_array = 1;
    lookup.args.lookup_mask = 0; /* match any instance */

    rc = ioctl (fd, IPC_ROUTER_IOCTL_LOOKUP_SERVER, &lookup);
    close (fd);

    if (rc < 0) {
        g_set_error (error, QMI_CORE_ERROR, QMI_CORE_ERROR_FAILED,
                     "LOOKUP_SERVER ioctl failed for service %u: %s",
                     service_id, g_strerror (errno));
        return FALSE;
    }

    if (lookup.args.num_entries_found < 1) {
        g_set_error (error, QMI_CORE_ERROR, QMI_CORE_ERROR_UNSUPPORTED,
                     "QMI service %u not found via IPC Router", service_id);
        return FALSE;
    }

    *out_node_id = lookup.info.node_id;
    *out_port_id = lookup.info.port_id;

    g_debug ("[msmipc] Service %u found at node=%u port=%u",
             service_id, *out_node_id, *out_port_id);
    return TRUE;
}

/* ── Client creation ──────────────────────────────────────────────────── */

static ClientInfo *
client_info_new (QmiEndpointMsmIpc  *self,
                 QmiService          service,
                 GError            **error)
{
    ClientInfo        *client_info;
    ClientRecvContext *recv_ctx;
    GList             *l;
    guint              max_cid = 0;
    guint              min_available_cid = 1;
    guint              cid = 0;
    uint32_t           node_id, port_id;
    int                fd;
    struct sockaddr_msm_ipc addr;

    /* Assign CID (same logic as qmi-endpoint-qrtr.c) */
    for (l = self->priv->clients; l; l = g_list_next (l)) {
        ClientInfo *ci = l->data;
        if (service != ci->service)
            continue;
        max_cid = ci->cid;
        if (min_available_cid == ci->cid)
            min_available_cid++;
    }

    cid = max_cid + 1;
    if (cid > G_MAXUINT8) {
        cid = min_available_cid;
        if (min_available_cid > G_MAXUINT8) {
            g_set_error (error, QMI_PROTOCOL_ERROR, QMI_PROTOCOL_ERROR_CLIENT_IDS_EXHAUSTED,
                         "Client IDs exhausted for service %s",
                         qmi_service_get_string (service));
            return NULL;
        }
    }

    /* Look up service port */
    if (!lookup_service_port (service, &node_id, &port_id, error))
        return NULL;

    /* Create socket and connect to service */
    fd = socket (AF_MSM_IPC, SOCK_DGRAM, 0);
    if (fd < 0) {
        g_set_error (error, QMI_CORE_ERROR, QMI_CORE_ERROR_FAILED,
                     "Failed to create AF_MSM_IPC socket: %s", g_strerror (errno));
        return NULL;
    }

    memset (&addr, 0, sizeof (addr));
    addr.family = AF_MSM_IPC;
    addr.address.addrtype = MSM_IPC_ADDR_ID;
    addr.address.addr.port_addr.node_id = node_id;
    addr.address.addr.port_addr.port_id = port_id;

    if (connect (fd, (struct sockaddr *)&addr, sizeof (addr)) < 0) {
        g_set_error (error, QMI_CORE_ERROR, QMI_CORE_ERROR_FAILED,
                     "Failed to connect to service %s at node=%u port=%u: %s",
                     qmi_service_get_string (service), node_id, port_id,
                     g_strerror (errno));
        close (fd);
        return NULL;
    }

    /* Create ClientInfo */
    client_info = g_slice_new0 (ClientInfo);
    client_info->service = service;
    client_info->cid = cid;
    client_info->fd = fd;

    /* Set up async receive via GLib fd watch */
    recv_ctx = g_slice_new (ClientRecvContext);
    recv_ctx->self = self;
    recv_ctx->client_info = client_info;

    client_info->fd_source_id = g_unix_fd_add_full (G_PRIORITY_DEFAULT,
                                                     fd,
                                                     G_IO_IN | G_IO_ERR | G_IO_HUP,
                                                     client_recv_cb,
                                                     recv_ctx,
                                                     client_recv_context_free);

    self->priv->clients = g_list_insert_sorted (self->priv->clients,
                                                 client_info,
                                                 (GCompareFunc) client_info_cmp);

    g_debug ("[%s] Created client for service %s (cid=%u, fd=%d, node=%u, port=%u)",
             qmi_endpoint_get_name (QMI_ENDPOINT (self)),
             qmi_service_get_string (service), cid, fd, node_id, port_id);

    return client_info;
}

/* ── CID allocation/release ───────────────────────────────────────────── */

static guint
allocate_client (QmiEndpointMsmIpc  *self,
                 QmiService          service,
                 GError            **error)
{
    ClientInfo *client_info;

    if (!self->priv->endpoint_open) {
        g_set_error (error, QMI_CORE_ERROR, QMI_CORE_ERROR_WRONG_STATE,
                     "Endpoint is not open");
        return 0;
    }

    client_info = client_info_new (self, service, error);
    if (!client_info)
        return 0;

    return client_info->cid;
}

static void
release_client (QmiEndpointMsmIpc *self,
                QmiService         service,
                guint              cid)
{
    ClientInfo *client_info;

    client_info = client_info_lookup (self, service, cid);
    if (!client_info)
        return;

    self->priv->clients = g_list_remove (self->priv->clients, client_info);
    client_info_free (client_info);
}

/* ── CTL message handling (copied from qmi-endpoint-qrtr.c) ──────────── */

static gboolean
construct_alloc_tlv (QmiMessage *message,
                     QmiService  service,
                     guint8      client)
{
    gsize init_offset;

    init_offset = qmi_message_tlv_write_init (message,
                                              QMI_MESSAGE_TLV_ALLOCATION_INFO,
                                              NULL);

    if (qmi_message_get_message_id (message) == QMI_MESSAGE_CTL_ALLOCATE_CID ||
        qmi_message_get_message_id (message) == QMI_MESSAGE_CTL_RELEASE_CID) {
        g_assert (service <= G_MAXUINT8);
        return init_offset &&
                qmi_message_tlv_write_guint8 (message, service, NULL) &&
                qmi_message_tlv_write_guint8 (message, client, NULL) &&
                qmi_message_tlv_write_complete (message, init_offset, NULL);
    }

    if (qmi_message_get_message_id (message) == QMI_MESSAGE_CTL_INTERNAL_ALLOCATE_CID_QRTR ||
        qmi_message_get_message_id (message) == QMI_MESSAGE_CTL_INTERNAL_RELEASE_CID_QRTR) {
        g_assert (service <= G_MAXUINT16);
        return init_offset &&
                qmi_message_tlv_write_guint16 (message, QMI_ENDIAN_LITTLE, service, NULL) &&
                qmi_message_tlv_write_guint8 (message, client, NULL) &&
                qmi_message_tlv_write_complete (message, init_offset, NULL);
    }

    g_assert_not_reached ();
}

static void
reply_protocol_error (QmiEndpointMsmIpc *self,
                      QmiMessage        *message,
                      QmiProtocolError   error)
{
    QmiMessage *response;

    response = qmi_message_response_new (message, error);
    if (response)
        add_qmi_message_to_buffer (self, response);
}

static void
handle_alloc_cid (QmiEndpointMsmIpc *self,
                  QmiMessage        *message)
{
    gsize                 offset = 0;
    gsize                 init_offset;
    QmiService            service = QMI_SERVICE_UNKNOWN;
    guint                 cid;
    g_autoptr(QmiMessage) response = NULL;
    g_autoptr(GError)     error = NULL;

    if ((init_offset = qmi_message_tlv_read_init (message, QMI_MESSAGE_TLV_ALLOCATION_INFO, NULL, &error)) == 0) {
        g_debug ("[%s] error allocating CID: %s",
                 qmi_endpoint_get_name (QMI_ENDPOINT (self)), error->message);
        reply_protocol_error (self, message, QMI_PROTOCOL_ERROR_MALFORMED_MESSAGE);
        return;
    }

    if (qmi_message_get_message_id (message) == QMI_MESSAGE_CTL_ALLOCATE_CID) {
        guint8 service_tmp;
        if (!qmi_message_tlv_read_guint8 (message, init_offset, &offset, &service_tmp, &error)) {
            reply_protocol_error (self, message, QMI_PROTOCOL_ERROR_MALFORMED_MESSAGE);
            return;
        }
        service = (QmiService) service_tmp;
    } else if (qmi_message_get_message_id (message) == QMI_MESSAGE_CTL_INTERNAL_ALLOCATE_CID_QRTR) {
        guint16 service_tmp;
        if (!qmi_message_tlv_read_guint16 (message, init_offset, &offset, QMI_ENDIAN_LITTLE, &service_tmp, &error)) {
            reply_protocol_error (self, message, QMI_PROTOCOL_ERROR_MALFORMED_MESSAGE);
            return;
        }
        service = (QmiService) service_tmp;
    } else
        g_assert_not_reached ();

    cid = allocate_client (self, service, &error);
    if (!cid) {
        g_debug ("[%s] error allocating CID: %s",
                 qmi_endpoint_get_name (QMI_ENDPOINT (self)), error->message);
        reply_protocol_error (self, message, QMI_PROTOCOL_ERROR_INTERNAL);
        return;
    }

    response = qmi_message_response_new (message, QMI_PROTOCOL_ERROR_NONE);
    if (!response)
        return;

    if (!construct_alloc_tlv (response, service, cid))
        return;

    add_qmi_message_to_buffer (self, g_steal_pointer (&response));
}

static void
handle_release_cid (QmiEndpointMsmIpc *self,
                    QmiMessage        *message)
{
    gsize                 offset = 0;
    gsize                 init_offset;
    QmiService            service;
    guint8                cid;
    g_autoptr(QmiMessage) response = NULL;
    g_autoptr(GError)     error = NULL;

    if ((init_offset = qmi_message_tlv_read_init (message, QMI_MESSAGE_TLV_ALLOCATION_INFO, NULL, &error)) == 0) {
        reply_protocol_error (self, message, QMI_PROTOCOL_ERROR_MALFORMED_MESSAGE);
        return;
    }

    if (qmi_message_get_message_id (message) == QMI_MESSAGE_CTL_RELEASE_CID) {
        guint8 service_tmp;
        if (!qmi_message_tlv_read_guint8 (message, init_offset, &offset, &service_tmp, &error)) {
            reply_protocol_error (self, message, QMI_PROTOCOL_ERROR_MALFORMED_MESSAGE);
            return;
        }
        service = (QmiService) service_tmp;
    } else if (qmi_message_get_message_id (message) == QMI_MESSAGE_CTL_INTERNAL_RELEASE_CID_QRTR) {
        guint16 service_tmp;
        if (!qmi_message_tlv_read_guint16 (message, init_offset, &offset, QMI_ENDIAN_LITTLE, &service_tmp, &error)) {
            reply_protocol_error (self, message, QMI_PROTOCOL_ERROR_MALFORMED_MESSAGE);
            return;
        }
        service = (QmiService) service_tmp;
    } else
        g_assert_not_reached ();

    if (!qmi_message_tlv_read_guint8 (message, init_offset, &offset, &cid, &error)) {
        reply_protocol_error (self, message, QMI_PROTOCOL_ERROR_MALFORMED_MESSAGE);
        return;
    }

    release_client (self, service, cid);

    response = qmi_message_response_new (message, QMI_PROTOCOL_ERROR_NONE);
    if (!response)
        return;

    if (!construct_alloc_tlv (response, service, cid))
        return;

    add_qmi_message_to_buffer (self, g_steal_pointer (&response));
}

static void
handle_ctl_message (QmiEndpointMsmIpc *self,
                    QmiMessage        *message)
{
    switch (qmi_message_get_message_id (message)) {
    case QMI_MESSAGE_CTL_ALLOCATE_CID:
    case QMI_MESSAGE_CTL_INTERNAL_ALLOCATE_CID_QRTR:
        handle_alloc_cid (self, message);
        break;
    case QMI_MESSAGE_CTL_RELEASE_CID:
    case QMI_MESSAGE_CTL_INTERNAL_RELEASE_CID_QRTR:
        handle_release_cid (self, message);
        break;
    case QMI_MESSAGE_CTL_SYNC:
        reply_protocol_error (self, message, QMI_PROTOCOL_ERROR_NONE);
        break;
    case QMI_MESSAGE_CTL_GET_VERSION_INFO:
    default:
        reply_protocol_error (self, message, QMI_PROTOCOL_ERROR_NOT_SUPPORTED);
        break;
    }
}

/* ── Endpoint virtual methods ─────────────────────────────────────────── */

static gboolean
endpoint_open_finish (QmiEndpoint   *self,
                      GAsyncResult  *res,
                      GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
endpoint_open (QmiEndpoint         *endpoint,
               gboolean             use_proxy,
               guint                timeout,
               GCancellable        *cancellable,
               GAsyncReadyCallback  callback,
               gpointer             user_data)
{
    QmiEndpointMsmIpc *self = QMI_ENDPOINT_MSMIPC (endpoint);
    GTask             *task;

    g_assert (!use_proxy);

    task = g_task_new (self, cancellable, callback, user_data);

    if (self->priv->endpoint_open) {
        g_task_return_new_error (task, QMI_CORE_ERROR, QMI_CORE_ERROR_WRONG_STATE,
                                "Already open");
        g_object_unref (task);
        return;
    }

    /* Verify AF_MSM_IPC is available */
    {
        int test_fd = socket (AF_MSM_IPC, SOCK_DGRAM, 0);
        if (test_fd < 0) {
            g_task_return_new_error (task, QMI_CORE_ERROR, QMI_CORE_ERROR_FAILED,
                                    "AF_MSM_IPC not available: %s", g_strerror (errno));
            g_object_unref (task);
            return;
        }
        close (test_fd);
    }

    g_assert (self->priv->clients == NULL);
    self->priv->endpoint_open = TRUE;

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static gboolean
endpoint_is_open (QmiEndpoint *self)
{
    return QMI_ENDPOINT_MSMIPC (self)->priv->endpoint_open;
}

static gboolean
endpoint_send (QmiEndpoint   *endpoint,
               QmiMessage    *message,
               guint          timeout,
               GCancellable  *cancellable,
               GError       **error)
{
    QmiEndpointMsmIpc     *self = QMI_ENDPOINT_MSMIPC (endpoint);
    ClientInfo            *client_info;
    QmiService             service;
    guint                  cid;
    gconstpointer          raw_message;
    gsize                  raw_message_len;
    ssize_t                n;

    service = qmi_message_get_service (message);

    /* CTL service handled locally */
    if (service == QMI_SERVICE_CTL) {
        handle_ctl_message (self, message);
        return TRUE;
    }

    cid = qmi_message_get_client_id (message);
    client_info = client_info_lookup (self, service, cid);
    if (!client_info) {
        g_set_error (error, QMI_CORE_ERROR, QMI_CORE_ERROR_WRONG_STATE,
                     "Unknown client %u for service %s",
                     cid, qmi_service_get_string (service));
        return FALSE;
    }

    /* Get raw QMI message (without QMUX header) */
    raw_message = qmi_message_get_data (message, &raw_message_len, error);
    if (!raw_message) {
        g_prefix_error (error, "Invalid QMI message: ");
        return FALSE;
    }

    n = send (client_info->fd, raw_message, raw_message_len, 0);
    if (n < 0) {
        g_set_error (error, QMI_CORE_ERROR, QMI_CORE_ERROR_FAILED,
                     "Failed to send to service %s: %s",
                     qmi_service_get_string (service), g_strerror (errno));
        return FALSE;
    }

    return TRUE;
}

static void
internal_close (QmiEndpointMsmIpc *self)
{
    g_clear_pointer (&self->priv->clients, client_info_list_free);
    self->priv->endpoint_open = FALSE;
}

static gboolean
endpoint_close_finish (QmiEndpoint   *self,
                       GAsyncResult  *res,
                       GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
endpoint_close (QmiEndpoint         *endpoint,
                guint                timeout,
                GCancellable        *cancellable,
                GAsyncReadyCallback  callback,
                gpointer             user_data)
{
    QmiEndpointMsmIpc *self = QMI_ENDPOINT_MSMIPC (endpoint);
    GTask             *task;

    task = g_task_new (self, cancellable, callback, user_data);
    internal_close (self);
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

/* ── Constructor ──────────────────────────────────────────────────────── */

QmiEndpointMsmIpc *
qmi_endpoint_msmipc_new (void)
{
    QmiEndpointMsmIpc  *self;
    g_autoptr(GFile)    gfile = NULL;
    g_autoptr(QmiFile)  file = NULL;

    gfile = g_file_new_for_uri ("msmipc://0");
    file = qmi_file_new (gfile);

    self = g_object_new (QMI_TYPE_ENDPOINT_MSMIPC,
                         QMI_ENDPOINT_FILE, file,
                         NULL);
    return self;
}

/* ── GObject lifecycle ────────────────────────────────────────────────── */

static void
qmi_endpoint_msmipc_init (QmiEndpointMsmIpc *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE ((self),
                                              QMI_TYPE_ENDPOINT_MSMIPC,
                                              QmiEndpointMsmIpcPrivate);
}

static void
dispose (GObject *object)
{
    QmiEndpointMsmIpc *self = QMI_ENDPOINT_MSMIPC (object);

    internal_close (self);

    G_OBJECT_CLASS (qmi_endpoint_msmipc_parent_class)->dispose (object);
}

static void
qmi_endpoint_msmipc_class_init (QmiEndpointMsmIpcClass *klass)
{
    GObjectClass     *object_class   = G_OBJECT_CLASS (klass);
    QmiEndpointClass *endpoint_class = QMI_ENDPOINT_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (QmiEndpointMsmIpcPrivate));

    object_class->dispose = dispose;

    endpoint_class->open         = endpoint_open;
    endpoint_class->open_finish  = endpoint_open_finish;
    endpoint_class->is_open      = endpoint_is_open;
    endpoint_class->send         = endpoint_send;
    endpoint_class->close        = endpoint_close;
    endpoint_class->close_finish = endpoint_close_finish;
}
