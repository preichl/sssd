/*
    Authors:
        Jan Cholasta <jcholast@redhat.com>

    Copyright (C) 2012 Red Hat

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

#include "util/util.h"
#include "util/crypto/sss_crypto.h"
#include "db/sysdb_ssh.h"
#include "providers/ldap/ldap_common.h"
#include "providers/ipa/ipa_common.h"
#include "providers/ipa/ipa_hostid.h"
#include "providers/ipa/ipa_hosts.h"

struct hosts_get_state {
    struct tevent_context *ev;
    struct ipa_hostid_ctx *ctx;
    struct sdap_id_op *op;
    struct sss_domain_info *domain;
    const char *name;
    const char *alias;

    size_t count;
    struct sysdb_attrs **hosts;
    int dp_error;
};

static errno_t
hosts_get_retry(struct tevent_req *req);
static void
hosts_get_connect_done(struct tevent_req *subreq);
static void
hosts_get_done(struct tevent_req *subreq);

struct tevent_req *
hosts_get_send(TALLOC_CTX *memctx,
               struct tevent_context *ev,
               struct ipa_hostid_ctx *hostid_ctx,
               const char *name,
               const char *alias)
{
    struct tevent_req *req;
    struct hosts_get_state *state;
    struct sdap_id_ctx *ctx;
    errno_t ret;

    ctx = hostid_ctx->sdap_id_ctx;

    req = tevent_req_create(memctx, &state, struct hosts_get_state);
    if (!req) return NULL;

    state->ev = ev;
    state->ctx = hostid_ctx;
    state->dp_error = DP_ERR_FATAL;

    state->op = sdap_id_op_create(state, ctx->conn->conn_cache);
    if (!state->op) {
        DEBUG(SSSDBG_OP_FAILURE, "sdap_id_op_create failed\n");
        ret = ENOMEM;
        goto fail;
    }

    state->domain = ctx->be->domain;
    state->name = name;
    state->alias = alias;

    ret = hosts_get_retry(req);
    if (ret != EOK) {
        goto fail;
    }

    return req;

fail:
    tevent_req_error(req, ret);
    tevent_req_post(req, ev);
    return req;
}

static errno_t
hosts_get_retry(struct tevent_req *req)
{
    struct hosts_get_state *state = tevent_req_data(req,
                                                    struct hosts_get_state);
    struct tevent_req *subreq;
    errno_t ret = EOK;

    subreq = sdap_id_op_connect_send(state->op, state, &ret);
    if (!subreq) {
        return ret;
    }

    tevent_req_set_callback(subreq, hosts_get_connect_done, req);
    return EOK;
}

static void
hosts_get_connect_done(struct tevent_req *subreq)
{
    struct tevent_req *req = tevent_req_callback_data(subreq,
                                                      struct tevent_req);
    struct hosts_get_state *state = tevent_req_data(req,
                                                    struct hosts_get_state);
    int dp_error = DP_ERR_FATAL;
    errno_t ret;

    ret = sdap_id_op_connect_recv(subreq, &dp_error);
    talloc_zfree(subreq);

    if (ret != EOK) {
        state->dp_error = dp_error;
        tevent_req_error(req, ret);
        return;
    }

    subreq = ipa_host_info_send(state, state->ev,
                                sdap_id_op_handle(state->op),
                                state->ctx->sdap_id_ctx->opts, state->name,
                                state->ctx->ipa_opts->host_map, NULL,
                                state->ctx->host_search_bases);
    if (!subreq) {
        tevent_req_error(req, ENOMEM);
        return;
    }
    tevent_req_set_callback(subreq, hosts_get_done, req);
}

static void
hosts_get_done(struct tevent_req *subreq)
{
    struct tevent_req *req = tevent_req_callback_data(subreq,
                                                      struct tevent_req);
    struct hosts_get_state *state = tevent_req_data(req,
                                                    struct hosts_get_state);
    int dp_error = DP_ERR_FATAL;
    errno_t ret;
    struct sysdb_attrs *attrs;
    time_t now = time(NULL);

    ret = ipa_host_info_recv(subreq, state,
                             &state->count, &state->hosts,
                             NULL, NULL);
    talloc_zfree(subreq);

    ret = sdap_id_op_done(state->op, ret, &dp_error);
    if (dp_error == DP_ERR_OK && ret != EOK) {
        /* retry */
        ret = hosts_get_retry(req);
        if (ret != EOK) {
            goto done;
        }
        return;
    }

    if (ret != EOK && ret != ENOENT) {
        goto done;
    }

    if (state->count == 0) {
        DEBUG(SSSDBG_OP_FAILURE,
              "No host with name [%s] found.\n", state->name);

        ret = sysdb_delete_ssh_host(state->domain, state->name);
        if (ret != EOK && ret != ENOENT) {
            goto done;
        }

        ret = EINVAL;
        goto done;
    }

    if (state->count > 1) {
        DEBUG(SSSDBG_CRIT_FAILURE,
              "Found more than one host with name [%s].\n", state->name);
        ret = EINVAL;
        goto done;
    }

    attrs = sysdb_new_attrs(state);
    if (!attrs) {
        ret = ENOMEM;
        goto done;
    }

    /* we are interested only in the host keys */
    ret = sysdb_attrs_copy_values(state->hosts[0], attrs, SYSDB_SSH_PUBKEY);
    if (ret != EOK) {
        goto done;
    }

    ret = sysdb_store_ssh_host(state->domain, state->name, state->alias,
                               state->domain->ssh_host_timeout, now, attrs);
    if (ret != EOK) {
        goto done;
    }

    dp_error = DP_ERR_OK;

