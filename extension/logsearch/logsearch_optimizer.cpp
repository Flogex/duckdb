#include "logsearch_optimizer.hpp"
#include "logsearch_index.hpp"
#include "logsearch_tokenizer.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/operator/logical_column_data_get.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
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

//===----------------------------------------------------------------------===//
// Helper: Extract timestamp bounds from filter expressions
//===----------------------------------------------------------------------===//

struct TimestampBounds {
	timestamp_t ts_min = Timestamp::FromEpochMicroSeconds(0);
	timestamp_t ts_max = Timestamp::FromEpochMicroSeconds(NumericLimits<int64_t>::Maximum());
	bool has_bounds = false;
};

static void ExtractTimestampBounds(vector<unique_ptr<Expression>> &expressions, LogicalGet &get,
                                   const string &ts_column_name, TimestampBounds &bounds) {
	if (ts_column_name.empty()) {
		return;
	}

	for (auto &expr : expressions) {
		if (expr->type != ExpressionType::COMPARE_GREATERTHAN &&
		    expr->type != ExpressionType::COMPARE_GREATERTHANOREQUALTO &&
		    expr->type != ExpressionType::COMPARE_LESSTHAN &&
		    expr->type != ExpressionType::COMPARE_LESSTHANOREQUALTO) {
			continue;
		}
		auto &comp = expr->Cast<BoundComparisonExpression>();

		BoundColumnRefExpression *col_ref = nullptr;
		BoundConstantExpression *const_expr = nullptr;
		bool col_is_left = false;

		if (comp.left->type == ExpressionType::BOUND_COLUMN_REF &&
		    comp.right->type == ExpressionType::VALUE_CONSTANT) {
			col_ref = &comp.left->Cast<BoundColumnRefExpression>();
			const_expr = &comp.right->Cast<BoundConstantExpression>();
			col_is_left = true;
		} else if (comp.right->type == ExpressionType::BOUND_COLUMN_REF &&
		           comp.left->type == ExpressionType::VALUE_CONSTANT) {
			col_ref = &comp.right->Cast<BoundColumnRefExpression>();
			const_expr = &comp.left->Cast<BoundConstantExpression>();
			col_is_left = false;
		}

		if (!col_ref || !const_expr) {
			continue;
		}
		if (const_expr->value.IsNull()) {
			continue;
		}

		// Check column name matches timestamp column
		if (col_ref->binding.table_index != get.table_index) {
			continue;
		}
		auto binding_col_idx = col_ref->binding.column_index;
		if (binding_col_idx >= get.names.size()) {
			continue;
		}
		if (get.names[binding_col_idx] != ts_column_name) {
			continue;
		}

		// Extract timestamp value
		timestamp_t ts_val;
		try {
			ts_val = const_expr->value.DefaultCastAs(LogicalType::TIMESTAMP).GetValue<timestamp_t>();
		} catch (...) {
			continue;
		}

		auto comparison_type = comp.GetExpressionType();
		if (!col_is_left) {
			switch (comparison_type) {
			case ExpressionType::COMPARE_GREATERTHAN:
				comparison_type = ExpressionType::COMPARE_LESSTHAN;
				break;
			case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
				comparison_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
				break;
			case ExpressionType::COMPARE_LESSTHAN:
				comparison_type = ExpressionType::COMPARE_GREATERTHAN;
				break;
			case ExpressionType::COMPARE_LESSTHANOREQUALTO:
				comparison_type = ExpressionType::COMPARE_GREATERTHANOREQUALTO;
				break;
			default:
				continue;
			}
		}

		switch (comparison_type) {
		case ExpressionType::COMPARE_GREATERTHAN:
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			if (ts_val > bounds.ts_min) {
				bounds.ts_min = ts_val;
			}
			bounds.has_bounds = true;
			break;
		case ExpressionType::COMPARE_LESSTHAN:
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			if (ts_val < bounds.ts_max) {
				bounds.ts_max = ts_val;
			}
			bounds.has_bounds = true;
			break;
		default:
			break;
		}
	}
}

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

	// Try each filter expression for index-friendly patterns
	for (idx_t expr_idx = 0; expr_idx < filter.expressions.size(); expr_idx++) {
		auto &expr = filter.expressions[expr_idx];

		IndexSearchRequest request;
		if (!TryMatchExpression(*expr, get, request)) {
			continue;
		}

		// Extract timestamp bounds for pruning
		TimestampBounds ts_bounds;
		if (!request.index->timestamp_column_name.empty()) {
			ExtractTimestampBounds(filter.expressions, get, request.index->timestamp_column_name, ts_bounds);
		}

		// Check selectivity
		if (!request.is_prefix) {
			bool too_common = false;
			for (auto &term : request.terms) {
				if (request.index->GetTermSelectivity(term) > SELECTIVITY_THRESHOLD) {
					too_common = true;
					break;
				}
			}
			if (too_common) {
				continue;
			}
		}

		// Query the inverted index
		vector<row_t> matching_row_ids;
		if (request.is_prefix) {
			matching_row_ids =
			    request.index->SearchPrefix(request.prefix, ts_bounds.ts_min, ts_bounds.ts_max, ts_bounds.has_bounds);
		} else {
			matching_row_ids =
			    request.index->SearchTerms(request.terms, ts_bounds.ts_min, ts_bounds.ts_max, ts_bounds.has_bounds);
		}

		if (matching_row_ids.empty()) {
			continue;
		}

		// Create ColumnDataCollection with matching row IDs
		vector<LogicalType> types = {LogicalType::ROW_TYPE};
		auto collection = make_uniq<ColumnDataCollection>(context, types);
		ColumnDataAppendState append_state;
		collection->InitializeAppend(append_state);

		DataChunk chunk;
		chunk.Initialize(context, types);
		for (idx_t i = 0; i < matching_row_ids.size(); i++) {
			idx_t chunk_idx = chunk.size();
			chunk.SetCardinality(chunk.size() + 1);
			chunk.SetValue(0, chunk_idx, Value::BIGINT(matching_row_ids[i]));
			if (chunk.size() == STANDARD_VECTOR_SIZE || i + 1 == matching_row_ids.size()) {
				collection->Append(append_state, chunk);
				chunk.Reset();
			}
		}

		// Create LogicalColumnDataGet to scan row IDs
		auto chunk_index = optimizer.binder.GenerateTableIndex();
		auto chunk_scan = make_uniq<LogicalColumnDataGet>(chunk_index, types, std::move(collection));

		// Create MARK join
		auto mark_index = optimizer.binder.GenerateTableIndex();
		auto join = make_uniq<LogicalComparisonJoin>(JoinType::MARK);
		join->mark_index = mark_index;
		join->AddChild(std::move(filter.children[0]));
		join->AddChild(std::move(chunk_scan));

		// Ensure rowid is in the LogicalGet's column list
		idx_t rowid_binding_idx = 0;
		bool has_rowid = false;
		auto &col_ids = get.GetColumnIds();
		for (idx_t i = 0; i < col_ids.size(); i++) {
			if (col_ids[i].IsRowIdColumn()) {
				rowid_binding_idx = i;
				has_rowid = true;
				break;
			}
		}
		if (!has_rowid) {
			rowid_binding_idx = col_ids.size();
			get.AddColumnId(COLUMN_IDENTIFIER_ROW_ID);
			if (!get.projection_ids.empty()) {
				get.projection_ids.push_back(rowid_binding_idx);
			}
		}

		// Join condition: rowid = rowid
		JoinCondition cond;
		cond.left =
		    make_uniq<BoundColumnRefExpression>(LogicalType::ROW_TYPE, ColumnBinding(get.table_index, rowid_binding_idx));
		cond.right = make_uniq<BoundColumnRefExpression>(LogicalType::ROW_TYPE, ColumnBinding(chunk_index, 0));
		cond.comparison = ExpressionType::COMPARE_EQUAL;
		join->conditions.push_back(std::move(cond));

		filter.children[0] = std::move(join);

		// Project out the mark column
		if (filter.projection_map.empty()) {
			auto child_bindings = filter.children[0]->GetColumnBindings();
			for (idx_t i = 0; i < child_bindings.size(); i++) {
				if (child_bindings[i].table_index != mark_index) {
					filter.projection_map.push_back(i);
				}
			}
		}

		// Replace matched expression with: mark_column AND original_filter
		auto mark_ref =
		    make_uniq<BoundColumnRefExpression>("logsearch_match", LogicalType::BOOLEAN, ColumnBinding(mark_index, 0));
		auto conjunction = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
		conjunction->children.push_back(std::move(mark_ref));
		conjunction->children.push_back(std::move(expr));
		filter.expressions[expr_idx] = std::move(conjunction);

		// Only rewrite one expression per filter for now
		break;
	}
}

void LogsearchOptimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	RewritePlan(input.context, input.optimizer, plan);
}

} // namespace duckdb
