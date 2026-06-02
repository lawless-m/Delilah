// Read-only transaction manager. Start/Commit/Rollback all no-op
// because DBISAM access here is SELECT-only.

#pragma once

#include "dbisam/storage/dbisam_transaction.hpp"

#include "duckdb/common/reference_map.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/transaction/transaction_manager.hpp"

#include <mutex>

namespace duckdb {

class DbisamCatalog;

class DbisamTransactionManager : public TransactionManager {
public:
    DbisamTransactionManager(AttachedDatabase &db, DbisamCatalog &catalog);

    Transaction &StartTransaction(ClientContext &context) override;
    ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override;
    void RollbackTransaction(Transaction &transaction) override;
    void Checkpoint(ClientContext &context, bool force) override;

private:
    DbisamCatalog &catalog_;
    std::mutex lock_;
    reference_map_t<Transaction, unique_ptr<DbisamTransaction>> transactions_;
};

} // namespace duckdb
