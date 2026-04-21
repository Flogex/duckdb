#include "logsearch_tokenizer.hpp"

#include <algorithm>
#include <cctype>

namespace duckdb {

vector<string> LogsearchTokenizer::Tokenize(const string_t &input) {
	return Tokenize(input.GetString());
}

vector<string> LogsearchTokenizer::Tokenize(const string &input) {
	vector<string> tokens;
	string current_token;
	current_token.reserve(32);

	for (auto c : input) {
		if (std::isalnum(static_cast<unsigned char>(c))) {
			current_token += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		} else {
			if (current_token.size() > 1) {
				tokens.push_back(std::move(current_token));
			}
			current_token.clear();
		}
	}
	// Last token
	if (current_token.size() > 1) {
		tokens.push_back(std::move(current_token));
	}
	return tokens;
}

} // namespace duckdb
