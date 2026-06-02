// OptimizerExtension that pushes a constant LIMIT n down into
// `dbisam_attached_scan` bind data so the first ReadFirstRecordBlock
// request to the server is sized n (or limit+offset) instead of the
// default batch_size. DuckDB still applies its own LIMIT post-scan
// — this is purely a fetch-volume optimisation.
//
// Plan shape we recognise (after standard DuckDB optimisation):
//
//   LOGICAL_LIMIT (constant n)
//     [LOGICAL_PROJECTION | LOGICAL_FILTER]* (pass-through)
//       LOGICAL_GET (function.name == "dbisam_attached_scan")
//
// Anything else (expression-typed limit, OFFSET via expression, etc.)
// is left alone — DuckDB still gets correct results via post-fetch
// LIMIT, just with the default batch volume.

#include "dbisam/storage/dbisam_attached_scan_bind.hpp"

#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/bound_result_modifier.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"

#include <cstdlib>
#include <cstdio>

namespace duckdb {

namespace {

// Walk down through pass-through operators looking for the dbisam_attached_scan
// LogicalGet. Returns nullptr if the chain is broken by an unfriendly op.
DbisamAttachedScanBindData *find_attached_scan_bind(LogicalOperator &op) {
    if (op.type == LogicalOperatorType::LOGICAL_GET) {
        auto &get = op.Cast<LogicalGet>();
        if (get.function.name == "dbisam_attached_scan" && get.bind_data) {
            return dynamic_cast<DbisamAttachedScanBindData *>(get.bind_data.get());
        }
        return nullptr;
    }
    if (op.type == LogicalOperatorType::LOGICAL_PROJECTION
        || op.type == LogicalOperatorType::LOGICAL_FILTER) {
        if (!op.children.empty()) {
            return find_attached_scan_bind(*op.children[0]);
        }
    }
    return nullptr;
}

void walk(LogicalOperator &op) {
    if (op.type == LogicalOperatorType::LOGICAL_LIMIT && !op.children.empty()) {
        auto &lim = op.Cast<LogicalLimit>();
        if (lim.limit_val.Type() == LimitNodeType::CONSTANT_VALUE) {
            idx_t n = lim.limit_val.GetConstantValue();
            idx_t offset = 0;
            if (lim.offset_val.Type() == LimitNodeType::CONSTANT_VALUE) {
                offset = lim.offset_val.GetConstantValue();
            }
            // Cap is limit+offset because we need to fetch enough rows
            // for DuckDB to discard `offset` and emit `n`.
            idx_t cap = n + offset;
            if (auto *bind = find_attached_scan_bind(*op.children[0])) {
                // Only LOWER a previously-set hint; never raise it
                // (e.g. nested LIMITs in subqueries).
                if (bind->limit_hint == 0 || cap < bind->limit_hint) {
                    bind->limit_hint = cap;
                    if (std::getenv("DBISAM_SQL_DEBUG")) {
                        std::fprintf(stderr,
                                     "[dbisam-opt] LIMIT %zu pushed into dbisam_attached_scan bind\n",
                                     static_cast<size_t>(cap));
                    }
                }
            }
        }
    }
    for (auto &child : op.children) {
        if (child) walk(*child);
    }
}

void OptimizeLimitPushdown(OptimizerExtensionInput &, unique_ptr<LogicalOperator> &plan) {
    if (plan) walk(*plan);
}

} // namespace

void RegisterDbisamOptimizer(ExtensionLoader &loader) {
    auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
    OptimizerExtension ext;
    ext.optimize_function = OptimizeLimitPushdown;
    OptimizerExtension::Register(config, ext);
}

} // namespace duckdb
