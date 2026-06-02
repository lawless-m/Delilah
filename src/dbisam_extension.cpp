#define DUCKDB_EXTENSION_MAIN

#include "dbisam_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/function/scalar_function.hpp"

namespace duckdb {

void RegisterDbisamScan(ExtensionLoader &loader);
void RegisterDbisamStorageExtension(ExtensionLoader &loader);

static constexpr const char *DBISAM_EXTENSION_VERSION = "0.0.1";

static void DbisamVersionScalar(DataChunk &args, ExpressionState &state, Vector &result) {
    result.SetVectorType(VectorType::CONSTANT_VECTOR);
    auto data = ConstantVector::GetData<string_t>(result);
    data[0] = StringVector::AddString(result, DBISAM_EXTENSION_VERSION);
}

static void LoadInternal(ExtensionLoader &loader) {
    ScalarFunction fn("dbisam_version", {}, LogicalType::VARCHAR, DbisamVersionScalar);
    loader.RegisterFunction(fn);
    RegisterDbisamScan(loader);
    RegisterDbisamStorageExtension(loader);
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(dbisam, loader) {
    duckdb::LoadInternal(loader);
}

} // extern "C"
