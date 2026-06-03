// Bind data for the attached scan function (`dbisam_attached_scan`).
//
// Lives in a header (not the anonymous namespace inside
// dbisam_table_entry.cpp) so the OptimizerExtension can dynamic_cast
// against it to push a LIMIT into `limit_hint` at plan-rewrite time.

#pragma once

#include "dbisam/schema.hpp"

#include "duckdb/function/table_function.hpp"

#include <string>
#include <vector>

namespace duckdb {

class DbisamTableEntry;

struct DbisamAttachedScanBindData : public TableFunctionData {
    DbisamTableEntry *table = nullptr;
    std::vector<std::string> all_names;
    std::vector<LogicalType> all_types;
    std::vector<dbisam::FieldType> all_field_types;

    // Set by the OptimizerExtension when it sees a LIMIT n that can be
    // pushed into the scan. 0 = no hint (fetch all). InitGlobal reads
    // this and shrinks the cursor's request batch size accordingly.
    // mutable because bind data is conceptually const after Bind, but
    // the optimizer runs later and needs to write here.
    mutable idx_t limit_hint = 0;

    // True when limit_hint may be emitted as a hard server-side `TOP n`
    // in the DBISAM SQL (so the server stops preparing the result early
    // — critical on large tables). Only safe when NO residual filter sits
    // between the LIMIT and the scan: a residual filter discards rows
    // after the scan, so a hard cap could starve DuckDB of rows it still
    // needs. False = batch-size hint only (always correct, just slower).
    mutable bool limit_hard_cap = false;
};

} // namespace duckdb
