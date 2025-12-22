project (FlosFuzzer CXX)

set(SOURCE_FILES
    main.cpp
)

add_executable(flos_fuzzer ${SOURCE_FILES})

target_link_libraries(flos_fuzzer PRIVATE duckdb_static)