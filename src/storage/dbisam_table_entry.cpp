// DbisamTableEntry::GetScanFunction returns a TableFunction whose bind
// captures the table name + transaction reference. At init time the
// scan reads `input.column_ids`, renders the projected SELECT, runs it
// via the shared transaction's Client, and buffers the result. Execute
// streams the buffer out chunk by chunk.

#include "dbisam/storage/dbisam_table_entry.hpp"
#include "dbisam/storage/dbisam_attached_scan_bind.hpp"
#include "dbisam/storage/dbisam_catalog.hpp"
#include "dbisam/storage/dbisam_filter_render.hpp"
#include "dbisam/storage/dbisam_transaction.hpp"

#include "dbisam/cursor.hpp"
#include "dbisam/row.hpp"
#include "dbisam/text.hpp"

#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/table_storage_info.hpp"

#include <cstdio>
#include <cstdlib>
#include <variant>

namespace duckdb {

DbisamTableEntry::DbisamTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
                                   std::vector<dbisam::FieldType> field_types)
    : TableCatalogEntry(catalog, schema, info),
      source_field_types(std::move(field_types)) {}

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

// Bind data lives in dbisam_attached_scan_bind.hpp so the
// OptimizerExtension can see the type. `using` here just keeps the
// internal references short.
using DbisamScanBindData = DbisamAttachedScanBindData;

struct DbisamScanState : public GlobalTableFunctionState {
    std::vector<column_t> column_ids;
    // Per output column: index into `projected_columns`/row data,
    // or SIZE_MAX if this output column is a ROWID (emit row-counter).
    std::vector<size_t> output_to_projected;
    std::vector<dbisam::Column> projected_columns;

    // Streaming state — keep the Client (transport) alive for as long
    // as we're scanning, and pull RecordBlocks lazily. When DuckDB
    // hits its LIMIT and stops calling Execute, this state is
    // destroyed and the CursorRunner destructor sends cleanup.
    std::unique_ptr<dbisam::Client> client;
    std::unique_ptr<dbisam::CursorRunner> cursor;

    // Most-recently-decoded batch (rows + blobs already resolved by
    // Client::decode_batch_with_blobs). We emit STANDARD_VECTOR_SIZE
    // rows at a time from it; when exhausted, pull the next block.
    std::vector<std::vector<dbisam::CellValue>> current_batch;
    idx_t batch_offset = 0;
    idx_t total_emitted = 0; // used for ROWID generation
    bool exhausted = false;
};

static unique_ptr<FunctionData> DbisamAttachedScanBind(ClientContext &,
                                                       TableFunctionBindInput &input,
                                                       vector<LogicalType> &return_types,
                                                       vector<string> &names) {
    auto bind = make_uniq<DbisamScanBindData>();
    bind->table = reinterpret_cast<DbisamTableEntry *>(input.inputs[0].GetPointer());
    auto &table = *bind->table;
    bind->all_field_types = table.source_field_types;
    for (auto &col : table.GetColumns().Logical()) {
        bind->all_names.push_back(col.GetName());
        bind->all_types.push_back(col.GetType());
        names.push_back(col.GetName());
        return_types.push_back(col.GetType());
    }
    return std::move(bind);
}

static bool IsBlobShaped(dbisam::FieldType ft) {
    return ft == dbisam::FieldType::Blob
        || ft == dbisam::FieldType::Memo
        || ft == dbisam::FieldType::Graphic;
}

