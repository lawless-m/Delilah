// Read-only Catalog backed by a remote DBISAM Exportmaster session.
// Mirrors the duckdb-sqlite SQLiteCatalog shape but rejects all
// write/DDL paths since DBISAM access here is SELECT-only.

#pragma once

#include "dbisam/client.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/case_insensitive_map.hpp"

#include <mutex>
#include <vector>

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

    // --- Session-scoped schema cache (shared across this ATTACH's
    // per-statement transactions). The DbisamTransaction methods delegate
    // here. All are mutex-guarded; entries reference this catalog + its
    // main schema, both of which outlive every statement. ---

    // Open a fresh, short-lived Client against the server.
    dbisam::Client OpenClient();

    // Table names (list_tables), cached after first call.
    const std::vector<std::string> &GetTableNames();

    // Full, schema-probed entry for `name` (the query/LookupEntry path).
    // Probes `SELECT * WHERE 1=0` on first request, then caches. Returns
    // nullptr if the table doesn't exist.
    optional_ptr<CatalogEntry> GetTableEntry(const std::string &name);

    // Entry for catalog enumeration (Scan / SHOW TABLES). In lazy mode
    // (default) this is a name-only entry — no schema probe. In eager mode
    // EnsureEagerLoaded() has already populated full entries, so this just
    // returns the full one.
    optional_ptr<CatalogEntry> GetScanEntry(const std::string &name);

    // Eager mode only: probe every table's schema once (serial — the
    // server rejects concurrent login storms) and cache it for the
    // session. No-op after the first call. No-op entirely when
    // opts.eager_schema is false.
    void EnsureEagerLoaded();

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

    std::mutex schema_mutex_;
    bool tables_loaded_ = false;
    std::vector<std::string> table_names_;
    bool eager_loaded_ = false;
    case_insensitive_map_t<unique_ptr<CatalogEntry>> entries_;      // full, schema-probed
    case_insensitive_map_t<unique_ptr<CatalogEntry>> scan_entries_; // name-only (lazy enum)
};

} // namespace duckdb
