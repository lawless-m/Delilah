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
};

} // namespace duckdb
