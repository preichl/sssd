/*
    Authors:
        Pavel Březina <pbrezina@redhat.com>

    Copyright (C) 2016 Red Hat

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "providers/data_provider/dp_iface_generated.h"
#include "providers/data_provider/dp_iface.h"
#include "sbus/sssd_dbus.h"
#include "util/util.h"

static void rdp_register_client_done(DBusPendingCall *pending, void *ptr)
{
    DBusError dbus_error;
    DBusMessage *reply;
    dbus_bool_t bret;
    int type;

    dbus_error_init(&dbus_error);

    reply = dbus_pending_call_steal_reply(pending);
    if (reply == NULL) {
        DEBUG(SSSDBG_FATAL_FAILURE, "Severe error. A reply callback was "
              "called but no reply was received and no timeout occurred\n");
        goto done;
    }

    type = dbus_message_get_type(reply);
    switch (type) {
    case DBUS_MESSAGE_TYPE_METHOD_RETURN:
        bret = dbus_message_get_args(reply, &dbus_error, DBUS_TYPE_INVALID);
        if (!bret) {
            DEBUG(SSSDBG_CRIT_FAILURE, "Failed to parse message\n");
            if (dbus_error_is_set(&dbus_error)) {
                dbus_error_free(&dbus_error);
            }
            goto done;
        }

        DEBUG(SSSDBG_CONF_SETTINGS, "Client is registered with DP\n");
        break;

    case DBUS_MESSAGE_TYPE_ERROR:
        DEBUG(SSSDBG_FATAL_FAILURE, "Data provider returned an error [%s]\n",
              dbus_message_get_error_name(reply));
        break;
    default:
        break;
    }

done:
    dbus_pending_call_unref(pending);
    dbus_message_unref(reply);
}

errno_t rdp_register_client(struct sbus_connection *conn,
                            const char *client_name)
{
    DBusMessage *msg;
    dbus_bool_t bret;
    errno_t ret;

    msg = dbus_message_new_method_call(NULL, DP_PATH, IFACE_DP_CLIENT,
                                       IFACE_DP_CLIENT_REGISTER);
    if (msg == NULL) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Unable to create message\n");
        return ENOMEM;
    }

    DEBUG(SSSDBG_CONF_SETTINGS, "Registering [%s] with data provider\n",
          client_name);

    bret = dbus_message_append_args(msg,
                                    DBUS_TYPE_STRING, &client_name,
                                    DBUS_TYPE_INVALID);
    if (!bret) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Failed to build message\n");
        ret = EIO;
        goto done;
    }

    ret = sbus_conn_send(conn, msg, 30000, rdp_register_client_done,
                         NULL, NULL);

done:
    dbus_message_unref(msg);
    return ret;
}
