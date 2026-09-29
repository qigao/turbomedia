#include "iris_orm_store.h"

#include <salts_error.h>
#include <tinytest.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TEST_ORM_RUNTIME_DRIVER
#error "TEST_ORM_RUNTIME_DRIVER is required"
#endif
#ifndef TEST_ORM_WRONG_DRIVER
#error "TEST_ORM_WRONG_DRIVER is required"
#endif

static int test_set_env(const char *name, const char *value) {
#ifdef _WIN32
    return _putenv_s(name, value ? value : "");
#else
    return value ? setenv(name, value, 1) : unsetenv(name);
#endif
}

static char *write_runtime_profile(const char *driver_module) {
    char *yaml_path = tt_make_temp_file("iris-orm-runtime", ".yaml");
    char yaml[4096];
    int written;

    check_not_null(yaml_path);
    if (!yaml_path)
        return NULL;
    written = snprintf(
        yaml, sizeof(yaml),
        "version: 1\n"
        "channels:\n"
        "  iris.test:\n"
        "    kind: record_store\n"
        "    config:\n"
        "      backend: postgresql\n"
        "      driver_module: '%s'\n"
        "      host: 127.0.0.1\n"
        "      port: 1\n"
        "      user: turbomedia-test\n"
        "      dbname: turbomedia-test\n"
        "      connect_timeout: 1\n"
        "      namespace_name: iris.test\n"
        "adapters: {}\n",
        driver_module);
    check_true(written > 0 && (size_t)written < sizeof(yaml));
    if (written <= 0 || (size_t)written >= sizeof(yaml)) {
        free(yaml_path);
        return NULL;
    }
    check_equal(tt_write_file(yaml_path, yaml, (size_t)written), 0);
    return yaml_path;
}

static void remove_profile(char *yaml_path) {
    if (!yaml_path)
        return;
    check_equal(tt_remove_file(yaml_path), 0);
    free(yaml_path);
}

static void expect_create_failure(const char *yaml, const char *message) {
    char *yaml_path = tt_make_temp_file("iris-orm-store", ".yaml");
    char error[256] = {0};
    iris_orm_store_owner_t *owner;

    check_not_null(yaml_path);
    if (!yaml_path)
        return;
    check_equal(tt_write_file(yaml_path, yaml, strlen(yaml)), 0);
    owner = iris_orm_store_owner_create(yaml_path, "iris.test", error,
                                        sizeof(error));
    check_null(owner);
    check_not_null(strstr(error, message));
    iris_orm_store_owner_destroy(owner);
    check_equal(tt_remove_file(yaml_path), 0);
    free(yaml_path);
}

