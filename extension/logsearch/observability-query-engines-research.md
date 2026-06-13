# Query engines for observability — a research synthesis for a DuckDB FTS extension

## 1. The shape of the problem: what logs actually look like

Before looking at systems, the characteristics of log data dictate almost every implementation decision downstream. The systems below all converge on similar answers because they're all responding to the same workload shape.

**Time-ordered, append-only, immutable.** Logs arrive roughly in timestamp order, are never updated after the fact, and are deleted in bulk by retention policy. Every system below exploits this: Husky partitions fragments by time window and only compacts within a window; Honeycomb Retriever rolls into segments every ~1M events or every day; ClickHouse MergeTree sorts by a timestamp-prefixed key; Loki chunks streams by time; Quickwit splits are timestamp-bounded. A query's time range is almost always the first and most powerful prune.

**Skewed access pattern — heavily recent.** The overwhelming majority of queries touch the last few minutes to hours; a long tail goes back days or weeks. Older data is queried rarely but needs to remain available. This is why tiered storage (hot NVMe/SSD → warm → S3) is the default architecture, and why caching the recent fragments aggressively pays off. Husky reports ~80% result-cache hit rate and only 0.6% of data actually getting scanned on average because so many queries hit the hot, recent, already-cached slice.

**Write-heavy, read-light (relative to other DBs).** A logging system ingests millions of events per second continuously but sees comparatively few queries per second. This flips the usual optimization budget: indexing CPU is expensive at this scale, and systems minimize per-row index work (e.g. Loki indexes labels only; Honeycomb indexes nothing but timestamp; ClickHouse skip-indexes are per-granule, not per-row).

**Schemaless / wide / sparse.** Logs have no fixed schema — every tenant emits different fields, and the field set grows over time. Most fields are null for any given row (a user-agent exists on HTTP request logs but not on internal spans). Husky calls itself "schemaless"; Honeycomb stores "one file per unique field" per segment and accepts that most are sparse; ClickHouse now leans on its new JSON type + `JSONAllPaths`/`JSONAllValues` for this. Any extension must handle data where "columns" are really a dynamic dictionary and adding a new field shouldn't require a schema migration.

**High cardinality is the defining pain point.** Fields like `request_id`, `user_id`, `trace_id`, `session_id`, `container_id` have billions of unique values. This is what breaks metrics systems built on inverted label indexes (Prometheus) and what makes traditional inverted-index systems like Elasticsearch expensive for logs (12–19× storage overhead is cited). A useful log FTS system either (a) scales the posting lists to cope, (b) uses probabilistic structures (Bloom filters) that don't grow per-value, or (c) leans on columnar scan speed and skips indexing altogether for very high-cardinality content. Observability workloads require the ability to *filter* on high-cardinality fields AND *aggregate* (group by, count distinct) over them, which is a genuinely harder workload than either pure search or pure analytics.

**Two query shapes dominate.** Husky articulates this cleanly: "needle in a haystack" point lookups (find the one event with this request_id or error message) versus "analytics-style" broad aggregations (p95 latency by service over the last week). Honeycomb frames it as "many rows, few columns." A good system needs both a selective-filter path (inverted index, bloom filter, sort-key prune) AND a fast scan/aggregate path (columnar, vectorized). This is the single most important implication for a DuckDB extension: the FTS path must compose cleanly with DuckDB's existing vectorized aggregation, not sit beside it as a separate engine.

**Large values, irregular distribution.** Messages range from 50 bytes to multi-KB stack traces. Text content is highly repetitive (same template strings emitted millions of times) which makes dictionary-based compression enormously effective, but a small fraction of lines blow up the tail distribution.

**Interactive latency expectations.** Users iterate on queries in a UI: type a filter, see results, refine. Sub-second response on hot data is the table-stakes SLO; Husky's p50/p75 fragment latency is ~2 ms with a long tail to hundreds of ms, which is why streaming partial results matters. Honeycomb's "virtualized storage" work cut median query time from 20s to 0.2s for multi-service queries — two orders of magnitude mattered to them. The extension needs an execution model that can start returning results before all data is processed.

