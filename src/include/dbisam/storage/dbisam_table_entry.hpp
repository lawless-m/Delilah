// Represents one DBISAM table within the attached catalog. Built lazily
// when DuckDB looks up the name (schema probed via `SELECT * FROM <tbl>
// WHERE 1=0`). Provides the scan TableFunction with projection pushdown.

#pragma once

#include "dbisam/schema.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"

#include <vector>

namespace duckdb {

class DbisamTableEntry : public TableCatalogEntry {
public:
    DbisamTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
                     std::vector<dbisam::FieldType> field_types);

    // Per-column DBISAM FieldType, indexed by ordinal. Cached at probe
    // time because the LogicalType→FieldType mapping is lossy (Memo and
    // String both map to VARCHAR) and the blob resolver in
    // dbisam_table_entry needs to know which projected columns are
    // blob-shaped so it can keep the PK column in the SELECT.
    std::vector<dbisam::FieldType> source_field_types;

    unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;
    TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;
    TableStorageInfo GetStorageInfo(ClientContext &context) override;
    void BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj,
                               LogicalUpdate &update, ClientContext &context) override;
};

} // namespace duckdb
