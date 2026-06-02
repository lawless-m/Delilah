// Per-context "transaction" for a DBISAM-attached database.
//
// Each logical query opens its own short-lived `Client` connection
// because the native protocol desyncs when multiple queries share a
// session (see ExportKing README and KNOWN_BUGS.md). The transaction
// itself only owns the catalog reference + cached TableCatalogEntry
// instances; it does NOT hold a long-lived Client.
//
// DBISAM access is SELECT-only — Start/Commit/Rollback are no-ops.

#pragma once

#include "dbisam/client.hpp"

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/transaction/transaction.hpp"

#include <mutex>
#include <vector>

namespace duckdb {

class DbisamCatalog;

class DbisamTransaction : public Transaction {
public:
    DbisamTransaction(DbisamCatalog &catalog, TransactionManager &mgr, ClientContext &context);
    ~DbisamTransaction() override;

    // Lazy: fetch and cache the TableCatalogEntry for `name`. Returns
    // nullptr if the table doesn't exist on the server.
    optional_ptr<CatalogEntry> GetCatalogEntry(const std::string &name);

    // Snapshot the table list (cached after first call this transaction).
    const std::vector<std::string> &GetTableNames();

    // Open a fresh, short-lived Client for one query against the
    // catalog's server. Caller owns it and discards after use.
    dbisam::Client OpenClient();

    static DbisamTransaction &Get(ClientContext &context, Catalog &catalog);

private:
    DbisamCatalog &catalog_;
    std::mutex lock_;
    case_insensitive_map_t<unique_ptr<CatalogEntry>> entries_;
    std::vector<std::string> table_names_;
    bool tables_loaded_ = false;
};

} // namespace duckdb
