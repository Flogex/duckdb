cmake_minimum_required(VERSION 2.8.12...3.29)

project(LogsearchExtension)

include_directories(include)

set(LOGSEARCH_EXTENSION_FILES
    logsearch_extension.cpp
    logsearch_index.cpp
    logsearch_tokenizer.cpp
    logsearch_build.cpp
    logsearch_optimizer.cpp)

build_static_extension(logsearch ${LOGSEARCH_EXTENSION_FILES})
set(PARAMETERS "-warnings")
build_loadable_extension(logsearch ${PARAMETERS} ${LOGSEARCH_EXTENSION_FILES})

install(
  TARGETS logsearch_extension
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
