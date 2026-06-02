#include "dbisam/storage/dbisam_transaction.hpp"
#include "dbisam/storage/dbisam_catalog.hpp"

namespace duckdb {

// The schema cache (table names + entries, lazy or eager) lives on the
// DbisamCatalog so it persists across this ATTACH's per-statement
// transactions. These methods just forward to it.

DbisamTransaction::DbisamTransaction(DbisamCatalog &catalog, TransactionManager &mgr, ClientContext &context)
    : Transaction(mgr, context), catalog_(catalog) {}

DbisamTransaction::~DbisamTransaction() = default;

DbisamTransaction &DbisamTransaction::Get(ClientContext &context, Catalog &catalog) {
    return Transaction::Get(context, catalog).Cast<DbisamTransaction>();
}

dbisam::Client DbisamTransaction::OpenClient() {
    return catalog_.OpenClient();
}

const std::vector<std::string> &DbisamTransaction::GetTableNames() {
    return catalog_.GetTableNames();
}

optional_ptr<CatalogEntry> DbisamTransaction::GetCatalogEntry(const std::string &name) {
    return catalog_.GetTableEntry(name);
}

optional_ptr<CatalogEntry> DbisamTransaction::GetScanEntry(const std::string &name) {
    return catalog_.GetScanEntry(name);
}

} // namespace duckdb
