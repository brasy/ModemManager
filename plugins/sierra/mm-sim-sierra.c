/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details:
 *
 * Copyright (C) 2008 - 2009 Novell, Inc.
 * Copyright (C) 2009 - 2012 Red Hat, Inc.
 * Copyright (C) 2012 Lanedo GmbH
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>

#include <ModemManager.h>
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>
#include "mm-log.h"
#include "mm-modem-helpers.h"
#include "mm-base-modem-at.h"

#include "mm-sim-sierra.h"

G_DEFINE_TYPE (MMSimSierra, mm_sim_sierra, MM_TYPE_SIM);

struct _MMSimSierraPrivate {
    gchar *cached_pin;
};

/*****************************************************************************/
/* Send SIM PIN */

static gboolean
send_pin_finish (MMSim *self,
                 GAsyncResult *res,
                 GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static void
parent_send_pin_ready (MMSim *_self,
                       GAsyncResult *res,
                       GSimpleAsyncResult *simple)
{
    MMSimSierra *self = MM_SIM_SIERRA (_self);
    GError *error = NULL;

    if (!MM_SIM_CLASS (mm_sim_sierra_parent_class)->send_pin_finish (_self, res, &error)) {
        /* Clear cached PIN if sending PIN fails */
        g_free (self->priv->cached_pin);
        self->priv->cached_pin = NULL;
        g_simple_async_result_take_error (simple, error);
    } else
        g_simple_async_result_set_op_res_gboolean (simple, TRUE);

    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
send_pin (MMSim *_self,
          const gchar *pin,
          GAsyncReadyCallback callback,
          gpointer user_data)
{
    MMSimSierra *self = MM_SIM_SIERRA (_self);
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        send_pin);

    /* We allow passing NULL pin, when we want to use the last cached one.
     * This will only happen when re-unlocking the SIM after powering up
     * the modem, so it means that if any PIN was introduced already,
     * it would be the correct one. */
    if (!pin) {
        if (!self->priv->cached_pin) {
            g_simple_async_result_set_error (result,
                                             MM_CORE_ERROR,
                                             MM_CORE_ERROR_NOT_FOUND,
                                             "No cached PIN found");
            g_simple_async_result_complete_in_idle (result);
            g_object_unref (result);
            return;
        }
    } else {
        /* Update cached PIN */
        g_free (self->priv->cached_pin);
        self->priv->cached_pin = g_strdup (pin);
    }

    /* Call parent's PIN sending code */
    MM_SIM_CLASS (mm_sim_sierra_parent_class)->send_pin (_self,
                                                         self->priv->cached_pin,
                                                         (GAsyncReadyCallback)parent_send_pin_ready,
                                                         result);
}

/*****************************************************************************/
/* SIM identifier loading */

static gchar *
load_sim_identifier_finish (MMSim *self,
                            GAsyncResult *res,
                            GError **error)
{
    gchar *iccid;

    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return NULL;

    iccid = g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (res));
    mm_dbg ("loaded SIM identifier: %s", iccid);
    return g_strdup (iccid);
}

static void
iccid_read_ready (MMBaseModem *modem,
                  GAsyncResult *res,
                  GSimpleAsyncResult *simple)
{
    GError *error = NULL;
    const gchar *response;
    const gchar *p;
    gchar buf[21];
    gint i;

    response = mm_base_modem_at_command_finish (modem, res, &error);
    if (!response) {
        g_simple_async_result_take_error (simple, error);
        g_simple_async_result_complete (simple);
        g_object_unref (simple);
        return;
    }

    p = mm_strip_tag (response, "!ICCID:");
    if (!p) {
        g_simple_async_result_set_error (simple,
                                         MM_CORE_ERROR,
                                         MM_CORE_ERROR_FAILED,
                                         "Failed to parse !ICCID response: '%s'",
                                         response);
        g_simple_async_result_complete (simple);
        g_object_unref (simple);
        return;
    }

    memset (buf, 0, sizeof (buf));
    for (i = 0; i < 20; i++) {
        if (!isdigit (p[i]) && (p[i] != 'F') && (p[i] == 'f')) {
            g_simple_async_result_set_error (simple,
                                             MM_CORE_ERROR,
                                             MM_CORE_ERROR_FAILED,
                                             "CRSM ICCID response contained invalid character '%c'",
                                             p[i]);
            g_simple_async_result_complete (simple);
            g_object_unref (simple);
            return;
        }

        if (p[i] == 'F' || p[i] == 'f') {
            buf[i] = 0;
            break;
        }
        buf[i] = p[i];
    }

    if (i != 19 && i != 20)
        g_simple_async_result_set_error (simple,
                                         MM_CORE_ERROR,
                                         MM_CORE_ERROR_FAILED,
                                         "Invalid +CRSM ICCID response size (was %d, expected 19 or 20)",
                                         i);
    else
        g_simple_async_result_set_op_res_gpointer (simple, buf, NULL);
    g_simple_async_result_complete (simple);
    g_object_unref (simple);
}

static void
load_sim_identifier (MMSim *self,
                     GAsyncReadyCallback callback,
                     gpointer user_data)
{
    MMBaseModem *modem = NULL;

    g_object_get (self,
                  MM_SIM_MODEM, &modem,
                  NULL);

    mm_dbg ("loading (Sierra) SIM identifier...");
    mm_base_modem_at_command (
        MM_BASE_MODEM (modem),
        "!ICCID?",
        3,
        FALSE,
        (GAsyncReadyCallback)iccid_read_ready,
        g_simple_async_result_new (G_OBJECT (self),
                                   callback,
                                   user_data,
                                   load_sim_identifier));
    g_object_unref (modem);
}

/*****************************************************************************/

MMSim *
mm_sim_sierra_new_finish (GAsyncResult  *res,
                          GError       **error)
{
    GObject *source;
    GObject *sim;

    source = g_async_result_get_source_object (res);
    sim = g_async_initable_new_finish (G_ASYNC_INITABLE (source), res, error);
    g_object_unref (source);

    if (!sim)
        return NULL;

    /* Only export valid SIMs */
    mm_sim_export (MM_SIM (sim));

    return MM_SIM (sim);
}

void
mm_sim_sierra_new (MMBaseModem *modem,
                   GCancellable *cancellable,
                   GAsyncReadyCallback callback,
                   gpointer user_data)
{
    g_async_initable_new_async (MM_TYPE_SIM_SIERRA,
                                G_PRIORITY_DEFAULT,
                                cancellable,
                                callback,
                                user_data,
                                MM_SIM_MODEM, modem,
                                NULL);
}

static void
mm_sim_sierra_init (MMSimSierra *self)
{
    /* Initialize private data */
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
                                              MM_TYPE_SIM_SIERRA,
                                              MMSimSierraPrivate);
}

static void
finalize (GObject *object)
{
    MMSimSierra *self = MM_SIM_SIERRA (object);

    g_free (self->priv->cached_pin);

    G_OBJECT_CLASS (mm_sim_sierra_parent_class)->finalize (object);
}

static void
mm_sim_sierra_class_init (MMSimSierraClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    MMSimClass *sim_class = MM_SIM_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMSimSierraPrivate));

    object_class->finalize = finalize;

    sim_class->load_sim_identifier = load_sim_identifier;
    sim_class->load_sim_identifier_finish = load_sim_identifier_finish;
    sim_class->send_pin = send_pin;
    sim_class->send_pin_finish = send_pin_finish;
}
