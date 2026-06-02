// dbisam_scan(host, user, password, catalog, sql [, encrypt_password=...,
//             port=..., compression=...]) — DuckDB table function.
//
// Opens one Exportmaster session per call, runs the SQL, materialises
// the full result into bind data, then streams it out one DataChunk at
// a time during execution.
//
// SELECT-only: this function does PrepareStatement / ExecuteStatement
// against the server; it never invokes DDL/DML reqcodes. The SQL
// string itself is the user's responsibility (a malicious SQL would
// hit the server's own access controls, not this extension).

#include "dbisam/client.hpp"
#include "dbisam/row.hpp"

#include "duckdb.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

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
    std::vector<dbisam::Column> columns;
    std::vector<std::vector<dbisam::CellValue>> rows;
};

struct DbisamScanState : public GlobalTableFunctionState {
    idx_t row_offset = 0;
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
    dbisam::ConnOpts opts;
    opts.host             = require_string(input.inputs[0], "host");
    opts.user             = require_string(input.inputs[1], "user");
    opts.password         = require_string(input.inputs[2], "password");
    opts.catalog          = require_string(input.inputs[3], "catalog");
    std::string sql       = require_string(input.inputs[4], "sql");
    opts.encrypt_password = "elevatesoft";
    opts.compression      = true;

    auto it = input.named_parameters.find("encrypt_password");
    if (it != input.named_parameters.end() && !it->second.IsNull()) {
        opts.encrypt_password = it->second.ToString();
    }
    it = input.named_parameters.find("port");
    if (it != input.named_parameters.end() && !it->second.IsNull()) {
        opts.port = static_cast<uint16_t>(it->second.GetValue<int32_t>());
    }
    it = input.named_parameters.find("compression");
    if (it != input.named_parameters.end() && !it->second.IsNull()) {
        opts.compression = it->second.GetValue<bool>();
    }

    auto result = make_uniq<DbisamScanBindData>();
    try {
        auto client = dbisam::Client::connect_and_login(opts);
        auto q = client.query_decoded(sql, 0);
        result->columns = std::move(q.columns);
        result->rows = std::move(q.rows);
    } catch (const std::exception &e) {
        throw IOException("dbisam_scan: %s", e.what());
    }

    for (const auto &c : result->columns) {
        return_types.push_back(map_field_type(c.field_type));
        names.push_back(c.name);
    }
    return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> DbisamScanInitGlobal(ClientContext &ctx,
                                                                 TableFunctionInitInput &input) {
    return make_uniq<DbisamScanState>();
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
        if (auto *s = std::get_if<std::string>(&cell)) {
            FlatVector::GetData<string_t>(out)[row] = StringVector::AddString(out, *s);
        } else if (auto *bytes = std::get_if<std::vector<uint8_t>>(&cell)) {
            // Memo resolved via OpenBlob arrives as raw bytes; treat as
            // text for the VARCHAR mapping (DBISAM ftMemo is text).
            FlatVector::GetData<string_t>(out)[row] = StringVector::AddString(
                out, const_char_ptr_cast(bytes->data()), bytes->size());
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
            // Phase 6 leaves unresolved blob handles as their raw 8 bytes;
            // Phase 7 will fetch the real payload via 0x0280.
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

static void DbisamScanExecute(ClientContext &ctx, TableFunctionInput &input, DataChunk &output) {
    auto &bind = input.bind_data->Cast<DbisamScanBindData>();
    auto &state = input.global_state->Cast<DbisamScanState>();

    idx_t total = bind.rows.size();
    if (state.row_offset >= total) {
        output.SetCardinality(0);
        return;
    }
    idx_t emit = std::min<idx_t>(STANDARD_VECTOR_SIZE, total - state.row_offset);
    for (idx_t col = 0; col < bind.columns.size(); ++col) {
        auto &out = output.data[col];
        out.SetVectorType(VectorType::FLAT_VECTOR);
        for (idx_t r = 0; r < emit; ++r) {
            const auto &cell = bind.rows[state.row_offset + r][col];
            write_cell(out, r, bind.columns[col].field_type, cell);
        }
    }
    output.SetCardinality(emit);
    state.row_offset += emit;
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
    loader.RegisterFunction(tf);
}

} // namespace duckdb
