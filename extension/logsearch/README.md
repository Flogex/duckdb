# logsearch — DuckDB Inverted Index Extension

Full-text search extension for DuckDB specialized for log message retrieval and observability workloads. Inspired by ClickHouse, Husky (Datadog), Quickwit/Tantivy, and Loki. See `observability-query-engines-research.md` for prior-art analysis.

## Status

MVP working. Single-column text index, in-memory, no persistence. Native DuckDB integration via `CREATE INDEX ... USING logsearch(column)`.

## Quick start

```sql
LOAD logsearch;

CREATE TABLE logs(ts TIMESTAMP, message VARCHAR);
-- ingest log rows ...

CREATE INDEX idx ON logs USING logsearch(message)
WITH (timestamp_column = 'ts');

-- Optimizer rewrites these transparently:
SELECT * FROM logs WHERE contains(message, 'timeout');
SELECT * FROM logs WHERE contains(message, 'timeout') AND ts > '2024-01-04';
SELECT * FROM logs WHERE prefix(message, 'error');
SELECT * FROM logs WHERE message = 'error connection timeout';
```

`EXPLAIN` should show `LOGSEARCH_INDEX_SCAN` instead of `SEQ_SCAN`.

---

## 1. Architecture

### High-level dataflow

```
┌─────────────────────────────────────────────────────────────┐
│                       Index Build                            │
├─────────────────────────────────────────────────────────────┤
│  CREATE INDEX ... USING logsearch                            │
│    ↓                                                         │
│  IndexType callbacks (bind / sort / global_init /            │
│    local_init / sink / combine / finalize)                   │
│    ↓                                                         │
│  Parallel sink: each thread tokenizes chunks,                │
│    routes to per-row-group partition by row_id               │
│    ↓                                                         │
│  Combine: merge local partitions into global                 │
│    ↓                                                         │
│  Finalize: attach timestamp zone maps via                    │
│    DataTable::GetPartitionStats()                            │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│                       Query path                              │
├─────────────────────────────────────────────────────────────┤
│  SELECT ... WHERE contains(msg, 'x') AND ts > 'y'             │
│    ↓                                                          │
│  pre_optimize hook (logsearch_optimizer.cpp):                 │
│    flatten AND → find matching index → extract ts bounds      │
│    → check selectivity → replace LogicalGet                   │
│    with LOGSEARCH_INDEX_SCAN table function                   │
│    ↓                                                          │
│  Execution InitGlobal (logsearch_index_scan.cpp):             │
│    look up index by name → query posting lists                │
│    → return sorted row_t[] (timestamp-pruned)                 │
│    ↓                                                          │
│  Execution Scan: DataTable::Fetch(row_ids)                    │
│    → only row groups containing matching rows touched         │
│    ↓                                                          │
│  Post-filter: original contains() ensures correctness         │
└─────────────────────────────────────────────────────────────┘
```

### Data structures

```
LogsearchIndex (BoundIndex subclass — one per table)
 ├── timestamp_column_name
 └── partitions: vector<unique_ptr<IndexPartition>>
      ├── partition[0]                ← aligned to row group 0
      │   ├── row_start, row_count
      │   ├── ts_min, ts_max          (from zone maps, optional)
      │   ├── dictionary: std::map<string, idx_t>
      │   └── posting_lists: vector<PostingList>
      │        └── PostingList = sorted vector<uint32_t>
      │                          (local row offsets in row group)
      ├── partition[1]                ← row group 1
      ⋮
```

**Partition = one DuckDB row group.** Posting lists store local offsets (uint32_t, bounded by ~122K). Global row ID = `partition.row_start + offset`.

### Files

| File | Role |
|---|---|
| `logsearch_extension.cpp` | Entry point. Registers IndexType + `pre_optimize_function` hook. |
| `logsearch_index.{hpp,cpp}` | `PostingList`, `IndexPartition`, `LogsearchIndex` (`BoundIndex` subclass). |
| `logsearch_build.{hpp,cpp}` | IndexType build callbacks (bind/sort/global_init/local_init/sink/combine/finalize). |
| `logsearch_tokenizer.{hpp,cpp}` | Splits on non-alphanumeric, lowercases, skips single-char tokens. |
| `logsearch_optimizer.{hpp,cpp}` | `pre_optimize_function`: matches filter patterns, rewrites `LogicalGet` to use index scan. |
| `logsearch_index_scan.{hpp,cpp}` | Custom table function `LOGSEARCH_INDEX_SCAN`. Queries index at execution time, fetches matching rows. |

