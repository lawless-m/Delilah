// DBISAM client state machine — connect → login → session-setup →
// ready-for-queries.
//
// The 4 session-setup bodies (SESSION_SETUP_PRE[2], the catalog-attach
// built from opts.catalog, SESSION_SETUP_POST) are byte-for-byte
// replays from a captured dbsys.exe session. Treating them as opaque
// blobs is what the PoC does and what we know works against the live
// server. Decoding their fields properly is open work — see
// DBISAM-PROTOCOL.md §7.
//
// Phase 4 subset of mrsflow-cli/src/exportmaster/client.rs — covers
// only the handshake. Query/cursor/blob plumbing comes in later phases.

#pragma once

#include "dbisam/framing.hpp"
#include "dbisam/row.hpp"
#include "dbisam/schema.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dbisam {
class CursorRunner;
}

namespace dbisam {

struct ConnOpts {
    std::string host;
    uint16_t port = 12005; // DBISAM default
    std::string user;
    std::string password;
    std::string encrypt_password;
    // DBISAM catalog attached during session-setup (reqcode 0x003C).
    // YOURCATALOG is the only catalog we've tested against.
    std::string catalog = "YOURCATALOG";
    // Wire compression (Zlib deflate end-to-end). Maps to Connect
    // handshake field 2 per DBISAM-PROTOCOL.md §6g. Schema-heavy
    // responses see 3-10× size reduction with this on.
    bool compression = true;
    // Rows per ReadFirstRecordBlock / ReadNextRecordBlock call.
    uint32_t batch_size = 5000;
    // cursor.@+0x3672 — table's physical-record bookmark size.
    // 56 matches every ex3win table with a single-column ≤14-char PK.
    size_t blob_slot_length = 56;
    // Catalog behaviour (not a wire field): when true, the first catalog
    // access probes every table's schema once and caches it for the
    // session, so information_schema / SHOW ALL TABLES / JDBC getColumns
    // (DBeaver) see full columns. Default false = lazy: SHOW TABLES is
    // instant but catalog-wide column introspection is empty until a
    // table is queried. Set via ATTACH (..., EAGER_SCHEMA true).
    bool eager_schema = false;
};

class Client {
public:
    static Client connect_and_login(const ConnOpts &opts);

    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;
    Client(Client &&) = default;
    Client &operator=(Client &&) = default;

    Transport &transport() { return transport_; }
    bool compression() const { return compression_; }
    uint32_t batch_size() const { return batch_size_; }
    void set_batch_size(uint32_t v) { batch_size_ = v; }
    size_t blob_slot_length() const { return blob_slot_length_; }

    // Send a PrepareStatement (reqcode 0x0320) carrying `sql` and return
    // the raw schema-bearing response. SQL is twice-length-prefixed
    // (Delphi TStringField convention) with a trailing CRLF, matching
    // the captured dbsys.exe flow.
    std::vector<uint8_t> query_raw(const std::string &sql);

    struct QueryResult {
        std::vector<Column> columns;
        std::vector<std::vector<CellValue>> rows;
    };

    // PrepareStatement → drive_cursor → decode every row. Caps at
    // `target_rows` if non-zero (0 = unlimited).
    QueryResult query_decoded(const std::string &sql, size_t target_rows = 0);

    // Streaming alternative to query_decoded — PrepareStatement, parse
    // schema, open the cursor (Execute + Receive poll + SetToBegin),
    // and hand back a CursorRunner the caller pulls batches from. The
    // Client must outlive the returned runner (its Transport is borrowed).
    struct StreamingScan {
        std::vector<Column> columns;
        std::unique_ptr<CursorRunner> runner;
    };
    StreamingScan start_streaming(const std::string &sql);

    // Decode one batch of row spans into CellValues, resolving any
    // blob/memo/graphic columns inline via the live transport. Assumes
    // `columns[0]` is the PK (callers using projection pushdown must
    // ensure PK injection — see dbisam_table_entry.cpp). `bookmarks` is
    // optional; phys can be 0 if absent.
    std::vector<std::vector<CellValue>> decode_batch_with_blobs(
        const std::vector<Column> &columns,
        const std::vector<std::pair<const uint8_t *, size_t>> &rows,
        const std::vector<std::pair<const uint8_t *, size_t>> &bookmarks);

    // SQLTables-equivalent native request (reqcode 0x0032). Returns the
    // list of table names in the attached catalog. No SQL involved —
    // talks directly to the server's metadata path.
    std::vector<std::string> list_tables();

private:
    Client(Transport &&t, bool compression, uint32_t batch_size, size_t blob_slot_length)
        : transport_(std::move(t)),
          compression_(compression),
          batch_size_(batch_size),
          blob_slot_length_(blob_slot_length) {}

    Transport transport_;
    bool compression_;
    uint32_t batch_size_;
    size_t blob_slot_length_;
};

// Wrap a login ciphertext in the LOGIN message body (reqcode 0x14):
// double-length prefix + single trailing zero. See DBISAM-PROTOCOL.md §5.
std::vector<uint8_t> build_login_body(const uint8_t *ct, size_t ct_len);

inline std::vector<uint8_t> build_login_body(const std::vector<uint8_t> &ct) {
    return build_login_body(ct.data(), ct.size());
}

// Catalog-attach body (reqcode 0x003C) for the given catalog name.
// Layout decoded from an uncompressed capture of
// pyodbc.connect(DSN=Exportmaster):
//
//   reqcode 0x003C | sub-flag 0x00 | inner_len u32 LE |
//   inner: [u32 LE name_len][catalog name][trailer `01 00 00 00 00`] |
//   trailing 2 bytes `64 00`
std::vector<uint8_t> build_catalog_attach_body(const std::string &catalog);

} // namespace dbisam
