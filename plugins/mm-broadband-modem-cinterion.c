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
 * Copyright (C) 2011 Ammonit Measurement GmbH
 * Copyright (C) 2011 Google Inc.
 * Author: Aleksander Morgado <aleksander@lanedo.com>
 */

#include <config.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#include "ModemManager.h"
#include "mm-modem-helpers.h"
#include "mm-serial-parsers.h"
#include "mm-log.h"
#include "mm-errors-types.h"
#include "mm-iface-modem.h"
#include "mm-iface-modem-3gpp.h"
#include "mm-base-modem-at.h"
#include "mm-broadband-modem-cinterion.h"

static void iface_modem_init      (MMIfaceModem *iface);
static void iface_modem_3gpp_init (MMIfaceModem3gpp *iface);

G_DEFINE_TYPE_EXTENDED (MMBroadbandModemCinterion, mm_broadband_modem_cinterion, MM_TYPE_BROADBAND_MODEM, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM, iface_modem_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_3GPP, iface_modem_3gpp_init));

struct _MMBroadbandModemCinterionPrivate {
    /* Flag to know if we should try AT^SIND or not to get psinfo */
    gboolean sind_psinfo;

    /* Command to go into sleep mode */
    gchar *sleep_mode_cmd;

    /* Supported networks */
    gboolean only_geran;
    gboolean only_utran;
    gboolean both_geran_utran;
};

/*****************************************************************************/
/* Unsolicited events enabling */

static gboolean
enable_unsolicited_events_finish (MMIfaceModem3gpp *self,
                                  GAsyncResult *res,
                                  GError **error)
{
    return !!mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, error);
}

static void
enable_unsolicited_events (MMIfaceModem3gpp *self,
                           GAsyncReadyCallback callback,
                           gpointer user_data)
{
    mm_base_modem_at_command_in_port (
        MM_BASE_MODEM (self),
        /* Only primary port is expected in the Cinterion modems */
        mm_base_modem_get_port_primary (MM_BASE_MODEM (self)),
        /* AT=CMER=[<mode>[,<keyp>[,<disp>[,<ind>[,<bfr>]]]]]
         *  but <ind> should be either not set, or equal to 0 or 2.
         * Enabled with 2.
         */
        "+CMER=3,0,0,2",
        3,
        FALSE,
        NULL, /* cancellable */
        callback,
        user_data);
}

/*****************************************************************************/
/* MODEM POWER DOWN */

static gboolean
modem_power_down_finish (MMIfaceModem *self,
                         GAsyncResult *res,
                         GError **error)
{
    /* Ignore errors */
    return TRUE;
}

static void
send_sleep_mode_command (MMBroadbandModemCinterion *self,
                         GSimpleAsyncResult *operation_result)
{
    if (self->priv->sleep_mode_cmd &&
        self->priv->sleep_mode_cmd[0])
        mm_base_modem_at_command_ignore_reply (MM_BASE_MODEM (self),
                                               self->priv->sleep_mode_cmd,
                                               5);
}

static void
supported_functionality_status_query_ready (MMBroadbandModemCinterion *self,
                                            GAsyncResult *res,
                                            GSimpleAsyncResult *operation_result)
{
    const gchar *response;
    GError *error = NULL;

    g_assert (self->priv->sleep_mode_cmd == NULL);

    response = mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, &error);
    if (!response) {
        mm_warn ("Couldn't query supported functionality status: '%s'",
                 error->message);
        g_error_free (error);
        self->priv->sleep_mode_cmd = g_strdup ("");
    } else {
        /* We need to get which power-off command to use to put the modem in low
         * power mode (with serial port open for AT commands, but with RF switched
         * off). According to the documentation of various Cinterion modems, some
         * support AT+CFUN=4 (HC25) and those which don't support it can use
         * AT+CFUN=7 (CYCLIC SLEEP mode with 2s timeout after last character
         * received in the serial port).
         *
         * So, just look for '4' in the reply; if not found, look for '7', and if
         * not found, report warning and don't use any.
         */
        if (strstr (response, "4") != NULL) {
            mm_dbg ("Device supports CFUN=4 sleep mode");
            self->priv->sleep_mode_cmd = g_strdup ("+CFUN=4");
        } else if (strstr (response, "7") != NULL) {
            mm_dbg ("Device supports CFUN=7 sleep mode");
            self->priv->sleep_mode_cmd = g_strdup ("+CFUN=7");
        } else {
            mm_warn ("Unknown functionality mode to go into sleep mode");
            self->priv->sleep_mode_cmd = g_strdup ("");
        }
    }

    send_sleep_mode_command (self, operation_result);
}

