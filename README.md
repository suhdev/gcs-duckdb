# gcs_adc

A DuckDB extension that lets DuckDB read and write Google Cloud Storage (`gs://`) using
**Google Application Default Credentials**: your `gcloud auth application-default login` credentials, a service
account key file, the GCE/GKE/Cloud Run metadata server, service account impersonation, or Workload Identity
Federation. No HMAC keys needed.

It adds a credential provider to the `gcs` secret type of DuckDB's `httpfs` extension, which keeps doing all of the
actual storage I/O. See [docs/SPEC.md](docs/SPEC.md) for the full design.

> Status: under development. See the issues for the roadmap.

## Building

Requires CMake, Ninja, a C++17 compiler, OpenSSL and libcurl development headers (or vcpkg).

```sh
git clone --recurse-submodules https://github.com/suhdev/gcs-duckdb
cd gcs-duckdb
GEN=ninja make release
```

Outputs:

* `build/release/duckdb`: DuckDB shell with the extension available
* `build/release/repository/<duckdb commit>/<platform>/gcs_adc.duckdb_extension`: the loadable extension
* `build/release/test/unittest`: the test runner

## Testing

```sh
./build/release/test/unittest "test/sql/*"
```

## Updating DuckDB

See [docs/UPDATING.md](docs/UPDATING.md).
