cmake_minimum_required(VERSION 3.5...3.29)

project(BloomIndexExtension)

include_directories(include)

set(BLOOM_INDEX_EXTENSION_FILES
    bloom_index_extension.cpp
    bloom_filter.cpp
    bloom_index.cpp
    bloom_index_optimizer.cpp)

build_static_extension(bloom_index ${BLOOM_INDEX_EXTENSION_FILES})
set(PARAMETERS "-warnings")
build_loadable_extension(bloom_index ${PARAMETERS} ${BLOOM_INDEX_EXTENSION_FILES})

install(
  TARGETS bloom_index_extension
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
