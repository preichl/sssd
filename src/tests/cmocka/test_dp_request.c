/*
    Copyright (C) 2015 Red Hat

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
#include <errno.h>
#include <popt.h>

#include "providers/backend.h"
#include "providers/data_provider/dp_private.h"
#include "providers/data_provider/dp.h"
#include "tests/cmocka/common_mock.h"
#include "tests/common.h"
#include "tests/cmocka/common_mock_be.h"

#define TESTS_PATH "tp_" BASE_FILE_STEM
#define TEST_CONF_DB "test_dp_request.ldb"
#define TEST_DOM_NAME "dp_request_test"
#define TEST_ID_PROVIDER "ldap"
/* #define TEST_EXT_MEMBER "extMember" */

//enum host_database default_host_dbs[] = { DB_FILES, DB_DNS, DB_SENTINEL };


struct test_ctx {
//    hash_table_t *table;
//    struct tevent_context *ev;
    struct sss_test_ctx *stc;
};

static int test_setup(void **state)
{
    struct test_ctx *test_ctx = NULL;
    static struct sss_test_conf_param params[] = {
        { "ldap_schema", "rfc2307bis" }, /* enable nested groups */
        /* { "ldap_search_base", OBJECT_BASE_DN }, */
        /* { "ldap_user_search_base", USER_BASE_DN }, */
        /* { "ldap_group_search_base", GROUP_BASE_DN }, */
        { NULL, NULL }
    };

    assert_true(leak_check_setup());

    test_ctx = talloc_zero(global_talloc_context, struct test_ctx);
    assert_non_null(test_ctx);

    //test_ctx->stc = create_ev_test_ctx(test_ctx);
//    assert_non_null(test_ctx->stc);

    test_ctx->stc = create_dom_test_ctx(test_ctx, TESTS_PATH, TEST_CONF_DB,
                                         TEST_DOM_NAME,
                                         TEST_ID_PROVIDER, params);
    assert_non_null(test_ctx->stc);


    *state = test_ctx;

    return 0;
}

static int test_teardown(void **state)
{
    talloc_zfree(*state);
//    assert_true(leak_check_teardown());
    return 0;
}

bool __wrap_be_is_offline(struct be_ctx *ctx)
{
    return false;
}

static errno_t mock_provider(struct tevent_context *ev,
                             struct be_ctx *be_ctx,
                             uid_t uid,
                             gid_t gid)
{
    struct data_provider *provider;

    errno_t ret;

    provider = talloc_zero(be_ctx, struct data_provider);
    if (provider == NULL) {
        return ENOMEM;
    }

    provider->ev = ev;
    provider->uid = uid;
    provider->gid = gid;
    provider->be_ctx = be_ctx;

    /* ret = dp_req_table_init(provider, &provider->requests.reply_table); */
    /* if (ret != EOK) { */
    /*     DEBUG(SSSDBG_CRIT_FAILURE, "Unable to initialize request table " */
    /*           "[%d]: %s\n", ret, sss_strerror(ret)); */
    /*     goto done; */
    /* } */

    ret = dp_init_modules(provider, &provider->modules);
    if (ret != EOK) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Unable to initialize DP modules "
              "[%d]: %s\n", ret, sss_strerror(ret));
        goto done;
    }

    ret = dp_init_targets(provider, be_ctx, provider, provider->modules,
                          &provider->targets);
    if (ret != EOK) {
        DEBUG(SSSDBG_CRIT_FAILURE, "Unable to initialize DP targets "
              "[%d]: %s\n", ret, sss_strerror(ret));
        goto done;
    }

//    talloc_set_destructor(provider, dp_destructor);

    /* ret = dp_init_dbus_server(provider); */
    /* if (ret != EOK) { */
    /*     DEBUG(SSSDBG_FATAL_FAILURE, "Unable to setup service bus [%d]: %s\n", */
    /*           ret, sss_strerror(ret)); */
    /*     goto done; */
    /* } */

    be_ctx->provider = provider;

    ret = EOK;

done:
    if (ret != EOK) {
        talloc_free(provider);
    }

    return ret;
}

static void test_foo(void **state)
{
    errno_t ret;
    const char *request_name;
    struct tevent_req *req;
    TALLOC_CTX *mem_ctx = global_talloc_context;
    struct test_ctx *test_ctx = talloc_get_type(*state, struct test_ctx);
    struct be_ctx * be_ctx = mock_be_ctx(mem_ctx, test_ctx->stc);

    ret = mock_provider(test_ctx->stc->ev, be_ctx, 1000, 1000);
    assert_int_equal(ret, EOK);

    req = dp_req_send(mem_ctx, be_ctx->provider,
                      NULL /* struct dp_client *dp_cli */,
                      NULL, /* struct sss_domain_info *domain */
                      "xxx", /* const char *name, */
                      DPT_ID, /* enum dp_targets target, */
                      DPM_ACCOUNT_HANDLER, /* enum dp_methods method, */
                      0, /* uint32_t dp_flags, */
                      NULL, /* void *request_data, */
                      &request_name);

    assert_non_null(req);
    assert_string_equal(request_name, "xxx");

    /* assert_non_null(req); */
    talloc_free(be_ctx);
    talloc_free(req);
}

int main(int argc, const char *argv[])
{
    poptContext pc;
    int opt;
    int rv;
    int no_cleanup = 0;
    struct poptOption long_options[] = {
        POPT_AUTOHELP
        SSSD_DEBUG_OPTS
        {"no-cleanup", 'n', POPT_ARG_NONE, &no_cleanup, 0,
         _("Do not delete the test database after a test run"), NULL },
        POPT_TABLEEND
    };

    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_foo,
                                        test_setup,
                                        test_teardown),
    };

    /* Set debug level to invalid value so we can deside if -d 0 was used. */
    debug_level = SSSDBG_INVALID;

    pc = poptGetContext(argv[0], argc, argv, long_options, 0);
    while((opt = poptGetNextOpt(pc)) != -1) {
        switch(opt) {
        default:
            fprintf(stderr, "\nInvalid option %s: %s\n\n",
                    poptBadOption(pc, 0), poptStrerror(opt));
            poptPrintUsage(pc, stderr, 0);
            return 1;
        }
    }
    poptFreeContext(pc);

    DEBUG_CLI_INIT(debug_level);

    /* Even though normally the tests should clean up after themselves
     * they might not after a failed run. Remove the old db to be sure */
    tests_set_cwd();
    test_dom_suite_cleanup(TESTS_PATH, TEST_CONF_DB, TEST_DOM_NAME);
    test_dom_suite_setup(TESTS_PATH);

    rv = cmocka_run_group_tests(tests, NULL, NULL);
    return rv;

    return cmocka_run_group_tests(tests, NULL, NULL);
}
