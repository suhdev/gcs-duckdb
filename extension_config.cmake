# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(gcs_adc
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# httpfs performs the actual GCS I/O; gcs_adc only supplies credentials. It is built
# alongside so the sqllogictests can exercise reads and writes end-to-end.
duckdb_extension_load(httpfs
    GIT_URL https://github.com/duckdb/duckdb-httpfs
    GIT_TAG f71cfdb4bff85b9e15b1f3b842d9aa395aff0167
)

# The DuckDB UI, built for the same DuckDB version so releases can ship it next to gcs_adc.
# b19dfd5 is duckdb-ui's "upgrade to duckdb v2.0.1" commit.
duckdb_extension_load(ui
    GIT_URL https://github.com/duckdb/duckdb-ui
    GIT_TAG b19dfd587ff5491f9fc7b8a2e9d59a74bfc0b547
)
