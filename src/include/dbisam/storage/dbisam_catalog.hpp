// Read-only Catalog backed by a remote DBISAM Exportmaster session.
// Mirrors the duckdb-sqlite SQLiteCatalog shape but rejects all
// write/DDL paths since DBISAM access here is SELECT-only.

#pragma once

#include "dbisam/client.hpp"

#include "duckdb/catalog/catalog.hpp"

namespace duckdb {

class DbisamSchemaEntry;

class DbisamCatalog : public Catalog {
public:
    DbisamCatalog(AttachedDatabase &db, std::string path, dbisam::ConnOpts opts);
    ~DbisamCatalog() override;

    std::string path;
    dbisam::ConnOpts opts;

public:
    void Initialize(bool load_builtin) override;
    string GetCatalogType() override { return "dbisam"; }

    optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) override;
    void ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) override;
    optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction,
                                                  const EntryLookupInfo &schema_lookup,
                                                  OnEntryNotFound if_not_found) override;

    DbisamSchemaEntry &GetMainSchema() { return *main_schema_; }

    // Write-side planning — DBISAM is SELECT-only here.
    PhysicalOperator &PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner,
                                        LogicalCreateTable &op, PhysicalOperator &plan) override;
    PhysicalOperator &PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner,
                                 LogicalInsert &op, optional_ptr<PhysicalOperator> plan) override;
    PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner,
                                 LogicalDelete &op, PhysicalOperator &plan) override;
    PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner,
                                 LogicalUpdate &op, PhysicalOperator &plan) override;
    unique_ptr<LogicalOperator> BindCreateIndex(Binder &binder, CreateStatement &stmt,
                                                TableCatalogEntry &table,
                                                unique_ptr<LogicalOperator> plan) override;

    DatabaseSize GetDatabaseSize(ClientContext &context) override;
    bool InMemory() override { return false; }
    string GetDBPath() override { return path; }

private:
    void DropSchema(ClientContext &context, DropInfo &info) override;
    unique_ptr<DbisamSchemaEntry> main_schema_;
};

} // namespace duckdb
