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
#include <dbus/dbus.h>

#include "sbus/sssd_dbus_errors.h"
#include "providers/data_provider/dp_private.h"
#include "providers/backend.h"
#include "util/dlinklist.h"
#include "util/util.h"

struct dp_req {
    struct data_provider *provider;
    struct dp_client *client;
    uint32_t dp_flags;

    struct sss_domain_info *domain;

    enum dp_targets target;
    enum dp_methods method;
    struct dp_method *execute;
    const char *name;
    uint32_t num;

    struct tevent_req *req;
    struct tevent_req *handler_req;
    void *request_data;

    /* Active request list. */
    struct dp_req *prev;
    struct dp_req *next;
};

static bool check_data_type(const char *expected,
                            const char *description,
                            void *ptr)
{
    void *tmp;

    /* If ptr is NULL we still return true since it is valid case. */
    tmp = talloc_check_name(ptr, expected);
    if (tmp != NULL || ptr == NULL) {
        return true;
    }

    DEBUG(SSSDBG_CRIT_FAILURE, "Invalid %s data type provided. Expected [%s], "
          "got [%s].\n", description, expected, talloc_get_name(ptr));

    return false;
}

static bool check_method_data(struct dp_method *method,
                              void *request_data)
{
    if (!check_data_type(method->method_dtype, "method", method->method_data)) {
        return false;
    }

    if (!check_data_type(method->request_dtype, "request", request_data)) {
        return false;
    }

    return true;
}

static int dp_req_destructor(struct dp_req *dp_req)
{
    DLIST_REMOVE(dp_req->provider->requests.active, dp_req);

    if (dp_req->provider->requests.num_active == 0) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Bug: there are no active requests!\n");
        return 0;
    }

    dp_req->provider->requests.num_active--;

    DP_REQ_DEBUG(SSSDBG_TRACE_FUNC, dp_req->name, "Request removed.");

    DEBUG(SSSDBG_TRACE_FUNC, "Number of active DP request: %u\n",
          dp_req->provider->requests.num_active);

    return 0;
}

static struct dp_req *
dp_req_new(TALLOC_CTX *mem_ctx,
           struct data_provider *provider,
           struct dp_client *dp_cli,
           struct sss_domain_info *domain,
           const char *name,
           enum dp_targets target,
           enum dp_methods method,
           uint32_t dp_flags,
           void *request_data,
           struct tevent_req *req)
{
    struct dp_req *dp_req;

    dp_req = talloc_zero(mem_ctx, struct dp_req);
    if (dp_req == NULL) {
        return NULL;
    }

    dp_req->provider = provider;
    dp_req->client = dp_cli;
    dp_req->domain = domain == NULL ? provider->be_ctx->domain : domain;
    dp_req->dp_flags = dp_flags;
    dp_req->target = target;
    dp_req->method = method;
    dp_req->request_data = request_data;
    dp_req->req = req;

    dp_req->execute = dp_find_method(provider, target, method);
    if (dp_req->execute == NULL) {
        goto fail;
    }

    /* If we run out of numbers we simply overflow. */
    dp_req->num = provider->requests.index++;
    dp_req->name = talloc_asprintf(dp_req, "%s #%u", name, dp_req->num);
    if (dp_req->name == NULL) {
        goto fail;
    }

    /* Attach this request to active request list. */
    DLIST_ADD(provider->requests.active, dp_req);
    provider->requests.num_active++;

    talloc_set_destructor(dp_req, dp_req_destructor);

    DP_REQ_DEBUG(SSSDBG_TRACE_FUNC, dp_req->name,
                 "New request. Flags [%#.4x].", dp_flags);

    DEBUG(SSSDBG_TRACE_FUNC, "Number of active DP request: %u\n",
          provider->requests.num_active);

    talloc_steal(dp_req, dp_req->request_data);

    return dp_req;

fail:
    talloc_free(dp_req);
    return NULL;
}

static errno_t
file_dp_request(TALLOC_CTX *mem_ctx,
                struct data_provider *provider,
                struct dp_client *dp_cli,
                struct sss_domain_info *domain,
                const char *name,
                enum dp_targets target,
                enum dp_methods method,
                uint32_t dp_flags,
                void *request_data,
                struct tevent_req *req,
                struct dp_req **_dp_req)
{
    struct dp_req_params *dp_params;
    dp_req_send_fn send_fn;
    struct dp_req *dp_req;
    struct be_ctx *be_ctx;
    errno_t ret;

    be_ctx = provider->be_ctx;

    dp_req = dp_req_new(mem_ctx, provider, dp_cli, domain, name, target,
                        method, dp_flags, request_data, req);

    if (dp_req == NULL) {
        ret = ENOMEM;
        goto done;
    }

    /* Check that provided data are of correct type. */

    if (!check_method_data(dp_req->execute, dp_req->request_data)) {
        ret = ERR_INVALID_DATA_TYPE;
        goto done;
    }

    /* Process data provider flags */

    if (dp_flags & DP_FAST_REPLY && be_is_offline(be_ctx)) {
        ret = ERR_OFFLINE;
        goto done;
    }

    /* File request */

    dp_params = talloc_zero(dp_req, struct dp_req_params);
    if (dp_params == NULL) {
        ret = ENOMEM;
        goto done;
    }

    dp_params->ev = provider->ev;
    dp_params->be_ctx = be_ctx;
    dp_params->domain = dp_req->domain;
    dp_params->target = dp_req->target;
    dp_params->method = dp_req->method;

