#include "dbisam/storage/dbisam_schema_entry.hpp"
#include "dbisam/storage/dbisam_catalog.hpp"
#include "dbisam/storage/dbisam_transaction.hpp"

#include "duckdb/common/exception/binder_exception.hpp"

namespace duckdb {

DbisamSchemaEntry::DbisamSchemaEntry(Catalog &catalog, CreateSchemaInfo &info)
    : SchemaCatalogEntry(catalog, info) {}

#define READ_ONLY(what) \
    throw BinderException("DBISAM catalogs are read-only (no " what ")")

optional_ptr<CatalogEntry> DbisamSchemaEntry::CreateTable(CatalogTransaction, BoundCreateTableInfo &) { READ_ONLY("CREATE TABLE"); }
optional_ptr<CatalogEntry> DbisamSchemaEntry::CreateFunction(CatalogTransaction, CreateFunctionInfo &) { READ_ONLY("CREATE FUNCTION"); }
optional_ptr<CatalogEntry> DbisamSchemaEntry::CreateIndex(CatalogTransaction, CreateIndexInfo &, TableCatalogEntry &) { READ_ONLY("CREATE INDEX"); }
optional_ptr<CatalogEntry> DbisamSchemaEntry::CreateView(CatalogTransaction, CreateViewInfo &) { READ_ONLY("CREATE VIEW"); }
optional_ptr<CatalogEntry> DbisamSchemaEntry::CreateSequence(CatalogTransaction, CreateSequenceInfo &) { READ_ONLY("CREATE SEQUENCE"); }
optional_ptr<CatalogEntry> DbisamSchemaEntry::CreateTableFunction(CatalogTransaction, CreateTableFunctionInfo &) { READ_ONLY("CREATE TABLE FUNCTION"); }
optional_ptr<CatalogEntry> DbisamSchemaEntry::CreateCopyFunction(CatalogTransaction, CreateCopyFunctionInfo &) { READ_ONLY("CREATE COPY FUNCTION"); }
optional_ptr<CatalogEntry> DbisamSchemaEntry::CreatePragmaFunction(CatalogTransaction, CreatePragmaFunctionInfo &) { READ_ONLY("CREATE PRAGMA FUNCTION"); }
optional_ptr<CatalogEntry> DbisamSchemaEntry::CreateCollation(CatalogTransaction, CreateCollationInfo &) { READ_ONLY("CREATE COLLATION"); }
optional_ptr<CatalogEntry> DbisamSchemaEntry::CreateType(CatalogTransaction, CreateTypeInfo &) { READ_ONLY("CREATE TYPE"); }
void DbisamSchemaEntry::Alter(CatalogTransaction, AlterInfo &) { READ_ONLY("ALTER"); }
void DbisamSchemaEntry::DropEntry(ClientContext &, DropInfo &) { READ_ONLY("DROP"); }

#undef READ_ONLY

void DbisamSchemaEntry::Scan(ClientContext &context, CatalogType type,
                              const std::function<void(CatalogEntry &)> &callback) {
    if (type != CatalogType::TABLE_ENTRY) {
        return;
    }
    auto &cat = catalog.Cast<DbisamCatalog>();
    auto &txn = DbisamTransaction::Get(context, catalog);
    if (cat.opts.eager_schema) {
        cat.EnsureEagerLoaded(); // probe + cache all tables once this session
        for (auto &name : txn.GetTableNames()) {
            auto entry = txn.GetCatalogEntry(name); // full entry, from cache
            if (entry) callback(*entry);
        }
    } else {
        for (auto &name : txn.GetTableNames()) {
            auto entry = txn.GetScanEntry(name); // name-only; no per-table probe
            if (entry) callback(*entry);
        }
    }
}

void DbisamSchemaEntry::Scan(CatalogType, const std::function<void(CatalogEntry &)> &) {
    throw InternalException("DbisamSchemaEntry::Scan (no-context) called");
}

optional_ptr<CatalogEntry> DbisamSchemaEntry::LookupEntry(CatalogTransaction transaction,
                                                           const EntryLookupInfo &lookup_info) {
    if (lookup_info.GetCatalogType() != CatalogType::TABLE_ENTRY) {
        return nullptr;
    }
    if (!transaction.transaction) {
        return nullptr;
    }
    auto &txn = transaction.transaction->Cast<DbisamTransaction>();
    return txn.GetCatalogEntry(lookup_info.GetEntryName());
}

} // namespace duckdb
