#include "logsearch_build.hpp"
#include "logsearch_tokenizer.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/partition_stats.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/storage/table_io_manager.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// Bind
//===----------------------------------------------------------------------===//

class LogsearchBuildBindData : public IndexBuildBindData {
public:
	string timestamp_column_name;
};

unique_ptr<IndexBuildBindData> LogsearchBuildBind(IndexBuildBindInput &input) {
	auto bind_data = make_uniq<LogsearchBuildBindData>();

	// Validate: must index exactly one VARCHAR column
	if (input.expressions.size() != 1) {
		throw BinderException("Logsearch index requires exactly one column");
	}
	if (input.expressions[0]->return_type.id() != LogicalTypeId::VARCHAR) {
		throw BinderException("Logsearch index can only be created on VARCHAR columns");
	}

	// Extract optional timestamp_column from WITH options
	auto it = input.info.options.find("timestamp_column");
	if (it != input.info.options.end()) {
		bind_data->timestamp_column_name = it->second.ToString();
	}

	return std::move(bind_data);
}

//===----------------------------------------------------------------------===//
// Sort — no sorting needed for inverted index build
//===----------------------------------------------------------------------===//

bool LogsearchBuildSort(IndexBuildSortInput &) {
	return false;
}

//===----------------------------------------------------------------------===//
// Global State
//===----------------------------------------------------------------------===//

class LogsearchBuildGlobalState : public IndexBuildGlobalState {
public:
	unique_ptr<LogsearchIndex> global_index;
	//! Pre-computed row group boundaries with timestamp ranges
	vector<RowGroupBoundary> boundaries;
};

unique_ptr<IndexBuildGlobalState> LogsearchBuildGlobalInit(IndexBuildInitGlobalStateInput &input) {
	auto state = make_uniq<LogsearchBuildGlobalState>();
	auto &bind_data = input.bind_data->Cast<LogsearchBuildBindData>();

	auto &storage = input.table.GetStorage();
	state->global_index = make_uniq<LogsearchIndex>(input.info.index_name, input.info.constraint_type,
	                                                input.storage_ids, TableIOManager::Get(storage),
	                                                input.expressions, storage.db);
	state->global_index->timestamp_column_name = bind_data.timestamp_column_name;

	// Resolve timestamp column storage index if specified
	optional_idx ts_storage_idx;
	if (!bind_data.timestamp_column_name.empty()) {
		auto &columns = input.table.GetColumns();
		for (idx_t i = 0; i < columns.LogicalColumnCount(); i++) {
			auto &col = columns.GetColumn(LogicalIndex(i));
			if (col.GetName() == bind_data.timestamp_column_name) {
				ts_storage_idx = columns.LogicalToPhysical(LogicalIndex(i)).index;
				break;
			}
		}
		if (!ts_storage_idx.IsValid()) {
			throw BinderException("Timestamp column '%s' not found in table", bind_data.timestamp_column_name);
		}
	}

	// Pre-compute row group boundaries and timestamp zone maps via public API
	auto partition_stats = storage.GetPartitionStats(input.context);
	for (auto &ps : partition_stats) {
		RowGroupBoundary boundary;
		boundary.row_start = ps.row_start.IsValid() ? ps.row_start.GetIndex() : 0;
		boundary.row_count = ps.count;
		boundary.has_timestamp = false;

		if (ts_storage_idx.IsValid() && ps.partition_row_group) {
			StorageIndex storage_index(ts_storage_idx.GetIndex());
			auto col_stats = ps.partition_row_group->GetColumnStatistics(storage_index);
			if (col_stats) {
				boundary.ts_min = NumericStats::GetMin<timestamp_t>(*col_stats);
				boundary.ts_max = NumericStats::GetMax<timestamp_t>(*col_stats);
				boundary.has_timestamp = true;
			}
		}

		state->boundaries.push_back(boundary);

		// Create partition in the global index
		auto partition = make_uniq<IndexPartition>(boundary.row_start, boundary.row_count);
		if (boundary.has_timestamp) {
			partition->has_timestamp_bounds = true;
			partition->ts_min = boundary.ts_min;
			partition->ts_max = boundary.ts_max;
		}
		state->global_index->partitions.push_back(std::move(partition));
	}

	return std::move(state);
}

//===----------------------------------------------------------------------===//
// Local State
//===----------------------------------------------------------------------===//

class LogsearchBuildLocalState : public IndexBuildLocalState {
public:
	unique_ptr<LogsearchIndex> local_index;
};

