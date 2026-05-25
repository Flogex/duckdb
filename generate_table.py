#!/usr/bin/env python3
"""Generate a disk-backed DuckDB table of 100M messages in timestamp order."""

import duckdb

DB_PATH = "messages.duckdb"
TABLE_NAME = "messages"
NUM_ROWS = 2_000_000_000
NUM_USERS = 500_000
BASE_TIMESTAMP = "TIMESTAMP '2024-01-01 00:00:00'"

con = duckdb.connect(DB_PATH)

con.execute(f"DROP TABLE IF EXISTS {TABLE_NAME}")

con.execute(f"""
    CREATE TABLE {TABLE_NAME} (
        ts        TIMESTAMP,
        user_id   UUID,
        message_id BIGINT,
        message   VARCHAR
    )
""")
print("A")

# Pre-generate the pool of NUM_USERS UUIDs once, indexed by a contiguous
# integer. Each message picks a random index into this pool, so the same
# user_id can appear many times (as expected for a messaging workload).
con.execute(f"""
    CREATE TEMP TABLE _users AS
    SELECT i AS idx, uuid() AS user_id
    FROM range(0, {NUM_USERS}) t(i)
""")

# Hash-join messages to the user pool. The join key references `m.i` (via
# `* 0`) so the planner binds the expression to the probe side and evaluates
# random() once per message row instead of once per _users row.
con.execute(f"""
    INSERT INTO {TABLE_NAME}
    SELECT
        {BASE_TIMESTAMP} + INTERVAL (m.i) SECOND AS ts,
        u.user_id                                 AS user_id,
        m.i                                       AS message_id,
        'message ' || m.i                         AS message
    FROM range(0, {NUM_ROWS}) m(i)
    JOIN _users u
      ON u.idx = CAST(floor(random() * {NUM_USERS} + m.i * 0) AS BIGINT)
    ORDER BY m.i
""")

(row_count,) = con.execute(f"SELECT COUNT(*) FROM {TABLE_NAME}").fetchone()
print(f"Inserted {row_count:,} rows into {TABLE_NAME} at {DB_PATH}")

con.close()
