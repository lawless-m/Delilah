// Render a DuckDB TableFilter as a DBISAM-compatible WHERE-clause fragment.
//
// Dialect choices follow MrsFlow's `Dbisam` Dialect (plan/fold.rs):
//   - identifiers double-quoted with embedded `"` doubled
//   - text literals single-quoted with embedded `'` doubled
//   - boolean as TRUE / FALSE
//   - date / timestamp as quoted string `'YYYY-MM-DD[ HH:MM:SS]'`
//     (DBISAM implicitly casts to the column type in comparison)
//
// Conservative: only filter shapes we can render exactly are pushed
// down. Unrecognised shapes return std::nullopt and DuckDB applies
// the filter post-fetch.

#pragma once

#include "duckdb/planner/table_filter.hpp"

#include <optional>
#include <string>

namespace duckdb {

// Quote an identifier per the DBISAM/Dibdog grammar — double quotes,
// embedded `"` doubled. Mixed-case and DBISAM-reserved names safely
// pass through this form. Shared by the filter renderer, the attached
// scan's SELECT builder, and the catalog's schema probe.
std::string QuoteDbisamIdent(const std::string &name);

// Render one filter as a DBISAM WHERE fragment over `column_name`.
// Returns std::nullopt if we can't represent it safely.
std::optional<std::string> RenderDbisamFilter(const TableFilter &filter,
                                              const std::string &column_name);

// Render a bound filter expression (generic expression pushdown /
// EXPRESSION_FILTER, e.g. `LEFT(col, 1) IN ('4', '6')`). Column refs
// render as `quoted_column` — generic pushdown guarantees exactly one
// source column. This doubles as the accept test for the scan's
// pushdown_expression callback: DuckDB ERASES the FILTER node when an
// expression is accepted, so acceptance is a hard obligation and the
// accept test and the scan-time renderer must be this same function.
std::optional<std::string> RenderDbisamExpression(const Expression &expr,
                                                  const std::string &quoted_column);

// Convenience: render the whole set, joined with AND, columns named
// via `column_names[column_id]`. Filters whose column_id is out of
// range OR whose filter shape we don't support are silently dropped
// (DuckDB applies them post-fetch — correctness preserved).
//
// `applied` receives the column-ids of filters we successfully pushed
// down, so the caller can tell DuckDB to skip them (if filter_prune
// is enabled). Empty result means no filters were pushable.
std::string RenderDbisamFilterSet(const TableFilterSet &filters,
                                  const std::vector<std::string> &column_names,
                                  std::vector<idx_t> &applied);

} // namespace duckdb
