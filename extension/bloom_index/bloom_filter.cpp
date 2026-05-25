#include "bloom_filter.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/numeric_utils.hpp"

#include <cmath>

namespace duckdb {

namespace {

constexpr idx_t BLOCK_BYTES = sizeof(BloomFilterBlock);

inline bool CheckBit(uint32_t x, uint8_t i) {
	return (x >> i) & 1u;
}

inline void SetBit(uint32_t &x, uint8_t i) {
	x |= uint32_t(1) << i;
}

struct MaskResult {
	uint8_t bit_set[8];
};

inline MaskResult Mask(uint32_t x) {
	static constexpr uint32_t SALT[8] = {0x47b6137bU, 0x44974d91U, 0x8824ad5bU, 0xa2b7289dU,
	                                     0x705495c7U, 0x2df1424bU, 0x9efc4947U, 0x5c6bfb31U};
	MaskResult r;
	for (idx_t i = 0; i < 8; i++) {
		r.bit_set[i] = (x * SALT[i]) >> 27;
	}
	return r;
}

} // namespace

void BloomFilterBlock::Insert(BloomFilterBlock &b, uint32_t x) {
	auto masked = Mask(x);
	for (idx_t i = 0; i < 8; i++) {
		SetBit(b.block[i], masked.bit_set[i]);
	}
}

bool BloomFilterBlock::Check(const BloomFilterBlock &b, uint32_t x) {
	auto masked = Mask(x);
	for (idx_t i = 0; i < 8; i++) {
		if (!CheckBit(b.block[i], masked.bit_set[i])) {
			return false;
		}
	}
	return true;
}

RowGroupBloomFilter::RowGroupBloomFilter(idx_t expected_n, double fpp) {
	if (expected_n == 0) {
		expected_n = 1;
	}
	if (!(fpp > 0 && fpp < 1)) {
		throw InvalidInputException("bloom_index: fpp must be in (0,1), got %f", fpp);
	}
	// m = -k*n / ln(1 - fpp^(1/k)), with k=8 split-block bits.
	double k = 8.0;
	double n = LossyNumericCast<double>(expected_n);
	double m = -k * n / std::log(1.0 - std::pow(fpp, 1.0 / k));
	idx_t bits_per_block = 32 * 8; // 32 bits per uint32 * 8 uint32s per block
	idx_t raw_blocks = LossyNumericCast<idx_t>(m / static_cast<double>(bits_per_block));
	block_count = NextPowerOfTwo(static_cast<uint64_t>(raw_blocks));
	if (block_count == 0) {
		block_count = 1;
	}
	data.assign(block_count * BLOCK_BYTES, 0);
}

RowGroupBloomFilter::RowGroupBloomFilter(idx_t block_count_p, vector<uint8_t> data_p) : block_count(block_count_p), data(std::move(data_p)) {
	if (data.size() != block_count * BLOCK_BYTES) {
		throw IOException("bloom_index: corrupt bloom filter (size mismatch)");
	}
}

void RowGroupBloomFilter::Insert(uint64_t hash) {
	auto blocks = reinterpret_cast<BloomFilterBlock *>(data.data());
	uint64_t i = ((hash >> 32) * block_count) >> 32;
	BloomFilterBlock::Insert(blocks[i], static_cast<uint32_t>(hash));
}

bool RowGroupBloomFilter::Check(uint64_t hash) const {
	auto blocks = reinterpret_cast<const BloomFilterBlock *>(data.data());
	uint64_t i = ((hash >> 32) * block_count) >> 32;
	return BloomFilterBlock::Check(blocks[i], static_cast<uint32_t>(hash));
}

void RowGroupBloomFilter::MergeFrom(const RowGroupBloomFilter &other) {
	if (other.block_count != block_count) {
		throw InternalException("bloom_index: cannot merge bloom filters with different block counts");
	}
	for (idx_t i = 0; i < data.size(); i++) {
		data[i] |= other.data[i];
	}
}

void RowGroupBloomFilter::Serialize(Serializer &serializer) const {
	serializer.WriteProperty(100, "block_count", block_count);
	serializer.WriteProperty(101, "data", data);
}

unique_ptr<RowGroupBloomFilter> RowGroupBloomFilter::Deserialize(Deserializer &deserializer) {
	auto block_count = deserializer.ReadProperty<idx_t>(100, "block_count");
	auto data = deserializer.ReadProperty<vector<uint8_t>>(101, "data");
	return make_uniq<RowGroupBloomFilter>(block_count, std::move(data));
}

} // namespace duckdb
