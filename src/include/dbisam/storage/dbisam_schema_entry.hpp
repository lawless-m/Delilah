// Single "main" schema for a DBISAM-attached database. DBISAM has no
// schema concept on the wire — all tables live in one flat namespace.

#pragma once

#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"

namespace duckdb {

class DbisamSchemaEntry : public SchemaCatalogEntry {
public:
    DbisamSchemaEntry(Catalog &catalog, CreateSchemaInfo &info);

    optional_ptr<CatalogEntry> CreateTable(CatalogTransaction, BoundCreateTableInfo &) override;
    optional_ptr<CatalogEntry> CreateFunction(CatalogTransaction, CreateFunctionInfo &) override;
    optional_ptr<CatalogEntry> CreateIndex(CatalogTransaction, CreateIndexInfo &, TableCatalogEntry &) override;
    optional_ptr<CatalogEntry> CreateView(CatalogTransaction, CreateViewInfo &) override;
    optional_ptr<CatalogEntry> CreateSequence(CatalogTransaction, CreateSequenceInfo &) override;
    optional_ptr<CatalogEntry> CreateTableFunction(CatalogTransaction, CreateTableFunctionInfo &) override;
    optional_ptr<CatalogEntry> CreateCopyFunction(CatalogTransaction, CreateCopyFunctionInfo &) override;
    optional_ptr<CatalogEntry> CreatePragmaFunction(CatalogTransaction, CreatePragmaFunctionInfo &) override;
    optional_ptr<CatalogEntry> CreateCollation(CatalogTransaction, CreateCollationInfo &) override;
    optional_ptr<CatalogEntry> CreateType(CatalogTransaction, CreateTypeInfo &) override;
    void Alter(CatalogTransaction, AlterInfo &) override;
    void Scan(ClientContext &context, CatalogType type,
              const std::function<void(CatalogEntry &)> &callback) override;
    void Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;
    void DropEntry(ClientContext &context, DropInfo &info) override;
    optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction transaction,
                                           const EntryLookupInfo &lookup_info) override;
};

} // namespace duckdb
