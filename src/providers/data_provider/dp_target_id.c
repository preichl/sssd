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

#include <talloc.h>
#include <tevent.h>

#include "sbus/sssd_dbus.h"
#include "providers/data_provider/dp_private.h"
#include "providers/data_provider/dp_iface.h"
#include "providers/backend.h"
#include "util/util.h"

#define FILTER_TYPE(str, type) {#str "=", sizeof(#str "=") - 1, type}

static bool check_attr_type(uint32_t attr_type)
{
    switch (attr_type) {
    case BE_ATTR_CORE:
    case BE_ATTR_MEM:
    case BE_ATTR_ALL:
        return true;
    default:
        return false;
    }

    return false;
}

static bool check_and_parse_filter(struct be_acct_req *data,
                                   const char *filter,
                                   const char *extra)
{
    /* We will use sizeof() to determine the length of a string so we don't
     * call strlen over and over again with each request. Not a bottleneck,
     * but unnecessary and simple to avoid. */
    static struct {
        const char *name;
        size_t lenght;
        uint32_t type;
    } types[] = {FILTER_TYPE("name", BE_FILTER_NAME),
                 FILTER_TYPE("idnumber", BE_FILTER_IDNUM),
                 FILTER_TYPE(DP_SEC_ID, BE_FILTER_SECID),
                 FILTER_TYPE(DP_CERT, BE_FILTER_CERT),
                 FILTER_TYPE(DP_WILDCARD, BE_FILTER_WILDCARD),
                 {0, 0, 0}};
    int i;

    if (filter == NULL) {
        return false;
    }

    for (i = 0; types[i].name != NULL; i++) {
        if (strncmp(filter, types[i].name, types[i].lenght) == 0) {
            data->filter_type = types[i].type;
            data->filter_value = discard_const(&filter[types[i].lenght]); /* todo remove discard const */
            data->extra_value = discard_const(extra); /* todo remove discard const */
            return true;
        }
    }

    if (strcmp(filter, ENUM_INDICATOR) == 0) {
        data->filter_type = BE_FILTER_ENUM;
        data->filter_value = NULL;
        data->extra_value = NULL;
        return true;
    }

    return false;
}

errno_t dp_get_account_info_handler(struct sbus_request *sbus_req,
                                    void *dp_cli,
                                    uint32_t dp_flags,
                                    uint32_t entry_type,
                                    uint32_t attr_type,
                                    const char *filter,
                                    const char *domain,
                                    const char *extra)
{
    struct be_acct_req *data;
    const char *key;

    if (!check_attr_type(attr_type)) {
        return EINVAL;
    }

    data = talloc_zero(sbus_req, struct be_acct_req);
    if (data == NULL) {
        return ENOMEM;
    }

    data->entry_type = entry_type;
    data->attr_type = attr_type;
    data->domain = discard_const(domain); /* todo remove discard const */

    if (!check_and_parse_filter(data, filter, extra)) {
        return EINVAL;
    }

    /* TODO generate key and handle prereq */
    key = NULL;

    dp_req_with_reply(dp_cli, domain, "Account", key, sbus_req, DPT_ID,
                      DPM_ACCOUNT_HANDLER, dp_flags, data,
                      dp_req_reply_std, struct dp_reply_std);

    return ENOTSUP;
}
