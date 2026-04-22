#include "logsearch_optimizer.hpp"
#include "logsearch_index.hpp"
#include "logsearch_index_scan.hpp"
#include "logsearch_tokenizer.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/table_index_list.hpp"

#include <cctype>

namespace duckdb {

//===----------------------------------------------------------------------===//
// Helper: Find a logsearch index for a given physical column in a table
//===----------------------------------------------------------------------===//

static LogsearchIndex *FindLogsearchIndex(LogicalGet &get, column_t physical_column_id) {
	auto table_entry = get.GetTable();
	if (!table_entry) {
		return nullptr;
	}
	if (table_entry->type != CatalogType::TABLE_ENTRY) {
		return nullptr;
	}
	auto &duck_table = table_entry->Cast<DuckTableEntry>();
	auto &storage = duck_table.GetStorage();
	auto &index_list = storage.GetDataTableInfo()->GetIndexes();

	LogsearchIndex *result = nullptr;
	for (auto &index : index_list.Indexes()) {
		if (!index.IsBound()) {
			continue;
		}
		auto &bound = index.Cast<BoundIndex>();
		if (bound.GetIndexType() != LogsearchIndex::TYPE_NAME) {
			continue;
		}
		auto &col_ids = index.GetColumnIds();
		for (auto col_id : col_ids) {
			if (col_id == physical_column_id) {
				result = &bound.Cast<LogsearchIndex>();
				break;
			}
		}
		if (result) {
			break;
		}
	}
	return result;
}

//===----------------------------------------------------------------------===//
// Helper: Get the physical column id for a column ref in a LogicalGet
//===----------------------------------------------------------------------===//

static optional_idx GetPhysicalColumnId(Expression &expr, LogicalGet &get) {
	if (expr.type != ExpressionType::BOUND_COLUMN_REF) {
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

//===----------------------------------------------------------------------===//
// Helper: Extract constant string from an expression
//===----------------------------------------------------------------------===//

static bool TryGetConstantString(Expression &expr, string &out) {
	if (expr.type != ExpressionType::VALUE_CONSTANT) {
		return false;
	}
	auto &constant = expr.Cast<BoundConstantExpression>();
	if (constant.value.type().id() != LogicalTypeId::VARCHAR) {
		return false;
	}
	if (constant.value.IsNull()) {
		return false;
	}
	out = constant.value.ToString();
	return true;
}

//===----------------------------------------------------------------------===//
// Helper: Try to match a filter expression against index-friendly patterns
//===----------------------------------------------------------------------===//

struct IndexSearchRequest {
	vector<string> terms;
	string prefix;
	bool is_prefix = false;
	LogsearchIndex *index = nullptr;
};

static bool TryMatchExpression(Expression &expr, LogicalGet &get, IndexSearchRequest &request) {
	// Pattern 1: col = 'constant'
	if (expr.type == ExpressionType::COMPARE_EQUAL) {
		auto &comp = expr.Cast<BoundComparisonExpression>();
		string value;
		auto col_id = GetPhysicalColumnId(*comp.left, get);
		if (col_id.IsValid() && TryGetConstantString(*comp.right, value)) {
			auto *idx = FindLogsearchIndex(get, col_id.GetIndex());
			if (idx) {
				request.terms = LogsearchTokenizer::Tokenize(value);
				request.index = idx;
				return !request.terms.empty();
			}
		}
		col_id = GetPhysicalColumnId(*comp.right, get);
		if (col_id.IsValid() && TryGetConstantString(*comp.left, value)) {
			auto *idx = FindLogsearchIndex(get, col_id.GetIndex());
			if (idx) {
				request.terms = LogsearchTokenizer::Tokenize(value);
				request.index = idx;
				return !request.terms.empty();
			}
		}
		return false;
	}

	// Pattern 2: contains(col, 'constant') or prefix(col, 'constant')
	if (expr.type == ExpressionType::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		if (func.function.name == "contains" && func.children.size() == 2) {
			auto col_id = GetPhysicalColumnId(*func.children[0], get);
			string value;
			if (col_id.IsValid() && TryGetConstantString(*func.children[1], value)) {
				auto *idx = FindLogsearchIndex(get, col_id.GetIndex());
				if (idx) {
					request.terms = LogsearchTokenizer::Tokenize(value);
					request.index = idx;
					return !request.terms.empty();
				}
			}
		}
		if (func.function.name == "prefix" && func.children.size() == 2) {
			auto col_id = GetPhysicalColumnId(*func.children[0], get);
			string value;
			if (col_id.IsValid() && TryGetConstantString(*func.children[1], value)) {
				auto *idx = FindLogsearchIndex(get, col_id.GetIndex());
				if (idx) {
					string lower_prefix;
					for (auto c : value) {
						lower_prefix += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
					}
					request.prefix = lower_prefix;
					request.is_prefix = true;
					request.index = idx;
					return !request.prefix.empty();
				}
			}
		}
		return false;
	}

	return false;
}

struct TimestampBounds {
	timestamp_t ts_min = Timestamp::FromEpochMicroSeconds(0);
	timestamp_t ts_max = Timestamp::FromEpochMicroSeconds(NumericLimits<int64_t>::Maximum());
	bool has_bounds = false;
};

//===----------------------------------------------------------------------===//
// Main optimizer rewrite
//===----------------------------------------------------------------------===//

static constexpr double SELECTIVITY_THRESHOLD = 0.20;

static void RewritePlan(ClientContext &context, Optimizer &optimizer, unique_ptr<LogicalOperator> &op) {
	// Recurse into children first
	for (auto &child : op->children) {
		RewritePlan(context, optimizer, child);
	}

	if (op->type != LogicalOperatorType::LOGICAL_FILTER) {
		return;
	}
	auto &filter = op->Cast<LogicalFilter>();

	if (filter.children.empty() || filter.children[0]->type != LogicalOperatorType::LOGICAL_GET) {
		return;
	}
	auto &get = filter.children[0]->Cast<LogicalGet>();

	// Collect all leaf expressions by flattening AND conjunctions.
	// Pre-optimize plan may have e.g. `contains(msg, 'x') AND ts > y` as one CONJUNCTION_AND.
	vector<Expression *> leaf_exprs;
	for (auto &expr : filter.expressions) {
		if (expr->type == ExpressionType::CONJUNCTION_AND) {
			auto &conj = expr->Cast<BoundConjunctionExpression>();
			for (auto &child : conj.children) {
				leaf_exprs.push_back(child.get());
			}
		} else {
			leaf_exprs.push_back(expr.get());
		}
	}

	// Try each leaf expression for index-friendly patterns
	IndexSearchRequest request;
	bool found_match = false;
	for (auto *leaf : leaf_exprs) {
		if (TryMatchExpression(*leaf, get, request)) {
			found_match = true;
			break;
		}
	}
	if (!found_match) {
		return;
	}

	// Extract timestamp bounds from ALL leaf expressions
	TimestampBounds ts_bounds;
	if (!request.index->timestamp_column_name.empty()) {
		// Build a temporary vector of unique_ptrs for ExtractTimestampBounds
		// Actually, let's just inline the timestamp extraction on the leaf expressions
		for (auto *leaf : leaf_exprs) {
			if (leaf->type != ExpressionType::COMPARE_GREATERTHAN &&
			    leaf->type != ExpressionType::COMPARE_GREATERTHANOREQUALTO &&
			    leaf->type != ExpressionType::COMPARE_LESSTHAN &&
			    leaf->type != ExpressionType::COMPARE_LESSTHANOREQUALTO) {
				continue;
			}
			auto &comp = leaf->Cast<BoundComparisonExpression>();

			BoundColumnRefExpression *col_ref = nullptr;
			bool col_is_left = false;

			// Identify column ref side and constant side.
			// The constant side may be wrapped in a CAST (e.g., CAST('2024-01-01' AS TIMESTAMP)).
			auto get_col_ref = [](Expression &e) -> BoundColumnRefExpression * {
				if (e.type == ExpressionType::BOUND_COLUMN_REF) {
					return &e.Cast<BoundColumnRefExpression>();
				}
				return nullptr;
			};
			auto is_foldable = [](Expression &e) -> bool {
				return e.IsFoldable();
			};

			if (get_col_ref(*comp.left) && is_foldable(*comp.right)) {
				col_ref = get_col_ref(*comp.left);
				col_is_left = true;
			} else if (get_col_ref(*comp.right) && is_foldable(*comp.left)) {
				col_ref = get_col_ref(*comp.right);
				col_is_left = false;
			}
			if (!col_ref) {
				continue;
			}

			// Evaluate the foldable expression to get the constant value
			auto &const_side = col_is_left ? *comp.right : *comp.left;
			Value const_value;
			if (!ExpressionExecutor::TryEvaluateScalar(context, const_side, const_value)) {
				continue;
			}
			if (const_value.IsNull()) {
				continue;
			}
			if (col_ref->binding.table_index != get.table_index) {
				continue;
			}
			auto binding_col_idx = col_ref->binding.column_index;
			// Resolve through column_ids to get the actual table column index
			auto &col_ids_ref = get.GetColumnIds();
			if (binding_col_idx >= col_ids_ref.size()) {
				continue;
			}
			auto table_col_idx = col_ids_ref[binding_col_idx].GetPrimaryIndex();
			if (table_col_idx >= get.names.size() ||
			    get.names[table_col_idx] != request.index->timestamp_column_name) {
				continue;
			}

			timestamp_t ts_val;
			try {
				ts_val = const_value.DefaultCastAs(LogicalType::TIMESTAMP).GetValue<timestamp_t>();
			} catch (...) {
				continue;
			}

			auto cmp_type = comp.GetExpressionType();
			if (!col_is_left) {
				// Flip: val > col → col < val
				switch (cmp_type) {
				case ExpressionType::COMPARE_GREATERTHAN:
					cmp_type = ExpressionType::COMPARE_LESSTHAN; break;
				case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
					cmp_type = ExpressionType::COMPARE_LESSTHANOREQUALTO; break;
				case ExpressionType::COMPARE_LESSTHAN:
					cmp_type = ExpressionType::COMPARE_GREATERTHAN; break;
				case ExpressionType::COMPARE_LESSTHANOREQUALTO:
					cmp_type = ExpressionType::COMPARE_GREATERTHANOREQUALTO; break;
				default: continue;
				}
			}

			switch (cmp_type) {
			case ExpressionType::COMPARE_GREATERTHAN:
			case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
				if (ts_val > ts_bounds.ts_min) { ts_bounds.ts_min = ts_val; }
				ts_bounds.has_bounds = true;
				break;
			case ExpressionType::COMPARE_LESSTHAN:
			case ExpressionType::COMPARE_LESSTHANOREQUALTO:
				if (ts_val < ts_bounds.ts_max) { ts_bounds.ts_max = ts_val; }
				ts_bounds.has_bounds = true;
				break;
			default: break;
			}
		}
	}

	// Check selectivity — estimate result count without materializing row IDs
	idx_t estimated_count = 0;
	if (!request.is_prefix) {
		// For multi-term AND, estimate = min term frequency (upper bound on intersection)
		idx_t min_df = NumericLimits<idx_t>::Maximum();
		for (auto &term : request.terms) {
			double sel = request.index->GetTermSelectivity(term);
			if (sel > SELECTIVITY_THRESHOLD) {
				return; // Term too common, skip index
			}
			idx_t total_rows = request.index->GetTotalRows();
			idx_t df = static_cast<idx_t>(sel * static_cast<double>(total_rows));
			min_df = MinValue(min_df, df);
		}
		estimated_count = min_df;
	} else {
		// Rough estimate for prefix — use total rows * some fraction
		estimated_count = request.index->GetTotalRows() / 10;
	}

	// Store search parameters in bind data — index is queried at execution time, not now.
	// No pointers, no row IDs — bind data is serializable.
	auto table_entry = get.GetTable();
	D_ASSERT(table_entry);

	auto bind_data = make_uniq<LogsearchScanBindData>();
	bind_data->catalog_name = table_entry->ParentCatalog().GetName();
	bind_data->schema_name = table_entry->ParentSchema().name;
	bind_data->table_name = table_entry->name;
	bind_data->index_name = request.index->name;
	bind_data->search_terms = request.terms;
	bind_data->search_prefix = request.prefix;
	bind_data->is_prefix = request.is_prefix;
	bind_data->ts_min = ts_bounds.ts_min;
	bind_data->ts_max = ts_bounds.ts_max;
	bind_data->has_ts_bounds = ts_bounds.has_bounds;
	bind_data->estimated_count = estimated_count;
	bind_data->all_types = get.returned_types;
	bind_data->all_names = get.names;

	// Create new LogicalGet with our index scan function, preserving table_index so
	// all column bindings from the filter (and above) remain valid.
	auto index_scan_func = LogsearchIndexScan::GetFunction();
	auto new_get = make_uniq<LogicalGet>(get.table_index, index_scan_func, std::move(bind_data), get.returned_types,
	                                     get.names, get.virtual_columns);

	// Copy column projection info from original get
	new_get->SetColumnIds(vector<ColumnIndex>(get.GetColumnIds()));
	new_get->projection_ids = get.projection_ids;

	// Replace the old LogicalGet — original filter expressions stay for correctness
	filter.children[0] = std::move(new_get);
}

void LogsearchOptimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	RewritePlan(input.context, input.optimizer, plan);
}

} // namespace duckdb
