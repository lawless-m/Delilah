// DBISAM NULL semantics audit (verified live against YOURHOST):
//
//   shape                  DBISAM         ANSI 3VL    safe to push as-is?
//   col =  x               excludes NULL  excludes    YES
//   col <> x               INCLUDES NULL  excludes    NO  — must wrap with "AND col IS NOT NULL"
//   col <  x   col <= x    excludes NULL  excludes    YES
//   col >  x   col >= x    excludes NULL  excludes    YES
//   col IN (...)           excludes NULL  excludes    YES
//   col NOT IN (...)       excludes NULL  excludes    YES
//   col IS NULL            matches NULL   matches     YES
//   col IS NOT NULL        excludes NULL  excludes    YES
//
// The divergence is `<>` only: DBISAM treats `NULL <> x` as TRUE
// (treating NULL as "some other value"); ANSI says NULL — row excluded.
// MrsFlow's Dbisam dialect flags the broader behaviour via
// `null_equals_null() -> true`. Verified on CUSTOMER.EVCUSTOMER (1 NULL
// row, 9554 total): `count(*) WHERE EVCUSTOMER <> TRUE` returned 6009
// instead of the ANSI-correct 6008. Compensation here renders `<>` as
// `(col <> x AND col IS NOT NULL)` so DBISAM applies ANSI semantics.
//
// NULL literals on either side of any comparison are never pushed
// (RenderValue refuses NULL); callers must use IS [NOT] NULL instead.

#include "dbisam/storage/dbisam_filter_render.hpp"

#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/in_filter.hpp"
#include "duckdb/planner/filter/null_filter.hpp"
#include "duckdb/planner/filter/optional_filter.hpp"

#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace duckdb {

// Per MrsFlow Dbisam dialect: bare for `[a-zA-Z_][a-zA-Z0-9_]*`, else
// double-quoted with embedded `"` doubled. We always emit quoted form
// for safety — Dibdog grammar accepts both, and bare-vs-quoted is
// semantically identical in this position.
std::string QuoteDbisamIdent(const std::string &name) {
    std::string out;
    out.reserve(name.size() + 2);
    out.push_back('"');
    for (char c : name) {
        out.push_back(c);
        if (c == '"') out.push_back('"');
    }
    out.push_back('"');
    return out;
}

namespace {

std::string QuoteText(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s) {
        out.push_back(c);
        if (c == '\'') out.push_back('\'');
    }
    out.push_back('\'');
    return out;
}

// Render a DuckDB Value as a DBISAM literal. nullopt if we can't.
std::optional<std::string> RenderValue(const Value &v) {
    if (v.IsNull()) {
        // Caller should special-case IS NULL; NULL inside a comparison
        // changes semantics and we don't generate that.
        return std::nullopt;
    }
    auto &t = v.type();
    switch (t.id()) {
    case LogicalTypeId::BOOLEAN:
        return v.GetValue<bool>() ? std::string("TRUE") : std::string("FALSE");
    case LogicalTypeId::TINYINT:
    case LogicalTypeId::SMALLINT:
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::BIGINT:
    case LogicalTypeId::HUGEINT:
    case LogicalTypeId::UTINYINT:
    case LogicalTypeId::USMALLINT:
    case LogicalTypeId::UINTEGER:
    case LogicalTypeId::UBIGINT:
    case LogicalTypeId::FLOAT:
    case LogicalTypeId::DOUBLE:
    case LogicalTypeId::DECIMAL:
        return v.ToString();
    case LogicalTypeId::VARCHAR:
        return QuoteText(StringValue::Get(v));
    case LogicalTypeId::DATE: {
        // DBISAM accepts a quoted 'YYYY-MM-DD' string in a comparison.
        date_t d = v.GetValue<date_t>();
        int32_t y, m, day;
        Date::Convert(d, y, m, day);
        std::ostringstream s;
        s << '\'' << y << '-';
        if (m < 10) s << '0';
        s << m << '-';
        if (day < 10) s << '0';
        s << day << '\'';
        return s.str();
    }
    case LogicalTypeId::TIMESTAMP: {
        timestamp_t ts = v.GetValue<timestamp_t>();
        date_t d = Timestamp::GetDate(ts);
        dtime_t t = Timestamp::GetTime(ts);
        int32_t y, mo, dd;
        Date::Convert(d, y, mo, dd);
        int32_t hr, mi, se, us;
        Time::Convert(t, hr, mi, se, us);
        std::ostringstream s;
        s << '\'' << y << '-';
        if (mo < 10) s << '0';
        s << mo << '-';
        if (dd < 10) s << '0';
        s << dd << ' ';
        if (hr < 10) s << '0';
        s << hr << ':';
        if (mi < 10) s << '0';
        s << mi << ':';
        if (se < 10) s << '0';
        s << se << '\'';
        return s.str();
    }
    default:
        return std::nullopt;
    }
}

std::optional<const char *> ComparisonOp(ExpressionType t) {
    switch (t) {
    case ExpressionType::COMPARE_EQUAL:              return "=";
    case ExpressionType::COMPARE_NOTEQUAL:           return "<>";
    case ExpressionType::COMPARE_LESSTHAN:           return "<";
    case ExpressionType::COMPARE_GREATERTHAN:        return ">";
    case ExpressionType::COMPARE_LESSTHANOREQUALTO:  return "<=";
    case ExpressionType::COMPARE_GREATERTHANOREQUALTO: return ">=";
    default: return std::nullopt;
    }
}

} // namespace

