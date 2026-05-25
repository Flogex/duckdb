#include "bloom_index.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/storage_info.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/storage/table_io_manager.hpp"

#include <algorithm>
#include <cstdio>

#define BLOOM_BUILD_TRACE(...)                                                                                         \
	do {                                                                                                               \
		std::fprintf(stderr, "[bloom_index/build] " __VA_ARGS__);                                                      \
		std::fprintf(stderr, "\n");                                                                                    \
	} while (0)

namespace duckdb {

template <class T>
static void HashTypedColumn(const UnifiedVectorFormat &fmt, idx_t count, vector<uint64_t> &out, vector<bool> &valid) {
	auto data = UnifiedVectorFormat::GetData<T>(fmt);
	for (idx_t i = 0; i < count; i++) {
		auto src = fmt.sel->get_index(i);
		if (!fmt.validity.RowIsValid(src)) {
			valid[i] = false;
			out[i] = 0;
		} else {
			valid[i] = true;
			out[i] = Hash<T>(data[src]);
		}
	}
}

template <class T, class CAST>
static void HashTypedColumnCast(const UnifiedVectorFormat &fmt, idx_t count, vector<uint64_t> &out,
                                vector<bool> &valid) {
	auto data = UnifiedVectorFormat::GetData<T>(fmt);
	for (idx_t i = 0; i < count; i++) {
		auto src = fmt.sel->get_index(i);
		if (!fmt.validity.RowIsValid(src)) {
			valid[i] = false;
			out[i] = 0;
		} else {
			valid[i] = true;
			out[i] = Hash<CAST>(static_cast<CAST>(data[src]));
		}
	}
}

//! Hash a column of values. Writes 64-bit hash to out[i]; valid[i] = false if value at i is NULL.
static void HashVectorColumn(Vector &v, idx_t count, vector<uint64_t> &out, vector<bool> &valid) {
	out.assign(count, 0);
	valid.assign(count, false);
	UnifiedVectorFormat fmt;
	v.ToUnifiedFormat(fmt);
	switch (v.GetType().InternalType()) {
	case PhysicalType::BOOL:
		HashTypedColumnCast<bool, uint32_t>(fmt, count, out, valid);
		break;
	case PhysicalType::INT8:
		HashTypedColumnCast<int8_t, uint32_t>(fmt, count, out, valid);
		break;
	case PhysicalType::INT16:
		HashTypedColumnCast<int16_t, uint32_t>(fmt, count, out, valid);
		break;
	case PhysicalType::INT32:
		HashTypedColumnCast<int32_t, uint32_t>(fmt, count, out, valid);
		break;
	case PhysicalType::INT64:
		HashTypedColumn<int64_t>(fmt, count, out, valid);
		break;
	case PhysicalType::UINT8:
		HashTypedColumnCast<uint8_t, uint32_t>(fmt, count, out, valid);
		break;
	case PhysicalType::UINT16:
		HashTypedColumnCast<uint16_t, uint32_t>(fmt, count, out, valid);
		break;
	case PhysicalType::UINT32:
		HashTypedColumn<uint32_t>(fmt, count, out, valid);
		break;
	case PhysicalType::UINT64:
		HashTypedColumn<uint64_t>(fmt, count, out, valid);
		break;
	case PhysicalType::INT128:
		HashTypedColumn<hugeint_t>(fmt, count, out, valid);
		break;
	case PhysicalType::UINT128:
		HashTypedColumn<uhugeint_t>(fmt, count, out, valid);
		break;
	case PhysicalType::FLOAT:
		HashTypedColumn<float>(fmt, count, out, valid);
		break;
	case PhysicalType::DOUBLE:
		HashTypedColumn<double>(fmt, count, out, valid);
		break;
	case PhysicalType::VARCHAR:
		HashTypedColumn<string_t>(fmt, count, out, valid);
		break;
	case PhysicalType::INTERVAL:
		HashTypedColumn<interval_t>(fmt, count, out, valid);
		break;
	default:
		throw InvalidTypeException(v.GetType(), "bloom_index: unsupported physical type for hashing");
	}
}

//! Hash a single Value (used by the optimizer when probing with a constant).
uint64_t BloomIndexHashValue(const Value &v) {
	if (v.IsNull()) {
		return 0;
	}
	switch (v.type().InternalType()) {
	case PhysicalType::BOOL:
		return Hash<uint32_t>(v.GetValue<bool>() ? 1u : 0u);
	case PhysicalType::INT8:
		return Hash<uint32_t>(static_cast<uint32_t>(v.GetValue<int8_t>()));
	case PhysicalType::INT16:
		return Hash<uint32_t>(static_cast<uint32_t>(v.GetValue<int16_t>()));
	case PhysicalType::INT32:
		return Hash<uint32_t>(static_cast<uint32_t>(v.GetValue<int32_t>()));
	case PhysicalType::INT64:
		return Hash<int64_t>(v.GetValue<int64_t>());
	case PhysicalType::UINT8:
		return Hash<uint32_t>(static_cast<uint32_t>(v.GetValue<uint8_t>()));
	case PhysicalType::UINT16:
		return Hash<uint32_t>(static_cast<uint32_t>(v.GetValue<uint16_t>()));
	case PhysicalType::UINT32:
		return Hash<uint32_t>(v.GetValue<uint32_t>());
	case PhysicalType::UINT64:
		return Hash<uint64_t>(v.GetValue<uint64_t>());
	case PhysicalType::INT128:
		return Hash<hugeint_t>(v.GetValue<hugeint_t>());
	case PhysicalType::UINT128:
		return Hash<uhugeint_t>(v.GetValue<uhugeint_t>());
	case PhysicalType::FLOAT:
		return Hash<float>(v.GetValue<float>());
	case PhysicalType::DOUBLE:
		return Hash<double>(v.GetValue<double>());
	case PhysicalType::VARCHAR: {
		auto s = v.GetValue<string>();
		return Hash(s.c_str(), s.size());
	}
	case PhysicalType::INTERVAL:
		return Hash<interval_t>(v.GetValue<interval_t>());
	default:
		return 0; // unsupported -> caller treats 0 as "cannot probe", returning all row groups
	}
}

//===--------------------------------------------------------------------===//
// BloomIndex
//===--------------------------------------------------------------------===//

BloomIndex::BloomIndex(const string &name_p, IndexConstraintType constraint_type,
                       const vector<column_t> &column_ids_p, TableIOManager &table_io_manager_p,
                       const vector<unique_ptr<Expression>> &unbound_expressions_p, AttachedDatabase &db_p,
                       double fpp_p, idx_t expected_n_p)
    : BoundIndex(name_p, BloomIndex::TYPE_NAME, constraint_type, column_ids_p, table_io_manager_p,
                 unbound_expressions_p, db_p),
      fpp(fpp_p), expected_n(expected_n_p) {
	if (column_ids_p.size() != 1) {
		throw NotImplementedException("bloom_index: only single-column indexes are supported");
	}
	if (fpp <= 0 || fpp >= 1) {
		throw InvalidInputException("bloom_index: fpp must be in (0,1)");
	}
	if (expected_n == 0) {
		expected_n = BloomIndex::DEFAULT_EXPECTED_N;
	}
}

unique_ptr<BoundIndex> BloomIndex::Create(CreateIndexInput &input) {
	std::fprintf(stderr, "[bloom_index/build] BloomIndex::Create called (create_instance path) for index '%s'\n",
	             input.name.c_str());
	double fpp = BloomIndex::DEFAULT_FPP;
	idx_t expected_n = BloomIndex::DEFAULT_EXPECTED_N;

	for (auto &opt : input.options) {
		if (StringUtil::CIEquals(opt.first, "fpp")) {
			fpp = opt.second.GetValue<double>();
		} else if (StringUtil::CIEquals(opt.first, "expected_n")) {
			expected_n = opt.second.GetValue<idx_t>();
		} else {
			throw InvalidInputException("bloom_index: unknown index option '%s'", opt.first);
		}
	}

	return make_uniq<BloomIndex>(input.name, input.constraint_type, input.column_ids, input.table_io_manager,
	                             input.unbound_expressions, input.db, fpp, expected_n);
}

RowGroupBloomFilter *BloomIndex::FindByRowStart(idx_t row_start) {
	lock_guard<mutex> l(blooms_lock);
	auto it = std::lower_bound(blooms.begin(), blooms.end(), row_start,
	                           [](const RowGroupBloom &b, idx_t v) { return b.row_start < v; });
	if (it == blooms.end() || it->row_start != row_start) {
		return nullptr;
	}
	return it->filter.get();
}

vector<BloomIndex::BloomSnapshotEntry> BloomIndex::SnapshotEntries() {
	lock_guard<mutex> l(blooms_lock);
	vector<BloomSnapshotEntry> out;
	out.reserve(blooms.size());
	for (auto &b : blooms) {
		out.push_back({b.row_start, b.row_count, b.filter.get()});
	}
	return out;
}

idx_t BloomIndex::LocateOrAllocateBucket(row_t row_id) {
	// Caller holds blooms_lock.
	auto rid = static_cast<idx_t>(row_id);
	// Binary search: find the bucket with row_start <= rid and rid < row_start + row_count.
	auto it = std::upper_bound(blooms.begin(), blooms.end(), rid,
	                           [](idx_t v, const RowGroupBloom &b) { return v < b.row_start; });
	if (it != blooms.begin()) {
		auto prev = std::prev(it);
		if (rid < prev->row_start + prev->row_count) {
			return static_cast<idx_t>(prev - blooms.begin());
		}
	}
	// Need a new tail bucket. Align row_start to DEFAULT_ROW_GROUP_SIZE for predictability.
	idx_t aligned_start = (rid / DEFAULT_ROW_GROUP_SIZE) * DEFAULT_ROW_GROUP_SIZE;
	if (!blooms.empty()) {
		auto &tail = blooms.back();
		idx_t tail_end = tail.row_start + tail.row_count;
		if (aligned_start < tail_end) {
			aligned_start = tail_end;
		}
	}
	RowGroupBloom rgb;
	rgb.row_start = aligned_start;
	rgb.row_count = DEFAULT_ROW_GROUP_SIZE;
	rgb.filter = make_uniq<RowGroupBloomFilter>(expected_n, fpp);
	blooms.push_back(std::move(rgb));
	return blooms.size() - 1;
}

void BloomIndex::InsertHashForRowId(uint64_t hash, row_t row_id) {
	lock_guard<mutex> l(blooms_lock);
	auto bucket_idx = LocateOrAllocateBucket(row_id);
	blooms[bucket_idx].filter->Insert(hash);
}

void BloomIndex::EnsureBucket(idx_t row_start, idx_t row_count) {
	lock_guard<mutex> l(blooms_lock);
	auto it = std::lower_bound(blooms.begin(), blooms.end(), row_start,
	                           [](const RowGroupBloom &b, idx_t v) { return b.row_start < v; });
	if (it != blooms.end() && it->row_start == row_start) {
		// Already exists. Extend row_count if needed.
		if (row_count > it->row_count) {
			it->row_count = row_count;
		}
		return;
	}
	RowGroupBloom rgb;
	rgb.row_start = row_start;
	rgb.row_count = row_count;
	rgb.filter = make_uniq<RowGroupBloomFilter>(expected_n, fpp);
	blooms.insert(it, std::move(rgb));
}

ErrorData BloomIndex::Append(IndexLock &l, DataChunk &chunk, Vector &row_ids) {
	return Insert(l, chunk, row_ids);
}

void BloomIndex::InsertKeys(DataChunk &key_chunk, Vector &row_ids) {
	auto count = key_chunk.size();
	if (count == 0) {
		return;
	}
	D_ASSERT(key_chunk.ColumnCount() == 1);

	vector<uint64_t> hashes;
	vector<bool> valid;
	HashVectorColumn(key_chunk.data[0], count, hashes, valid);

	UnifiedVectorFormat row_ids_fmt;
	row_ids.ToUnifiedFormat(row_ids_fmt);
	auto row_id_data = UnifiedVectorFormat::GetData<row_t>(row_ids_fmt);

	lock_guard<mutex> l(blooms_lock);
	for (idx_t i = 0; i < count; i++) {
		if (!valid[i]) {
			continue;
		}
		auto src = row_ids_fmt.sel->get_index(i);
		auto bucket_idx = LocateOrAllocateBucket(row_id_data[src]);
		blooms[bucket_idx].filter->Insert(hashes[i]);
	}
}

ErrorData BloomIndex::Insert(IndexLock &, DataChunk &chunk, Vector &row_ids) {
	if (chunk.size() == 0) {
		return ErrorData();
	}
	// `chunk` may be the full table_chunk passed by DataTable::AppendToIndexes. Use ExecuteExpressions to extract the
	// indexed column via the bound_expressions set up by BoundIndex.
	DataChunk key_chunk;
	key_chunk.Initialize(Allocator::DefaultAllocator(), logical_types);
	ExecuteExpressions(chunk, key_chunk);
	InsertKeys(key_chunk, row_ids);
	return ErrorData();
}

void BloomIndex::ResetStorage(IndexLock &) {
	lock_guard<mutex> l(blooms_lock);
	blooms.clear();
}

bool BloomIndex::MergeIndexes(IndexLock &, BoundIndex &other_index) {
	auto &other = other_index.Cast<BloomIndex>();
	lock_guard<mutex> l1(blooms_lock);
	lock_guard<mutex> l2(other.blooms_lock);
	for (auto &src : other.blooms) {
		auto it = std::lower_bound(blooms.begin(), blooms.end(), src.row_start,
		                           [](const RowGroupBloom &b, idx_t v) { return b.row_start < v; });
		if (it != blooms.end() && it->row_start == src.row_start) {
			if (it->filter->BlockCount() == src.filter->BlockCount()) {
				it->filter->MergeFrom(*src.filter);
			} else {
				// Differently sized blooms (shouldn't happen post-CREATE, but be safe): keep ours.
			}
		} else {
			RowGroupBloom rgb;
			rgb.row_start = src.row_start;
			rgb.row_count = src.row_count;
			rgb.filter = make_uniq<RowGroupBloomFilter>(expected_n, fpp);
			rgb.filter->MergeFrom(*src.filter);
			blooms.insert(it, std::move(rgb));
		}
	}
	return true;
}

void BloomIndex::Vacuum(IndexLock &) {
	// No-op: deletes are not tracked; vacuuming would require a full rebuild.
}

idx_t BloomIndex::GetInMemorySize(IndexLock &) {
	lock_guard<mutex> l(blooms_lock);
	idx_t total = 0;
	for (auto &b : blooms) {
		total += b.filter->MemoryUsage();
	}
	return total;
}

void BloomIndex::Verify(IndexLock &) {
}

string BloomIndex::ToString(IndexLock &, bool) {
	lock_guard<mutex> l(blooms_lock);
	return StringUtil::Format("BloomIndex(name=%s, row_groups=%llu, fpp=%f, expected_n=%llu)", name,
	                          (unsigned long long)blooms.size(), fpp, (unsigned long long)expected_n);
}

void BloomIndex::VerifyAllocations(IndexLock &) {
}

void BloomIndex::VerifyBuffers(IndexLock &) {
}

string BloomIndex::GetConstraintViolationMessage(VerifyExistenceType, idx_t, DataChunk &) {
	throw InternalException("bloom_index: indexes do not enforce constraints");
}

IndexStorageInfo BloomIndex::SerializeToDisk(QueryContext, const case_insensitive_map_t<Value> &) {
	// v1: in-memory only. On reattach, the catalog will reload the index entry but Create() returns an empty index.
	// A follow-up will persist per-row-group blooms via the index block manager.
	return IndexStorageInfo(name);
}

IndexStorageInfo BloomIndex::SerializeToWAL(const case_insensitive_map_t<Value> &) {
	return IndexStorageInfo(name);
}

//===--------------------------------------------------------------------===//
// IndexType registration
//===--------------------------------------------------------------------===//

namespace {

class BloomBuildBindData : public IndexBuildBindData {
public:
	double fpp = BloomIndex::DEFAULT_FPP;
	idx_t expected_n = BloomIndex::DEFAULT_EXPECTED_N;
};

class BloomBuildGlobalState : public IndexBuildGlobalState {
public:
	unique_ptr<BoundIndex> global_index;
};

class BloomBuildLocalState : public IndexBuildLocalState {
public:
	unique_ptr<BoundIndex> local_index;
};

unique_ptr<IndexBuildBindData> BloomBuildBind(IndexBuildBindInput &input) {
	BLOOM_BUILD_TRACE("BloomBuildBind");
	auto data = make_uniq<BloomBuildBindData>();
	for (auto &opt : input.info.options) {
		if (StringUtil::CIEquals(opt.first, "fpp")) {
			data->fpp = opt.second.GetValue<double>();
		} else if (StringUtil::CIEquals(opt.first, "expected_n")) {
			data->expected_n = opt.second.GetValue<idx_t>();
		} else {
			throw InvalidInputException("bloom_index: unknown index option '%s'", opt.first);
		}
	}
	return std::move(data);
}

bool BloomBuildSort(IndexBuildSortInput &) {
	return false; // no sorting required
}

unique_ptr<IndexBuildGlobalState> BloomBuildGlobalInit(IndexBuildInitGlobalStateInput &input) {
	BLOOM_BUILD_TRACE("BloomBuildGlobalInit");
	auto &bind_data = input.bind_data->Cast<BloomBuildBindData>();
	auto state = make_uniq<BloomBuildGlobalState>();
	auto &storage = input.table.GetStorage();
	state->global_index =
	    make_uniq<BloomIndex>(input.info.index_name, input.info.constraint_type, input.storage_ids,
	                          TableIOManager::Get(storage), input.expressions, storage.db, bind_data.fpp,
	                          bind_data.expected_n);

	auto &bloom = state->global_index->Cast<BloomIndex>();
	auto partition_stats = storage.GetPartitionStats(input.context);
	BLOOM_BUILD_TRACE("  partition_stats.size at build = %llu", (unsigned long long)partition_stats.size());
	for (auto &p : partition_stats) {
		if (!p.row_start.IsValid()) {
			continue;
		}
		bloom.EnsureBucket(p.row_start.GetIndex(), p.count);
	}
	BLOOM_BUILD_TRACE("  global bloom now has %llu buckets",
	                  (unsigned long long)bloom.SnapshotEntries().size());
	return std::move(state);
}

unique_ptr<IndexBuildLocalState> BloomBuildLocalInit(IndexBuildInitLocalStateInput &input) {
	auto state = make_uniq<BloomBuildLocalState>();
	auto &gstate_bind = input.bind_data->Cast<BloomBuildBindData>();
	auto &storage = input.table.GetStorage();
	state->local_index =
	    make_uniq<BloomIndex>(input.info.index_name, input.info.constraint_type, input.storage_ids,
	                          TableIOManager::Get(storage), input.expressions, storage.db, gstate_bind.fpp,
	                          gstate_bind.expected_n);
	// Pre-allocate matching buckets in the local index so OR-merge in combine works directly.
	auto &local_bloom = state->local_index->Cast<BloomIndex>();
	auto partition_stats = storage.GetPartitionStats(input.context);
	for (auto &p : partition_stats) {
		if (!p.row_start.IsValid()) {
			continue;
		}
		local_bloom.EnsureBucket(p.row_start.GetIndex(), p.count);
	}
	return std::move(state);
}

void BloomBuildSink(IndexBuildSinkInput &input, DataChunk &key_chunk, DataChunk &row_chunk) {
	auto &lstate = input.local_state.Cast<BloomBuildLocalState>();
	auto &local_bloom = lstate.local_index->Cast<BloomIndex>();
	static thread_local idx_t sink_call_count = 0;
	if (sink_call_count++ < 3) {
		BLOOM_BUILD_TRACE("BloomBuildSink #%llu key_chunk size=%llu cols=%llu, row_chunk size=%llu cols=%llu",
		                  (unsigned long long)sink_call_count, (unsigned long long)key_chunk.size(),
		                  (unsigned long long)key_chunk.ColumnCount(), (unsigned long long)row_chunk.size(),
		                  (unsigned long long)row_chunk.ColumnCount());
	}
	local_bloom.InsertKeys(key_chunk, row_chunk.data[0]);
}

void BloomBuildCombine(IndexBuildCombineInput &input) {
	BLOOM_BUILD_TRACE("BloomBuildCombine");
	auto &gstate = input.global_state.Cast<BloomBuildGlobalState>();
	auto &lstate = input.local_state.Cast<BloomBuildLocalState>();
	BLOOM_BUILD_TRACE("  global buckets before=%llu, local buckets=%llu",
	                  (unsigned long long)gstate.global_index->Cast<BloomIndex>().SnapshotEntries().size(),
	                  (unsigned long long)lstate.local_index->Cast<BloomIndex>().SnapshotEntries().size());
	IndexLock dummy;
	gstate.global_index->MergeIndexes(dummy, *lstate.local_index);
	BLOOM_BUILD_TRACE("  global buckets after=%llu",
	                  (unsigned long long)gstate.global_index->Cast<BloomIndex>().SnapshotEntries().size());
}

unique_ptr<BoundIndex> BloomBuildFinalize(IndexBuildFinalizeInput &input) {
	auto &gstate = input.global_state.Cast<BloomBuildGlobalState>();
	BLOOM_BUILD_TRACE("BloomBuildFinalize: global buckets=%llu",
	                  (unsigned long long)gstate.global_index->Cast<BloomIndex>().SnapshotEntries().size());
	return std::move(gstate.global_index);
}

} // namespace

IndexType BloomIndex::GetBloomIndexType() {
	IndexType t;
	t.name = BloomIndex::TYPE_NAME;
	t.create_instance = BloomIndex::Create;
	t.build_bind = BloomBuildBind;
	t.build_sort = BloomBuildSort;
	t.build_global_init = BloomBuildGlobalInit;
	t.build_local_init = BloomBuildLocalInit;
	t.build_sink = BloomBuildSink;
	t.build_combine = BloomBuildCombine;
	t.build_finalize = BloomBuildFinalize;
	return t;
}

} // namespace duckdb
