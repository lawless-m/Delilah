#include "dbisam/storage/dbisam_catalog.hpp"
#include "dbisam/storage/dbisam_schema_entry.hpp"

#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/storage/database_size.hpp"

namespace duckdb {

DbisamCatalog::DbisamCatalog(AttachedDatabase &db, std::string path_p, dbisam::ConnOpts opts_p)
    : Catalog(db), path(std::move(path_p)), opts(std::move(opts_p)) {}

DbisamCatalog::~DbisamCatalog() = default;

void DbisamCatalog::Initialize(bool /*load_builtin*/) {
    CreateSchemaInfo info;
    main_schema_ = make_uniq<DbisamSchemaEntry>(*this, info);
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
