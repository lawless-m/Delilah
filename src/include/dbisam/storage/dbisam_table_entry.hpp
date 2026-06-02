// Represents one DBISAM table within the attached catalog. Built lazily
// when DuckDB looks up the name (schema probed via `SELECT * FROM <tbl>
// WHERE 1=0`). Provides the scan TableFunction with projection pushdown.

#pragma once

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"

namespace duckdb {

class DbisamTableEntry : public TableCatalogEntry {
public:
    DbisamTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info);

    unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;
    TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;
    TableStorageInfo GetStorageInfo(ClientContext &context) override;
    void BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj,
                               LogicalUpdate &update, ClientContext &context) override;
};

} // namespace duckdb
