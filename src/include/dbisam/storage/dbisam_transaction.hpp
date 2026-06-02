// Per-context "transaction" for a DBISAM-attached database.
//
// Each logical query opens its own short-lived `Client` connection
// because the native protocol desyncs when multiple queries share a
// session (see ExportKing README and KNOWN_BUGS.md). The transaction
// holds no state of its own: the schema cache (table names + entries)
// lives on the DbisamCatalog so it persists across statements; these
// methods just forward to it.
//
// DBISAM access is SELECT-only — Start/Commit/Rollback are no-ops.

#pragma once

#include "dbisam/client.hpp"

#include "duckdb/transaction/transaction.hpp"

#include <string>
#include <vector>

namespace duckdb {

class DbisamCatalog;

class DbisamTransaction : public Transaction {
public:
    DbisamTransaction(DbisamCatalog &catalog, TransactionManager &mgr, ClientContext &context);
    ~DbisamTransaction() override;

    // Full, schema-probed entry for `name` (query / LookupEntry path).
    // Returns nullptr if the table doesn't exist on the server.
    optional_ptr<CatalogEntry> GetCatalogEntry(const std::string &name);

    // Table names (cached on the catalog after first call).
    const std::vector<std::string> &GetTableNames();

    // Entry for catalog enumeration (SHOW TABLES). Name-only in lazy mode
    // (no schema probe); full when eager mode has pre-loaded it.
    optional_ptr<CatalogEntry> GetScanEntry(const std::string &name);

    // Open a fresh, short-lived Client for one query against the
    // catalog's server. Caller owns it and discards after use.
    dbisam::Client OpenClient();

    static DbisamTransaction &Get(ClientContext &context, Catalog &catalog);

private:
    DbisamCatalog &catalog_;
};

} // namespace duckdb
