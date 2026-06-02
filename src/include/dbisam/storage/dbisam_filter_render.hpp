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

// Render one filter as a DBISAM WHERE fragment over `column_name`.
// Returns std::nullopt if we can't represent it safely.
std::optional<std::string> RenderDbisamFilter(const TableFilter &filter,
                                              const std::string &column_name);

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
