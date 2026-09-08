#include "iris_orm_store.h"

#include <tinytest.h>

#include <stdlib.h>
#include <string.h>

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

    it("rejects unknown PostgreSQL fields") {
        static const char yaml[] = "version: 1\n"
                                   "channels:\n"
                                   "  iris.test:\n"
                                   "    kind: record_store\n"
                                   "    config:\n"
                                   "      backend: postgresql\n"
                                   "      host: 127.0.0.1\n"
                                   "      namespace_name: iris.test\n"
                                   "      silent_fallback: true\n"
                                   "adapters: {}\n";
        expect_create_failure(yaml, "unknown PostgreSQL");
    }

    it("passes a valid PostgreSQL profile to the connector") {
        static const char yaml[] = "version: 1\n"
                                   "channels:\n"
                                   "  iris.test:\n"
                                   "    kind: record_store\n"
                                   "    config:\n"
                                   "      backend: postgresql\n"
                                   "      host: 127.0.0.1\n"
                                   "      port: 1\n"
                                   "      user: turbomedia-test\n"
                                   "      dbname: turbomedia-test\n"
                                   "      connect_timeout: 1\n"
                                   "      namespace_name: iris.test\n"
                                   "adapters: {}\n";
        expect_create_failure(yaml, "cannot connect TurboDB ORM");
    }
}
