#include "logsearch_index_scan.hpp"
#include "logsearch_index.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/storage/table/table_index_list.hpp"
#include "duckdb/transaction/duck_transaction.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// Global state — queries the index and holds row IDs for parallel fetch
//===----------------------------------------------------------------------===//

struct LogsearchScanGlobalState : public GlobalTableFunctionState {
	//! Reference to the physical storage
	optional_ptr<DataTable> table;
	//! Column IDs to fetch (storage-level)
	vector<StorageIndex> storage_column_ids;
	//! Row IDs from the index — computed at execution time, not plan time
	vector<row_t> row_ids;
	//! Current position in row_ids (atomic for parallel scan)
	atomic<idx_t> current_idx;

	idx_t MaxThreads() const override {
		if (row_ids.empty()) {
			return 1;
		}
		return (row_ids.size() + STANDARD_VECTOR_SIZE - 1) / STANDARD_VECTOR_SIZE;
	}
};

//===----------------------------------------------------------------------===//
// Local state — per thread
//===----------------------------------------------------------------------===//

struct LogsearchScanLocalState : public LocalTableFunctionState {
	ColumnFetchState fetch_state;
};

//===----------------------------------------------------------------------===//
// Helper: find the LogsearchIndex by name from a table's index list
//===----------------------------------------------------------------------===//

static LogsearchIndex *FindIndexByName(DataTable &table, const string &index_name) {
	auto &index_list = table.GetDataTableInfo()->GetIndexes();
	LogsearchIndex *result = nullptr;
	for (auto &index : index_list.Indexes()) {
		if (!index.IsBound()) {
			continue;
		}
		if (index.GetIndexName() != index_name) {
			continue;
		}
		auto &bound = index.Cast<BoundIndex>();
		if (bound.GetIndexType() != LogsearchIndex::TYPE_NAME) {
			continue;
		}
		result = &bound.Cast<LogsearchIndex>();
		break;
	}
	return result;
}

//===----------------------------------------------------------------------===//
// Bind — only called for direct invocation (not our use case)
//===----------------------------------------------------------------------===//

static unique_ptr<FunctionData> LogsearchScanBind(ClientContext &, TableFunctionBindInput &,
                                                   vector<LogicalType> &, vector<string> &) {
	throw InternalException("LogsearchIndexScan should not be called directly — it is injected by the optimizer");
}

//===----------------------------------------------------------------------===//
// Init global — look up index, query posting lists, prepare row IDs
//===----------------------------------------------------------------------===//

static unique_ptr<GlobalTableFunctionState> LogsearchScanInitGlobal(ClientContext &context,
                                                                     TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<LogsearchScanBindData>();
	auto state = make_uniq<LogsearchScanGlobalState>();

	// Look up the table
	auto &catalog = Catalog::GetCatalog(context, bind_data.catalog_name);
	auto &table_entry = catalog.GetEntry<TableCatalogEntry>(context, bind_data.schema_name, bind_data.table_name);
	auto &duck_table = table_entry.Cast<DuckTableEntry>();
	state->table = &duck_table.GetStorage();

	// Convert column indices to storage indices
	for (const auto &col_idx : input.column_indexes) {
		state->storage_column_ids.push_back(duck_table.GetStorageIndex(col_idx));
	}

	// Look up the index by name and query it NOW (execution time, fresh data)
	auto *index = FindIndexByName(*state->table, bind_data.index_name);
	if (index) {
		if (bind_data.is_prefix) {
			state->row_ids = index->SearchPrefix(bind_data.search_prefix, bind_data.ts_min, bind_data.ts_max,
			                                     bind_data.has_ts_bounds);
		} else {
			state->row_ids = index->SearchTerms(bind_data.search_terms, bind_data.ts_min, bind_data.ts_max,
			                                    bind_data.has_ts_bounds);
		}
	}
	// If index was dropped between plan and execution, row_ids is empty → 0 results.
	// The original filter on top ensures correctness either way.

	state->current_idx = 0;
	return std::move(state);
}

//===----------------------------------------------------------------------===//
// Init local — per-thread state
//===----------------------------------------------------------------------===//

static unique_ptr<LocalTableFunctionState> LogsearchScanInitLocal(ExecutionContext &, TableFunctionInitInput &,
                                                                    GlobalTableFunctionState *) {
	auto result = make_uniq<LogsearchScanLocalState>();
	return std::move(result);
}

//===----------------------------------------------------------------------===//
// Scan — fetch rows by row ID in batches
//===----------------------------------------------------------------------===//

static void LogsearchScanFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &gstate = data_p.global_state->Cast<LogsearchScanGlobalState>();
	auto &lstate = data_p.local_state->Cast<LogsearchScanLocalState>();

	if (gstate.row_ids.empty()) {
		return;
	}

	auto &storage = *gstate.table;
	auto &tx = DuckTransaction::Get(context, storage.db);

	// Grab next batch of row IDs (atomic increment for parallelism)
	idx_t start = gstate.current_idx.fetch_add(STANDARD_VECTOR_SIZE);
	if (start >= gstate.row_ids.size()) {
		return; // No more work
	}
	idx_t count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, gstate.row_ids.size() - start);

	// Create a Vector of row IDs for this batch
	Vector row_id_vector(LogicalType::ROW_TYPE, count);
	auto row_id_data = FlatVector::GetData<row_t>(row_id_vector);
	for (idx_t i = 0; i < count; i++) {
		row_id_data[i] = gstate.row_ids[start + i];
	}

	// Fetch columns — only touches row groups containing these row IDs
	storage.Fetch(tx, output, gstate.storage_column_ids, row_id_vector, count, lstate.fetch_state);
}

//===----------------------------------------------------------------------===//
// Cardinality estimate
//===----------------------------------------------------------------------===//

static unique_ptr<NodeStatistics> LogsearchScanCardinality(ClientContext &, const FunctionData *bind_data_p) {
	auto &bind_data = bind_data_p->Cast<LogsearchScanBindData>();
	return make_uniq<NodeStatistics>(bind_data.estimated_count, bind_data.estimated_count);
}

//===----------------------------------------------------------------------===//
// Get the table function
//===----------------------------------------------------------------------===//

TableFunction LogsearchIndexScan::GetFunction() {
	TableFunction func("logsearch_index_scan", {}, LogsearchScanFunction, LogsearchScanBind,
	                   LogsearchScanInitGlobal, LogsearchScanInitLocal);
	func.projection_pushdown = true;
	func.filter_pushdown = false;
	func.cardinality = LogsearchScanCardinality;
	return func;
}

} // namespace duckdb