spec("Iris PostgreSQL ORM record store") {
    it("rejects non-PostgreSQL backends") {
        static const char yaml[] = "version: 1\n"
                                   "channels:\n"
                                   "  iris.test:\n"
                                   "    kind: record_store\n"
                                   "    config:\n"
                                   "      backend: sqlite\n"
                                   "      namespace_name: iris.test\n"
                                   "adapters: {}\n";
        expect_create_failure(yaml, "requires the PostgreSQL ORM backend");
    }

    it("requires an explicit PostgreSQL runtime driver module") {
        static const char yaml[] = "version: 1\n"
                                   "channels:\n"
                                   "  iris.test:\n"
                                   "    kind: record_store\n"
                                   "    config:\n"
                                   "      backend: postgresql\n"
                                   "      host: 127.0.0.1\n"
                                   "      namespace_name: iris.test\n"
                                   "adapters: {}\n";
        expect_create_failure(yaml, "requires an explicit driver_module");
    }

    it("rejects unknown PostgreSQL fields") {
        static const char yaml[] = "version: 1\n"
                                   "channels:\n"
                                   "  iris.test:\n"
                                   "    kind: record_store\n"
                                   "    config:\n"
                                   "      backend: postgresql\n"
                                   "      driver_module: /definitely/missing/turbodb_driver_postgresql\n"
                                   "      host: 127.0.0.1\n"
                                   "      namespace_name: iris.test\n"
                                   "      silent_fallback: true\n"
                                   "adapters: {}\n";
        expect_create_failure(yaml, "unknown PostgreSQL");
    }

    it("passes a valid PostgreSQL profile to the runtime driver loader") {
        static const char yaml[] = "version: 1\n"
                                   "channels:\n"
                                   "  iris.test:\n"
                                   "    kind: record_store\n"
                                   "    config:\n"
                                   "      backend: postgresql\n"
                                   "      driver_module: /definitely/missing/turbodb_driver_postgresql\n"
                                   "      host: 127.0.0.1\n"
                                   "      port: 1\n"
                                   "      user: turbomedia-test\n"
                                   "      dbname: turbomedia-test\n"
                                   "      connect_timeout: 1\n"
                                   "      namespace_name: iris.test\n"
                                   "adapters: {}\n";
        expect_create_failure(yaml, "cannot load TurboDB PostgreSQL driver");
    }

    it("rejects a runtime module whose canonical driver id is not PostgreSQL") {
        char *yaml_path = write_runtime_profile(TEST_ORM_WRONG_DRIVER);
        char error[256] = {0};
        iris_orm_store_owner_t *owner;

        check_not_null(yaml_path);
        if (!yaml_path)
            return;
        owner = iris_orm_store_owner_create(yaml_path, "iris.test", error,
                                            sizeof(error));
        check_null(owner);
        check_not_null(strstr(error, "cannot load TurboDB PostgreSQL driver"));
        iris_orm_store_owner_destroy(owner);
        remove_profile(yaml_path);
    }

    it("surfaces runtime connection failure without a fallback") {
        char *yaml_path = write_runtime_profile(TEST_ORM_RUNTIME_DRIVER);
        char error[256] = {0};
        iris_orm_store_owner_t *owner;

        check_not_null(yaml_path);
        if (!yaml_path)
            return;
        check_equal(test_set_env("TURBOMEDIA_ORM_TEST_FAIL_CONNECT", "1"), 0);
        owner = iris_orm_store_owner_create(yaml_path, "iris.test", error,
                                            sizeof(error));
        check_equal(test_set_env("TURBOMEDIA_ORM_TEST_FAIL_CONNECT", NULL), 0);
        check_null(owner);
        check_not_null(strstr(error, "cannot connect TurboDB ORM"));
        iris_orm_store_owner_destroy(owner);
        remove_profile(yaml_path);
    }

    it("retains runtime ownership when close is busy and allows retry") {
        char *yaml_path = write_runtime_profile(TEST_ORM_RUNTIME_DRIVER);
        char error[256] = {0};
        iris_orm_store_owner_t *owner;

        check_not_null(yaml_path);
        if (!yaml_path)
            return;
        owner = iris_orm_store_owner_create(yaml_path, "iris.test", error,
                                            sizeof(error));
        check_not_null(owner);
        if (!owner) {
            remove_profile(yaml_path);
            return;
        }
        check_not_null(iris_orm_store_owner_store(owner));

        check_equal(test_set_env("TURBOMEDIA_ORM_TEST_BUSY_CLOSE", "1"), 0);
        memset(error, 0, sizeof(error));
        check_equal(iris_orm_store_owner_close(owner, error, sizeof(error)),
                    SALTS_EBUSY);
        check_not_null(strstr(error, "cannot close TurboDB ORM runtime"));

        check_equal(test_set_env("TURBOMEDIA_ORM_TEST_BUSY_CLOSE", NULL), 0);
        memset(error, 0, sizeof(error));
        check_equal(iris_orm_store_owner_close(owner, error, sizeof(error)),
                    SALTS_OK);
        iris_orm_store_owner_destroy(owner);
        remove_profile(yaml_path);
    }
}
