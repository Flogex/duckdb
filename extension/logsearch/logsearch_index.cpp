#include "logsearch_index.hpp"
#include "logsearch_tokenizer.hpp"

#include "duckdb/common/types/vector.hpp"
#include "duckdb/storage/table_io_manager.hpp"

#include <algorithm>

namespace duckdb {

//===----------------------------------------------------------------------===//
// PostingList
//===----------------------------------------------------------------------===//

void PostingList::Add(uint32_t local_offset) {
	D_ASSERT(data_.empty() || local_offset > data_.back());
	data_.push_back(local_offset);
}

idx_t PostingList::GetCount() const {
	return data_.size();
}

bool PostingList::IsEmpty() const {
	return data_.empty();
}

const vector<uint32_t> &PostingList::GetData() const {
	return data_;
}

vector<row_t> PostingList::ToGlobalRowIds(idx_t row_start) const {
	vector<row_t> result;
	result.reserve(data_.size());
	for (auto offset : data_) {
		result.push_back(UnsafeNumericCast<row_t>(row_start + offset));
	}
	return result;
}

PostingList PostingList::Intersect(const PostingList &a, const PostingList &b) {
	PostingList result;
	idx_t i = 0, j = 0;
	const auto &ad = a.data_;
	const auto &bd = b.data_;
	while (i < ad.size() && j < bd.size()) {
		if (ad[i] == bd[j]) {
			result.data_.push_back(ad[i]);
			i++;
			j++;
		} else if (ad[i] < bd[j]) {
			i++;
		} else {
			j++;
		}
	}
	return result;
}

PostingList PostingList::Union(const PostingList &a, const PostingList &b) {
	PostingList result;
	idx_t i = 0, j = 0;
	const auto &ad = a.data_;
	const auto &bd = b.data_;
	while (i < ad.size() && j < bd.size()) {
		if (ad[i] == bd[j]) {
			result.data_.push_back(ad[i]);
			i++;
			j++;
		} else if (ad[i] < bd[j]) {
			result.data_.push_back(ad[i]);
			i++;
		} else {
			result.data_.push_back(bd[j]);
			j++;
		}
	}
	while (i < ad.size()) {
		result.data_.push_back(ad[i++]);
	}
	while (j < bd.size()) {
		result.data_.push_back(bd[j++]);
	}
	return result;
}

idx_t PostingList::GetMemoryUsage() const {
	return sizeof(PostingList) + data_.capacity() * sizeof(uint32_t);
}

//===----------------------------------------------------------------------===//
// IndexPartition
//===----------------------------------------------------------------------===//

IndexPartition::IndexPartition(idx_t row_start, idx_t row_count) : row_start(row_start), row_count(row_count) {
}

void IndexPartition::AddTerm(const string &term, uint32_t local_offset) {
	auto it = dictionary_.find(term);
	idx_t list_idx;
	if (it == dictionary_.end()) {
		list_idx = posting_lists_.size();
		dictionary_[term] = list_idx;
		posting_lists_.emplace_back();
	} else {
		list_idx = it->second;
	}
	auto &pl = posting_lists_[list_idx];
	// During build, offsets may arrive out of order from different threads.
	// For local builds within a single thread, offsets come in order.
	// For merge, we handle it separately.
	if (!pl.GetData().empty() && local_offset <= pl.GetData().back()) {
		// Duplicate or out-of-order — skip duplicates, handle later for merge
		if (local_offset == pl.GetData().back()) {
			return; // duplicate
		}
		// Out of order not expected during single-thread build
		D_ASSERT(false);
		return;
	}
	pl.Add(local_offset);
}

const PostingList *IndexPartition::Search(const string &term) const {
	auto it = dictionary_.find(term);
	if (it == dictionary_.end()) {
		return nullptr;
	}
	return &posting_lists_[it->second];
}

vector<const PostingList *> IndexPartition::PrefixSearch(const string &prefix) const {
	vector<const PostingList *> results;
	auto it = dictionary_.lower_bound(prefix);
	while (it != dictionary_.end()) {
		if (it->first.compare(0, prefix.size(), prefix) != 0) {
			break;
		}
		results.push_back(&posting_lists_[it->second]);
		++it;
	}
	return results;
}

idx_t IndexPartition::GetDocFrequency(const string &term) const {
	auto pl = Search(term);
	return pl ? pl->GetCount() : 0;
}

idx_t IndexPartition::GetTotalRows() const {
	return row_count;
}

bool IndexPartition::OverlapsTimestampRange(timestamp_t query_min, timestamp_t query_max) const {
	if (!has_timestamp_bounds) {
		return true; // No bounds — can't prune, assume overlap
	}
	return ts_min <= query_max && ts_max >= query_min;
}

idx_t IndexPartition::GetMemoryUsage() const {
	idx_t size = sizeof(IndexPartition);
	for (auto &pair : dictionary_) {
		size += pair.first.size() + sizeof(idx_t) + 64; // map node overhead estimate
	}
	for (auto &pl : posting_lists_) {
		size += pl.GetMemoryUsage();
	}
	return size;
}

void IndexPartition::Merge(const IndexPartition &other) {
	D_ASSERT(row_start == other.row_start && row_count == other.row_count);
	for (auto &pair : other.dictionary_) {
		const auto &term = pair.first;
		const auto &other_pl = other.posting_lists_[pair.second];
		auto it = dictionary_.find(term);
		if (it == dictionary_.end()) {
			// New term — copy posting list
			idx_t list_idx = posting_lists_.size();
			dictionary_[term] = list_idx;
			posting_lists_.push_back(other_pl);
		} else {
			// Existing term — merge posting lists (union)
			auto &my_pl = posting_lists_[it->second];
			my_pl = PostingList::Union(my_pl, other_pl);
		}
	}
}

//===----------------------------------------------------------------------===//
// LogsearchIndex
//===----------------------------------------------------------------------===//

LogsearchIndex::LogsearchIndex(const string &name, IndexConstraintType constraint_type,
                               const vector<column_t> &column_ids, TableIOManager &table_io_manager,
                               const vector<unique_ptr<Expression>> &unbound_expressions, AttachedDatabase &db)
    : BoundIndex(name, TYPE_NAME, constraint_type, column_ids, table_io_manager, unbound_expressions, db) {
}

unique_ptr<BoundIndex> LogsearchIndex::Create(CreateIndexInput &input) {
	return make_uniq<LogsearchIndex>(input.name, input.constraint_type, input.column_ids, input.table_io_manager,
	                                 input.unbound_expressions, input.db);
}

//===----------------------------------------------------------------------===//
// Search operations
//===----------------------------------------------------------------------===//

vector<row_t> LogsearchIndex::SearchTerm(const string &term, timestamp_t ts_min, timestamp_t ts_max,
                                         bool has_ts_bounds) const {
	vector<row_t> result;
	for (auto &partition : partitions) {
		if (has_ts_bounds && !partition->OverlapsTimestampRange(ts_min, ts_max)) {
			continue; // Skip this partition — timestamp range doesn't overlap
		}
		auto pl = partition->Search(term);
		if (pl) {
			auto global_ids = pl->ToGlobalRowIds(partition->row_start);
			result.insert(result.end(), global_ids.begin(), global_ids.end());
		}
	}
	// Result is sorted because partitions are in row_start order and posting lists are sorted
	D_ASSERT(std::is_sorted(result.begin(), result.end()));
	return result;
}

vector<row_t> LogsearchIndex::SearchTerms(const vector<string> &terms, timestamp_t ts_min, timestamp_t ts_max,
                                           bool has_ts_bounds) const {
	if (terms.empty()) {
		return {};
	}
	// Search first term
	auto result = SearchTerm(terms[0], ts_min, ts_max, has_ts_bounds);
	// Intersect with each subsequent term
	for (idx_t i = 1; i < terms.size(); i++) {
		auto other = SearchTerm(terms[i], ts_min, ts_max, has_ts_bounds);
		// Intersect sorted arrays
		vector<row_t> intersected;
		idx_t a = 0, b = 0;
		while (a < result.size() && b < other.size()) {
			if (result[a] == other[b]) {
				intersected.push_back(result[a]);
				a++;
				b++;
			} else if (result[a] < other[b]) {
				a++;
			} else {
				b++;
			}
		}
		result = std::move(intersected);
	}
	return result;
}

vector<row_t> LogsearchIndex::SearchPrefix(const string &prefix, timestamp_t ts_min, timestamp_t ts_max,
                                           bool has_ts_bounds) const {
	vector<row_t> result;
	for (auto &partition : partitions) {
		if (has_ts_bounds && !partition->OverlapsTimestampRange(ts_min, ts_max)) {
			continue;
		}
		auto posting_lists = partition->PrefixSearch(prefix);
		// Union all posting lists for matching terms, then convert to global row IDs
		PostingList merged;
		for (auto *pl : posting_lists) {
			merged = PostingList::Union(merged, *pl);
		}
		auto global_ids = merged.ToGlobalRowIds(partition->row_start);
		result.insert(result.end(), global_ids.begin(), global_ids.end());
	}
	D_ASSERT(std::is_sorted(result.begin(), result.end()));
	return result;
}

double LogsearchIndex::GetTermSelectivity(const string &term) const {
	idx_t total_df = 0;
	idx_t total_rows = 0;
	for (auto &partition : partitions) {
		total_df += partition->GetDocFrequency(term);
		total_rows += partition->GetTotalRows();
	}
	if (total_rows == 0) {
		return 0.0;
	}
	return static_cast<double>(total_df) / static_cast<double>(total_rows);
}

idx_t LogsearchIndex::GetTotalRows() const {
	idx_t total = 0;
	for (auto &partition : partitions) {
		total += partition->GetTotalRows();
	}
	return total;
}

//===----------------------------------------------------------------------===//
// BoundIndex interface
//===----------------------------------------------------------------------===//

IndexPartition *LogsearchIndex::FindPartition(row_t row_id) {
	// Binary search in partitions by row_start
	idx_t lo = 0, hi = partitions.size();
	while (lo < hi) {
		idx_t mid = (lo + hi) / 2;
		auto &p = partitions[mid];
		if (row_id < UnsafeNumericCast<row_t>(p->row_start)) {
			hi = mid;
		} else if (row_id >= UnsafeNumericCast<row_t>(p->row_start + p->row_count)) {
			lo = mid + 1;
		} else {
			return p.get();
		}
	}
	return nullptr;
}

void LogsearchIndex::AddRow(row_t row_id, const string_t &value) {
	auto *partition = FindPartition(row_id);
	if (!partition) {
		// Row doesn't belong to any existing partition — create a new one
		// This can happen when new rows are appended
		auto new_partition = make_uniq<IndexPartition>(UnsafeNumericCast<idx_t>(row_id), 1);
		partition = new_partition.get();
		partitions.push_back(std::move(new_partition));
		// TODO: this doesn't maintain sorted order of partitions. Fine for append-only.
	}

	auto local_offset = UnsafeNumericCast<uint32_t>(UnsafeNumericCast<idx_t>(row_id) - partition->row_start);
	auto tokens = LogsearchTokenizer::Tokenize(value);
	for (auto &token : tokens) {
		partition->AddTerm(token, local_offset);
	}
}

ErrorData LogsearchIndex::Append(IndexLock &, DataChunk &chunk, Vector &row_ids) {
	// Execute expressions to get the indexed column values
	DataChunk expression_result;
	expression_result.Initialize(Allocator::DefaultAllocator(), logical_types);
	ExecuteExpressions(chunk, expression_result);

	auto &text_vector = expression_result.data[0];
	auto &row_id_vector = row_ids;

	UnifiedVectorFormat text_data;
	text_vector.ToUnifiedFormat(expression_result.size(), text_data);

	auto row_id_data = FlatVector::GetData<row_t>(row_id_vector);

	for (idx_t i = 0; i < expression_result.size(); i++) {
		auto text_idx = text_data.sel->get_index(i);
		if (!text_data.validity.RowIsValid(text_idx)) {
			continue; // Skip NULL values
		}
		auto text_value = UnifiedVectorFormat::GetData<string_t>(text_data)[text_idx];
		AddRow(row_id_data[i], text_value);
	}
	return ErrorData();
}

void LogsearchIndex::CommitDrop(IndexLock &) {
	partitions.clear();
}

ErrorData LogsearchIndex::Insert(IndexLock &l, DataChunk &chunk, Vector &row_ids) {
	return Append(l, chunk, row_ids);
}

bool LogsearchIndex::MergeIndexes(IndexLock &, BoundIndex &other_index) {
	auto &other = other_index.Cast<LogsearchIndex>();
	for (auto &other_partition : other.partitions) {
		// Find matching partition by row_start
		bool found = false;
		for (auto &my_partition : partitions) {
			if (my_partition->row_start == other_partition->row_start) {
				my_partition->Merge(*other_partition);
				found = true;
				break;
			}
		}
		if (!found) {
			partitions.push_back(std::move(other_partition));
		}
	}
	// Sort partitions by row_start
	std::sort(partitions.begin(), partitions.end(),
	          [](const unique_ptr<IndexPartition> &a, const unique_ptr<IndexPartition> &b) {
		          return a->row_start < b->row_start;
	          });
	return true;
}

void LogsearchIndex::Vacuum(IndexLock &) {
	// No-op for now
}

idx_t LogsearchIndex::GetInMemorySize(IndexLock &) {
	idx_t size = sizeof(LogsearchIndex);
	for (auto &partition : partitions) {
		size += partition->GetMemoryUsage();
	}
	return size;
}

void LogsearchIndex::Verify(IndexLock &) {
	// Verify sorted invariant on all posting lists
	(void)partitions; // verification is done in Add() via D_ASSERT
}

string LogsearchIndex::ToString(IndexLock &, bool) {
	string result = "LogsearchIndex: " + name + "\n";
	result += "  Partitions: " + to_string(partitions.size()) + "\n";
	idx_t total_memory = 0;
	for (auto &partition : partitions) {
		total_memory += partition->GetMemoryUsage();
	}
	result += "  Total memory: " + to_string(total_memory) + " bytes\n";
	result += "  Total rows: " + to_string(GetTotalRows()) + "\n";
	return result;
}

void LogsearchIndex::VerifyAllocations(IndexLock &) {
	// No-op — no custom allocator yet
}

idx_t LogsearchIndex::TryDelete(IndexLock &, DataChunk &, Vector &, optional_ptr<SelectionVector>,
                                optional_ptr<SelectionVector>) {
	// Deletes from the inverted index are a no-op for now.
	// The index may return stale row IDs, but the original filter is kept as a post-filter
	// for correctness, so stale results are filtered out by DuckDB.
	// TODO: implement proper delete tracking (e.g., a deleted-rows bitset per partition)
	return 0;
}

void LogsearchIndex::VerifyBuffers(IndexLock &) {
	// No custom buffers to verify
}

string LogsearchIndex::GetConstraintViolationMessage(VerifyExistenceType, idx_t, DataChunk &) {
	return "Logsearch index does not enforce constraints";
}

} // namespace duckdb