---

## 2. Datadog Husky — the detailed reference point

Husky is the best-documented production log query engine. It stores more than 100 trillion events and handles billions of queries/day, and the architecture is described in a four-part series that's worth reading end-to-end. It's what I'd treat as the canonical reference design.

**Overall shape.** Husky is an unbundled, distributed, schemaless, vectorized columnar store. Storage is object store (S3/GCS/Azure Blob), metadata lives in FoundationDB, compute is stateless and scales independently. Data is immutable fragments organized into tables, bucketed by time window. This shape — "object store as the source of truth, metadata service for coordination, stateless readers as cache" — is now the dominant architecture for log systems at scale.

**Query path: four services.**
- **Query planner** — entry point. Resolves context (facets, index configs), validates, throttles, and splits a query into multiple time-based steps that run in parallel. Merges sub-results.
- **Query orchestrator** — fetches fragment metadata, prunes fragments via zone-map checks (reportedly cuts work 60% for structured events, 30% on average), dispatches fragment queries to readers, aggregates.
- **Metadata service** — thin front-end over FoundationDB; handles atomicity for MVCC-style snapshot reads and the 5-second FDB transaction limit.
- **Reader service** — actually executes the fragment queries. This is where the interesting optimization work lives.

**Fragment layout.** Each fragment is a columnar file split into **row groups** of a few hundred rows. Each row group has a header containing per-column min/max and similar metadata. Reader uses an iterator-based Volcano-style execution model with `open`/`next`/`close`. The scan operator returns a **lazy reference** to the row group — nothing is decoded until accessed, so a cost-based optimizer can evaluate cheap predicates first and skip the expensive column decodes if nothing matches. Small row groups keep intermediate results in CPU cache and make vectorized SIMD work natural.

**Text search (the FTS path).** Each fragment has an optional **segment** file attached. The segment stores:
- **Posting lists** per term, stored as bitsets. Crucially, each term maps to *two* posting lists: one for "standard text fields" (message, title, stacktrace) and one for "all other attributes." This is how the distinction between standard search and full-text-across-everything is implemented.
- **Hashed n-grams** (4-grams in the example) for wildcard queries. Hashing keeps segment size bounded; false positives are acceptable because n-grams are used as a narrow-down step, not for exact matches. Per-segment caps on n-gram count.
- Posting-list bitsets are combined with boolean rewrites: `(login AND error) OR unavailable` becomes bitset intersection/union at query time.

**Three caches, three different roles.** All cache immutability is free — fragments never change, so invalidation isn't a concern, only eviction.
- **Result cache** — caches fragment-level query results. ~80% hit rate. Works because dashboards auto-refresh, monitors run repeatedly, and time ranges overlap.
- **Blob-range cache** — caches byte ranges from object storage on local disk (RocksDB-backed). ~70% hit rate. Uses singleflight-style dedup so concurrent queries for the same column-range share a single fetch. Adaptive: if disk I/O is saturated, it bypasses and goes direct to blob.
- **Predicate cache** — caches bitsets for expensive predicates per fragment. Only ~3% hit rate, but each hit saves ~15× the cost of computing it, making the economics work. Picks the top N% most expensive predicates it has seen recently.

**The Sankey of pruning.** Out of 1,000 fragment queries: 300 pruned at metadata level, 560 by result cache, 78 by column metadata, 28 by other caches, 30 from local disk cache, 4 actually hit blob storage. Only **0.4%** of queries cause object-storage reads. This cascade — metadata prune, cache, per-fragment column metadata, posting lists, row-group metadata, finally lazy column decode — is the heart of the design.

