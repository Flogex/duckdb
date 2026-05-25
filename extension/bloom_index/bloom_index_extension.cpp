#include "bloom_index_extension.hpp"
#include "bloom_index.hpp"
#include "bloom_index_optimizer.hpp"

#include "duckdb/execution/index/index_type_set.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);

	config.GetIndexTypes().RegisterIndexType(BloomIndex::GetBloomIndexType());

	OptimizerExtension opt_ext;
	opt_ext.pre_optimize_function = BloomIndexPreOptimize;
	OptimizerExtension::Register(config, opt_ext);
}

void BloomIndexExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string BloomIndexExtension::Name() {
	return "bloom_index";
}

std::string BloomIndexExtension::Version() const {
#ifdef EXT_VERSION_BLOOM_INDEX
	return EXT_VERSION_BLOOM_INDEX;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(bloom_index, loader) {
	duckdb::LoadInternal(loader);
}
}
