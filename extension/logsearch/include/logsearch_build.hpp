//===----------------------------------------------------------------------===//
//                         DuckDB
//
// logsearch_build.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "logsearch_index.hpp"
#include "duckdb/execution/index/index_type.hpp"

namespace duckdb {

//! Build callback implementations for the logsearch IndexType.
//! These follow the same pattern as ART's build callbacks in art_index.cpp.

unique_ptr<IndexBuildBindData> LogsearchBuildBind(IndexBuildBindInput &input);
bool LogsearchBuildSort(IndexBuildSortInput &input);
unique_ptr<IndexBuildGlobalState> LogsearchBuildGlobalInit(IndexBuildInitGlobalStateInput &input);
unique_ptr<IndexBuildLocalState> LogsearchBuildLocalInit(IndexBuildInitLocalStateInput &input);
void LogsearchBuildSink(IndexBuildSinkInput &input, DataChunk &key_chunk, DataChunk &row_chunk);
void LogsearchBuildCombine(IndexBuildCombineInput &input);
unique_ptr<BoundIndex> LogsearchBuildFinalize(IndexBuildFinalizeInput &input);

} // namespace duckdb