### Optimizer rule

Runs as `pre_optimize_function` (before built-in optimizers, so filter pushdown hasn't moved predicates into table scan filters yet).

Steps:

1. Walk plan, find `LogicalFilter` over `LogicalGet`
2. Flatten `CONJUNCTION_AND` into leaf expressions
3. Match patterns:
   - `contains(col, 'x')`
   - `prefix(col, 'x')`
   - `col = 'value'`
4. For matched column, look up `logsearch` index in table's index list
5. Extract timestamp bounds from co-occurring `ts > / >= / < / <=` predicates
   - Uses `Expression::IsFoldable()` + `ExpressionExecutor::TryEvaluateScalar()` to resolve CAST-wrapped constants
6. Check selectivity at index level (cheap — sum of posting list sizes ÷ total rows)
7. If selectivity ≤ 20% threshold:
   - Build serializable `LogsearchScanBindData` (catalog/schema/table/index name + search params + ts bounds)
   - Replace `LogicalGet` with new `LogicalGet` using `logsearch_index_scan` function (same `table_index` preserved → bindings stay valid)
   - Original filter expressions kept on top for correctness
8. Otherwise: leave plan unchanged (full scan via DuckDB SEQ_SCAN)

### Execution

`LOGSEARCH_INDEX_SCAN` table function:

- `InitGlobal`: look up table + index by name, query posting lists with timestamp pruning → sorted `vector<row_t>`
- `Scan`: each thread grabs batch of `STANDARD_VECTOR_SIZE` row IDs (atomic counter), calls `DataTable::Fetch(tx, output, column_ids, row_id_vector, count, fetch_state)`
- Row groups not containing matching row IDs are never touched

Result has natural partition-ordered sort (partitions sorted by `row_start`, posting lists sorted within partition).

### Timestamp pruning

Per partition: `[ts_min, ts_max]` from zone maps via `DataTable::GetPartitionStats(context)` → `PartitionRowGroup::GetColumnStatistics()` → `NumericStats::GetMin/GetMax<timestamp_t>()`.

At query time: skip partition if `[ts_min, ts_max] ∩ [query_ts_min, query_ts_max] = ∅`.

DuckDB applies exact timestamp filtering on surviving rows.

---

## 2. Design decisions and trade-offs

### Per-row-group partitions vs single global posting list

**Chose**: per-row-group partitions.

**Why**:
- Time pruning is wholesale skip (2 compares vs scanning entire posting list)
- Each partition bounded → posting lists fit cache, roaring chunks dense
- Build parallelism: each partition independent
- Retention drop: free partition wholesale
- New partition for new row group: simple append

**Trade-off**: more dictionary lookups per query (N partitions × M terms). Mitigated by:
- Timestamp pruning typically eliminates 90%+ of partitions
- Per-partition bloom filter (future) skips dict load entirely
- Sparse-block dictionary lookup is ~100ns

### `pre_optimize_function` vs `optimize_function`

**Chose**: `pre_optimize_function`.

**Why**: built-in filter pushdown moves `contains()` predicates from `LogicalFilter` into `LogicalGet.table_filters` before `optimize_function` runs. By that point, there's no `LogicalFilter` node to intercept.

**Trade-off**: we run before other optimizations, so DuckDB has to re-do filter pushdown on the original filter we keep on top. Acceptable cost.

### Replace `LogicalGet` vs MARK join with row IDs

**Chose**: replace `LogicalGet` with custom table function.

**Why**: MARK join still scans entire table (just adds boolean column). For 100M rows and 1K matches, scans 100M rows. Replacing `LogicalGet` calls `DataTable::Fetch()` which only touches row groups containing matching IDs.

**Trade-off**:
- Less optimizer integration (zone maps on other columns can't prune further fetches)
- Custom function = black box to cost model
- For log workload, zone maps on non-timestamp columns rarely help (random distribution across row groups), so loss is minimal

### Index query at plan time vs execution time

**Chose**: execution time (in `InitGlobal`).

**Why**:
- Bind data must be serializable (no pointers, no materialized row IDs) for distributed execution
- Fresh data at execution time → newer commits visible
- Memory not held between plan and execute

**Trade-off**: selectivity check at plan time uses estimated counts (sum of doc frequencies), not actual intersection size. Estimate can be off for compound queries.

### `std::map` dictionary vs sorted vector vs hash table

**Chose** (v1): `std::map<string, idx_t>`.

**Why**: simplest correct implementation. Supports prefix scan via `lower_bound`.

**Trade-off**: pointer chasing per lookup, poor cache locality.

**Plan**: replace with sorted block + sparse index (SSTable-like) in v2 — 2-3x faster lookup, spillable to disk, prefix scan stays sequential.

### Sorted `vector<uint32_t>` posting lists vs roaring bitmaps

**Chose** (v1): sorted `vector<uint32_t>`.

**Why**: simplest correct implementation, fast for sparse lists.

**Trade-off**:
- No SIMD-friendly bitwise ops
- No NOT support
- Memory-inefficient for dense lists (low-cardinality terms)

**Plan**: integrate CRoaring in v2. Library handles dense/sparse switching, AVX2/AVX-512 intersection, fast NOT, smaller memory.

### Tokenizer

**Chose**: split on non-alphanumeric, lowercase, skip single-char.

**Why**: trivial, works for English log content.

**Trade-off**:
- `192.168.1.100` → `["192", "168", "100"]`. Can't search for IP as unit.
- No stemming, no stop-word removal
- Case insensitive in index — post-filter via `contains()` enforces original case

**Plan**: per-column tokenizer config in v2 (log-aware, regex-based, n-gram).

### `TryDelete` no-op + dirty index for rollback

**Chose**: index reflects writes immediately, no rollback semantics.

**Why**:
- Append-only log workload — rollbacks rare
- `Fetch()` respects MVCC visibility → uncommitted/rolled-back rows filtered out
- Avoids implementing delta indexes (`SupportsDeltaIndexes()`)

**Trade-off**: index has stale entries for rolled-back rows. Post-filter saves correctness. For non-log workloads with frequent rollbacks, this is wrong.

**Plan**: implement delta index path in v2 for MVCC correctness.

### Selectivity threshold = 20%

**Chose**: skip index if `df / total_rows > 20%`.

**Why**: above this point, fetching individual rows is slower than full scan.

**Trade-off**: rigid threshold causes cliff. `contains(msg, 'the')` (95% selectivity) falls back to scan; `contains(msg, 'request')` (19%) uses index. Performance difference at boundary is noisy.

**Plan**: cost-based decision in v2 using actual posting list sizes + system bandwidth estimates.

### Single-column index

**Chose** (v1): one indexed column per index.

**Trade-off**: can't combine `service = 'api'` with `contains(message, 'timeout')` at index level.

**Plan**: multi-column indexing in v2 with key-prefix encoding (`"message:error"`, `"service:api"`). Compound predicates intersect posting lists across columns. Killer feature DuckDB lacks (no bloom filters in storage).

---

## 3. Open questions and future improvements

### Correctness questions

- **`contains()` semantics**: tokenizer splits search string into tokens AND-intersected. `contains(msg, 'on tim')` becomes `["tim"]` (single-char "on" dropped). Index returns superset; post-filter catches it. Should we refuse to use index for sub-token queries to avoid over-fetch?
- **Insert into partial row group**: new rows in last (incomplete) row group → currently creates singleton partition per row. Need fix: extend last partition or use delta index path.
- **Zone map updates on Append**: partition's `ts_min/ts_max` set at build time, not refreshed on subsequent inserts → stale bounds. Either track ts during sink (need ts column as index expression) or re-query stats periodically.
- **Index after compaction**: DuckDB can rewrite row groups during checkpoint/vacuum. Row IDs change. Index becomes invalid. Currently no hook; would need full rebuild.
- **Concurrent modification**: index queried in `InitGlobal`, rows fetched in `Scan`. Concurrent commits between these phases → row IDs may be stale. Post-filter is safe for false positives; missing recent inserts is data-freshness concern, not correctness.

### Performance work

- **Replace `std::map` with sparse-block dictionary**: 2-3x speedup on lookup, prefix scan stays sequential, spillable
- **Roaring bitmaps for posting lists**: 5-10x smaller for dense lists, SIMD intersection, free NOT/complement
- **Per-partition bloom filters**: skip cold dictionary loads (~1KB per partition, 50 ns check)
- **Partition-level parallelism**: each thread owns full partition workflow (current: row-ID-level batches)
- **`LIMIT` pushdown**: scan partitions newest-first, stop after limit satisfied. Big win for `ORDER BY ts DESC LIMIT N` UI queries
- **Count-from-index**: resolve `count(*)` entirely from posting list sizes (skip table scan)
- **Block cache**: LRU over `BufferManager`-backed blocks; pin hot, evict cold
- **CRoaring vendored**: pulls in hand-tuned AVX2/AVX-512 intersection paths
- **Result cache per partition**: 80%+ hit rate observed in Husky for dashboards

### Features

- **Multi-column indexing**: `CREATE INDEX ... USING logsearch(message, service, level)`. Key-prefix terms (`"service:api"`). Compound posting list intersection across columns.
- **N-gram secondary index**: enables `LIKE '%pattern%'` acceleration
- **Phrase queries**: positional data per term, separate parallel stream
- **Persistence**: per-partition file with SSTable + roaring blocks; load on demand via `BufferManager`
- **Retention PRAGMA**: `CALL logsearch_prune('idx', '7 days')` and `WITH (max_retention = '7 days')`. Drop partitions where `ts_max < cutoff`.
- **Background index build**: async builder thread fills index for new committed row groups, queries fall back to scan on unindexed segments
- **Reversed terms in dictionary**: `LIKE '%suffix'` acceleration via reverse index
- **Delta index path** (`SupportsDeltaIndexes() = true`): per-txn delta index, merged on commit, dropped on rollback. Required for MVCC correctness on non-append-only workloads.

### Open design questions

- **Optimizer hook timing**: `pre_optimize_function` works but loses out on subsequent optimizations. Cleaner option: intercept at table_scan.cpp level like ART does. Requires DuckDB core changes.
- **Cardinality detection at build**: should we auto-decide which columns to index based on cardinality stats?
- **Per-column tokenizer config**: how to express? `WITH (tokenizer = 'log_aware', tokenizer.message = 'standard', tokenizer.path = 'path_split')`?
- **NULL semantics**: separate "is_null" posting list, or filter at fetch?
- **Sharing terms across partitions**: same `"error"` stored in 1000 partitions. Cross-partition dedup is complex; front-coding already cheap.

---

## 4. DuckDB integration

### Index registration

DuckDB has an `IndexType` plugin system. Each index type registers a set of callbacks with `DBConfig::GetIndexTypes()`. See `src/include/duckdb/execution/index/index_type.hpp`.

```cpp
// In extension Load:
config.GetIndexTypes().RegisterIndexType(LogsearchIndex::GetLogsearchIndexType());
```

`CREATE INDEX ... USING logsearch(col)` is routed to our callbacks via `IndexBinder::BindIndex` (`src/planner/expression_binder/index_binder.cpp`).

### IndexType build callbacks

Parallel build pipeline:

```
table scan → [sort, optional] → PHYSICAL_CREATE_INDEX
                                        ↓
                              build_global_init (1 thread)
                                        ↓
                              build_local_init (N threads)
                                        ↓
                       chunks → build_sink (N threads, parallel)
                                        ↓
                              build_combine (merge into global)
                                        ↓
                              build_finalize (1 thread)
                                        ↓
                              register BoundIndex in TableIndexList
```

Each callback receives an input struct (`IndexBuildBindInput`, etc.) with table reference, column IDs, expressions, and a custom state object. See `art_index.cpp` for the canonical example.

### `BoundIndex` interface

Subclass `BoundIndex` for the actual index. Pure virtuals we must implement:

- `Append(IndexLock&, DataChunk&, Vector& row_ids)` — add rows
- `Insert(IndexLock&, DataChunk&, Vector& row_ids)` — add rows with constraint check (we delegate to Append)
- `CommitDrop(IndexLock&)` — free all data
- `MergeIndexes(IndexLock&, BoundIndex& other)` — combine two indexes (used in build_combine)
- `Vacuum(IndexLock&)` — cleanup, no-op for us
- `GetInMemorySize(IndexLock&)` — for memory accounting
- `Verify(IndexLock&)` — integrity check
- `ToString(IndexLock&, bool ascii)` — debug
- `VerifyAllocations(IndexLock&)` — allocator integrity
- `GetConstraintViolationMessage(...)` — for unique/PK constraints (we don't enforce constraints)

Plus `TryDelete` and `VerifyBuffers` need overriding (defaults throw exceptions during INSERT rollback).

### Optimizer extension

`OptimizerExtension` lets extensions register hooks before or after built-in optimizers:

```cpp
OptimizerExtension opt_ext;
opt_ext.pre_optimize_function = LogsearchOptimize;  // runs before
OptimizerExtension::Register(config, opt_ext);
```

We modify the `unique_ptr<LogicalOperator> &plan` in place.

### Table function for index scan

Replacing the scan operator is done by creating a new `LogicalGet` with a custom `TableFunction`. The function has callbacks for bind, init global/local, scan, cardinality, etc.

Key APIs:
- `LogicalGet::table_index` — preserved when replacing so all column bindings upstream remain valid
- `LogicalGet::GetColumnIds()` / `SetColumnIds()` — projection control
- `LogicalGet::projection_ids` — projection pushdown
- `DataTable::Fetch(tx, result, column_ids, row_id_vector, count, fetch_state)` — fetch rows by row ID, MVCC-aware, only touches row groups containing the IDs

### Row group access

DuckDB's storage is organized as `RowGroupCollection` of `RowGroup` (~122K rows each). Row IDs are global integers; row group is found via binary search by `row_start`.

Public APIs (used by our build callbacks):

- `DataTable::GetPartitionStats(ClientContext&)` → `vector<PartitionStatistics>`
  - Each entry has `row_start`, `count`, and `partition_row_group` accessor for zone map stats
- `PartitionRowGroup::GetColumnStatistics(StorageIndex)` → `unique_ptr<BaseStatistics>`
- `NumericStats::GetMin<T>(BaseStatistics&)` / `GetMax<T>(...)` — type-safe min/max access

We don't reach into `DataTable::row_groups` (private member) — public `GetPartitionStats` gives the data we need.

### MVCC and visibility

- `DataTable::Fetch()` respects transaction snapshot — uncommitted rows from other transactions invisible, deleted rows filtered out
- Our index doesn't track MVCC; index entries reference row IDs regardless of visibility
- Correctness: post-filter via `Fetch()` MVCC + original predicate ensures wrong rows never returned
- Caveat: rolled-back rows leave dead entries in index (TryDelete no-op). Doesn't affect correctness; affects memory.

### Allocator (current: default malloc, planned: DuckDB-backed)

DuckDB has multiple allocation tiers:

- `Allocator::Get(db)` — tracked allocations, counts toward `memory_limit`, pinned forever
- `BufferManager::Allocate(MemoryTag, size)` → `BufferHandle` — evictable, can spill to disk
- `FixedSizeAllocator` — block-based, fixed-size slots, used by ART for tree nodes

Our extension currently uses default `std::vector` / `std::map` allocators → untracked by DuckDB's memory accounting. Means:
- Memory used by index not visible in `pragma memory_usage`
- DuckDB can't evict our memory under pressure
- Process can OOM while DuckDB stays within limit

**Planned abstraction** (v2): wrap allocation in `MemoryManager` class with two tiers (small/pinned via `Allocator::Get`, large/evictable via `BufferManager`). STL-allocator adapter for `std::vector<uint32_t, LogsearchAlloc<uint32_t>>` so existing code stays the same.

This shields the rest of the extension from changes to DuckDB's allocation APIs.

### Index storage (extension API limitation)

`ExtensionLoader` does not currently expose `RegisterIndexType` as a public method — we reach into `DBConfig::GetIndexTypes()` directly. Stable, but not the cleanest path. Future DuckDB versions may add a proper API.

Persistence support (`SerializeToDisk`, `SerializeToWAL`) on `BoundIndex` exists but we haven't implemented it — index is in-memory only.

---

## Testing

```bash
GEN=ninja make debug
./build/debug/test/unittest "test/sql/logsearch/*"
```

Test files in `test/sql/logsearch/`:

- `test_logsearch_basic.test` — CREATE, contains, prefix, exact, ts filter, NULL, empty, DROP INDEX
- `test_logsearch_plan_validation.test` — EXPLAIN checks (index used vs skipped)
- `test_logsearch_correctness.test` — EXCEPT queries: indexed result = brute-force result
- `test_logsearch_edge_cases.test` — single row, all NULL, long messages, special chars, multi-index, case sensitivity
- `test_logsearch_projections.test` — column subsets, SELECT *, aggregation, ORDER BY, LIMIT, non-indexed filter combination

141 assertions, all pass.

---

## References

- DuckDB ART index: `src/execution/index/art/art_index.cpp`
- BoundIndex interface: `src/include/duckdb/execution/index/bound_index.hpp`
- IndexType: `src/include/duckdb/execution/index/index_type.hpp`
- OptimizerExtension: `src/include/duckdb/optimizer/optimizer_extension.hpp`
- InClauseRewriter (MARK join pattern): `src/optimizer/in_clause_rewriter.cpp`
- DuckDB table_scan with ART index: `src/function/table/table_scan.cpp`
- Research synthesis: `observability-query-engines-research.md` in this directory
