#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

namespace duckdb {

// Split-block bloom filter (Parquet-compatible layout, ported from extension/parquet/parquet_statistics).
// 8 x uint32 = 32-byte block. Block index selected by upper 32 bits of hash; in-block bit positions selected by salt
// table.
struct BloomFilterBlock {
	uint32_t block[8] = {0};

	static void Insert(BloomFilterBlock &b, uint32_t x);
	static bool Check(const BloomFilterBlock &b, uint32_t x);
};

class RowGroupBloomFilter {
public:
	//! Sized for a target false-positive rate at the given expected element count.
	RowGroupBloomFilter(idx_t expected_n, double fpp);
	//! Construct from an already-sized buffer (used during deserialization).
	RowGroupBloomFilter(idx_t block_count, vector<uint8_t> data);

	void Insert(uint64_t hash);
	bool Check(uint64_t hash) const;

	void MergeFrom(const RowGroupBloomFilter &other);

	idx_t MemoryUsage() const {
		return data.size();
	}
	idx_t BlockCount() const {
		return block_count;
	}

	void Serialize(Serializer &serializer) const;
	static unique_ptr<RowGroupBloomFilter> Deserialize(Deserializer &deserializer);

private:
	idx_t block_count;
	vector<uint8_t> data; // block_count * sizeof(BloomFilterBlock) bytes
};

} // namespace duckdb
