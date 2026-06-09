// dbisam_scan(host, user, password, catalog, sql [, encrypt_password=...,
//             port=..., compression=...]) — DuckDB table function.
//
// Opens one Exportmaster session for schema discovery (during Bind) and
// a second one for the actual scan (during InitGlobal). The scan
// connection holds the cursor open across Execute calls and pulls one
// RecordBlock at a time so DuckDB's LIMIT naturally stops the fetch
// after the first batch that satisfies it.
//
// SELECT-only: this function does PrepareStatement / ExecuteStatement
// against the server; it never invokes DDL/DML reqcodes. The SQL
// string itself is the user's responsibility (a malicious SQL would
// hit the server's own access controls, not this extension).

#include "dbisam/client.hpp"
#include "dbisam/cursor.hpp"
#include "dbisam/row.hpp"
#include "dbisam/text.hpp"

#include "duckdb.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <cctype>
#include <memory>
#include <string>
#include <utility>
#include <variant>

namespace duckdb {

namespace {

LogicalType map_field_type(dbisam::FieldType ft) {
    using FT = dbisam::FieldType;
    switch (ft) {
    case FT::String:
    case FT::Memo:
    case FT::Calculated:
    case FT::Unknown:    return LogicalType::VARCHAR;
    case FT::Date:       return LogicalType::DATE;
    case FT::DateTime:   return LogicalType::TIMESTAMP;
    case FT::Time:       return LogicalType::TIME;
    case FT::Integer:
    case FT::AutoInc:
    case FT::Smallint:   return LogicalType::INTEGER;
    case FT::Largeint:   return LogicalType::BIGINT;
    case FT::Boolean:    return LogicalType::BOOLEAN;
    case FT::Float:
    case FT::Currency:   return LogicalType::DOUBLE;
    case FT::Blob:
    case FT::Graphic:
    case FT::Bytes:
    case FT::VarBytes:   return LogicalType::BLOB;
    }
    return LogicalType::VARCHAR;
}

struct DbisamScanBindData : public TableFunctionData {
    dbisam::ConnOpts opts;
    std::string sql;
    std::vector<dbisam::Column> columns; // schema discovered at bind time
};

struct DbisamScanState : public GlobalTableFunctionState {
    std::unique_ptr<dbisam::Client> client;
    std::unique_ptr<dbisam::CursorRunner> cursor;
    std::vector<std::vector<dbisam::CellValue>> current_batch;
    idx_t batch_offset = 0;
    bool exhausted = false;
};

static std::string require_string(const Value &v, const char *what) {
    if (v.IsNull()) {
        throw BinderException("dbisam_scan: parameter '%s' must not be NULL", what);
    }
    return v.ToString();
}

static unique_ptr<FunctionData> DbisamScanBind(ClientContext &ctx,
                                               TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types,
                                               vector<string> &names) {
    if (input.inputs.size() < 5) {
        throw BinderException("dbisam_scan(host, user, password, catalog, sql, ...) requires 5 positional args");
    }
    auto bind = make_uniq<DbisamScanBindData>();
    bind->opts.host             = require_string(input.inputs[0], "host");
    bind->opts.user             = require_string(input.inputs[1], "user");
    bind->opts.password         = require_string(input.inputs[2], "password");
    bind->opts.catalog          = require_string(input.inputs[3], "catalog");
    bind->sql                   = require_string(input.inputs[4], "sql");
    bind->opts.encrypt_password = "elevatesoft";
    bind->opts.compression      = true;

    auto it = input.named_parameters.find("encrypt_password");
    if (it != input.named_parameters.end() && !it->second.IsNull()) {
        bind->opts.encrypt_password = it->second.ToString();
    }
    it = input.named_parameters.find("port");
    if (it != input.named_parameters.end() && !it->second.IsNull()) {
        bind->opts.port = static_cast<uint16_t>(it->second.GetValue<int32_t>());
    }
    it = input.named_parameters.find("compression");
    if (it != input.named_parameters.end() && !it->second.IsNull()) {
        bind->opts.compression = it->second.GetValue<bool>();
    }
    it = input.named_parameters.find("lenient_decode");
    if (it != input.named_parameters.end() && !it->second.IsNull()) {
        bind->opts.lenient_decode = it->second.GetValue<bool>();
    }
    // Optional explicit DBISAM TOP-n cap. The SQL goes verbatim to the
    // server; appending `TOP n` here lets the server stop early instead
    // of streaming-with-early-termination on our side. Users who write
    // the TOP themselves don't need this.
    it = input.named_parameters.find("top");
    if (it != input.named_parameters.end() && !it->second.IsNull()) {
        int64_t n = it->second.GetValue<int64_t>();
        if (n > 0) {
            // Only append if the user hasn't already written TOP. Cheap
            // case-insensitive check: look for " TOP " near the end.
            std::string upper = bind->sql;
            for (auto &c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (upper.find(" TOP ") == std::string::npos) {
                bind->sql += " TOP " + std::to_string(n);
            }
        }
    }

    // Schema-only probe: connect, send PrepareStatement, parse columns,
    // disconnect. InitGlobal will open a fresh connection for the scan.
    try {
        auto probe = dbisam::Client::connect_and_login(bind->opts);
        auto resp = probe.query_raw(bind->sql);
        auto [columns, _end] = dbisam::parse_schema(resp);
        bind->columns = std::move(columns);
    } catch (const std::exception &e) {
        throw IOException("dbisam_scan: %s", e.what());
    }

    for (const auto &c : bind->columns) {
        return_types.push_back(map_field_type(c.field_type));
        names.push_back(c.name);
    }
    return std::move(bind);
}

static unique_ptr<GlobalTableFunctionState> DbisamScanInitGlobal(ClientContext &,
                                                                 TableFunctionInitInput &input) {
    auto &bind = input.bind_data->CastNoConst<DbisamScanBindData>();
    auto state = make_uniq<DbisamScanState>();
    try {
        state->client = std::make_unique<dbisam::Client>(
            dbisam::Client::connect_and_login(bind.opts));
        auto scan = state->client->start_streaming(bind.sql);
        state->cursor = std::move(scan.runner);
        // The output vectors were typed from the bind-time probe. If the
        // scan-time schema diverged (table altered between bind and
        // execute), writing cells through the new field types into
        // vectors allocated for the old ones would corrupt memory —
        // fail loudly instead.
        if (scan.columns.size() != bind.columns.size()) {
            throw std::runtime_error("schema changed between bind and scan: " +
                                     std::to_string(bind.columns.size()) + " columns at bind, " +
                                     std::to_string(scan.columns.size()) + " at scan");
        }
        for (size_t i = 0; i < scan.columns.size(); ++i) {
            if (scan.columns[i].field_type != bind.columns[i].field_type) {
                throw std::runtime_error("schema changed between bind and scan: column " +
                                         scan.columns[i].name + " changed type");
            }
        }
    } catch (const std::exception &e) {
        throw IOException("dbisam_scan: %s", e.what());
    }
    return std::move(state);
}

// Write one decoded cell into output.data[col].row at row index `row`.
// Schema's FieldType is the authority for interpretation.
static void write_cell(Vector &out, idx_t row, dbisam::FieldType ft,
                       const dbisam::CellValue &cell) {
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
        } else if (auto *bytes = std::get_if<std::vector<uint8_t>>(&cell)) {
            auto u = dbisam::decode_dbisam_text(bytes->data(), bytes->size());
            FlatVector::GetData<string_t>(out)[row] = StringVector::AddString(out, u);
        } else {
            FlatVector::SetNull(out, row, true);
        }
        return;
    }
    case FT::Date: {
        auto *v = std::get_if<int32_t>(&cell);
        if (!v) { FlatVector::SetNull(out, row, true); return; }
        FlatVector::GetData<date_t>(out)[row] = date_t(*v);
        return;
    }
    case FT::DateTime: {
        auto *v = std::get_if<int64_t>(&cell);
        if (!v) { FlatVector::SetNull(out, row, true); return; }
        FlatVector::GetData<timestamp_t>(out)[row] = timestamp_t(*v);
        return;
    }
    case FT::Time: {
        auto *v = std::get_if<int64_t>(&cell);
        if (!v) { FlatVector::SetNull(out, row, true); return; }
        FlatVector::GetData<dtime_t>(out)[row] = dtime_t(*v);
        return;
    }
    case FT::Integer:
    case FT::AutoInc:
    case FT::Smallint: {
        auto *v = std::get_if<int32_t>(&cell);
        if (!v) { FlatVector::SetNull(out, row, true); return; }
        FlatVector::GetData<int32_t>(out)[row] = *v;
        return;
    }
    case FT::Largeint: {
        auto *v = std::get_if<int64_t>(&cell);
        if (!v) { FlatVector::SetNull(out, row, true); return; }
        FlatVector::GetData<int64_t>(out)[row] = *v;
        return;
    }
    case FT::Boolean: {
        auto *v = std::get_if<bool>(&cell);
        if (!v) { FlatVector::SetNull(out, row, true); return; }
        FlatVector::GetData<bool>(out)[row] = *v;
        return;
    }
    case FT::Float:
    case FT::Currency: {
        auto *v = std::get_if<double>(&cell);
        if (!v) { FlatVector::SetNull(out, row, true); return; }
        FlatVector::GetData<double>(out)[row] = *v;
        return;
    }
    case FT::Blob:
    case FT::Graphic:
    case FT::Bytes:
    case FT::VarBytes: {
        if (auto *bytes = std::get_if<std::vector<uint8_t>>(&cell)) {
            FlatVector::GetData<string_t>(out)[row] =
                StringVector::AddStringOrBlob(out,
                    const_char_ptr_cast(bytes->data()), bytes->size());
        } else if (auto *h = std::get_if<dbisam::BlobHandle>(&cell)) {
            FlatVector::GetData<string_t>(out)[row] =
                StringVector::AddStringOrBlob(out,
                    const_char_ptr_cast(h->bytes.data()), h->bytes.size());
        } else {
            FlatVector::SetNull(out, row, true);
        }
        return;
    }
    }
}