**Tenant isolation — shuffle sharding.** Pure consistent hashing gives affinity + load balancing but not tenant isolation (one tenant's expensive query can evict another's cache). Shuffle sharding assigns each tenant a subset of reader workers sized to their usage, so noisy neighbors are contained. New nodes get load-soaked by lowering their virtual-node count in the hash ring until their cache warms up.

**Streaming partial results.** Rather than block on the slowest fragment (long-tail latency kills p99), Husky streams results as they arrive. This requires a checkpointing system to deduplicate on retry, which is more complex than blocking RPCs but eliminates head-of-line blocking from one slow fragment.

**What Husky tells us about building an FTS extension:** the hot path isn't "run the inverted index" — it's *avoid running anything*. Metadata pruning, result caching, column-min/max skip, and posting-list intersection all happen before any data is read. The inverted index itself is a relatively small piece of a much larger skip-and-cache stack.

---

## 3. Honeycomb Retriever — the minimalist schemaless column store

Retriever is Scuba-inspired but trades Scuba's in-memory model for local SSD plus S3, which is the key cost/scale tradeoff for a startup vs Facebook.

**Data model.** Events are (dataset, timestamp, arbitrary key-value bag). No schema. No index other than timestamp.

**Physical layout.** For each dataset, data is grouped into **segments** (rotated every ~1M events or daily). Within each segment, **one file per unique field** (e.g. `timestamp.int64`, `error.varstring`, `duration_ms.int64`). Sparse fields produce sparse files — a user-agent file will be dense for HTTP request events and empty for internal DB-call spans. The "segment" concept serves the same role as Husky's fragment: a time-bounded unit of work that queries can skip over in bulk.

**Query model.** The reader does a straightforward scan over column files for rows in the time range, filters, aggregates. Aggregations are mergeable (HyperLogLog for distinct counts, t-digest for percentiles) so fan-out to multiple readers produces partial aggregates that the coordinator merges cheaply. **No inverted index at all** — Honeycomb argues that for "many rows, few columns" aggregation workloads, fast columnar scans on SSD are enough, and the complexity of a search index isn't worth it for their workload.

**Lambda fan-out.** When querying large S3-backed historical segments, they parallelize aggregation by spawning many short-lived AWS Lambda functions, each reading some S3 data and returning partial aggregates. This is a slick way to burst compute for one expensive query without keeping a large reader fleet provisioned.

**Virtualized datasets (2024 change).** To support queries spanning many services (large customers with 1000s of services), they introduced a mapping layer between logical datasets (services) and physical datasets (disk organization). Frequently co-queried services get interleaved into "container datasets" with a synthetic `virtual_dataset` column. Reported 100× speedup on median query time. The lesson is that **physical layout should match expected query patterns**, and a virtualization layer lets you re-optimize without rewriting all the data.

**What Retriever tells us:** if you're columnar and fast, you may not need an FTS index at all for log retrieval — you may just need fast scans. This is the "Loki-like" position applied harder. For a DuckDB extension, this is worth taking seriously: DuckDB is already a vectorized columnar engine, and for many workloads, `WHERE message LIKE '%error%'` with timestamp pruning may outperform any index. The FTS extension should make sense *in addition to* the base scan path, not replace it.

---

## 4. ClickHouse — the current mainstream choice