static void
modem_power_down (MMIfaceModem *self,
                  GAsyncReadyCallback callback,
                  gpointer user_data)
{
    MMBroadbandModemCinterion *cinterion = MM_BROADBAND_MODEM_CINTERION (self);
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        modem_power_down);

    /* If sleep command already decided, use it. */
    if (cinterion->priv->sleep_mode_cmd)
        send_sleep_mode_command (MM_BROADBAND_MODEM_CINTERION (self),
                                 result);
    else
        mm_base_modem_at_command (
            MM_BASE_MODEM (self),
            "+CFUN=?",
            3,
            FALSE,
            NULL, /* cancellable */
            (GAsyncReadyCallback)supported_functionality_status_query_ready,
            result);
}

/*****************************************************************************/
/* ACCESS TECHNOLOGIES */

static gboolean
load_access_technologies_finish (MMIfaceModem *self,
                                 GAsyncResult *res,
                                 MMModemAccessTechnology *access_technologies,
                                 guint *mask,
                                 GError **error)
{
    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return FALSE;

    *access_technologies = (MMModemAccessTechnology) GPOINTER_TO_UINT (
        g_simple_async_result_get_op_res_gpointer (
            G_SIMPLE_ASYNC_RESULT (res)));
    *mask = MM_MODEM_ACCESS_TECHNOLOGY_ANY;
    return TRUE;
}

static MMModemAccessTechnology
get_access_technology_from_smong_gprs_status (const gchar *gprs_status,
                                              GError **error)
{
    if (strlen (gprs_status) == 1) {
        switch (gprs_status[0]) {
        case '0':
            return MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
        case '1':
        case '2':
            return MM_MODEM_ACCESS_TECHNOLOGY_GPRS;
        case '3':
        case '4':
            return MM_MODEM_ACCESS_TECHNOLOGY_EDGE;
        default:
            break;
        }
    }

    g_set_error (error,
                 MM_CORE_ERROR,
                 MM_CORE_ERROR_INVALID_ARGS,
                 "Couldn't get network capabilities, "
                 "invalid GPRS status value: '%s'",
                 gprs_status);
    return MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
}

