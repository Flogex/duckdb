//===----------------------------------------------------------------------===//
//                         DuckDB
//
// logsearch_tokenizer.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

class LogsearchTokenizer {
public:
	//! Tokenize a string into lowercase tokens split on non-alphanumeric characters.
	//! Skips empty tokens and single-character tokens.
	static vector<string> Tokenize(const string_t &input);
	static vector<string> Tokenize(const string &input);

	// TODO: log-aware tokenization (IPs, UUIDs, dotted paths, k=v pairs)
	// TODO: reversed terms in dictionary for suffix search
};

} // namespace duckdb
