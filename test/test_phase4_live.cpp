// Live handshake against a real Exportmaster server.
// Reads connection details from env vars; skips if any are missing.
//   DBISAM_HOST      (default rivsem01)
//   DBISAM_PORT      (default 12005)
//   DBISAM_USER
//   DBISAM_PASSWORD
//   DBISAM_ENCRYPT   (default "elevatesoft" per DBISAM-PROTOCOL.md §5)
//   DBISAM_CATALOG   (default YOURCATALOG)

#include "dbisam/client.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

using namespace dbisam;

static std::string env(const char *key, const char *fallback) {
    const char *v = std::getenv(key);
    return v ? std::string(v) : std::string(fallback);
}

int main() {
    const char *user = std::getenv("DBISAM_USER");
    const char *pw   = std::getenv("DBISAM_PASSWORD");
    if (!user || !pw) {
        std::printf("SKIP: set DBISAM_USER and DBISAM_PASSWORD to run live handshake\n");
        return 0;
    }

    ConnOpts opts;
    opts.host             = env("DBISAM_HOST", "YOURHOST");
    opts.port             = static_cast<uint16_t>(std::atoi(env("DBISAM_PORT", "12005").c_str()));
    opts.user             = user;
    opts.password         = pw;
    opts.encrypt_password = env("DBISAM_ENCRYPT", "elevatesoft");
    opts.catalog          = env("DBISAM_CATALOG", "YOURCATALOG");
    opts.compression      = true;

    std::printf("connecting to %s:%u as %s (catalog %s, compression %d)\n",
                opts.host.c_str(), opts.port, opts.user.c_str(),
                opts.catalog.c_str(), opts.compression ? 1 : 0);

    try {
        auto client = Client::connect_and_login(opts);
        std::printf("handshake OK: fd=%d compression=%d batch=%u slot_len=%zu\n",
                    client.transport().fd(),
                    client.compression() ? 1 : 0,
                    client.batch_size(),
                    client.blob_slot_length());
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
