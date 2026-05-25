#include "bloom_index_optimizer.hpp"

#include "bloom_index.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/table_index_list.hpp"

#include <algorithm>
#include <cstdio>
#include <unordered_set>

namespace duckdb {

namespace {

//! Walk down through any LogicalProjection layers to find the LogicalGet beneath a LogicalFilter.
optional_ptr<LogicalGet> FindChildGet(LogicalOperator &op) {
	LogicalOperator *cur = &op;
	while (cur) {
		if (cur->type == LogicalOperatorType::LOGICAL_GET) {
			return &cur->Cast<LogicalGet>();
		}
		if (cur->type == LogicalOperatorType::LOGICAL_PROJECTION && cur->children.size() == 1) {
			cur = cur->children[0].get();
			continue;
		}
		return nullptr;
	}
	return nullptr;
}

//! Resolve a column reference on the given LogicalGet to its physical (storage) column id.
optional_idx ResolvePhysicalColumnId(const Expression &expr, const LogicalGet &get) {
	if (expr.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		return optional_idx();
	}
	auto &col_ref = expr.Cast<BoundColumnRefExpression>();
	if (col_ref.binding.table_index != get.table_index) {
		return optional_idx();
	}
	auto binding_col_idx = col_ref.binding.column_index;
	auto &column_ids = get.GetColumnIds();
	if (binding_col_idx >= column_ids.size()) {
		return optional_idx();
	}
	return column_ids[binding_col_idx].GetPrimaryIndex();
}

//! Try to evaluate a foldable expression to a constant Value. Handles BoundConstantExpression directly and any
//! other foldable shape (e.g. BoundCastExpression(BoundConstantExpression)) via TryEvaluateScalar.
bool TryGetConstantValue(const Expression &expr, ClientContext &context, Value &out) {
	if (expr.GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
		out = expr.Cast<BoundConstantExpression>().value;
		return true;
	}
	if (!expr.IsFoldable()) {
		return false;
	}
	return ExpressionExecutor::TryEvaluateScalar(context, expr, out);
}

struct EligibleFilter {
	column_t physical_column_id;
	//! Candidate hash set — survivor must Check() positive on at least one hash.
	vector<uint64_t> candidate_hashes;
};

//! Try to extract one EligibleFilter from a top-level filter expression.
//! Returns false if the expression isn't pattern-eligible. AND-conjunctions are recursed by the caller.
bool TryExtractEligibleFilter(const Expression &expr, const LogicalGet &get, ClientContext &context,
                              EligibleFilter &out) {
	// COMPARE_IN: BoundOperatorExpression with first child column-ref, rest constants/foldable.
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR &&
	    expr.GetExpressionType() == ExpressionType::COMPARE_IN) {
		auto &op = expr.Cast<BoundOperatorExpression>();
		if (op.children.empty()) {
			return false;
		}
		auto col_id = ResolvePhysicalColumnId(*op.children[0], get);
		if (!col_id.IsValid()) {
			return false;
		}
		vector<uint64_t> hashes;
		for (idx_t i = 1; i < op.children.size(); i++) {
			Value v;
			if (!TryGetConstantValue(*op.children[i], context, v)) {
				return false;
			}
			if (v.IsNull()) {
				continue;
			}
			auto h = BloomIndexHashValue(v);
			if (h == 0) {
				return false; // unsupported type -> cannot prune
			}
			hashes.push_back(h);
		}
		if (hashes.empty()) {
			return false;
		}
		out.physical_column_id = col_id.GetIndex();
		out.candidate_hashes = std::move(hashes);
		return true;
	}

	// COMPARE_EQUAL / COMPARE_NOT_DISTINCT_FROM: BoundFunctionExpression.
	if (BoundComparisonExpression::IsComparison(expr)) {
		auto type = expr.GetExpressionType();
		if (type != ExpressionType::COMPARE_EQUAL && type != ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
			return false;
		}
		auto &comp = expr.Cast<BoundFunctionExpression>();
		auto &left = BoundComparisonExpression::Left(comp);
		auto &right = BoundComparisonExpression::Right(comp);

		Value constant_value;
		optional_idx col_id;
		if (TryGetConstantValue(left, context, constant_value)) {
			col_id = ResolvePhysicalColumnId(right, get);
		} else if (TryGetConstantValue(right, context, constant_value)) {
			col_id = ResolvePhysicalColumnId(left, get);
		}
		if (!col_id.IsValid()) {
			return false;
		}
		if (constant_value.IsNull()) {
			// IS NOT DISTINCT FROM NULL: cannot probe (bloom doesn't track NULLs).
			return false;
		}
		auto h = BloomIndexHashValue(constant_value);
		if (h == 0) {
			return false;
		}
		out.physical_column_id = col_id.GetIndex();
		out.candidate_hashes = {h};
		return true;
	}
	return false;
}

//! Recursively flatten top-level AND-conjunctions into individual eligible filters.
void CollectEligibleFilters(const Expression &expr, const LogicalGet &get, ClientContext &context,
                            vector<EligibleFilter> &out) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION &&
	    expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		for (auto &child : conj.children) {
			CollectEligibleFilters(*child, get, context, out);
		}
		return;
	}
	EligibleFilter f;
	if (TryExtractEligibleFilter(expr, get, context, f)) {
		out.push_back(std::move(f));
	}
}

