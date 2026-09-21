// Unit tests for the TableFilter -> DBISAM WHERE renderer. Links
// duckdb_static (for the TableFilter classes) but needs no server.

#include "dbisam/storage/dbisam_filter_render.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/dynamic_filter.hpp"
#include "duckdb/planner/filter/optional_filter.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace duckdb;

static int g_failures = 0;

#define CHECK(cond) do {                                                       \
    if (!(cond)) {                                                             \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        ++g_failures;                                                          \
    }                                                                          \
} while (0)

#define CHECK_THROWS(stmt) do {                                                \
    bool _threw = false;                                                       \
    try { (void)(stmt); }                                                      \
    catch (const std::exception &) { _threw = true; }                          \
    if (!_threw) {                                                             \
        std::fprintf(stderr, "FAIL: expected throw: %s (%s:%d)\n",             \
                     #stmt, __FILE__, __LINE__);                               \
        ++g_failures;                                                          \
    }                                                                          \
} while (0)

static unique_ptr<TableFilter> Cmp(ExpressionType op, Value v) {
    return make_uniq<ConstantFilter>(op, std::move(v));
}

static unique_ptr<TableFilter> OptionalDynamic() {
    return make_uniq<OptionalFilter>(make_uniq<DynamicFilter>());
}

// The shape DuckDB builds for `WHERE CODE LIKE 'BPC-%'`.
static unique_ptr<ConjunctionAndFilter> PrefixRange() {
    auto f = make_uniq<ConjunctionAndFilter>();
    f->child_filters.push_back(Cmp(ExpressionType::COMPARE_GREATERTHANOREQUALTO, Value("BPC-")));
    f->child_filters.push_back(Cmp(ExpressionType::COMPARE_LESSTHAN, Value("BPC.")));
    return f;
}

static std::string RenderSet(unique_ptr<TableFilter> filter) {
    TableFilterSet set;
    set.filters[0] = std::move(filter);
    std::vector<idx_t> applied;
    return RenderDbisamFilterSet(set, {"CODE"}, applied);
}

static void like_prefix_range_renders() {
    CHECK(RenderSet(PrefixRange()) == "(\"CODE\" >= 'BPC-' AND \"CODE\" < 'BPC.')");
}

// Regression: `WHERE CODE LIKE 'BPC-%' ORDER BY code DESC LIMIT 30`.
// Top-N ANDs an optional Dynamic Filter onto the mandatory range; the
// unrenderable optional child used to sink the whole AND group and the
// scan ran with no WHERE, returning unfiltered rows.
static void topn_dynamic_filter_does_not_drop_range() {
    auto f = PrefixRange();
    f->child_filters.push_back(OptionalDynamic());
    CHECK(RenderSet(std::move(f)) == "(\"CODE\" >= 'BPC-' AND \"CODE\" < 'BPC.')");
}

static void lone_optional_dynamic_filter_is_skipped() {
    CHECK(RenderSet(OptionalDynamic()).empty());
    auto f = make_uniq<ConjunctionAndFilter>();
    f->child_filters.push_back(OptionalDynamic());
    CHECK(RenderSet(std::move(f)).empty());
}

// DuckDB does not post-filter pushed filters, so an unrenderable
// mandatory filter must throw rather than return unfiltered rows.
static void unrenderable_mandatory_filter_throws() {
    CHECK_THROWS(RenderSet(Cmp(ExpressionType::COMPARE_EQUAL, Value::TIME(dtime_t(0)))));
    auto f = PrefixRange();
    f->child_filters.push_back(Cmp(ExpressionType::COMPARE_EQUAL, Value::TIME(dtime_t(0))));
    CHECK_THROWS(RenderSet(std::move(f)));
}

int main() {
    like_prefix_range_renders();
    topn_dynamic_filter_does_not_drop_range();
    lone_optional_dynamic_filter_is_skipped();
    unrenderable_mandatory_filter_throws();
    if (g_failures == 0) {
        std::printf("all filter-render tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d failures\n", g_failures);
    return 1;
}