std::optional<std::string> RenderDbisamFilter(const TableFilter &filter,
                                              const std::string &column_name) {
    auto qcol = QuoteDbisamIdent(column_name);
    switch (filter.filter_type) {
    case TableFilterType::IS_NULL:
        return qcol + " IS NULL";
    case TableFilterType::IS_NOT_NULL:
        return qcol + " IS NOT NULL";
    case TableFilterType::CONSTANT_COMPARISON: {
        const auto &cf = filter.Cast<ConstantFilter>();
        auto op = ComparisonOp(cf.comparison_type);
        if (!op) return std::nullopt;
        auto lit = RenderValue(cf.constant);
        if (!lit) return std::nullopt;
        // DBISAM NULL divergence — see top-of-file audit table. `<>`
        // includes NULL rows; wrap to enforce ANSI semantics.
        if (cf.comparison_type == ExpressionType::COMPARE_NOTEQUAL) {
            return "(" + qcol + " " + *op + " " + *lit + " AND " + qcol + " IS NOT NULL)";
        }
        return qcol + " " + *op + " " + *lit;
    }
    case TableFilterType::CONJUNCTION_AND: {
        const auto &cf = filter.Cast<ConjunctionAndFilter>();
        std::string out = "(";
        bool first = true;
        for (auto &child : cf.child_filters) {
            auto rendered = RenderDbisamFilter(*child, column_name);
            if (!rendered) return std::nullopt; // all-or-nothing for an AND group
            if (!first) out += " AND ";
            out += *rendered;
            first = false;
        }
        out += ")";
        return out;
    }
    case TableFilterType::OPTIONAL_FILTER: {
        // DuckDB wraps several pushdown-eligible shapes in OPTIONAL_FILTER
        // — observed live for multi-element IN-lists. The wrapper just
        // says "DuckDB will fall back to its own evaluation if you skip
        // this"; render the child if we can.
        const auto &of = filter.Cast<OptionalFilter>();
        if (!of.child_filter) return std::nullopt;
        return RenderDbisamFilter(*of.child_filter, column_name);
    }
    case TableFilterType::IN_FILTER: {
        const auto &inf = filter.Cast<InFilter>();
        if (inf.values.empty()) return std::nullopt;
        std::string out = qcol + " IN (";
        bool first = true;
        for (const auto &val : inf.values) {
            auto lit = RenderValue(val);
            if (!lit) return std::nullopt; // all-or-nothing
            if (!first) out += ", ";
            out += *lit;
            first = false;
        }
        out += ")";
        return out;
    }
    case TableFilterType::CONJUNCTION_OR: {
        const auto &cf = filter.Cast<ConjunctionOrFilter>();
        std::string out = "(";
        bool first = true;
        for (auto &child : cf.child_filters) {
            auto rendered = RenderDbisamFilter(*child, column_name);
            if (!rendered) return std::nullopt;
            if (!first) out += " OR ";
            out += *rendered;
            first = false;
        }
        out += ")";
        return out;
    }
    default:
        return std::nullopt;
    }
}

// String form of TableFilterType for diagnostics. Just the cases we
// recognise in the renderer; everything else falls through.
static const char *FilterTypeName(TableFilterType t) {
    switch (t) {
    case TableFilterType::CONSTANT_COMPARISON: return "CONSTANT_COMPARISON";
    case TableFilterType::IS_NULL:             return "IS_NULL";
    case TableFilterType::IS_NOT_NULL:         return "IS_NOT_NULL";
    case TableFilterType::CONJUNCTION_OR:      return "CONJUNCTION_OR";
    case TableFilterType::CONJUNCTION_AND:     return "CONJUNCTION_AND";
    case TableFilterType::STRUCT_EXTRACT:      return "STRUCT_EXTRACT";
    case TableFilterType::OPTIONAL_FILTER:     return "OPTIONAL_FILTER";
    case TableFilterType::IN_FILTER:           return "IN_FILTER";
    case TableFilterType::DYNAMIC_FILTER:      return "DYNAMIC_FILTER";
    case TableFilterType::EXPRESSION_FILTER:   return "EXPRESSION_FILTER";
    case TableFilterType::BLOOM_FILTER:        return "BLOOM_FILTER";
    default:                                   return "<unknown>";
    }
}

std::string RenderDbisamFilterSet(const TableFilterSet &filters,
                                  const std::vector<std::string> &column_names,
                                  std::vector<idx_t> &applied) {
    std::string out;
    bool debug = std::getenv("DBISAM_FILTER_DEBUG") != nullptr;
    for (auto &entry : filters.filters) {
        idx_t col_idx = entry.first;
        if (debug) {
            const char *colname = col_idx < column_names.size()
                                      ? column_names[col_idx].c_str()
                                      : "<oob>";
            std::fprintf(stderr, "[dbisam-filter] col=%zu (%s) type=%s\n",
                         col_idx, colname, FilterTypeName(entry.second->filter_type));
        }
        if (col_idx >= column_names.size()) continue;
        auto rendered = RenderDbisamFilter(*entry.second, column_names[col_idx]);
        if (!rendered) {
            if (debug) std::fprintf(stderr, "[dbisam-filter]   -> not rendered (unsupported shape)\n");
            continue;
        }
        if (debug) std::fprintf(stderr, "[dbisam-filter]   -> %s\n", rendered->c_str());
        if (!out.empty()) out += " AND ";
        out += *rendered;
        applied.push_back(col_idx);
    }
    return out;
}

} // namespace duckdb
