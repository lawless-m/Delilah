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

    // Schema probe — `SELECT * FROM "<tbl>" WHERE 1=0` returns the
    // schema response. PrepareStatement alone carries the schema
    // blocks; no need to drive the cursor for a row-less result, so
    // query_raw + parse_schema is one round-trip vs ~5 for the
    // full query_decoded path. Double-quoted identifiers are
    // accepted per the Dibdog grammar (`identifier//1` production),
    // keeping mixed-case / special-char table names safe.
    //
    // Fresh connection per probe — the native protocol desyncs when
    // multiple queries share a session.
    std::string probe = "SELECT * FROM \"" + name + "\" WHERE 1=0";
    std::vector<dbisam::Column> columns;
    try {
        auto client = OpenClient();
        auto resp = client.query_raw(probe);
        auto [cols, _end] = dbisam::parse_schema(resp);
        columns = std::move(cols);
    } catch (const std::exception &) {
        // Treat as "table doesn't exist" — let DuckDB surface a clean
        // "table not found" rather than a wire-protocol error.
        return nullptr;
    }

    CreateTableInfo info(catalog_.GetMainSchema(), name);
    std::vector<dbisam::FieldType> field_types;
    field_types.reserve(columns.size());
    for (const auto &col : columns) {
        info.columns.AddColumn(ColumnDefinition(col.name, MapFieldType(col.field_type)));
        field_types.push_back(col.field_type);
    }
    auto entry = make_uniq<DbisamTableEntry>(catalog_, catalog_.GetMainSchema(), info,
                                             std::move(field_types));
    auto *raw = entry.get();
    entries_[name] = std::move(entry);
    return raw;
}

} // namespace duckdb
