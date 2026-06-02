// DbisamTableEntry::GetScanFunction returns a TableFunction whose bind
// captures the table name + transaction reference. At init time the
// scan reads `input.column_ids`, renders the projected SELECT, runs it
// via the shared transaction's Client, and buffers the result. Execute
// streams the buffer out chunk by chunk.

#include "dbisam/storage/dbisam_table_entry.hpp"
#include "dbisam/storage/dbisam_catalog.hpp"
#include "dbisam/storage/dbisam_transaction.hpp"

#include "dbisam/row.hpp"

#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/table_storage_info.hpp"

#include <variant>

namespace duckdb {

DbisamTableEntry::DbisamTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info)
    : TableCatalogEntry(catalog, schema, info) {}

unique_ptr<BaseStatistics> DbisamTableEntry::GetStatistics(ClientContext &, column_t) {
    return nullptr;
}

void DbisamTableEntry::BindUpdateConstraints(Binder &, LogicalGet &, LogicalProjection &,
                                              LogicalUpdate &, ClientContext &) {}

TableStorageInfo DbisamTableEntry::GetStorageInfo(ClientContext &) {
    TableStorageInfo info;
    info.cardinality = 10000; // unknown — DBISAM list_tables doesn't give counts
    return info;
}

namespace {

struct DbisamScanBindData : public TableFunctionData {
    DbisamTableEntry *table;
    std::vector<std::string> all_names;
    std::vector<LogicalType> all_types;
};

struct DbisamScanState : public GlobalTableFunctionState {
    std::vector<column_t> column_ids;
    // Per output column: index into `projected_columns`/row data,
    // or SIZE_MAX if this output column is a ROWID (emit row-counter).
    std::vector<size_t> output_to_projected;
    std::vector<dbisam::Column> projected_columns;
    std::vector<std::vector<dbisam::CellValue>> rows;
    idx_t row_offset = 0;
};

static unique_ptr<FunctionData> DbisamAttachedScanBind(ClientContext &,
                                                       TableFunctionBindInput &input,
                                                       vector<LogicalType> &return_types,
                                                       vector<string> &names) {
    auto bind = make_uniq<DbisamScanBindData>();
    bind->table = reinterpret_cast<DbisamTableEntry *>(input.inputs[0].GetPointer());
    auto &table = *bind->table;
    for (auto &col : table.GetColumns().Logical()) {
        bind->all_names.push_back(col.GetName());
        bind->all_types.push_back(col.GetType());
        names.push_back(col.GetName());
        return_types.push_back(col.GetType());
    }
    return std::move(bind);
}

// Quote an identifier per the DBISAM/Dibdog grammar — double quotes,
// embedded `"` doubled. Mixed-case and DBISAM-reserved names safely
// pass through this form.
static std::string QuoteIdent(const std::string &name) {
    std::string out = "\"";
    out.reserve(name.size() + 2);
    for (char c : name) {
        out += c;
        if (c == '"') out += '"';
    }
    out += '"';
    return out;
}

static unique_ptr<GlobalTableFunctionState>
DbisamAttachedScanInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
    auto &bind = input.bind_data->CastNoConst<DbisamScanBindData>();
    auto state = make_uniq<DbisamScanState>();
    state->column_ids = input.column_ids;

    // Two passes through column_ids:
    //   (a) build the SELECT — real columns only, in the order they
    //       appear in column_ids;
    //   (b) build output_to_projected — for each output column, the
    //       index into the projected response (or SIZE_MAX for ROWID).
    // DuckDB sends COLUMN_IDENTIFIER_ROW_ID (== UINT64_MAX) for queries
    // that only need the row count (e.g. count(*)); the output vector
    // is BIGINT and we just emit sequential row indices into it.
    std::vector<std::string> select_cols;
    state->output_to_projected.reserve(state->column_ids.size());
    for (auto cid : state->column_ids) {
        if (cid == COLUMN_IDENTIFIER_ROW_ID || cid >= bind.all_names.size()) {
            state->output_to_projected.push_back(SIZE_MAX);
            continue;
        }
        state->output_to_projected.push_back(select_cols.size());
        select_cols.push_back(QuoteIdent(bind.all_names[cid]));
    }
    // Always need at least one real column to drive the cursor and get
    // a row count. Pick column 0 if the request was rowid-only.
    bool driver_only = select_cols.empty();
    if (driver_only) {
        if (bind.all_names.empty()) {
            return std::move(state);
        }
        select_cols.push_back(QuoteIdent(bind.all_names[0]));
    }

    std::string sql = "SELECT ";
    for (size_t i = 0; i < select_cols.size(); ++i) {
        if (i) sql += ", ";
        sql += select_cols[i];
    }
    sql += " FROM " + QuoteIdent(bind.table->name);

    // Fresh Client per scan — native protocol desyncs when multiple
    // queries share a session (per ExportKing README).
    auto &txn = DbisamTransaction::Get(context, bind.table->catalog);
    auto client = txn.OpenClient();
    auto result = client.query_decoded(sql, 0);
    state->projected_columns = std::move(result.columns);
    state->rows = std::move(result.rows);
    return std::move(state);
}