static void
smong_query_ready (MMBroadbandModemCinterion *self,
                   GAsyncResult *res,
                   GSimpleAsyncResult *operation_result)
{
    const gchar *response;
    GError *error = NULL;
    GMatchInfo *match_info = NULL;
    GRegex *regex;

    response = mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, &error);
    if (!response) {
        /* Let the error be critical. */
        g_simple_async_result_take_error (operation_result, error);
        g_simple_async_result_complete (operation_result);
        g_object_unref (operation_result);
        return;
    }

    /* The AT^SMONG command returns a cell info table, where the second
     * column identifies the "GPRS status", which is exactly what we want.
     * So we'll try to read that second number in the values row.
     *
     * AT^SMONG
     * GPRS Monitor
     * BCCH  G  PBCCH  PAT MCC  MNC  NOM  TA      RAC    # Cell #
     * 0776  1  -      -   214   03  2    00      01
     * OK
     */
    regex = g_regex_new (".*GPRS Monitor\\r\\n"
                         "BCCH\\s*G.*\\r\\n"
                         "(\\d*)\\s*(\\d*)\\s*", 0, 0, NULL);
    if (g_regex_match_full (regex, response, strlen (response), 0, 0, &match_info, NULL)) {
        gchar *gprs_status;
        MMModemAccessTechnology act;

        gprs_status = g_match_info_fetch (match_info, 2);
        act = get_access_technology_from_smong_gprs_status (gprs_status, &error);
        g_free (gprs_status);

        if (error)
            g_simple_async_result_take_error (operation_result, error);
        else {
            /* We'll default to use SMONG then */
            self->priv->sind_psinfo = FALSE;
            g_simple_async_result_set_op_res_gpointer (operation_result,
                                                       GUINT_TO_POINTER (act),
                                                       NULL);
        }
    } else {
        /* We'll reset here the flag to try to use SIND/psinfo the next time */
        self->priv->sind_psinfo = TRUE;

        g_simple_async_result_set_error (operation_result,
                                         MM_CORE_ERROR,
                                         MM_CORE_ERROR_INVALID_ARGS,
                                         "Couldn't get network capabilities, "
                                         "invalid SMONG reply: '%s'",
                                         response);
    }

    g_match_info_free (match_info);
    g_regex_unref (regex);

    g_simple_async_result_complete (operation_result);
    g_object_unref (operation_result);
}

static MMModemAccessTechnology
get_access_technology_from_psinfo (const gchar *psinfo,
                                   GError **error)
{
    if (strlen (psinfo) == 1) {
        switch (psinfo[0]) {
        case '0':
            return MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
        case '1':
        case '2':
            return MM_MODEM_ACCESS_TECHNOLOGY_GPRS;
        case '3':
        case '4':
            return MM_MODEM_ACCESS_TECHNOLOGY_EDGE;
        case '5':
        case '6':
            return MM_MODEM_ACCESS_TECHNOLOGY_UMTS;
        case '7':
        case '8':
            return MM_MODEM_ACCESS_TECHNOLOGY_HSDPA;
        default:
            break;
        }
    }

    g_set_error (error,
                 MM_CORE_ERROR,
                 MM_CORE_ERROR_INVALID_ARGS,
                 "Couldn't get network capabilities, "
                 "invalid psinfo value: '%s'",
                 psinfo);
    return MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
}

static void
sind_query_ready (MMBroadbandModemCinterion *self,
                  GAsyncResult *res,
                  GSimpleAsyncResult *operation_result)
{
    const gchar *response;
    GError *error = NULL;
    GMatchInfo *match_info = NULL;
    GRegex *regex;

    response = mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, &error);
    if (!response) {
        /* Let the error be critical. */
        g_simple_async_result_take_error (operation_result, error);
        g_simple_async_result_complete (operation_result);
        g_object_unref (operation_result);
        return;
    }

    /* The AT^SIND? command replies a list of several different indicators.
     * We will only look for 'psinfo' which is the one which may tell us
     * the available network access technology. Note that only 3G-enabled
     * devices seem to have this indicator.
     *
     * AT+SIND?
     * ^SIND: battchg,1,1
     * ^SIND: signal,1,99
     * ...
     */
    regex = g_regex_new ("\\r\\n\\^SIND:\\s*psinfo,\\s*(\\d*),\\s*(\\d*)", 0, 0, NULL);
    if (g_regex_match_full (regex, response, strlen (response), 0, 0, &match_info, NULL)) {
        MMModemAccessTechnology act;
        gchar *ind_value;

        ind_value = g_match_info_fetch (match_info, 2);
        act = get_access_technology_from_psinfo (ind_value, &error);
        g_free (ind_value);
        g_simple_async_result_set_op_res_gpointer (operation_result, GUINT_TO_POINTER (act), NULL);
        g_simple_async_result_complete (operation_result);
        g_object_unref (operation_result);
    } else {
        /* If there was no 'psinfo' indicator, we'll try AT^SMONG and read the cell
         * info table. */
        mm_base_modem_at_command (
            MM_BASE_MODEM (self),
            "^SMONG",
            3,
            FALSE,
            NULL, /* cancellable */
            (GAsyncReadyCallback)smong_query_ready,
            operation_result);
    }

    g_match_info_free (match_info);
    g_regex_unref (regex);
}

