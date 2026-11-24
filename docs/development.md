# Development

## Layout

```text
src/
  gcs_adc_extension.cpp          entry point: settings and registration
  adc_secret_provider.cpp        CREATE SECRET (TYPE gcs, PROVIDER adc)
  credential_info_function.cpp   gcs_adc_credential_info()
  adc/credential_file.cpp        credential JSON parsing
  adc/credential_chain.cpp       env / gcloud / metadata chain, token cache
  adc/token_minter.cpp           OAuth2, JWT, metadata, IAM, STS flows
  adc/jwt.cpp                    RS256 signing (OpenSSL)
  adc/http_client.cpp            GET/POST through DuckDB's HTTPUtil
  adc/json.cpp, adc/encoding.cpp yyjson wrapper, base64url, URL/JSON encoding
test/
  sql/*.test                     tests that need no network
  sql/mock/*.test                end-to-end tests against the mock server
  mock/mock_google.py            fake Google endpoints (stdlib Python)
  mock/run_mock_tests.sh         starts the mock and runs test/sql/mock
  fixtures/, keys/               static fixtures and a throwaway RSA key
duckdb/, extension-ci-tools/     submodules (DuckDB v2.0.1, v2.0-cyanoptera)
```

## Building

```sh
git clone --recurse-submodules https://github.com/suhdev/gcs-duckdb
cd gcs-duckdb
GEN=ninja make release      # or: make debug
```

`extension_config.cmake` builds `httpfs` (pinned to the commit DuckDB v2.0.1 uses) next to `gcs_adc`, so the tests
can do real I/O. It also builds `ui` (pinned to duckdb-ui's DuckDB v2.0.1 commit) so releases can ship it. The first build compiles DuckDB itself and takes a while; after that, changes to `src/` rebuild in
seconds. Installing `ccache` helps a lot.

## Testing

```sh
./build/release/test/unittest "test/sql/*"   # no network needed
make test_mock                                # end-to-end, needs python3 and openssl
```

### The mock server

`test/mock/mock_google.py` serves, on one local port:

* an OAuth2 token endpoint that checks `refresh_token` grants and **verifies the RS256 signature** of `jwt-bearer`
  assertions against `test/keys/test_service_account.pub.pem`,
* an STS token-exchange endpoint and a URL subject-token source,
* IAM Credentials `generateAccessToken`,
* a metadata server (checking `Metadata-Flavor`),
* a minimal GCS XML API (GET with Range, HEAD, PUT, DELETE, ListObjectsV2, multipart upload) that **rejects tokens it
  did not issue, or has revoked or expired**.

On start it writes credential fixtures that point at its own port. `run_mock_tests.sh` exports
`GOOGLE_APPLICATION_CREDENTIALS`, `CLOUDSDK_CONFIG` and `GCE_METADATA_HOST` so every chain step resolves against the
mock, and passes `GCS_ADC_MOCK_PORT` / `GCS_ADC_MOCK_DIR` to the tests (`{GCS_ADC_MOCK_DIR}` in `.test` files).

Tests control the mock through plain HTTP reads, for example:

```sql
SELECT * FROM read_text('http://localhost:{GCS_ADC_MOCK_PORT}/__control?revoke_all=1');
SELECT * FROM read_json('http://localhost:{GCS_ADC_MOCK_PORT}/__stats');
```

| Control | Effect |
| --- | --- |
| `token_lifetime=N` | Tokens issued from now on last N seconds |
| `revoke_all=1` | Forget every issued token (the next GCS request gets 401) |
| `metadata=off` / `on` | Make the metadata server answer 404 |
| `reset_stats=1` | Zero the request counters |

!!! warning "Skips are failures"
    DuckDB's sqllogictest runner reports a test that hits an HTTP error as **skipped**, not failed.
    `run_mock_tests.sh` therefore fails if any mock test is skipped.

### Running the mock by hand

```sh
python3 test/mock/mock_google.py --dir /tmp/mockfx --port 8765
```

## Code style

```sh
make format-fix     # clang-format, black, cmake-format
make tidy-check     # clang-tidy, as CI runs it
```

## CI

| Workflow | What it does |
| --- | --- |
| `MainDistributionPipeline.yml` | Builds and tests the extension on every platform with `extension-ci-tools`, and runs the format and tidy checks |
| `MockTests.yml` | Builds on Linux and runs `make test_mock` |
| `Docs.yml` | Builds this site with `mkdocs build --strict`, and deploys it to GitHub Pages from `main` |
| `Release.yml` | On a `v*` tag, builds every platform and publishes a GitHub release ([releasing](releasing.md)) |

## Docs

```sh
pip install -r docs/requirements.txt
mkdocs serve
```
