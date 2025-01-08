SKIPPED_TESTS = set([
    'test/sql/types/map/map_empty.test',
    'test/extension/wrong_function_type.test',  # <-- JSON is always loaded
    'test/sql/insert/test_insert_invalid.test',  # <-- doesn't parse properly
    'test/sql/cast/cast_error_location.test',  # <-- python exception doesn't contain error location yet
    'test/sql/pragma/test_query_log.test',  # <-- query_log gets filled with NULL when con.query(...) is used
    'test/sql/json/table/read_json_objects.test',  # <-- Python client is always loaded with JSON available
    'test/sql/copy/csv/zstd_crash.test',  # <-- Python client is always loaded with Parquet available
    'test/sql/error/extension_function_error.test',  # <-- Python client is always loaded with TPCH available
    'test/sql/types/timestamp/test_timestamp_tz.test',  # <-- Python client is always loaded wih ICU available - making the TIMESTAMPTZ::DATE cast pass
    'test/sql/parser/invisible_spaces.test',  # <-- Parser is getting tripped up on the invisible spaces
    'test/sql/copy/csv/code_cov/csv_state_machine_invalid_utf.test',  # <-- ConversionException is empty, see Python Mega Issue (duckdb-internal #1488)
    'test/sql/copy/csv/test_csv_timestamp_tz.test',  # <-- ICU is always loaded
    'test/fuzzer/duckfuzz/duck_fuzz_column_binding_tests.test',  # <-- ICU is always loaded
    'test/sql/pragma/test_custom_optimizer_profiling.test',  # Because of logic related to enabling 'restart' statement capabilities, this will not measure the right statement
    'test/sql/pragma/test_custom_profiling_settings.test',  # Because of logic related to enabling 'restart' statement capabilities, this will not measure the right statement
    'test/sql/copy/csv/test_copy.test',  # JSON is always loaded
    'test/sql/copy/csv/test_timestamptz_12926.test',  # ICU is always loaded
    'test/fuzzer/pedro/in_clause_optimization_error.test',  # error message differs due to a different execution path
    'test/sql/order/test_limit_parameter.test',  # error message differs due to a different execution path
    'test/sql/catalog/test_set_search_path.test',  # current_query() is not the same
    'test/sql/catalog/table/create_table_parameters.test',  # prepared statement error quirks
    'test/sql/pragma/profiling/test_custom_profiling_rows_scanned.test',  # we perform additional queries that mess with the expected metrics
    'test/sql/pragma/profiling/test_custom_profiling_disable_metrics.test',  # we perform additional queries that mess with the expected metrics
    'test/sql/pragma/profiling/test_custom_profiling_result_set_size.test',  # we perform additional queries that mess with the expected metrics
])

failing_tests = [
    "test/fuzzer/duckfuzz/semi_join_has_correct_left_right_relations.test"
]