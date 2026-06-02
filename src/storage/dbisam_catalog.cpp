#include "dbisam/storage/dbisam_catalog.hpp"
#include "dbisam/storage/dbisam_schema_entry.hpp"
#include "dbisam/storage/dbisam_table_entry.hpp"

#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/parser/column_definition.hpp"
#include "duckdb/parser/column_list.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/storage/database_size.hpp"

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

// Name-only entry — no columns, no probe. Satisfies catalog enumeration
// (SHOW TABLES reads only the name) in lazy mode.
unique_ptr<CatalogEntry> BuildNameOnlyEntry(DbisamCatalog &catalog, const std::string &name) {
    CreateTableInfo info(catalog.GetMainSchema(), name);
    return make_uniq<DbisamTableEntry>(catalog, catalog.GetMainSchema(), info,
                                       std::vector<dbisam::FieldType>{});
}

} // namespace

DbisamCatalog::DbisamCatalog(AttachedDatabase &db, std::string path_p, dbisam::ConnOpts opts_p)
    : Catalog(db), path(std::move(path_p)), opts(std::move(opts_p)) {}

DbisamCatalog::~DbisamCatalog() = default;

void DbisamCatalog::Initialize(bool /*load_builtin*/) {
    CreateSchemaInfo info;
    main_schema_ = make_uniq<DbisamSchemaEntry>(*this, info);
}

dbisam::Client DbisamCatalog::OpenClient() {
    return dbisam::Client::connect_and_login(opts);
}

const std::vector<std::string> &DbisamCatalog::GetTableNames() {
    std::lock_guard<std::mutex> g(schema_mutex_);
    if (!tables_loaded_) {
        auto client = OpenClient();
        table_names_ = client.list_tables();
        tables_loaded_ = true;
    }
    return table_names_;
}

optional_ptr<CatalogEntry> DbisamCatalog::GetTableEntry(const std::string &name) {
    std::lock_guard<std::mutex> g(schema_mutex_);
    auto it = entries_.find(name);
    if (it != entries_.end()) {
        return it->second.get();
    }

    // Schema probe — `SELECT * FROM "<tbl>" WHERE 1=0` returns the schema
    // response. PrepareStatement alone carries the schema blocks, so
    // query_raw + parse_schema is one round-trip vs ~5 for the full
    // decoded path. Double-quoted identifiers are accepted per the Dibdog
    // grammar, keeping mixed-case / special-char table names safe. Fresh
    // connection per probe — the native protocol desyncs when multiple
    // queries share a session.
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

    auto entry = BuildTableEntry(*this, name, columns);
    auto *raw = entry.get();
    entries_[name] = std::move(entry);
    return raw;
}

optional_ptr<CatalogEntry> DbisamCatalog::GetScanEntry(const std::string &name) {
    std::lock_guard<std::mutex> g(schema_mutex_);
    // Prefer a fully-probed entry if one already exists (eager mode, or a
    // table queried earlier this session).
    auto it = entries_.find(name);
    if (it != entries_.end()) {
        return it->second.get();
    }
    auto sit = scan_entries_.find(name);
    if (sit != scan_entries_.end()) {
        return sit->second.get();
    }
    auto entry = BuildNameOnlyEntry(*this, name);
    auto *raw = entry.get();
    scan_entries_[name] = std::move(entry);
    return raw;
}

void DbisamCatalog::EnsureEagerLoaded() {
    if (!opts.eager_schema) {
        return;
    }
    {
        std::lock_guard<std::mutex> g(schema_mutex_);
        if (eager_loaded_) {
            return;
        }
    }
    // GetTableNames()/GetTableEntry() take schema_mutex_ themselves, so
    // probe outside the lock. GetTableEntry serially probes + caches each
    // table into entries_ (serial: the server rejects concurrent logins).
    for (const auto &name : GetTableNames()) {
        GetTableEntry(name);
    }
    std::lock_guard<std::mutex> g(schema_mutex_);
    eager_loaded_ = true;
}

optional_ptr<CatalogEntry> DbisamCatalog::CreateSchema(CatalogTransaction, CreateSchemaInfo &) {
    throw BinderException("DBISAM catalogs do not support creating schemas (read-only)");
}

void DbisamCatalog::ScanSchemas(ClientContext &, std::function<void(SchemaCatalogEntry &)> callback) {
    callback(*main_schema_);
}

optional_ptr<SchemaCatalogEntry>
DbisamCatalog::LookupSchema(CatalogTransaction, const EntryLookupInfo &schema_lookup,
                            OnEntryNotFound if_not_found) {
    auto &name = schema_lookup.GetEntryName();
    if (name == DEFAULT_SCHEMA || name == INVALID_SCHEMA) {
        return main_schema_.get();
    }
    if (if_not_found == OnEntryNotFound::RETURN_NULL) {
        return nullptr;
    }
    throw BinderException("DBISAM catalogs only have a single schema \"%s\"", std::string(DEFAULT_SCHEMA));
}

PhysicalOperator &DbisamCatalog::PlanCreateTableAs(ClientContext &, PhysicalPlanGenerator &,
                                                   LogicalCreateTable &, PhysicalOperator &) {
    throw BinderException("DBISAM catalogs are read-only (no CREATE TABLE)");
}

PhysicalOperator &DbisamCatalog::PlanInsert(ClientContext &, PhysicalPlanGenerator &,
                                            LogicalInsert &, optional_ptr<PhysicalOperator>) {
    throw BinderException("DBISAM catalogs are read-only (no INSERT)");
}

PhysicalOperator &DbisamCatalog::PlanDelete(ClientContext &, PhysicalPlanGenerator &,
                                            LogicalDelete &, PhysicalOperator &) {
    throw BinderException("DBISAM catalogs are read-only (no DELETE)");
}

PhysicalOperator &DbisamCatalog::PlanUpdate(ClientContext &, PhysicalPlanGenerator &,
                                            LogicalUpdate &, PhysicalOperator &) {
    throw BinderException("DBISAM catalogs are read-only (no UPDATE)");
}

unique_ptr<LogicalOperator> DbisamCatalog::BindCreateIndex(Binder &, CreateStatement &,
                                                            TableCatalogEntry &,
                                                            unique_ptr<LogicalOperator>) {
    throw BinderException("DBISAM catalogs are read-only (no CREATE INDEX)");
}

DatabaseSize DbisamCatalog::GetDatabaseSize(ClientContext &) {
    DatabaseSize result;
    result.total_blocks = 0;
    result.block_size = 0;
    result.free_blocks = 0;
    result.used_blocks = 0;
    result.bytes = 0;
    result.wal_size = idx_t(-1);
    return result;
}

void DbisamCatalog::DropSchema(ClientContext &, DropInfo &) {
    throw BinderException("DBISAM catalogs do not support dropping schemas (read-only)");
}

} // namespace duckdb