//! Returns nullptr if no bloom index applies. Restricted to native DuckDB tables only.
//! As a side effect, returns the DuckTableEntry via out_duck_table for the caller.
optional_ptr<DuckTableEntry> ResolveDuckTable(LogicalGet &get) {
	auto entry = get.GetTable();
	if (!entry) {
		return nullptr;
	}
	if (entry->type != CatalogType::TABLE_ENTRY) {
		return nullptr;
	}
	if (!entry->IsDuckTable()) {
		return nullptr;
	}
	return &entry->Cast<DuckTableEntry>();
}

//! For a set of physical column ids referenced by eligible filters, check whether any ART index covers them.
//! Rule A: if true, bail (do not block ART).
bool ArtCoversAnyOfFilterColumns(TableIndexList &index_list,
                                 const std::unordered_set<column_t> &filter_column_ids) {
	for (auto &index : index_list.Indexes()) {
		if (!index.IsBound()) {
			continue;
		}
		if (index.GetIndexType() != ART::TYPE_NAME) {
			continue;
		}
		for (auto col : index.GetColumnIds()) {
			if (filter_column_ids.count(col) > 0) {
				return true;
			}
		}
	}
	return false;
}

//! Find the BloomIndex covering a given physical column.
optional_ptr<BloomIndex> FindBloomIndexForColumn(TableIndexList &index_list, column_t physical_col_id) {
	for (auto &index : index_list.Indexes()) {
		if (!index.IsBound()) {
			continue;
		}
		if (index.GetIndexType() != BloomIndex::TYPE_NAME) {
			continue;
		}
		auto &bound = index.Cast<BoundIndex>();
		for (auto col : bound.GetColumnIds()) {
			if (col == physical_col_id) {
				return &bound.Cast<BloomIndex>();
			}
		}
	}
	return nullptr;
}

#define BLOOM_TRACE(...)                                                                                               \
	do {                                                                                                               \
		std::fprintf(stderr, "[bloom_index] " __VA_ARGS__);                                                            \
		std::fprintf(stderr, "\n");                                                                                    \
	} while (0)

