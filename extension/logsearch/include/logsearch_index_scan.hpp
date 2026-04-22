//===----------------------------------------------------------------------===//
//                         DuckDB
//
// logsearch_index_scan.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

//! Table function that fetches specific rows by row ID from a table.
//! Used by the logsearch optimizer to replace full table scans with
//! targeted row fetches based on inverted index results.
//!
//! The index is queried at execution time (in InitGlobal), not at plan time.
//! This ensures fresh data and avoids materializing row IDs during planning.
//! Row groups that contain no matching row IDs are never touched.
struct LogsearchIndexScan {
	static TableFunction GetFunction();
};

//! Bind data for the index scan.
//! Contains only serializable search parameters — no pointers, no row IDs.
//! The actual index lookup happens at execution time in InitGlobal.
struct LogsearchScanBindData : public TableFunctionData {
	//! Table location
	string catalog_name;
	string schema_name;
	string table_name;

	//! Index name (looked up by name at execution time)
	string index_name;

	//! Search parameters
	vector<string> search_terms;  // Token search (AND semantics)
	string search_prefix;         // Prefix search
	bool is_prefix = false;

	//! Timestamp bounds for partition pruning
	timestamp_t ts_min;
	timestamp_t ts_max;
	bool has_ts_bounds = false;

	//! Estimated result count (from selectivity check at optimizer time)
	idx_t estimated_count = 0;

	//! Column types and names (copied from original LogicalGet)
	vector<LogicalType> all_types;
	vector<string> all_names;
};

} // namespace duckdb