static void WriteCell(Vector &out, idx_t row, dbisam::FieldType ft, const dbisam::CellValue &cell) {
    using FT = dbisam::FieldType;
    if (std::holds_alternative<dbisam::NullValue>(cell)) {
        FlatVector::SetNull(out, row, true);
        return;
    }
    switch (ft) {
    case FT::String:
    case FT::Memo:
    case FT::Calculated:
    case FT::Unknown: {
        if (auto *s = std::get_if<std::string>(&cell)) {
            FlatVector::GetData<string_t>(out)[row] = StringVector::AddString(out, *s);
        } else if (auto *b = std::get_if<std::vector<uint8_t>>(&cell)) {
            FlatVector::GetData<string_t>(out)[row] = StringVector::AddString(
                out, const_char_ptr_cast(b->data()), b->size());
        } else {
            FlatVector::SetNull(out, row, true);
        }
        return;
    }
    case FT::Date: {
        if (auto *v = std::get_if<int32_t>(&cell)) {
            FlatVector::GetData<date_t>(out)[row] = date_t(*v);
        } else FlatVector::SetNull(out, row, true);
        return;
    }
    case FT::DateTime: {
        if (auto *v = std::get_if<int64_t>(&cell)) {
            FlatVector::GetData<timestamp_t>(out)[row] = timestamp_t(*v);
        } else FlatVector::SetNull(out, row, true);
        return;
    }
    case FT::Time: {
        if (auto *v = std::get_if<int64_t>(&cell)) {
            FlatVector::GetData<dtime_t>(out)[row] = dtime_t(*v);
        } else FlatVector::SetNull(out, row, true);
        return;
    }
    case FT::Integer:
    case FT::AutoInc:
    case FT::Smallint: {
        if (auto *v = std::get_if<int32_t>(&cell)) {
            FlatVector::GetData<int32_t>(out)[row] = *v;
        } else FlatVector::SetNull(out, row, true);
        return;
    }
    case FT::Largeint: {
        if (auto *v = std::get_if<int64_t>(&cell)) {
            FlatVector::GetData<int64_t>(out)[row] = *v;
        } else FlatVector::SetNull(out, row, true);
        return;
    }
    case FT::Boolean: {
        if (auto *v = std::get_if<bool>(&cell)) {
            FlatVector::GetData<bool>(out)[row] = *v;
        } else FlatVector::SetNull(out, row, true);
        return;
    }
    case FT::Float:
    case FT::Currency: {
        if (auto *v = std::get_if<double>(&cell)) {
            FlatVector::GetData<double>(out)[row] = *v;
        } else FlatVector::SetNull(out, row, true);
        return;
    }
    case FT::Blob:
    case FT::Graphic:
    case FT::Bytes:
    case FT::VarBytes: {
        if (auto *b = std::get_if<std::vector<uint8_t>>(&cell)) {
            FlatVector::GetData<string_t>(out)[row] = StringVector::AddStringOrBlob(
                out, const_char_ptr_cast(b->data()), b->size());
        } else if (auto *h = std::get_if<dbisam::BlobHandle>(&cell)) {
            FlatVector::GetData<string_t>(out)[row] = StringVector::AddStringOrBlob(
                out, const_char_ptr_cast(h->bytes.data()), h->bytes.size());
        } else FlatVector::SetNull(out, row, true);
        return;
    }
    }
}

static void DbisamAttachedScanExecute(ClientContext &, TableFunctionInput &input, DataChunk &output) {
    auto &bind = input.bind_data->Cast<DbisamScanBindData>();
    auto &state = input.global_state->Cast<DbisamScanState>();
    (void)bind;

    idx_t total = state.rows.size();
    if (state.row_offset >= total) {
        output.SetCardinality(0);
        return;
    }
    idx_t emit = std::min<idx_t>(STANDARD_VECTOR_SIZE, total - state.row_offset);

    // Each output column is either a ROWID (emit sequential int64) or
    // maps to a projected response column via output_to_projected[outcol].
    for (idx_t outcol = 0; outcol < output.ColumnCount(); ++outcol) {
        auto &out = output.data[outcol];
        out.SetVectorType(VectorType::FLAT_VECTOR);
        size_t projected_idx = outcol < state.output_to_projected.size()
                                   ? state.output_to_projected[outcol]
                                   : SIZE_MAX;
        if (projected_idx == SIZE_MAX) {
            // ROWID — emit row counter as int64.
            auto data = FlatVector::GetData<int64_t>(out);
            for (idx_t r = 0; r < emit; ++r) {
                data[r] = static_cast<int64_t>(state.row_offset + r);
            }
            continue;
        }
        if (projected_idx >= state.projected_columns.size()) {
            for (idx_t r = 0; r < emit; ++r) FlatVector::SetNull(out, r, true);
            continue;
        }
        const auto ft = state.projected_columns[projected_idx].field_type;
        for (idx_t r = 0; r < emit; ++r) {
            const auto &cell = state.rows[state.row_offset + r][projected_idx];
            WriteCell(out, r, ft, cell);
        }
    }
    output.SetCardinality(emit);
    state.row_offset += emit;
}

} // namespace

TableFunction DbisamTableEntry::GetScanFunction(ClientContext &, unique_ptr<FunctionData> &bind_data) {
    TableFunction tf("dbisam_attached_scan", {LogicalType::POINTER},
                     DbisamAttachedScanExecute, DbisamAttachedScanBind,
                     DbisamAttachedScanInitGlobal);
    tf.projection_pushdown = true;

    // Pre-populate bind_data so the planner can request statistics etc.
    // before re-binding. The actual bind callback re-derives the same
    // information from `input.inputs[0]` (a POINTER to `this`).
    auto bind = make_uniq<DbisamScanBindData>();
    bind->table = this;
    for (auto &col : GetColumns().Logical()) {
        bind->all_names.push_back(col.GetName());
        bind->all_types.push_back(col.GetType());
    }
    bind_data = std::move(bind);

    return tf;
}

} // namespace duckdb
