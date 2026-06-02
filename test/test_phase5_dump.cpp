// Phase 5 diagnostic: dump the raw PrepareStatement response.
#include "dbisam/client.hpp"
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

using namespace dbisam;

static std::string env(const char *k, const char *fb) {
    const char *v = std::getenv(k);
    return v ? std::string(v) : std::string(fb);
}

int main() {
    ConnOpts opts;
    opts.host = env("DBISAM_HOST", "YOURHOST");
    opts.port = static_cast<uint16_t>(std::atoi(env("DBISAM_PORT", "12005").c_str()));
    opts.user = std::getenv("DBISAM_USER") ? std::getenv("DBISAM_USER") : "";
    opts.password = std::getenv("DBISAM_PASSWORD") ? std::getenv("DBISAM_PASSWORD") : "";
    opts.encrypt_password = env("DBISAM_ENCRYPT", "elevatesoft");
    opts.catalog = env("DBISAM_CATALOG", "YOURCATALOG");
    opts.compression = true;

    std::string sql = env("DBISAM_SQL", "SELECT * FROM CUSTOMER WHERE 1=0");
    try {
        auto client = Client::connect_and_login(opts);
        auto r = client.query_raw(sql);
        std::printf("response: %zu bytes\n", r.size());
        std::printf("body[0]=0x%02X reqcode=0x%04X\n", r[0],
                    static_cast<unsigned>(r[1]) | (static_cast<unsigned>(r[2]) << 8));
        std::printf("first 64 bytes:\n  ");
        for (size_t i = 0; i < r.size() && i < 64; ++i) {
            std::printf("%02X ", r[i]);
            if ((i & 0xF) == 0xF) std::printf("\n  ");
        }
        std::printf("\n");
        // Scan for schema block marker 03 00 00 01 00 (with namelen byte > 0 at +5).
        size_t first_match = SIZE_MAX;
        size_t match_count = 0;
        for (size_t i = 0; i + 6 < r.size(); ++i) {
            if (r[i] == 0x03 && r[i+1] == 0x00 && r[i+2] == 0x00
                && r[i+3] == 0x01 && r[i+4] == 0x00 && r[i+5] > 0) {
                if (first_match == SIZE_MAX) first_match = i;
                ++match_count;
            }
        }
        std::printf("schema marker (03 00 00 01 00 + namelen>0): first=%zu count=%zu\n",
                    first_match, match_count);
        // Generic 03 00 00 ord_lo ord_hi count (any first ord).
        size_t any_marker_count = 0;
        for (size_t i = 0; i + 6 < r.size(); ++i) {
            if (r[i] == 0x03 && r[i+1] == 0x00 && r[i+2] == 0x00 && r[i+5] > 0 && r[i+5] < 64) {
                ++any_marker_count;
            }
        }
        std::printf("any 03 00 00 ?? 00 namelen-like: %zu\n", any_marker_count);
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