done:
    state->dp_error = dp_error;
    if (ret == EOK) {
        tevent_req_done(req);
    } else {
        tevent_req_error(req, ret);
    }
}

static errno_t
hosts_get_recv(struct tevent_req *req,
               int *dp_error_out)
{
    struct hosts_get_state *state = tevent_req_data(req,
                                                    struct hosts_get_state);

    if (dp_error_out) {
        *dp_error_out = state->dp_error;
    }

    TEVENT_REQ_RETURN_ON_ERROR(req);

    return EOK;
}

struct ipa_hostid_handler_state {
    struct dp_reply_std reply;
};

static void ipa_hostid_handler_done(struct tevent_req *subreq);

struct tevent_req *
ipa_hostid_handler_send(TALLOC_CTX *mem_ctx,
                       struct ipa_hostid_ctx *hostid_ctx,
                       struct dp_hostid_data *data,
                       struct dp_req_params *params)
{
    struct ipa_hostid_handler_state *state;
    struct tevent_req *subreq;
    struct tevent_req *req;
    errno_t ret;

    req = tevent_req_create(mem_ctx, &state, struct ipa_hostid_handler_state);
    if (req == NULL) {
        DEBUG(SSSDBG_CRIT_FAILURE, ("tevent_req_create() failed\n"));
        return NULL;
    }

    subreq = hosts_get_send(state, params->ev, hostid_ctx,
                            data->name, data->alias);
    if (subreq == NULL) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Unable to send request\n");
        ret = ENOMEM;
        goto immediately;
    }

    tevent_req_set_callback(subreq, ipa_hostid_handler_done, req);

    return req;

immediately:
    if (ret == EOK) {
        tevent_req_done(req);
    } else {
        tevent_req_error(req, ret);
    }
    tevent_req_post(req, params->ev);

    return req;
}

static void ipa_hostid_handler_done(struct tevent_req *subreq)
{
    struct ipa_hostid_handler_state *state;
    struct tevent_req *req;
    int dp_error;
    errno_t ret;

    req = tevent_req_callback_data(subreq, struct tevent_req);
    state = tevent_req_data(req, struct ipa_hostid_handler_state);

    ret = hosts_get_recv(req, &dp_error);

    state->reply.dp_error = dp_error;
    state->reply.error = ret;
    state->reply.message = sss_strerror(ret);

    /* For backward compatibility we always return EOK to DP now. */
    tevent_req_done(req);
}

errno_t
ipa_hostid_handler_recv(TALLOC_CTX *mem_ctx,
                       struct tevent_req *req,
                       struct dp_reply_std *data)
{
    struct ipa_hostid_handler_state *state = NULL;

    state = tevent_req_data(req, struct ipa_hostid_handler_state);

    TEVENT_REQ_RETURN_ON_ERROR(req);

    *data = state->reply;

    return EOK;
}
