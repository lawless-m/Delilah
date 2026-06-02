#include "dbisam/storage/dbisam_transaction_manager.hpp"
#include "dbisam/storage/dbisam_catalog.hpp"
#include "dbisam/storage/dbisam_transaction.hpp"

namespace duckdb {

DbisamTransactionManager::DbisamTransactionManager(AttachedDatabase &db, DbisamCatalog &catalog)
    : TransactionManager(db), catalog_(catalog) {}

Transaction &DbisamTransactionManager::StartTransaction(ClientContext &context) {
    auto t = make_uniq<DbisamTransaction>(catalog_, *this, context);
    auto &result = *t;
    std::lock_guard<std::mutex> g(lock_);
    transactions_[result] = std::move(t);
    return result;
}

ErrorData DbisamTransactionManager::CommitTransaction(ClientContext &, Transaction &transaction) {
    std::lock_guard<std::mutex> g(lock_);
    transactions_.erase(transaction);
    return ErrorData();
}

void DbisamTransactionManager::RollbackTransaction(Transaction &transaction) {
    std::lock_guard<std::mutex> g(lock_);
    transactions_.erase(transaction);
}

void DbisamTransactionManager::Checkpoint(ClientContext &, bool) {
    // No-op — DBISAM access is read-only.
}

} // namespace duckdb
