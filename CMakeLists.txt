cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME gcs_adc)

# DuckDB's extension distribution supports vcpkg. As such, dependencies can be
# added in ./vcpkg.json and then used in cmake with find_package. Feel free to
# remove or replace with other dependencies. Note that it should also be removed
# from vcpkg.json to prevent needlessly installing it..
find_package(OpenSSL REQUIRED)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD
    "17"
    CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)
# DuckDB's bundled yyjson (C++ extensions statically link DuckDB, which contains
# it)
include_directories(${CMAKE_SOURCE_DIR}/third_party/yyjson/include)

set(EXTENSION_SOURCES
    src/gcs_adc_extension.cpp
    src/adc_secret_provider.cpp
    src/credential_info_function.cpp
    src/adc/credential_chain.cpp
    src/adc/credential_file.cpp
    src/adc/encoding.cpp
    src/adc/http_client.cpp
    src/adc/json.cpp
    src/adc/jwt.cpp
    src/adc/token_minter.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Link OpenSSL in both the static library as the loadable extension
target_link_libraries(${EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(${LOADABLE_EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
