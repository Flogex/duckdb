#include "catch.hpp"
#include "duckdb.hpp"
#include "test_helpers.hpp"

#include <cstdint>
#include <string>

using duckdb::Connection;
using duckdb::DuckDB;

namespace {

void SchemaPurchasesLike(Connection &con) {
	REQUIRE_NO_FAIL(con.Query(R"(
		CREATE TABLE t (
			event_id          VARCHAR PRIMARY KEY,
			transaction_id    VARCHAR,
			attribution_token VARCHAR,
			ts                TIMESTAMP,
			customer_id       VARCHAR,
			product_id        BIGINT,
			quantity          INTEGER,
			value             DECIMAL(18,2)
		)
	)"));
	for (auto col : {"transaction_id", "attribution_token", "ts", "customer_id", "product_id"}) {
		REQUIRE_NO_FAIL(con.Query(std::string("CREATE INDEX idx_") + col + " ON t(" + col + ")"));
	}
}

void PopulatePurchases(Connection &con, int64_t n_rows) {
	REQUIRE_NO_FAIL(con.Query(
	    "INSERT INTO t SELECT "
	    "  md5(i::VARCHAR)                                       AS event_id, "
	    "  'tx_'   || (i % 1000)::VARCHAR                        AS transaction_id, "
	    "  CASE WHEN i % 5  = 0 THEN NULL "
	    "       ELSE 'attr_' || (i % 137)::VARCHAR END           AS attribution_token, "
	    "  TIMESTAMP '2026-04-01' + INTERVAL (i % 86400) SECOND  AS ts, "
	    "  CASE WHEN i % 11 = 0 THEN NULL "
	    "       ELSE 'cust_' || (i % 500)::VARCHAR END           AS customer_id, "
	    "  (i % 250)::BIGINT                                     AS product_id, "
	    "  (i % 5)                                               AS quantity, "
	    "  (i * 0.13)::DECIMAL(18,2)                             AS value "
	    "FROM range(" +
	    std::to_string(n_rows) + ") t(i)"));
}

std::string RunTxn(Connection &con, const std::string &sql) {
	auto begin = con.Query("BEGIN");
	if (begin->HasError()) {
		return begin->GetError();
	}
	auto stmt = con.Query(sql);
	if (stmt->HasError()) {
		auto err = stmt->GetError();
		con.Query("ROLLBACK");
		return err;
	}
	auto commit = con.Query("COMMIT");
	if (commit->HasError()) {
		return commit->GetError();
	}
	return {};
}

} // namespace

TEST_CASE("Many Updates") {
	DuckDB db(nullptr);
	Connection con(db);
	SchemaPurchasesLike(con);
	PopulatePurchases(con, 250000LL);

	auto err = RunTxn(con, R"(
		UPDATE t SET
			transaction_id    = transaction_id || '_v2',
			attribution_token = COALESCE(attribution_token, 'none') || '_v2',
			customer_id       = COALESCE(customer_id, 'anon')      || '_v2',
			product_id        = product_id + 1000000,
			ts                = ts + INTERVAL 1 SECOND
	)");
	REQUIRE(err.empty());
}
