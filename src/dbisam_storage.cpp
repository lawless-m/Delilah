// StorageExtension entry point for `ATTACH '...' AS x (TYPE dbisam)`.

#include "dbisam/storage/dbisam_attach_options.hpp"
#include "dbisam/storage/dbisam_catalog.hpp"
#include "dbisam/storage/dbisam_transaction_manager.hpp"

#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/transaction_manager.hpp"

namespace duckdb {

namespace {

unique_ptr<Catalog> DbisamAttach(optional_ptr<StorageExtensionInfo>, ClientContext &,
                                 AttachedDatabase &db, const string &/*name*/,
                                 AttachInfo &info, AttachOptions &attach_options) {
    auto opts = dbisam::parse_attach_path(info.path);
    for (auto &entry : attach_options.options) {
        if (StringUtil::CIEquals(entry.first, "user")) {
            opts.user = entry.second.ToString();
        } else if (StringUtil::CIEquals(entry.first, "password")) {
            opts.password = entry.second.ToString();
        } else if (StringUtil::CIEquals(entry.first, "encrypt_password")) {
            opts.encrypt_password = entry.second.ToString();
        } else if (StringUtil::CIEquals(entry.first, "port")) {
            opts.port = static_cast<uint16_t>(entry.second.GetValue<int32_t>());
        } else if (StringUtil::CIEquals(entry.first, "compression")) {
            opts.compression = entry.second.GetValue<bool>();
        } else if (StringUtil::CIEquals(entry.first, "eager_schema")) {
            opts.eager_schema = entry.second.GetValue<bool>();
        } else {
            throw NotImplementedException("Unsupported parameter for DBISAM Attach: %s", entry.first);
        }
    }
    return make_uniq<DbisamCatalog>(db, info.path, std::move(opts));
}

unique_ptr<TransactionManager> DbisamCreateTransactionManager(optional_ptr<StorageExtensionInfo>,
                                                              AttachedDatabase &db, Catalog &catalog) {
    auto &cat = catalog.Cast<DbisamCatalog>();
    return make_uniq<DbisamTransactionManager>(db, cat);
}

class DbisamStorageExtension : public StorageExtension {
public:
    DbisamStorageExtension() {
        attach = DbisamAttach;
        create_transaction_manager = DbisamCreateTransactionManager;
    }
};

} // namespace

void RegisterDbisamStorageExtension(ExtensionLoader &loader) {
    auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
    StorageExtension::Register(config, "dbisam", make_shared_ptr<DbisamStorageExtension>());
}

} // namespace duckdb