static void
load_access_technologies (MMIfaceModem *self,
                          GAsyncReadyCallback callback,
                          gpointer user_data)
{
    MMBroadbandModemCinterion *broadband = MM_BROADBAND_MODEM_CINTERION (self);
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        load_access_technologies);

    if (broadband->priv->sind_psinfo) {
        mm_base_modem_at_command (
            MM_BASE_MODEM (self),
            "^SIND?",
            3,
            FALSE,
            NULL, /* cancellable */
            (GAsyncReadyCallback)sind_query_ready,
            result);
        return;
    }

    mm_base_modem_at_command (
        MM_BASE_MODEM (self),
        "^SMONG",
        3,
        FALSE,
        NULL, /* cancellable */
        (GAsyncReadyCallback)smong_query_ready,
        result);
}

/*****************************************************************************/
/* SUPPORTED MODES */

static MMModemMode
load_supported_modes_finish (MMIfaceModem *self,
                             GAsyncResult *res,
                             GError **error)
{
    if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error))
        return MM_MODEM_MODE_NONE;

    return (MMModemMode) GPOINTER_TO_UINT (g_simple_async_result_get_op_res_gpointer (
                                               G_SIMPLE_ASYNC_RESULT (res)));
}

static void
supported_networks_query_ready (MMBroadbandModemCinterion *self,
                                GAsyncResult *res,
                                GSimpleAsyncResult *operation_result)
{
    const gchar *response;
    GError *error = NULL;
    MMModemMode mode;

    response = mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, &error);
    if (!response) {
        /* Let the error be critical. */
        g_simple_async_result_take_error (operation_result, error);
        g_simple_async_result_complete (operation_result);
        g_object_unref (operation_result);
        return;
    }

    /* Note: Documentation says that AT+WS46=? is replied with '+WS46:' followed
     * by a list of supported network modes between parenthesis, but the EGS5
     * used to test this didn't use the 'WS46:' prefix. Also, more than one
     * numeric ID may appear in the list, that's why they are checked
     * separately. */

    mode = MM_MODEM_MODE_NONE;

    if (strstr (response, "12") != NULL) {
        mm_dbg ("Device allows 2G-only network mode");
        self->priv->only_geran = TRUE;
        mode |= MM_MODEM_MODE_2G;
    }

    if (strstr (response, "22") != NULL) {
        mm_dbg ("Device allows 3G-only network mode");
        self->priv->only_utran = TRUE;
        mode |= MM_MODEM_MODE_3G;
    }

    if (strstr (response, "25") != NULL) {
        mm_dbg ("Device allows 2G/3G network mode");
        self->priv->both_geran_utran = TRUE;
        mode |= (MM_MODEM_MODE_2G | MM_MODEM_MODE_3G);
    }

    /* If no expected ID found, error */
    if (mode == MM_MODEM_MODE_NONE)
        g_simple_async_result_set_error (operation_result,
                                         MM_CORE_ERROR,
                                         MM_CORE_ERROR_FAILED,
                                         "Invalid list of supported networks: '%s'",
                                         response);
    else
        g_simple_async_result_set_op_res_gpointer (operation_result,
                                                   GUINT_TO_POINTER (mode),
                                                   NULL);

    g_simple_async_result_complete (operation_result);
    g_object_unref (operation_result);
}

static void
load_supported_modes (MMIfaceModem *self,
                      GAsyncReadyCallback callback,
                      gpointer user_data)
{
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        load_supported_modes);

    mm_base_modem_at_command (
        MM_BASE_MODEM (self),
        "+WS46=?",
        3,
        FALSE,
        NULL, /* cancellable */
        (GAsyncReadyCallback)supported_networks_query_ready,
        result);
}

