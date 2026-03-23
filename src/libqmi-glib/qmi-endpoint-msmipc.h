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
 */

#ifndef _LIBQMI_GLIB_QMI_ENDPOINT_MSMIPC_H_
#define _LIBQMI_GLIB_QMI_ENDPOINT_MSMIPC_H_

#include "qmi-endpoint.h"

#define QMI_TYPE_ENDPOINT_MSMIPC            (qmi_endpoint_msmipc_get_type ())
#define QMI_ENDPOINT_MSMIPC(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), QMI_TYPE_ENDPOINT_MSMIPC, QmiEndpointMsmIpc))
#define QMI_ENDPOINT_MSMIPC_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  QMI_TYPE_ENDPOINT_MSMIPC, QmiEndpointMsmIpcClass))
#define QMI_IS_ENDPOINT_MSMIPC(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), QMI_TYPE_ENDPOINT_MSMIPC))
#define QMI_IS_ENDPOINT_MSMIPC_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  QMI_TYPE_ENDPOINT_MSMIPC))
#define QMI_ENDPOINT_MSMIPC_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  QMI_TYPE_ENDPOINT_MSMIPC, QmiEndpointMsmIpcClass))

typedef struct _QmiEndpointMsmIpc QmiEndpointMsmIpc;
typedef struct _QmiEndpointMsmIpcClass QmiEndpointMsmIpcClass;
typedef struct _QmiEndpointMsmIpcPrivate QmiEndpointMsmIpcPrivate;

struct _QmiEndpointMsmIpc {
    /*< private >*/
    QmiEndpoint parent;
    QmiEndpointMsmIpcPrivate *priv;
};

struct _QmiEndpointMsmIpcClass {
    /*< private >*/
    QmiEndpointClass parent;
};

GType qmi_endpoint_msmipc_get_type (void);

QmiEndpointMsmIpc *qmi_endpoint_msmipc_new (void);

#endif /* _LIBQMI_GLIB_QMI_ENDPOINT_MSMIPC_H_ */
