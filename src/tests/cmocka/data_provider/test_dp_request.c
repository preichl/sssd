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
#include "tests/cmocka/data_provider/mock_dp.h"

#define TESTS_PATH "tp_" BASE_FILE_STEM
#define TEST_CONF_DB "test_dp_request.ldb"
#define TEST_DOM_NAME "dp_request_test"
#define TEST_ID_PROVIDER "ldap"

struct test_ctx {
    struct sss_test_ctx *tctx;
    struct be_ctx *be_ctx;
    struct data_provider *provider;
    struct dp_method *dp_methods;
};

static int test_setup(void **state)
{
    struct test_ctx *test_ctx;

    assert_true(leak_check_setup());

    test_ctx = talloc_zero(global_talloc_context, struct test_ctx);
    assert_non_null(test_ctx);

    test_ctx->tctx = create_dom_test_ctx(test_ctx, TESTS_PATH, TEST_CONF_DB,
                                         TEST_DOM_NAME, TEST_ID_PROVIDER, NULL);
    assert_non_null(test_ctx->tctx);

    test_ctx->be_ctx = mock_be_ctx(test_ctx, test_ctx->tctx);
    test_ctx->provider = mock_dp(test_ctx, test_ctx->be_ctx);
    test_ctx->dp_methods = mock_dp_get_methods(test_ctx->provider, DPT_ID);

    check_leaks_push(test_ctx);

    *state = test_ctx;

    return 0;
}

static int test_teardown(void **state)
{
    struct test_ctx *test_ctx;

    test_ctx = talloc_get_type_abort(*state, struct test_ctx);
    assert_true(check_leaks_pop(test_ctx));
    talloc_zfree(test_ctx);

    test_dom_suite_cleanup(TESTS_PATH, TEST_CONF_DB, TEST_DOM_NAME);
    assert_true(leak_check_teardown());
    return 0;
}

bool __wrap_be_is_offline(struct be_ctx *ctx)
{
    return false;
}

static void test_foo(void **state)
{
    struct test_ctx *test_ctx;
    const char *request_name;
    struct tevent_req *req;

    test_ctx = talloc_get_type(*state, struct test_ctx);

    // nastavit send_fn, recv_fn, private data a datové typy
    dp_set_method(test_ctx->dp_methods, DPM_ACCOUNT_HANDLER, NULL, NULL, NULL, void, void, char);

    req = dp_req_send(test_ctx, test_ctx->provider, NULL, NULL, "test-req",
                      DPT_ID, DPM_ACCOUNT_HANDLER, 0, NULL, &request_name);
    assert_non_null(req);
    //assert_string_equal(request_name, "test-req #1");

    // event loop ...
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
