// Phase 5 live driver: connect, run a SELECT, print schema + decoded rows.
// SQL defaults to a tiny test query; override with DBISAM_SQL env var.

#include "dbisam/client.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <variant>

using namespace dbisam;

static std::string env(const char *key, const char *fallback) {
    const char *v = std::getenv(key);
    return v ? std::string(v) : std::string(fallback);
}

static const char *ft_name(FieldType ft) {
    switch (ft) {
    case FieldType::Calculated: return "Calculated";
    case FieldType::String:     return "String";
    case FieldType::Date:       return "Date";
    case FieldType::Blob:       return "Blob";
    case FieldType::Memo:       return "Memo";
    case FieldType::Graphic:    return "Graphic";
    case FieldType::Boolean:    return "Boolean";
    case FieldType::Smallint:   return "Smallint";
    case FieldType::Integer:    return "Integer";
    case FieldType::AutoInc:    return "AutoInc";
    case FieldType::Currency:   return "Currency";
    case FieldType::Float:      return "Float";
    case FieldType::Bytes:      return "Bytes";
    case FieldType::Time:       return "Time";
    case FieldType::DateTime:   return "DateTime";
    case FieldType::VarBytes:   return "VarBytes";
    case FieldType::Largeint:   return "Largeint";
    case FieldType::Unknown:    return "Unknown";
    }
    return "?";
}

static void print_cell(const CellValue &c) {
    std::visit([](const auto &v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, NullValue>) {
            std::printf("NULL");
        } else if constexpr (std::is_same_v<T, std::string>) {
            std::printf("\"%s\"", v.c_str());
        } else if constexpr (std::is_same_v<T, int32_t>) {
            std::printf("%d", v);
        } else if constexpr (std::is_same_v<T, int64_t>) {
            std::printf("%lld", static_cast<long long>(v));
        } else if constexpr (std::is_same_v<T, double>) {
            std::printf("%g", v);
        } else if constexpr (std::is_same_v<T, bool>) {
            std::printf("%s", v ? "true" : "false");
        } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
            std::printf("<%zu bytes>", v.size());
        } else if constexpr (std::is_same_v<T, BlobHandle>) {
            std::printf("<blob handle>");
        }
    }, c);
}

int main() {
    const char *user = std::getenv("DBISAM_USER");
    const char *pw   = std::getenv("DBISAM_PASSWORD");
    if (!user || !pw) {
        std::printf("SKIP: set DBISAM_USER and DBISAM_PASSWORD to run\n");
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

    std::string sql = env("DBISAM_SQL", "SELECT TOP 3 * FROM CUSTOMER");
    size_t cap = static_cast<size_t>(std::atoi(env("DBISAM_LIMIT", "5").c_str()));

    try {
        auto client = Client::connect_and_login(opts);
        std::printf("connected; running: %s (cap=%zu)\n", sql.c_str(), cap);
        auto r = client.query_decoded(sql, cap);

        std::printf("schema (%zu columns):\n", r.columns.size());
        for (size_t i = 0; i < r.columns.size(); ++i) {
            const auto &c = r.columns[i];
            std::printf("  [%zu] %-20s %-10s max=%u row_offset=%u\n",
                        i + 1, c.name.c_str(), ft_name(c.field_type), c.max, c.row_offset);
        }
        std::printf("rows (%zu):\n", r.rows.size());
        for (size_t i = 0; i < r.rows.size(); ++i) {
            std::printf("  row %zu: ", i);
            for (size_t j = 0; j < r.rows[i].size(); ++j) {
                if (j > 0) std::printf(" | ");
                print_cell(r.rows[i][j]);
            }
            std::printf("\n");
        }
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
