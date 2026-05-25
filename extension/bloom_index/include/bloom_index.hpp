#pragma once

#include "bloom_filter.hpp"
#include "duckdb/execution/index/bound_index.hpp"
#include "duckdb/execution/index/index_type.hpp"
#include "duckdb/common/mutex.hpp"

namespace duckdb {

//! Hash a constant value the same way BloomIndex hashes column values. Returns 0 for unsupported / NULL values
//! (callers should treat 0 as "cannot probe" and conservatively keep the row group).
uint64_t BloomIndexHashValue(const Value &v);

struct RowGroupBloom {
	idx_t row_start;
	idx_t row_count;
	unique_ptr<RowGroupBloomFilter> filter;
};

//! Per-row-group bloom filter index for equality pruning on high-cardinality non-unique columns.
class BloomIndex : public BoundIndex {
public:
	static constexpr const char *TYPE_NAME = "BLOOM";
	static constexpr double DEFAULT_FPP = 0.01;
	static constexpr idx_t DEFAULT_EXPECTED_N = 122880; // STANDARD_ROW_GROUP_SIZE

public:
	BloomIndex(const string &name, IndexConstraintType constraint_type, const vector<column_t> &column_ids,
	           TableIOManager &table_io_manager, const vector<unique_ptr<Expression>> &unbound_expressions,
	           AttachedDatabase &db, double fpp, idx_t expected_n);

	//! Factory used by IndexType::create_instance. Loads existing on-disk state if storage_info present.
	static unique_ptr<BoundIndex> Create(CreateIndexInput &input);

	//! Returns the IndexType descriptor for registration.
	static IndexType GetBloomIndexType();

public:
	//! Find the bloom corresponding to a given row_start (exact match). nullptr if absent.
	RowGroupBloomFilter *FindByRowStart(idx_t row_start);

	//! Read-only snapshot of all row group blooms; copy out for the optimizer to probe under lock-free.
	struct BloomSnapshotEntry {
		idx_t row_start;
		idx_t row_count;
		RowGroupBloomFilter *filter;
	};
	vector<BloomSnapshotEntry> SnapshotEntries();

	double GetFpp() const {
		return fpp;
	}
	idx_t GetExpectedN() const {
		return expected_n;
	}

public:
	// BoundIndex virtuals
	ErrorData Append(IndexLock &l, DataChunk &chunk, Vector &row_ids) override;
	ErrorData Insert(IndexLock &l, DataChunk &chunk, Vector &row_ids) override;
	void ResetStorage(IndexLock &index_lock) override;
	bool MergeIndexes(IndexLock &state, BoundIndex &other_index) override;
	void Vacuum(IndexLock &l) override;
	idx_t GetInMemorySize(IndexLock &state) override;
	void Verify(IndexLock &l) override;
	string ToString(IndexLock &l, bool display_ascii = false) override;
	void VerifyAllocations(IndexLock &l) override;
	void VerifyBuffers(IndexLock &l) override;
	string GetConstraintViolationMessage(VerifyExistenceType verify_type, idx_t failed_index, DataChunk &input) override;
	IndexStorageInfo SerializeToDisk(QueryContext context, const case_insensitive_map_t<Value> &options) override;
	IndexStorageInfo SerializeToWAL(const case_insensitive_map_t<Value> &options) override;

	//! Build-side helper: route (hash, row_t) into the right RowGroupBloom, allocating new buckets at row group
	//! boundaries when row_t exceeds the current tail.
	void InsertHashForRowId(uint64_t hash, row_t row_id);

	//! Build-side helper: pre-allocate buckets given the (row_start, row_count) layout we observed at CREATE INDEX.
	void EnsureBucket(idx_t row_start, idx_t row_count);

	//! Insert a single-column key chunk and matching row IDs (no ExecuteExpressions, used by the IndexType build sink).
	void InsertKeys(DataChunk &key_chunk, Vector &row_ids);

private:
	mutex blooms_lock;
	//! Per-row-group blooms sorted by row_start.
	vector<RowGroupBloom> blooms;

	double fpp;
	idx_t expected_n;

	//! Compute bucket index for a row_t; if no bucket covers it, allocate a new tail bucket aligned to
	//! STANDARD_ROW_GROUP_SIZE. Caller must hold blooms_lock.
	idx_t LocateOrAllocateBucket(row_t row_id);
};

} // namespace duckdb