ClickHouse is displacing Elasticsearch for logs/observability (it's what ClickStack, SigNoz, and many in-house stacks are built on). Its new text-index feature went GA in version 26.2 and is the most directly relevant reference for a DuckDB extension.

**Context.** MergeTree table engine: data sorted by a user-specified key (typically `(timestamp, ...)`), stored in **parts**, split into **granules** of 8192 rows. Skip indexes are granule-level and tell the engine which granules *might* contain matching rows.

**Text index structure.** Three files per part:
- **Dictionary blocks file (.dct)** — sorted tokens in blocks of 512 (configurable via `dictionary_block_size`). Front-coding compression by default.
- **Index header file (.idx)** — sparse index: first token and offset of each dictionary block. Same idea as the sparse primary-key index. This is the part you load into memory to locate a term quickly.
- **Posting lists file (.pst)** — posting lists stored as **roaring bitmaps** (not raw integer arrays), which gives fast AND/OR intersection/union and good compression. Large posting lists are split into blocks of 1M rows by default.

**Granularity is "infinite."** Unlike other skip indexes (which might have granularity 1–16 meaning the skip index covers that many granules), the text index has granularity 100M — effectively one index per part. This is a critical design choice: with row-level posting lists, you don't need per-granule skip metadata; you can compute the exact matching row set from the index.

**Tokenizers, built-in and configurable.** `splitByNonAlpha` (the default for English), `splitByString` with custom separators, `asciiCJK` (Unicode word boundaries, handles CJK as single-char tokens), `ngrams(N)` (fixed-length n-grams, 1–8), `sparseGrams(min,max,cutoff)` (variable-length n-grams), and `array` (no tokenization, each value is a token — useful for Map key/value indexing). A **preprocessor** expression runs before tokenization for case folding, UTF-8 normalization, HTML stripping, etc., and the function layer automatically applies the same transform to search terms.

**Merging, not rebuilding.** When parts merge (the background compaction all MergeTree tables do), text indexes can be merged directly instead of rebuilt: read the sorted dictionaries, union them into a new dictionary, recompute row-number mappings for the postings. This is possible because the dictionary is sorted — exactly the thing that makes SSTable-style merges work. If a part doesn't have a materialized index, it's built into a temp file and merged in. **Any log FTS extension needs this property**; rebuilding indexes from scratch on every compaction is catastrophic at log volumes.

**Direct read optimization.** If the query's WHERE clause only uses functions the index can fully answer (`hasToken`, `hasAnyTokens`, `hasAllTokens`), ClickHouse can answer entirely from the index without touching the underlying text column. Example from their benchmarks: `hasToken(comment, 'ClickHouse')` over 28.7M rows drops from 0.362s / 9.51 GB scanned to 0.008s / 3.15 MB — a 45× speedup purely from not decoding the text column. **This is the single biggest win** and any FTS extension should design for it from the start: represent the filter as a row bitset from the index, and let the engine skip decoding the text column entirely when nothing downstream needs it.

**Direct read as a hint.** For functions that *could* have false positives from the index (like `LIKE '%foo%'` when tokens can't be fully extracted), the index still produces a filter that runs as a cheap PREWHERE stage, reducing the rows that hit the expensive filter. So even when the index isn't exact, it's still a selectivity boost.

**Three specialized caches** (off by default, tunable per-server):
- **Header cache** — the sparse per-dict-block offsets.
- **Tokens cache** — deserialized dictionary blocks.
- **Posting lists cache** — deserialized bitsets.

Same pattern as Husky's three caches but scoped differently: all three are about not re-parsing the on-disk index format on every query.

**Text vs bloom-filter comparison (from the ClickHouse docs themselves).** Bloom filter indexes (`tokenbf_v1`, `ngrambf_v1`) are probabilistic, skip-only, coarse-grained (granule-level), small (kB–MB per part), and hard to tune. Text indexes are deterministic, row-level, purpose-built for multi-token search, and large (dozens–hundreds of MB per part). For serious log search, text indexes win; for low-volume "check if this column maybe contains this token," bloom filters are enough.

**What ClickHouse tells us:** the standard inverted-index design (dictionary + posting lists with roaring bitmaps) is still the right baseline, but the detailed wins come from (1) merging indexes on part merge, (2) direct-read to skip column decoding, (3) caching the deserialized index structures, and (4) good tokenizer/preprocessor hygiene.

---

## 5. Snowflake — bloom-filter-based pruning at micro-partition granularity

Snowflake is the opposite design point from a purpose-built inverted index: everything is derived from bloom filters over already-columnar micro-partitions (their name for ~50–500 MB data files), without building the full Elasticsearch-style index.

**Search Optimization Service (SOS).** A background maintenance process builds a **search access path**: a set of blocked bloom filters per column per micro-partition. The bloom filter records which values *definitely aren't* in a micro-partition; queries check it and skip the partition if the target value is guaranteed absent. John Ryan's summary captures it well: "Whereas a B-Tree index records where the data IS. A bloom filter records where the data ISN'T."

**Pruning index innards (from patent US11803551B2).** Bloom filters are *hierarchical* and *blocked*. The system tunes parameters (bits per entry, number of hash functions, block size) based on a user-specified target false-positive rate, balancing disk, CPU, and accuracy. Frequently-updated partitions get different parameters than stable ones. The filters live separately from the data, built and maintained asynchronously.

**Semi-structured data.** For VARIANT/OBJECT/ARRAY columns, Snowflake indexes *all leaf fields* within the column, auto-detecting types. They report 2–3× latency improvement typically, up to 20× for some customers. Supports equality, IN, ARRAY_CONTAINS, ARRAYS_OVERLAP, substring, regex, null-check, and full-text SEARCH. There's also a default bloom filter over the *paths* (not values) in semi-structured columns, which is how they skip partitions that don't have the queried key at all.

**Substring and regex.** Extended in 2023 to handle `LIKE`, `ILIKE`, `RLIKE` by indexing substrings as well; the extension is configured per-column via `ADD SEARCH OPTIMIZATION ON SUBSTRING(C4)`.

**Tradeoffs.** Recommended only for large tables (thousands of micro-partitions), with at least 100K–200K distinct values in the filter column, and queries that return few rows. Costs are real: the maintenance service is a significant continuous spend, and storage overhead can be 30–50% of the table. Skipped if the optimizer thinks a full scan would be cheaper.

**What Snowflake tells us:** for log retrieval, "where the data *isn't*" is often as valuable as where it is, and a purely probabilistic structure is much cheaper to build and maintain than a full inverted index. For a DuckDB extension, it's worth considering whether some queries (point lookups on trace_id, request_id) are better served by per-row-group bloom filters than by posting lists. The answer is probably "both, as complementary tools": bloom filters for very high cardinality exact-match fields, text index for the message body and common log tokens.

---

## 6. Databricks — bloom filters plus Z-order, now deprecated in favor of layout

Databricks' story is instructive as a counter-example: they explicitly *deprecated* their Bloom filter index in recent releases because for their typical workload, better file layout (Z-order, liquid clustering) plus Photon's predictive I/O subsumed the benefit.

**Delta Bloom filter indexes (deprecated).** Per-file bloom filters on declared columns, with configurable false-positive rate and expected item count. Only indexes files written *after* creation; requires `OPTIMIZE` to backfill. Useful for high-cardinality equality lookups that Z-order can't handle (e.g., columns beyond the first 32 Z-order columns).

**Z-order and liquid clustering.** Reorganizes files so related values are colocated. Z-order is the space-filling-curve trick (multi-column colocation); liquid clustering (Delta 3.0+) improves on Z-order with incremental reclustering and more flexible column choices. This is a **layout** optimization, not an index — the engine relies on min/max statistics in the Parquet footer to skip whole files.

**Predictive I/O (Photon).** Advanced heuristics in the Photon execution engine that achieve file skipping without explicit indexes, by reading the existing Parquet statistics more cleverly and adapting during query execution. Databricks now argues this makes dedicated bloom indexes redundant for most workloads.

**What Databricks tells us:** the first-order optimization for log retrieval is **sort data by a good key** and **have the engine aggressively use zone-map / min-max statistics**. A separate FTS structure is worthwhile specifically for the content that doesn't naturally sort (message bodies, high-cardinality IDs that don't correlate with insertion time). Before building an index, check that the base columnar scan with time-range pruning isn't already fast enough.

---

## 7. Quickwit / Tantivy — the log-specialized Lucene

Quickwit is a distributed search engine built on Tantivy (a Rust Lucene-inspired library), specifically optimized for low-QPS, high-volume log workloads. It's the most directly comparable open-source full-text log system.

**Splits, not shards.** An index is divided into **splits** — UUID-identified, self-contained units holding (inverted index + columnar fast fields for aggregations + row-based doc store + hotcache). Splits live on object storage. The hotcache is a pre-computed header file that lets a split be "opened" in <60ms from S3 without streaming the whole thing.

**Decoupled storage and compute.** Searchers (stateless) query splits directly from S3. Indexers write new splits. Metastore tracks split metadata. Janitor cleans up old splits. This is the same shape as Husky.

**Immutable segments (inherited from Lucene/Tantivy).** Segments are append-only; deletes are tracked separately in a `.del` bitset. Updates = delete + add. Merges are streaming k-way merges of sorted term dictionaries, exactly like ClickHouse's text index merge.

**Time-pruning via split metadata.** Each split carries its `[timestamp_min, timestamp_max]`, so queries prune at the split level before doing any index work.

**Tantivy internals (which matter for an extension author).** The core inverted index is two data structures chained: a **term dictionary** (finite-state transducer / FST for the term → offset lookup, inherited from Lucene's FST-based approach) and a **posting-list store** giving `DocId` iterators. Optional position and term-frequency data per posting for phrase queries and BM25 scoring. **Fast fields** (Tantivy's columnar stores) sit alongside the inverted index for aggregations on the same segments — this is how Quickwit supports both "find this log line" and "count errors per service per minute" on the same data.

**Why BM25 for logs.** BM25 is the standard relevance-ranking score (used in Lucene/ES, Tantivy, ClickHouse's new scoring, DuckDB's existing FTS extension). For log *retrieval*, ranking relevance matters less than for web search — often users want the latest matching log, not the most "relevant" one. The common pattern is to use the index for *filtering* (AND of tokens) and order by timestamp, not by BM25. An extension should expose BM25 but default to or strongly support timestamp-order retrieval.

**What Quickwit tells us:** the object-storage-native, Lucene-style architecture is the right choice specifically when you can tolerate ~1s latency in exchange for order-of-magnitude cost savings vs Elasticsearch. For a DuckDB extension, this is less directly applicable (DuckDB is embedded, not a distributed service), but the internal fact that inverted index + columnar fast fields coexist in the same segment is directly reusable: the FTS index should live *alongside* the columnar storage, not in a separate side structure, so that predicate evaluation and column access share the same I/O.

---

## 8. Grafana Loki — the "don't index the content" extreme

Loki is worth mentioning as the opposite design choice from an FTS system: it indexes *only labels* (like `{service="api", region="us-east-1"}`) and stores log bodies compressed in chunks. Body search is brute-force grep on the selected streams.

**Label index + chunks.** TSDB-style label index maps label sets to chunk references; chunks are compressed log lines grouped by stream (= unique label combination) and time. High-cardinality values like request_id must **not** go into labels (they explode the index) — they stay in the log body.

**Bloom filters (3.0+, then revised in 3.3).** Added per-stream bloom filters to accelerate "needle in a haystack" body searches. Initially n-gram-based over the full log content, then revised to bloom over **structured metadata** keys/values, which are orders of magnitude smaller and cheaper to build. A separate Bloom Gateway service serves chunk-filter requests during query planning.

**What Loki tells us:** there's a real design question about *what* to index. Indexing every token in every log line is expensive and often not needed. Indexing just the label/tag set plus a bloom filter over a small set of known high-value tokens (trace_id, request_id, specific error codes) may be the right balance for many log workloads. A DuckDB extension probably wants to offer **granular control over what gets indexed** — e.g., "index the message column fully, but only index structured metadata keys for these other columns."

---

## 9. DuckDB's existing FTS extension — baseline and gaps

DuckDB already ships an `fts` extension (experimental, loosely SQLite FTS5-like):

- Creates an index via `PRAGMA create_fts_index(table, id_column, *value_columns)`.
- Supports a Porter stemmer with 25+ languages, stopword lists, regex-based token ignore, case folding, accent stripping.
- Builds an inverted index stored as DuckDB tables in a dedicated schema `fts_main_<tablename>`.
- Scoring via a `match_bm25(id, query_string, ...)` macro. Parameters: `k1`, `b`, `conjunctive`, `fields`.
- Follows the approach from the paper "Old Dogs Are Great at New Tricks" — implemented mostly in SQL, which means DuckDB's vectorized execution and parallelism accelerate it "for free."

**Gaps relative to a log-retrieval FTS extension:**
1. **No incremental update** — the index doesn't auto-update when rows are added; must be rebuilt with `overwrite := 1`. Fatal for log workloads where data streams in continuously.
2. **No time-based pruning** — the index is monolithic per table, not segmented by time.
3. **No direct-read optimization** — returns a score from the macro; DuckDB still needs to filter and project the base column.
4. **Stored as regular tables** — reasonable for dev-ergonomics (queryable, portable) but not optimized for the access patterns of log search (bitset intersection, roaring bitmaps, skip metadata).
5. **No tokenizer specialization for log content** — path splitting, IP addresses, hex IDs, JSON keys, k/v pairs — these are the things log tokenizers actually need.
6. **No n-gram or wildcard acceleration** beyond what BM25 token matching provides.
7. **No composition with DuckDB zone maps** — DuckDB does have min/max statistics per row group; the FTS path should interoperate with them.

---

## 10. Implications for a DuckDB FTS extension for log retrieval

Pulling it all together, here's what the observability-query-engine literature argues for:

**Physical organization first.**
- Segment data by **time window**. Every serious system does this. A fragment / segment / part / split is a time-bounded, immutable unit of work. It's the first-order prune and it's where caching, retention, and deletion all pivot.
- Within a segment, stay columnar and lean on **row-group-level zone maps** (min/max, bloom, distinct-values-if-small). DuckDB already has row groups in its native storage and Parquet; the FTS metadata should attach per row group so it composes with existing pruning.
- Build the index **per segment**, not globally. This gives you incremental ingestion (new segment = new index, no global rebuild) and enables streaming-partial-results fan-out.

**Index structure.**
- **Inverted index with roaring-bitmap posting lists.** Deterministic (no false positives like bloom), row-level granularity, composable via AND/OR/NOT. This is what ClickHouse, Tantivy/Quickwit, and Husky all land on.
- **Sorted term dictionary with sparse lookup index** (front-coded block-compressed dictionary + sparse offset table, per ClickHouse; or FST per Tantivy). Loading the whole dictionary into memory is wasteful at log scale.
- **Hashed n-gram secondary index** for wildcard/substring queries. Bounded size, accepts false positives because it only narrows the search. Husky's approach is a cleaner implementation than separate bloom-filter skip indexes.
- **Standard vs all-fields posting lists.** Husky's split between a "message-and-usual-text-fields" posting list and an "all-other-fields" posting list is a clean way to give users both `message:error` (fast, targeted) and `*:error` (full-text-everywhere) without indexing everything twice.

**Tokenization.**
- Configurable tokenizer (at minimum: split on non-alpha, split on separators list, n-grams, Unicode-aware / CJK) and configurable preprocessor (case fold, UTF-8 normalize, accent strip). Follow ClickHouse's lead here.
- **Special consideration for log tokens:** IP addresses, UUIDs/hex IDs, dotted paths, URLs, key=value pairs. These shouldn't be shattered into useless sub-tokens by a naive alphanumeric split. A good default tokenizer for logs treats these as single tokens where possible.
- Apply the same preprocessor at query time, automatically, to avoid miss-matched normalization.

**Query-time composition with DuckDB.**
- **Expose the filter as a bitset**, not just a score macro. The single biggest win in ClickHouse's text-index GA is direct-read: if the WHERE clause can be fully answered from the index, the engine skips reading the text column entirely. DuckDB's vectorized execution is perfectly shaped to consume a bitset as a selection vector.
- **Compose with zone maps.** Before running the index lookup, let the query optimizer use DuckDB's existing row-group min/max on the timestamp column. Inverted-index evaluation should happen *after* time pruning, not before.
- **Support conjunctive and disjunctive queries efficiently** (AND is the common case for log search: "service=X AND message contains Y AND level=error"). Roaring bitmap intersection is near-free once the postings are loaded.
- **Don't force BM25.** Log users usually want `ORDER BY timestamp DESC LIMIT N`, not relevance ranking. Offer BM25 but make the "filter only, order by timestamp" path the default and the fast path.

**Storage economics.**
- Index storage should be a **modest fraction** of raw log size — probably 10–30%, not Elasticsearch's 10–20×. Achieved via (a) roaring bitmaps, (b) dictionary compression, (c) omitting positions/tf unless the user enables phrase queries or BM25, (d) hashed n-grams rather than raw.
- **Merging on segment compaction** is essential. Read sorted term dictionaries from each segment, streaming-merge into a new dictionary, remap row numbers. ClickHouse and Lucene both do this; it must be cheaper than a rebuild.

**Ingestion.**
- **Incremental**: a new batch of rows forms a new small segment with its own index, and segments get compacted in the background. This is the fundamental mechanism that unlocks continuous log ingestion and is the biggest missing piece from DuckDB's current FTS extension.
- **Async index build** on segment creation is fine — queries fall back to column scan on unindexed segments (ClickHouse's pattern). This lets ingestion stay fast even if indexing lags.

**Caching.**
- DuckDB is embedded, so you don't need the three-tier service caching that Husky has, but the principle applies: cache deserialized dictionary headers, cache the term→posting-list mapping for hot terms, cache roaring bitmaps for repeated predicates. Immutable segments make all of these trivially consistent.

**What *not* to build (yet).**
- Full distributed fan-out — DuckDB is embedded; if distribution is needed, it's a job for the surrounding system (MotherDuck, or the user's orchestration).
- Complex relevance features (proximity boosting, custom similarity, learning-to-rank). Logs don't need them.
- Full-text writes that are part of transactions. Segments are immutable, built in the background, swapped in atomically.

---

## 11. The shortest possible summary

For a DuckDB log-retrieval FTS extension, the evidence from Husky, Retriever, ClickHouse, Snowflake, Databricks, Quickwit/Tantivy, and Loki converges on:

1. **Time-segment the data** and do most of the pruning at the segment and row-group level before touching any index.
2. **Build a per-segment inverted index** with a sorted dictionary + roaring-bitmap posting lists, mergeable on compaction.
3. **Add hashed n-grams** for wildcard support; accept false positives because they only narrow, not qualify.
4. **Expose the filter as a row bitset** that DuckDB's execution engine can consume as a selection vector to skip column decoding entirely (the "direct-read" win).
5. **Design tokenization for log content**, not prose — IPs, UUIDs, paths, k/v pairs are tokens.
6. **Default to timestamp-ordered retrieval**, with BM25 as an option rather than the main event.
7. **Index incrementally** — new segments get indexed in the background, old segments get their indexes merged during compaction. No global rebuild.
8. **Cache deserialized index structures**; segments are immutable so cache invalidation is trivial.
9. **Offer per-column indexing control** — not every column is worth inverting; some should use bloom filters, some min/max zone maps, some nothing at all.
10. **Compose with DuckDB's existing columnar/vectorized primitives** rather than building a parallel engine.