void TryOptimizeGet(ClientContext &context, LogicalFilter &filter, LogicalGet &get) {
	BLOOM_TRACE("TryOptimizeGet entered, get.function=%s", get.function.name.c_str());

	// Step 2: restrict to DuckTableEntry.
	auto duck_table = ResolveDuckTable(get);
	if (!duck_table) {
		BLOOM_TRACE("  bail: not a DuckTable (entry=%p)", (void *)get.GetTable().get());
		return;
	}
	BLOOM_TRACE("  table = %s", duck_table->name.c_str());

	if (!get.scan_partition_indices.empty()) {
		BLOOM_TRACE("  bail: scan_partition_indices already set (size=%zu)", get.scan_partition_indices.size());
		return;
	}

	auto &storage = duck_table->GetStorage();
	auto &info = storage.GetDataTableInfo();
	auto &index_list = info->GetIndexes();
	BLOOM_TRACE("  total indexes on table = %zu", index_list.Count());
	if (index_list.Empty()) {
		BLOOM_TRACE("  bail: no indexes");
		return;
	}
	info->BindIndexes(context, BloomIndex::TYPE_NAME);
	for (auto &idx : index_list.Indexes()) {
		BLOOM_TRACE("  - index name=%s type=%s bound=%d cols=%zu", idx.GetIndexName().c_str(),
		            idx.GetIndexType().c_str(), (int)idx.IsBound(), idx.GetColumnIds().size());
		for (auto col : idx.GetColumnIds()) {
			BLOOM_TRACE("      column physical_id=%llu", (unsigned long long)col);
		}
	}

	// Step 4: collect eligible filters from the LogicalFilter (top-level AND already split into separate expressions).
	vector<EligibleFilter> eligible;
	for (auto &expr : filter.expressions) {
		CollectEligibleFilters(*expr, get, context, eligible);
	}
	BLOOM_TRACE("  eligible filters from LogicalFilter: %zu", eligible.size());
	for (auto &e : eligible) {
		BLOOM_TRACE("    eligible col physical_id=%llu, %zu hashes", (unsigned long long)e.physical_column_id,
		            e.candidate_hashes.size());
	}

	// Also try TableFilters already pushed into the LogicalGet (this is where filter pushdown ends up for some
	// predicates depending on plan shape).
	BLOOM_TRACE("  LogicalGet table_filters.HasFilters=%d", (int)get.table_filters.HasFilters());

	if (eligible.empty()) {
		BLOOM_TRACE("  bail: no eligible filters");
		return;
	}

	std::unordered_set<column_t> filter_columns;
	for (auto &e : eligible) {
		filter_columns.insert(e.physical_column_id);
	}

	if (ArtCoversAnyOfFilterColumns(index_list, filter_columns)) {
		BLOOM_TRACE("  bail: ART covers a filter column");
		return;
	}

	struct ResolvedFilter {
		BloomIndex *bloom;
		vector<uint64_t> hashes;
	};
	vector<ResolvedFilter> resolved;
	for (auto &e : eligible) {
		auto bloom = FindBloomIndexForColumn(index_list, e.physical_column_id);
		if (!bloom) {
			BLOOM_TRACE("  no bloom index found for column physical_id=%llu",
			            (unsigned long long)e.physical_column_id);
			continue;
		}
		resolved.push_back({bloom.get(), std::move(e.candidate_hashes)});
	}
	BLOOM_TRACE("  resolved filters = %zu", resolved.size());
	if (resolved.empty()) {
		BLOOM_TRACE("  bail: no resolved filters");
		return;
	}

	auto partition_stats = storage.GetPartitionStats(context);
	BLOOM_TRACE("  partition_stats.size = %zu", partition_stats.size());
	if (partition_stats.empty()) {
		BLOOM_TRACE("  bail: empty partition stats");
		return;
	}

	// Trace: first 5 partition row_starts vs first 5 bloom row_starts.
	for (idx_t i = 0; i < 5 && i < partition_stats.size(); i++) {
		auto &p = partition_stats[i];
		BLOOM_TRACE("    partition[%zu] row_start=%llu count=%llu valid=%d", i,
		            (unsigned long long)(p.row_start.IsValid() ? p.row_start.GetIndex() : 0),
		            (unsigned long long)p.count, (int)p.row_start.IsValid());
	}
	for (auto &rf : resolved) {
		auto snap = rf.bloom->SnapshotEntries();
		BLOOM_TRACE("    bloom has %zu buckets", snap.size());
		for (idx_t i = 0; i < 5 && i < snap.size(); i++) {
			BLOOM_TRACE("      bucket[%zu] row_start=%llu row_count=%llu", i,
			            (unsigned long long)snap[i].row_start, (unsigned long long)snap[i].row_count);
		}
	}

	vector<idx_t> survivors;
	survivors.reserve(partition_stats.size());
	idx_t no_bloom = 0;
	for (idx_t i = 0; i < partition_stats.size(); i++) {
		auto &p = partition_stats[i];
		bool kept = true;
		for (auto &rf : resolved) {
			RowGroupBloomFilter *bf = nullptr;
			if (p.row_start.IsValid()) {
				bf = rf.bloom->FindByRowStart(p.row_start.GetIndex());
			}
			if (!bf) {
				no_bloom++;
				continue;
			}
			bool any_hit = false;
			for (auto h : rf.hashes) {
				if (bf->Check(h)) {
					any_hit = true;
					break;
				}
			}
			if (!any_hit) {
				kept = false;
				break;
			}
		}
		if (kept) {
			survivors.push_back(i);
		}
	}
	BLOOM_TRACE("  survivors = %zu / %zu (no_bloom per-partition-filter-pairs=%llu)", survivors.size(),
	            partition_stats.size(), (unsigned long long)no_bloom);

	if (survivors.size() == partition_stats.size()) {
		BLOOM_TRACE("  bail: all partitions survive (no pruning)");
		return;
	}

	BLOOM_TRACE("  setting partitions_to_scan to %zu survivors", survivors.size());
	get.SetPartitionsToScan(std::move(survivors));
}

void Visit(ClientContext &context, LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_FILTER) {
		auto &filter = op.Cast<LogicalFilter>();
		BLOOM_TRACE("Visit LogicalFilter expressions=%zu children=%zu", filter.expressions.size(),
		            filter.children.size());
		if (filter.children.size() == 1) {
			auto get = FindChildGet(*filter.children[0]);
			if (get) {
				TryOptimizeGet(context, filter, *get);
			} else {
				BLOOM_TRACE("  no LogicalGet child (skipped via FindChildGet)");
			}
		}
	} else if (op.type == LogicalOperatorType::LOGICAL_GET) {
		auto &get = op.Cast<LogicalGet>();
		BLOOM_TRACE("Visit bare LogicalGet table_filters.HasFilters=%d (no parent LogicalFilter — bloom currently "
		            "matches only LogicalFilter→LogicalGet)",
		            (int)get.table_filters.HasFilters());
	}
	for (auto &child : op.children) {
		Visit(context, *child);
	}
}

} // namespace

void BloomIndexPreOptimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	if (!plan) {
		return;
	}
	Visit(input.context, *plan);
}

} // namespace duckdb
