//===----------------------------------------------------------------------===//
//                         DuckDB
//
// logsearch_optimizer.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/optimizer_extension.hpp"

namespace duckdb {

//! Optimizer function that rewrites filter expressions on indexed columns
//! to use the logsearch inverted index via MARK joins.
void LogsearchOptimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);

} // namespace duckdb
