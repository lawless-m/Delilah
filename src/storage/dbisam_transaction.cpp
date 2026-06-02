#include "dbisam/storage/dbisam_transaction.hpp"
#include "dbisam/storage/dbisam_catalog.hpp"
#include "dbisam/storage/dbisam_schema_entry.hpp"
#include "dbisam/storage/dbisam_table_entry.hpp"

#include "duckdb/parser/column_definition.hpp"
#include "duckdb/parser/column_list.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"

namespace duckdb {

namespace {

LogicalType MapFieldType(dbisam::FieldType ft) {
    using FT = dbisam::FieldType;
    switch (ft) {
    case FT::String:
    case FT::Memo:
    case FT::Calculated:
    case FT::Unknown:    return LogicalType::VARCHAR;
    case FT::Date:       return LogicalType::DATE;
    case FT::DateTime:   return LogicalType::TIMESTAMP;
    case FT::Time:       return LogicalType::TIME;
    case FT::Integer:
    case FT::AutoInc:
    case FT::Smallint:   return LogicalType::INTEGER;
    case FT::Largeint:   return LogicalType::BIGINT;
    case FT::Boolean:    return LogicalType::BOOLEAN;
    case FT::Float:
    case FT::Currency:   return LogicalType::DOUBLE;
    case FT::Blob:
    case FT::Graphic:
    case FT::Bytes:
    case FT::VarBytes:   return LogicalType::BLOB;
    }
    return LogicalType::VARCHAR;
}

// Build a fully-typed TableCatalogEntry from a probed column list.
unique_ptr<CatalogEntry> BuildTableEntry(DbisamCatalog &catalog, const std::string &name,
                                         const std::vector<dbisam::Column> &columns) {
    CreateTableInfo info(catalog.GetMainSchema(), name);
    std::vector<dbisam::FieldType> field_types;
    field_types.reserve(columns.size());
    for (const auto &col : columns) {
        info.columns.AddColumn(ColumnDefinition(col.name, MapFieldType(col.field_type)));
        field_types.push_back(col.field_type);
    }
    return make_uniq<DbisamTableEntry>(catalog, catalog.GetMainSchema(), info, std::move(field_types));
}

// Build a name-only entry — no columns. Used to satisfy catalog
// enumeration (SHOW TABLES), which reads only the entry name, WITHOUT
// paying a per-table schema probe. Columns are filled lazily by
// GetCatalogEntry (LookupEntry) when the table is actually queried.
unique_ptr<CatalogEntry> BuildNameOnlyEntry(DbisamCatalog &catalog, const std::string &name) {
    CreateTableInfo info(catalog.GetMainSchema(), name);
    return make_uniq<DbisamTableEntry>(catalog, catalog.GetMainSchema(), info,
                                       std::vector<dbisam::FieldType>{});
}

} // namespace

DbisamTransaction::DbisamTransaction(DbisamCatalog &catalog, TransactionManager &mgr, ClientContext &context)
    : Transaction(mgr, context), catalog_(catalog) {}

DbisamTransaction::~DbisamTransaction() = default;

DbisamTransaction &DbisamTransaction::Get(ClientContext &context, Catalog &catalog) {
    return Transaction::Get(context, catalog).Cast<DbisamTransaction>();
}

dbisam::Client DbisamTransaction::OpenClient() {
    return dbisam::Client::connect_and_login(catalog_.opts);
}

const std::vector<std::string> &DbisamTransaction::GetTableNames() {
    std::lock_guard<std::mutex> g(lock_);
    if (!tables_loaded_) {
        auto client = OpenClient();
        table_names_ = client.list_tables();
        tables_loaded_ = true;
    }
    return table_names_;
}

optional_ptr<CatalogEntry> DbisamTransaction::GetCatalogEntry(const std::string &name) {
    std::lock_guard<std::mutex> g(lock_);
    auto it = entries_.find(name);
    if (it != entries_.end()) {
        return it->second.get();
    }

    // Schema probe — `SELECT * FROM "<tbl>" WHERE 1=0` returns the schema
    // response. PrepareStatement alone carries the schema blocks, so
    // query_raw + parse_schema is one round-trip vs ~5 for the full
    // decoded path. Double-quoted identifiers are accepted per the Dibdog
    // grammar, keeping mixed-case / special-char table names safe.
    //
    // Fresh connection per probe — the native protocol desyncs when
    // multiple queries share a session.
    std::vector<dbisam::Column> columns;
    try {
        auto client = OpenClient();
        auto resp = client.query_raw("SELECT * FROM \"" + name + "\" WHERE 1=0");
        auto [cols, _end] = dbisam::parse_schema(resp);
        columns = std::move(cols);
    } catch (const std::exception &) {
        // Treat as "table doesn't exist" — let DuckDB surface a clean
        // "table not found" rather than a wire-protocol error.
        return nullptr;
    }

    auto entry = BuildTableEntry(catalog_, name, columns);
    auto *raw = entry.get();
    entries_[name] = std::move(entry);
    return raw;
}

optional_ptr<CatalogEntry> DbisamTransaction::GetScanEntry(const std::string &name) {
    std::lock_guard<std::mutex> g(lock_);
    // Reuse a fully-probed entry if one already exists this transaction.
    auto it = entries_.find(name);
    if (it != entries_.end()) {
        return it->second.get();
    }
    auto sit = scan_entries_.find(name);
    if (sit != scan_entries_.end()) {
        return sit->second.get();
    }
    auto entry = BuildNameOnlyEntry(catalog_, name);
    auto *raw = entry.get();
    scan_entries_[name] = std::move(entry);
    return raw;
}

} // namespace duckdb