/*****************************************************************************/
/* FLOW CONTROL */

static gboolean
setup_flow_control_finish (MMIfaceModem *self,
                           GAsyncResult *res,
                           GError **error)
{
    return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res), error);
}

static void
setup_flow_control_ready (MMBroadbandModemCinterion *self,
                          GAsyncResult *res,
                          GSimpleAsyncResult *operation_result)
{
    GError *error = NULL;

    if (!mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, &error))
        /* Let the error be critical. We DO need RTS/CTS in order to have
         * proper modem disabling. */
        g_simple_async_result_take_error (operation_result, error);
    else
        g_simple_async_result_set_op_res_gboolean (operation_result, TRUE);

    g_simple_async_result_complete (operation_result);
    g_object_unref (operation_result);
}

static void
setup_flow_control (MMIfaceModem *self,
                    GAsyncReadyCallback callback,
                    gpointer user_data)
{
    GSimpleAsyncResult *result;

    result = g_simple_async_result_new (G_OBJECT (self),
                                        callback,
                                        user_data,
                                        setup_flow_control);

    /* We need to enable RTS/CTS so that CYCLIC SLEEP mode works */
    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              "\\Q3",
                              3,
                              FALSE,
                              NULL, /* cancellable */
                              (GAsyncReadyCallback)setup_flow_control_ready,
                              result);
}

/*****************************************************************************/

MMBroadbandModemCinterion *
mm_broadband_modem_cinterion_new (const gchar *device,
                                  const gchar *driver,
                                  const gchar *plugin,
                                  guint16 vendor_id,
                                  guint16 product_id)
{
    return g_object_new (MM_TYPE_BROADBAND_MODEM_CINTERION,
                         MM_BASE_MODEM_DEVICE, device,
                         MM_BASE_MODEM_DRIVER, driver,
                         MM_BASE_MODEM_PLUGIN, plugin,
                         MM_BASE_MODEM_VENDOR_ID, vendor_id,
                         MM_BASE_MODEM_PRODUCT_ID, product_id,
                         NULL);
}

static void
mm_broadband_modem_cinterion_init (MMBroadbandModemCinterion *self)
{
    /* Initialize private data */
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE ((self),
                                              MM_TYPE_BROADBAND_MODEM_CINTERION,
                                              MMBroadbandModemCinterionPrivate);

    /* Set defaults */
    self->priv->sind_psinfo = TRUE; /* Initially, always try to get psinfo */
}

static void
finalize (GObject *object)
{
    MMBroadbandModemCinterion *self = MM_BROADBAND_MODEM_CINTERION (object);

    g_free (self->priv->sleep_mode_cmd);

    G_OBJECT_CLASS (mm_broadband_modem_cinterion_parent_class)->finalize (object);
}

static void
iface_modem_init (MMIfaceModem *iface)
{
    iface->load_supported_modes = load_supported_modes;
    iface->load_supported_modes_finish = load_supported_modes_finish;
    iface->load_access_technologies = load_access_technologies;
    iface->load_access_technologies_finish = load_access_technologies_finish;
    iface->setup_flow_control = setup_flow_control;
    iface->setup_flow_control_finish = setup_flow_control_finish;
    iface->modem_power_down = modem_power_down;
    iface->modem_power_down_finish = modem_power_down_finish;
}

static void
iface_modem_3gpp_init (MMIfaceModem3gpp *iface)
{
    iface->enable_unsolicited_events = enable_unsolicited_events;
    iface->enable_unsolicited_events_finish = enable_unsolicited_events_finish;
}

static void
mm_broadband_modem_cinterion_class_init (MMBroadbandModemCinterionClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMBroadbandModemCinterionPrivate));

    /* Virtual methods */
    object_class->finalize = finalize;
}
