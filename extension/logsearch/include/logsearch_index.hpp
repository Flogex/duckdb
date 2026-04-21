//===----------------------------------------------------------------------===//
//                         DuckDB
//
// logsearch_index.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/index/bound_index.hpp"
#include "duckdb/execution/index/index_type.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/storage/table/append_state.hpp"

#include <map>

namespace duckdb {

//===----------------------------------------------------------------------===//
// PostingList — sorted vector of local row offsets within a row group
//===----------------------------------------------------------------------===//
class PostingList {
public:
	PostingList() = default;

	//! Add a local row offset. Must be called in sorted order.
	void Add(uint32_t local_offset);

	//! Number of entries
	idx_t GetCount() const;
	bool IsEmpty() const;

	//! Access the underlying sorted data
	const vector<uint32_t> &GetData() const;

	//! Convert local offsets to global row IDs
	vector<row_t> ToGlobalRowIds(idx_t row_start) const;

	//! Set operations — both inputs must be sorted
	static PostingList Intersect(const PostingList &a, const PostingList &b);
	static PostingList Union(const PostingList &a, const PostingList &b);

	//! Memory usage in bytes
	idx_t GetMemoryUsage() const;

	// TODO: evaluate roaring bitmaps as backend
	// TODO: evaluate skip lists for faster intersection
	// TODO: delta compression of row IDs

private:
	vector<uint32_t> data_;
};

//===----------------------------------------------------------------------===//
// IndexPartition — per-row-group inverted index
//===----------------------------------------------------------------------===//
class IndexPartition {
public:
	IndexPartition(idx_t row_start, idx_t row_count);

	//! Row group boundaries
	idx_t row_start;
	idx_t row_count;

	//! Timestamp range for pruning (optional)
	bool has_timestamp_bounds = false;
	timestamp_t ts_min;
	timestamp_t ts_max;

	//! Add a term occurrence at a local offset
	void AddTerm(const string &term, uint32_t local_offset);

	//! Look up a term — returns nullptr if not found
	const PostingList *Search(const string &term) const;

	//! Prefix search — returns all posting lists for terms starting with prefix
	vector<const PostingList *> PrefixSearch(const string &prefix) const;

	//! Document frequency for a term (posting list size)
	idx_t GetDocFrequency(const string &term) const;

	//! Total rows in this partition
	idx_t GetTotalRows() const;

	//! Check if a timestamp range overlaps with this partition
	bool OverlapsTimestampRange(timestamp_t query_min, timestamp_t query_max) const;

	//! Memory usage in bytes
	idx_t GetMemoryUsage() const;

	//! Merge another partition into this one (for combine step).
	//! The other partition must have the same row_start and row_count.
	void Merge(const IndexPartition &other);

private:
	//! Dictionary: term -> index into posting_lists_
	std::map<string, idx_t> dictionary_;
	//! Posting lists indexed by dictionary value
	vector<PostingList> posting_lists_;
};

//===----------------------------------------------------------------------===//
// LogsearchIndex — BoundIndex subclass, one per table
//===----------------------------------------------------------------------===//
class LogsearchIndex : public BoundIndex {
public:
	static constexpr const char *TYPE_NAME = "logsearch";

	LogsearchIndex(const string &name, IndexConstraintType constraint_type, const vector<column_t> &column_ids,
	               TableIOManager &table_io_manager, const vector<unique_ptr<Expression>> &unbound_expressions,
	               AttachedDatabase &db);

	//! The IndexType descriptor for registration
	static IndexType GetLogsearchIndexType();

	//! Factory for create_instance callback
	static unique_ptr<BoundIndex> Create(CreateIndexInput &input);

	//! Partitions (one per row group)
	vector<unique_ptr<IndexPartition>> partitions;

	//! Timestamp column name (from WITH options, empty if not set)
	string timestamp_column_name;

	//! Search for a term across all partitions, with optional timestamp pruning.
	//! Returns sorted global row IDs.
	vector<row_t> SearchTerm(const string &term, timestamp_t ts_min = Timestamp::FromEpochMicroSeconds(0),
	                         timestamp_t ts_max = Timestamp::FromEpochMicroSeconds(NumericLimits<int64_t>::Maximum()),
	                         bool has_ts_bounds = false) const;

	//! Search for multiple terms (AND semantics) with optional timestamp pruning.
	vector<row_t> SearchTerms(const vector<string> &terms,
	                          timestamp_t ts_min = Timestamp::FromEpochMicroSeconds(0),
	                          timestamp_t ts_max = Timestamp::FromEpochMicroSeconds(NumericLimits<int64_t>::Maximum()),
	                          bool has_ts_bounds = false) const;

	//! Prefix search — find all terms starting with prefix, union posting lists.
	vector<row_t> SearchPrefix(const string &prefix, timestamp_t ts_min = Timestamp::FromEpochMicroSeconds(0),
	                           timestamp_t ts_max = Timestamp::FromEpochMicroSeconds(NumericLimits<int64_t>::Maximum()),
	                           bool has_ts_bounds = false) const;

	//! Get selectivity of a term (fraction of total rows)
	double GetTermSelectivity(const string &term) const;

	//! Total rows across all partitions
	idx_t GetTotalRows() const;

	//===--------------------------------------------------------------------===//
	// BoundIndex interface
	//===--------------------------------------------------------------------===//
	ErrorData Append(IndexLock &l, DataChunk &chunk, Vector &row_ids) override;
	void CommitDrop(IndexLock &index_lock) override;
	ErrorData Insert(IndexLock &l, DataChunk &chunk, Vector &row_ids) override;
	bool MergeIndexes(IndexLock &state, BoundIndex &other_index) override;
	void Vacuum(IndexLock &l) override;
	idx_t GetInMemorySize(IndexLock &state) override;
	void Verify(IndexLock &l) override;
	string ToString(IndexLock &l, bool display_ascii = false) override;
	void VerifyAllocations(IndexLock &l) override;
	idx_t TryDelete(IndexLock &state, DataChunk &entries, Vector &row_identifiers,
	                optional_ptr<SelectionVector> deleted_sel = nullptr,
	                optional_ptr<SelectionVector> non_deleted_sel = nullptr) override;
	void VerifyBuffers(IndexLock &l) override;
	string GetConstraintViolationMessage(VerifyExistenceType verify_type, idx_t failed_index,
	                                     DataChunk &input) override;

	//! Find the partition for a given global row_id. Returns nullptr if not found.
	IndexPartition *FindPartition(row_t row_id);

private:

	//! Add a row to the index
	void AddRow(row_t row_id, const string_t &value);
};

//===----------------------------------------------------------------------===//
// Row group boundary info — pre-computed during build
//===----------------------------------------------------------------------===//
struct RowGroupBoundary {
	idx_t row_start;
	idx_t row_count;
	timestamp_t ts_min;
	timestamp_t ts_max;
	bool has_timestamp;
};

} // namespace duckdb
