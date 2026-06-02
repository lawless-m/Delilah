// Unit tests for dbisam::parse_attach_path — the ATTACH connection
// string parser. Pure C++ (no DuckDB harness needed).

#include "dbisam/storage/dbisam_attach_options.hpp"

#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <string>

using namespace dbisam;

static int g_failures = 0;

#define CHECK(cond) do {                                                       \
    if (!(cond)) {                                                             \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        ++g_failures;                                                          \
    }                                                                          \
} while (0)

#define CHECK_THROWS(stmt) do {                                                \
    bool _threw = false;                                                       \
    try { (void)(stmt); }                                                      \
    catch (const std::exception &) { _threw = true; }                          \
    if (!_threw) {                                                             \
        std::fprintf(stderr, "FAIL: expected throw: %s (%s:%d)\n",             \
                     #stmt, __FILE__, __LINE__);                               \
        ++g_failures;                                                          \
    }                                                                          \
} while (0)

static void full_url_with_port() {
    auto o = parse_attach_path("em://YOURUSER:YOURPASSWORD@YOURHOST:12005/YOURCATALOG");
    CHECK(o.host == "YOURHOST");
    CHECK(o.port == 12005);
    CHECK(o.user == "YOURUSER");
    CHECK(o.password == "YOURPASSWORD");
    CHECK(o.catalog == "YOURCATALOG");
}

static void full_url_without_port_defaults_to_12005() {
    auto o = parse_attach_path("em://YOURUSER:YOURPASSWORD@YOURHOST/YOURCATALOG");
    CHECK(o.host == "YOURHOST");
    CHECK(o.port == 12005);  // ConnOpts default
    CHECK(o.user == "YOURUSER");
    CHECK(o.password == "YOURPASSWORD");
    CHECK(o.catalog == "YOURCATALOG");
}

static void bare_form_no_scheme() {
    auto o = parse_attach_path("YOURUSER:YOURPASSWORD@YOURHOST/YOURCATALOG");
    CHECK(o.host == "YOURHOST");
    CHECK(o.user == "YOURUSER");
    CHECK(o.password == "YOURPASSWORD");
    CHECK(o.catalog == "YOURCATALOG");
}

static void no_userinfo_relies_on_attach_options() {
    // ATTACH '...' (USER 'x', PASSWORD 'y') feeds creds via named opts;
    // path itself can omit them.
    auto o = parse_attach_path("YOURHOST/YOURCATALOG");
    CHECK(o.host == "YOURHOST");
    CHECK(o.user == "");
    CHECK(o.password == "");
    CHECK(o.catalog == "YOURCATALOG");
}

static void userinfo_with_empty_password() {
    auto o = parse_attach_path("em://user@host/cat");
    CHECK(o.user == "user");
    CHECK(o.password == "");
}

static void missing_catalog_throws() {
    CHECK_THROWS(parse_attach_path("em://u:p@host"));
    CHECK_THROWS(parse_attach_path("em://u:p@host/"));
}

static void missing_host_throws() {
    CHECK_THROWS(parse_attach_path("em:///YOURCATALOG"));
    CHECK_THROWS(parse_attach_path("/YOURCATALOG"));
}

static void bad_port_throws() {
    CHECK_THROWS(parse_attach_path("em://u:p@host:notanumber/cat"));
    CHECK_THROWS(parse_attach_path("em://u:p@host:99999/cat")); // out of range
    CHECK_THROWS(parse_attach_path("em://u:p@host:0/cat"));     // below range
    CHECK_THROWS(parse_attach_path("em://u:p@host:-5/cat"));    // negative
}

static void encrypt_password_defaults_to_elevatesoft() {
    auto o = parse_attach_path("em://u:p@host/cat");
    CHECK(o.encrypt_password == "elevatesoft");
}

static void compression_defaults_to_true() {
    auto o = parse_attach_path("em://u:p@host/cat");
    CHECK(o.compression == true);
}

static void password_with_at_sign_only_first_splits() {
    // First @ separates userinfo from hostpath. Subsequent @s belong to
    // the path. We don't try to handle @ in passwords (would need URL
    // encoding); document via this test that it goes into hostpath.
    auto o = parse_attach_path("em://u:p@@host/cat");
    CHECK(o.user == "u");
    CHECK(o.password == "p"); // split at first :
    // hostpath = "@host/cat" → host_only = "@host", catalog = "cat"
    // (this would never actually connect — test documents the parser's
    // greedy first-@ behaviour)
    CHECK(o.host == "@host");
    CHECK(o.catalog == "cat");
}

int main() {
    full_url_with_port();
    full_url_without_port_defaults_to_12005();
    bare_form_no_scheme();
    no_userinfo_relies_on_attach_options();
    userinfo_with_empty_password();
    missing_catalog_throws();
    missing_host_throws();
    bad_port_throws();
    encrypt_password_defaults_to_elevatesoft();
    compression_defaults_to_true();
    password_with_at_sign_only_first_splits();
    if (g_failures == 0) {
        std::printf("all attach-options tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d failures\n", g_failures);
    return 1;
}