unique_ptr<IndexBuildLocalState> LogsearchBuildLocalInit(IndexBuildInitLocalStateInput &input) {
	auto state = make_uniq<LogsearchBuildLocalState>();
	auto &bind_data = input.bind_data->Cast<LogsearchBuildBindData>();

	auto &storage = input.table.GetStorage();
	state->local_index = make_uniq<LogsearchIndex>(input.info.index_name, input.info.constraint_type, input.storage_ids,
	                                               TableIOManager::Get(storage), input.expressions, storage.db);
	state->local_index->timestamp_column_name = bind_data.timestamp_column_name;

	// Create matching partitions in the local index (same structure as global)
	// Re-compute boundaries from storage since we can't access global state here
	auto partition_stats = storage.GetPartitionStats(input.context);
	for (auto &ps : partition_stats) {
		idx_t row_start = ps.row_start.IsValid() ? ps.row_start.GetIndex() : 0;
		auto partition = make_uniq<IndexPartition>(row_start, ps.count);
		state->local_index->partitions.push_back(std::move(partition));
	}

	return std::move(state);
}

//===----------------------------------------------------------------------===//
// Sink — process data chunks
//===----------------------------------------------------------------------===//

void LogsearchBuildSink(IndexBuildSinkInput &input, DataChunk &key_chunk, DataChunk &row_chunk) {
	auto &lstate = input.local_state.Cast<LogsearchBuildLocalState>();
	auto &local_index = *lstate.local_index;

	auto &text_vector = key_chunk.data[0];
	auto &row_id_vector = row_chunk.data[0];

	UnifiedVectorFormat text_data;
	text_vector.ToUnifiedFormat(key_chunk.size(), text_data);
	auto row_id_data = FlatVector::GetData<row_t>(row_id_vector);

	for (idx_t i = 0; i < key_chunk.size(); i++) {
		auto text_idx = text_data.sel->get_index(i);
		if (!text_data.validity.RowIsValid(text_idx)) {
			continue; // Skip NULL
		}

		auto row_id = row_id_data[i];
		auto text_value = UnifiedVectorFormat::GetData<string_t>(text_data)[text_idx];

		// Find the partition for this row_id
		auto *partition = local_index.FindPartition(row_id);
		if (!partition) {
			continue; // Shouldn't happen during build
		}

		auto local_offset = UnsafeNumericCast<uint32_t>(UnsafeNumericCast<idx_t>(row_id) - partition->row_start);
		auto tokens = LogsearchTokenizer::Tokenize(text_value);
		for (auto &token : tokens) {
			partition->AddTerm(token, local_offset);
		}
	}
}

//===----------------------------------------------------------------------===//
// Combine — merge local into global
//===----------------------------------------------------------------------===//

void LogsearchBuildCombine(IndexBuildCombineInput &input) {
	auto &gstate = input.global_state.Cast<LogsearchBuildGlobalState>();
	auto &lstate = input.local_state.Cast<LogsearchBuildLocalState>();

	IndexLock lock;
	gstate.global_index->InitializeLock(lock);
	gstate.global_index->MergeIndexes(lock, *lstate.local_index);
}

//===----------------------------------------------------------------------===//
// Finalize — return the completed index
//===----------------------------------------------------------------------===//

unique_ptr<BoundIndex> LogsearchBuildFinalize(IndexBuildFinalizeInput &input) {
	auto &gstate = input.global_state.Cast<LogsearchBuildGlobalState>();
	return std::move(gstate.global_index);
}

//===----------------------------------------------------------------------===//
// IndexType registration
//===----------------------------------------------------------------------===//

IndexType LogsearchIndex::GetLogsearchIndexType() {
	IndexType logsearch_type;
	logsearch_type.name = LogsearchIndex::TYPE_NAME;
	logsearch_type.create_instance = LogsearchIndex::Create;
	logsearch_type.build_bind = LogsearchBuildBind;
	logsearch_type.build_sort = LogsearchBuildSort;
	logsearch_type.build_global_init = LogsearchBuildGlobalInit;
	logsearch_type.build_local_init = LogsearchBuildLocalInit;
	logsearch_type.build_sink = LogsearchBuildSink;
	logsearch_type.build_combine = LogsearchBuildCombine;
	logsearch_type.build_finalize = LogsearchBuildFinalize;
	return logsearch_type;
}

} // namespace duckdb