    send_fn = dp_req->execute->send_fn;
    dp_req->handler_req = send_fn(dp_req, dp_req->execute->method_data,
                                  dp_req->request_data, dp_params);
    if (dp_req->handler_req == NULL) {
        ret = ENOMEM;
        goto done;
    }

    *_dp_req = dp_req;

    ret = EOK;

done:
    if (ret != EOK) {
        talloc_free(dp_req);
    }

    return ret;
}

struct dp_req_state {
    struct dp_req *dp_req;
    dp_req_recv_fn recv_fn;
    void *output_data;
};

static void dp_req_done(struct tevent_req *subreq);

struct tevent_req *dp_req_send(TALLOC_CTX *mem_ctx,
                               struct data_provider *provider,
                               struct dp_client *dp_cli,
                               struct sss_domain_info *domain,
                               const char *name,
                               enum dp_targets target,
                               enum dp_methods method,
                               uint32_t dp_flags,
                               void *request_data,
                               const char **_request_name)
{
    struct dp_req_state *state;
    struct tevent_req *req;
    struct dp_req *dp_req;
    errno_t ret;

    req = tevent_req_create(mem_ctx, &state, struct dp_req_state);
    if (req == NULL) {
        DEBUG(SSSDBG_CRIT_FAILURE, ("tevent_req_create() failed\n"));
        return NULL;
    }

    ret = file_dp_request(state, provider, dp_cli, domain, name, target,
                          method, dp_flags, request_data, req, &dp_req);
    if (ret != EOK) {
        goto immediately;
    }

    state->dp_req = dp_req;
    state->recv_fn = dp_req->execute->recv_fn;
    state->output_data = talloc_zero_size(state, dp_req->execute->output_size);
    if (state->output_data == NULL) {
        ret = ENOMEM;
        goto immediately;
    }

    talloc_set_type(state->output_data, dp_req->execute->output_dtype);

    tevent_req_set_callback(dp_req->handler_req, dp_req_done, dp_req);

    if (_request_name != NULL) {
        *_request_name = dp_req->name;
    }

    return req;

immediately:
    if (ret == EOK) {
        tevent_req_done(req);
    } else {
        tevent_req_error(req, ret);
    }
    tevent_req_post(req, provider->ev);

    return req;
}

static void dp_req_done(struct tevent_req *subreq)
{
    struct dp_req_state *state;
    struct tevent_req *req;
    errno_t ret;

    req = tevent_req_callback_data(subreq, struct tevent_req);
    state = tevent_req_data(req, struct dp_req_state);

    ret = state->recv_fn(state->output_data, subreq, state->output_data);

    /* subreq is the same as dp_req->handler_req */
    talloc_zfree(subreq);
    state->dp_req->handler_req = NULL;

    DP_REQ_DEBUG(SSSDBG_TRACE_FUNC, state->dp_req->name,
                 "Request handler finished [%d]: %s", ret, sss_strerror(ret));

    if (ret != EOK) {
        tevent_req_error(req, ret);
        return;
    }

    tevent_req_done(req);
}

errno_t _dp_req_recv(TALLOC_CTX *mem_ctx,
                     struct tevent_req *req,
                     const char *output_dtype,
                     void **_output_data)
{
    struct dp_req_state *state;

    state = tevent_req_data(req, struct dp_req_state);

    if (state->dp_req != NULL) {
        DP_REQ_DEBUG(SSSDBG_TRACE_FUNC, state->dp_req->name,
                     "Receiving request data.");
    } else {
        /* dp_req may be NULL in case we error when filing request */
        DEBUG(SSSDBG_TRACE_FUNC,
              "Receiving data of prematurely interrupted request!\n");
    }

    TEVENT_REQ_RETURN_ON_ERROR(req);

    if (!check_data_type(output_dtype, "output", state->output_data)) {
        return ERR_INVALID_DATA_TYPE;
    }

    *_output_data = talloc_steal(mem_ctx, state->output_data);

    return EOK;
}

static void dp_terminate_request(struct dp_req *dp_req)
{
    if (dp_req->handler_req == NULL) {
        /* This may occur when the handler already finished but the caller
         * of dp request did not yet recieved data/free dp_req. We just
         * return here. */
        return;
    }

    /* We will end the handler request and mark dp request as terminated. */

    DP_REQ_DEBUG(SSSDBG_TRACE_ALL, dp_req->name, "Terminating.");

    talloc_zfree(dp_req->handler_req);
    tevent_req_error(dp_req->req, ERR_TERMINATED);
}

static void dp_terminate_request_list(struct data_provider *provider,
                                      const char *domain)
{
    struct dp_req *next;
    struct dp_req *cur;

    if (provider == NULL || provider->requests.active == NULL) {
        return;
    }

    for (cur = provider->requests.active; cur != NULL; cur = next) {
        next = cur->next;
        if (domain == NULL || strcmp(cur->domain->name, domain) == 0) {
            dp_terminate_request(cur);
        }
    }
}

void dp_terminate_active_requests(struct data_provider *provider)
{
    DEBUG(SSSDBG_TRACE_FUNC, "Terminating active data provider requests\n");

    dp_terminate_request_list(provider, NULL);
}

void dp_terminate_domain_requests(struct data_provider *provider,
                                  const char *domain)
{
    DEBUG(SSSDBG_TRACE_FUNC, "Terminating active data provider requests "
          "for domain [%s]\n", domain);

    if (domain == NULL) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Bug: domain is NULL!\n");
        return;
    }

    dp_terminate_request_list(provider, domain);
}