static unique_ptr<GlobalTableFunctionState>
DbisamAttachedScanInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
    auto &bind = input.bind_data->CastNoConst<DbisamScanBindData>();
    auto state = make_uniq<DbisamScanState>();
    state->column_ids = input.column_ids;

    // Detect projections that touch a Memo/Blob/Graphic column. The
    // blob resolver in query_decoded uses columns[0] of the response
    // as the PK source (row[first_off+1..+pk_width]); if the projection
    // dropped the PK, the slot lookup fails server-side and every blob
    // comes back empty. Force-include the PK column (table column 0)
    // at SELECT position 0 in that case, then skip it in the output
    // mapping.
    bool needs_pk = false;
    for (auto cid : state->column_ids) {
        if (cid < bind.all_field_types.size() && IsBlobShaped(bind.all_field_types[cid])) {
            needs_pk = true;
            break;
        }
    }
    // The blob resolver requires the PK at SELECT *position 0*
    // (decode_batch_with_blobs reads columns.front()), so membership of
    // column 0 elsewhere in the projection isn't enough — it must be the
    // first real column. Otherwise inject it at position 0 (a duplicate
    // SELECT entry if it also appears later is harmless).
    bool pk_leads_projection = false;
    for (auto cid : state->column_ids) {
        if (cid == COLUMN_IDENTIFIER_ROW_ID || cid >= bind.all_names.size()) {
            continue; // rowid pseudo-columns don't appear in the SELECT
        }
        pk_leads_projection = (cid == 0);
        break;
    }
    bool pk_injected = needs_pk && !pk_leads_projection && !bind.all_names.empty();

    // Two passes through column_ids:
    //   (a) build the SELECT — real columns only, in the order they
    //       appear in column_ids, optionally prefixed by the PK;
    //   (b) build output_to_projected — for each output column, the
    //       index into the projected response (or SIZE_MAX for ROWID).
    // DuckDB sends COLUMN_IDENTIFIER_ROW_ID (== UINT64_MAX) for queries
    // that only need the row count (e.g. count(*)); the output vector
    // is BIGINT and we just emit sequential row indices into it.
    std::vector<std::string> select_cols;
    if (pk_injected) {
        select_cols.push_back(QuoteDbisamIdent(bind.all_names[0]));
    }
    state->output_to_projected.reserve(state->column_ids.size());
    for (auto cid : state->column_ids) {
        if (cid == COLUMN_IDENTIFIER_ROW_ID || cid >= bind.all_names.size()) {
            state->output_to_projected.push_back(SIZE_MAX);
            continue;
        }
        state->output_to_projected.push_back(select_cols.size());
        select_cols.push_back(QuoteDbisamIdent(bind.all_names[cid]));
    }
    // Always need at least one real column to drive the cursor and get
    // a row count. Pick column 0 if the request was rowid-only.
    bool driver_only = select_cols.empty();
    if (driver_only) {
        if (bind.all_names.empty()) {
            return std::move(state);
        }
        select_cols.push_back(QuoteDbisamIdent(bind.all_names[0]));
    }

    std::string sql = "SELECT ";
    for (size_t i = 0; i < select_cols.size(); ++i) {
        if (i) sql += ", ";
        sql += select_cols[i];
    }
    sql += " FROM " + QuoteDbisamIdent(bind.table->name);

    // Filter pushdown: TableFilterSet's column_ids are indices into the
    // SCAN'S column_ids (i.e. into the projected columns), not into the
    // table's full column list. Resolve each to the real column name via
    // bind.all_names[column_ids[col_idx]] before rendering.
    if (input.filters) {
        std::vector<std::string> filter_col_names;
        filter_col_names.reserve(state->column_ids.size());
        for (auto cid : state->column_ids) {
            if (cid < bind.all_names.size()) {
                filter_col_names.push_back(bind.all_names[cid]);
            } else {
                filter_col_names.emplace_back(); // ROWID etc — no name
            }
        }
        std::vector<idx_t> applied;
        std::string where = RenderDbisamFilterSet(*input.filters, filter_col_names, applied);
        if (!where.empty()) {
            sql += " WHERE " + where;
        }
    }

    // Hard server-side row cap. When the LIMIT was pushed with no residual
    // filter, append DBISAM's trailing `TOP n` so the server stops
    // preparing the result after n rows instead of materialising the whole
    // table first — without this, `... LIMIT 5` on a large table waits for
    // the full server-side prepare. Trailing clause, after WHERE.
    if (bind.limit_hint > 0 && bind.limit_hard_cap) {
        sql += " TOP " + std::to_string(bind.limit_hint);
    }

    if (std::getenv("DBISAM_SQL_DEBUG")) {
        std::fprintf(stderr, "[dbisam-sql] %s\n", sql.c_str());
    }
    // Fresh Client per scan — native protocol desyncs when multiple
    // queries share a session (per ExportKing README). Open it now
    // and keep alive in state for streaming; CursorRunner borrows the
    // transport.
    auto &txn = DbisamTransaction::Get(context, bind.table->catalog);
    state->client = std::make_unique<dbisam::Client>(txn.OpenClient());
    // LIMIT pushdown: if the OptimizerExtension stamped a limit_hint
    // on bind data, shrink the cursor's request size so the first
    // ReadFirstRecordBlock fetches at most that many rows. DuckDB's
    // LIMIT then stops calling Execute after the first batch.
    if (bind.limit_hint > 0 && bind.limit_hint < state->client->batch_size()) {
        state->client->set_batch_size(static_cast<uint32_t>(bind.limit_hint));
    }
    auto scan = state->client->start_streaming(sql);
    state->projected_columns = std::move(scan.columns);
    state->cursor = std::move(scan.runner);
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
        // DBISAM ftString/ftMemo are Windows-1252; decode to UTF-8 so
        // the resulting VARCHAR is valid for downstream sinks (Parquet,
        // Arrow, JSON). UTF-8-clean strings pass through unchanged.
        if (auto *s = std::get_if<std::string>(&cell)) {
            auto u = dbisam::decode_dbisam_text(*s);
            FlatVector::GetData<string_t>(out)[row] = StringVector::AddString(out, u);
        } else if (auto *b = std::get_if<std::vector<uint8_t>>(&cell)) {
            auto u = dbisam::decode_dbisam_text(b->data(), b->size());
            FlatVector::GetData<string_t>(out)[row] = StringVector::AddString(out, u);
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

    // Pull more rows from the server if we've drained the current batch.
    if (state.batch_offset >= state.current_batch.size()) {
        if (state.exhausted || !state.cursor) {
            output.SetCardinality(0);
            return;
        }
        // Skip empty/blank batches that occasionally arrive before the
        // server signals EoC.
        while (state.batch_offset >= state.current_batch.size() && !state.exhausted) {
            auto block = state.cursor->next_block();
            if (block.eoc && block.rows.empty()) {
                state.exhausted = true;
                output.SetCardinality(0);
                return;
            }
            state.current_batch = state.client->decode_batch_with_blobs(
                state.projected_columns, block.rows, block.bookmarks);
            state.batch_offset = 0;
            if (block.eoc) {
                state.exhausted = true; // emit this batch, stop after
            }
        }
        if (state.current_batch.empty()) {
            output.SetCardinality(0);
            return;
        }
    }

    idx_t available = state.current_batch.size() - state.batch_offset;
    idx_t emit = std::min<idx_t>(STANDARD_VECTOR_SIZE, available);

    for (idx_t outcol = 0; outcol < output.ColumnCount(); ++outcol) {
        auto &out = output.data[outcol];
        out.SetVectorType(VectorType::FLAT_VECTOR);
        size_t projected_idx = outcol < state.output_to_projected.size()
                                   ? state.output_to_projected[outcol]
                                   : SIZE_MAX;
        if (projected_idx == SIZE_MAX) {
            auto data = FlatVector::GetData<int64_t>(out);
            for (idx_t r = 0; r < emit; ++r) {
                data[r] = static_cast<int64_t>(state.total_emitted + r);
            }
            continue;
        }
        if (projected_idx >= state.projected_columns.size()) {
            for (idx_t r = 0; r < emit; ++r) FlatVector::SetNull(out, r, true);
            continue;
        }
        const auto ft = state.projected_columns[projected_idx].field_type;
        for (idx_t r = 0; r < emit; ++r) {
            const auto &row_cells = state.current_batch[state.batch_offset + r];
            if (projected_idx >= row_cells.size()) {
                FlatVector::SetNull(out, r, true);
            } else {
                WriteCell(out, r, ft, row_cells[projected_idx]);
            }
        }
    }
    output.SetCardinality(emit);
    state.batch_offset += emit;
    state.total_emitted += emit;
}

} // namespace

TableFunction DbisamTableEntry::GetScanFunction(ClientContext &, unique_ptr<FunctionData> &bind_data) {
    TableFunction tf("dbisam_attached_scan", {LogicalType::POINTER},
                     DbisamAttachedScanExecute, DbisamAttachedScanBind,
                     DbisamAttachedScanInitGlobal);
    tf.projection_pushdown = true;
    tf.filter_pushdown = true;
    // filter_prune left at default (false) — DuckDB still post-filters
    // even when we push down, so any unsupported filter shapes we
    // silently dropped get evaluated correctly on the DuckDB side.

    // Pre-populate bind_data so the planner can request statistics etc.
    // before re-binding. The actual bind callback re-derives the same
    // information from `input.inputs[0]` (a POINTER to `this`).
    auto bind = make_uniq<DbisamScanBindData>();
    bind->table = this;
    bind->all_field_types = source_field_types;
    for (auto &col : GetColumns().Logical()) {
        bind->all_names.push_back(col.GetName());
        bind->all_types.push_back(col.GetType());
    }
    bind_data = std::move(bind);

    return tf;
}

} // namespace duckdb
