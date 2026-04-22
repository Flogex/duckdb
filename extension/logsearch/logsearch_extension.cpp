#include "logsearch_extension.hpp"
#include "logsearch_index.hpp"
#include "logsearch_optimizer.hpp"

#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/execution/index/index_type_set.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);

	// Register the logsearch index type
	config.GetIndexTypes().RegisterIndexType(LogsearchIndex::GetLogsearchIndexType());

	// Register the optimizer extension — use pre_optimize so we run BEFORE filter pushdown
	OptimizerExtension opt_ext;
	opt_ext.pre_optimize_function = LogsearchOptimize;
	OptimizerExtension::Register(config, opt_ext);
}

void LogsearchExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string LogsearchExtension::Name() {
	return "logsearch";
}

std::string LogsearchExtension::Version() const {
#ifdef EXT_VERSION_LOGSEARCH
	return EXT_VERSION_LOGSEARCH;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(logsearch, loader) {
	duckdb::LoadInternal(loader);
}
}