static void DbisamScanExecute(ClientContext &, TableFunctionInput &input, DataChunk &output) {
    auto &bind = input.bind_data->Cast<DbisamScanBindData>();
    auto &state = input.global_state->Cast<DbisamScanState>();

    // Pull more rows from the server if we've drained the current batch.
    if (state.batch_offset >= state.current_batch.size()) {
        if (state.exhausted || !state.cursor) {
            output.SetCardinality(0);
            return;
        }
        while (state.batch_offset >= state.current_batch.size() && !state.exhausted) {
            auto block = state.cursor->next_block();
            if (block.eoc && block.rows.empty()) {
                state.exhausted = true;
                output.SetCardinality(0);
                return;
            }
            state.current_batch = state.client->decode_batch_with_blobs(
                bind.columns, block.rows, block.bookmarks);
            state.batch_offset = 0;
            if (block.eoc) state.exhausted = true;
        }
        if (state.current_batch.empty()) {
            output.SetCardinality(0);
            return;
        }
    }

    idx_t available = state.current_batch.size() - state.batch_offset;
    idx_t emit = std::min<idx_t>(STANDARD_VECTOR_SIZE, available);
    for (idx_t col = 0; col < bind.columns.size() && col < output.ColumnCount(); ++col) {
        auto &out = output.data[col];
        out.SetVectorType(VectorType::FLAT_VECTOR);
        for (idx_t r = 0; r < emit; ++r) {
            const auto &row_cells = state.current_batch[state.batch_offset + r];
            if (col >= row_cells.size()) {
                FlatVector::SetNull(out, r, true);
            } else {
                write_cell(out, r, bind.columns[col].field_type, row_cells[col]);
            }
        }
    }
    output.SetCardinality(emit);
    state.batch_offset += emit;
}

} // namespace

void RegisterDbisamScan(ExtensionLoader &loader) {
    TableFunction tf("dbisam_scan",
                     {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
                      LogicalType::VARCHAR, LogicalType::VARCHAR},
                     DbisamScanExecute, DbisamScanBind, DbisamScanInitGlobal);
    tf.named_parameters["encrypt_password"] = LogicalType::VARCHAR;
    tf.named_parameters["port"] = LogicalType::INTEGER;
    tf.named_parameters["compression"] = LogicalType::BOOLEAN;
    tf.named_parameters["lenient_decode"] = LogicalType::BOOLEAN;
    tf.named_parameters["top"] = LogicalType::BIGINT;
    loader.RegisterFunction(tf);
}

} // namespace duckdb
