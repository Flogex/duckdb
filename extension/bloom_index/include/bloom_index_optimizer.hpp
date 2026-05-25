#pragma once

#include "duckdb/optimizer/optimizer_extension.hpp"

namespace duckdb {

//! Pre-optimize hook: scans logical plan for LogicalFilter -> LogicalGet on a DuckTableEntry with a BloomIndex,
//! probes the per-row-group blooms with eligible equality predicates, and calls LogicalGet::SetPartitionsToScan
//! with the surviving row group indexes.
void BloomIndexPreOptimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);

} // namespace duckdb
