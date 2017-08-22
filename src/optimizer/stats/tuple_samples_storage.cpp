//===----------------------------------------------------------------------===//
//
//                         Peloton
//
// tuple_samples_storage.cpp
//
// Identification: src/optimizer/stats/tuple_samples_storage.cpp
//
// Copyright (c) 2015-16, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "optimizer/stats/tuple_samples_storage.h"

#include "catalog/catalog.h"
#include "concurrency/transaction_manager_factory.h"
#include "executor/executor_context.h"
#include "executor/insert_executor.h"
#include "executor/seq_scan_executor.h"
#include "optimizer/stats/tuple_sampler.h"
#include "planner/insert_plan.h"

namespace peloton {
namespace optimizer {

// Get instance of the global tuple samples storage
TupleSamplesStorage *TupleSamplesStorage::GetInstance() {
  static TupleSamplesStorage global_tuple_samples_storage;
  return &global_tuple_samples_storage;
}

/**
 * TuplesSamplesStorage - Constructor of TuplesSamplesStorage.
 * In the construcotr, `stats` table and `samples_db` database are created.
 */
TupleSamplesStorage::TupleSamplesStorage() {
  pool_.reset(new type::EphemeralPool());
  CreateSamplesDatabase();
}

/**
 * CreateSamplesDatabase - Create a database for storing samples tables.
 */
void TupleSamplesStorage::CreateSamplesDatabase() {
  auto &txn_manager = concurrency::TransactionManagerFactory::GetInstance();
  auto txn = txn_manager.BeginTransaction();
  catalog::Catalog::GetInstance()->CreateDatabase(SAMPLES_DB_NAME, txn);
  txn_manager.CommitTransaction(txn);
}

/**
 * AddSamplesTable - Add a samples table into the 'samples_db'.
 * The table name is generated by concatenating db_id and table_id with '_'.
 */
void TupleSamplesStorage::AddSamplesTable(
    storage::DataTable *data_table,
    std::vector<std::unique_ptr<storage::Tuple>> &sampled_tuples) {
  auto schema = data_table->GetSchema();
  auto schema_copy = catalog::Schema::CopySchema(schema);
  std::unique_ptr<catalog::Schema> schema_ptr(schema_copy);
  auto catalog = catalog::Catalog::GetInstance();
  bool is_catalog = false;
  std::string samples_table_name = GenerateSamplesTableName(
      data_table->GetDatabaseOid(), data_table->GetOid());

  auto &txn_manager = concurrency::TransactionManagerFactory::GetInstance();
  auto txn = txn_manager.BeginTransaction();
  catalog->CreateTable(std::string(SAMPLES_DB_NAME), samples_table_name,
                       std::move(schema_ptr), txn, is_catalog);

  auto samples_table = catalog->GetTableWithName(std::string(SAMPLES_DB_NAME),
                                                 samples_table_name, txn);

  for (auto &tuple : sampled_tuples) {
    InsertSampleTuple(samples_table, std::move(tuple), txn);
  }
  txn_manager.CommitTransaction(txn);
}

ResultType TupleSamplesStorage::DeleteSamplesTable(
    oid_t database_id, oid_t table_id, concurrency::Transaction *txn) {
  auto &txn_manager = concurrency::TransactionManagerFactory::GetInstance();
  bool single_statement_txn = false;
  if (txn == nullptr) {
    single_statement_txn = true;
    txn = txn_manager.BeginTransaction();
  }

  auto catalog = catalog::Catalog::GetInstance();
  std::string samples_table_name =
      GenerateSamplesTableName(database_id, table_id);
  auto result = catalog->DropTable(SAMPLES_DB_NAME, samples_table_name, txn);

  if (single_statement_txn) {
    txn_manager.CommitTransaction(txn);
  }

  std::string result_str;
  if (result == ResultType::SUCCESS) {
    result_str = "success";
  } else {
    result_str = "false";
  }

  LOG_DEBUG("Drop table %s, result: %s", samples_table_name.c_str(),
            result_str.c_str());
  return result;
}

bool TupleSamplesStorage::InsertSampleTuple(
    storage::DataTable *samples_table, std::unique_ptr<storage::Tuple> tuple,
    concurrency::Transaction *txn) {
  if (txn == nullptr) {
    return false;
  }

  std::unique_ptr<executor::ExecutorContext> context(
      new executor::ExecutorContext(txn));
  planner::InsertPlan node(samples_table, std::move(tuple));
  executor::InsertExecutor executor(&node, context.get());
  executor.Init();
  bool status = executor.Execute();

  return status;
}

ResultType TupleSamplesStorage::CollectSamplesForTable(
    storage::DataTable *data_table, concurrency::Transaction *txn) {
  if (txn == nullptr) {
    LOG_TRACE("Do not have transaction to collect samples for table: %s",
              data_table->GetName().c_str());
    return ResultType::FAILURE;
  }
  TupleSampler tuple_sampler(data_table);
  tuple_sampler.AcquireSampleTuples(SAMPLE_COUNT_PER_TABLE);
  DeleteSamplesTable(data_table->GetDatabaseOid(), data_table->GetOid());
  AddSamplesTable(data_table, tuple_sampler.GetSampledTuples());
  return ResultType::SUCCESS;
}

std::unique_ptr<std::vector<std::unique_ptr<executor::LogicalTile>>>
TupleSamplesStorage::GetTuplesWithSeqScan(storage::DataTable *data_table,
                                          std::vector<oid_t> column_offsets,
                                          concurrency::Transaction *txn) {
  if (txn == nullptr) {
    LOG_TRACE("Do not have transaction to perform the sequential scan");
    return nullptr;
  }

  // Sequential scan
  std::unique_ptr<executor::ExecutorContext> context(
      new executor::ExecutorContext(txn));

  planner::SeqScanPlan seq_scan_node(data_table, nullptr, column_offsets);
  executor::SeqScanExecutor seq_scan_executor(&seq_scan_node, context.get());

  // Execute
  seq_scan_executor.Init();
  std::unique_ptr<std::vector<std::unique_ptr<executor::LogicalTile>>>
      result_tiles(new std::vector<std::unique_ptr<executor::LogicalTile>>());

  while (seq_scan_executor.Execute()) {
    result_tiles->push_back(
        std::unique_ptr<executor::LogicalTile>(seq_scan_executor.GetOutput()));
  }

  return result_tiles;
}

/**
 * GetTupleSamples - Query tuple samples by db_id and table_id.
 */
std::unique_ptr<std::vector<std::unique_ptr<executor::LogicalTile>>>
TupleSamplesStorage::GetTupleSamples(oid_t database_id, oid_t table_id) {
  auto catalog = catalog::Catalog::GetInstance();
  std::string samples_table_name =
      GenerateSamplesTableName(database_id, table_id);
  auto &txn_manager = concurrency::TransactionManagerFactory::GetInstance();
  auto txn = txn_manager.BeginTransaction();
  auto data_table =
      catalog->GetTableWithName(SAMPLES_DB_NAME, samples_table_name, txn);

  auto col_count = data_table->GetSchema()->GetColumnCount();
  std::vector<oid_t> column_ids;
  for (size_t col_id = 0; col_id < col_count; ++col_id) {
    column_ids.push_back(col_id);
  }

  auto result_tiles = GetTuplesWithSeqScan(data_table, column_ids, txn);
  txn_manager.CommitTransaction(txn);

  return result_tiles;
}

/**
 * GetColumnSamples - Query column samples by db_id, table_id and column_id.
 */
void TupleSamplesStorage::GetColumnSamples(
    oid_t database_id, oid_t table_id, oid_t column_id,
    std::vector<type::Value> &column_samples) {
  auto catalog = catalog::Catalog::GetInstance();
  std::string samples_table_name =
      GenerateSamplesTableName(database_id, table_id);
  auto &txn_manager = concurrency::TransactionManagerFactory::GetInstance();
  auto txn = txn_manager.BeginTransaction();
  auto data_table =
      catalog->GetTableWithName(SAMPLES_DB_NAME, samples_table_name, txn);

  std::vector<oid_t> column_ids({column_id});
  auto result_tiles = GetTuplesWithSeqScan(data_table, column_ids, txn);
  txn_manager.CommitTransaction(txn);

  LOG_DEBUG("Result tiles count: %lu", result_tiles->size());
  if (result_tiles->size() != 0) {
    auto tile = (*result_tiles)[0].get();
    LOG_DEBUG("Tuple count: %lu", tile->GetTupleCount());

    for (size_t tuple_id = 0; tuple_id < tile->GetTupleCount(); ++tuple_id) {
      column_samples.push_back(tile->GetValue(tuple_id, 0));
    }
  }
}

}  // namespace optimizer
}  // namespace peloton
