# `gcs_adc`: Google Application Default Credentials for DuckDB

Status: Draft v1 · Target DuckDB: v2.0.1 (template branch `v2.0-cyanoptera`)

## 1. Summary

`gcs_adc` is a DuckDB extension that lets DuckDB read and write Google Cloud Storage (`gs://` / `gcs://`) using
**Google Application Default Credentials (ADC)**: the same credential discovery that `gcloud`, the Google Cloud
client libraries and `gsutil` use. With it, a user who has run `gcloud auth application-default login`, or who runs
on GCE / GKE / Cloud Run / Cloud Functions, or who has `GOOGLE_APPLICATION_CREDENTIALS` pointing at a key file, can do:

```sql
INSTALL gcs_adc FROM community;  -- once published; until then LOAD the built binary
LOAD gcs_adc;

CREATE SECRET (TYPE gcs, PROVIDER adc);

SELECT * FROM 'gs://my-bucket/events/*.parquet';
COPY (SELECT * FROM t) TO 'gs://my-bucket/out/t.parquet';
```

No HMAC keys, no copy-pasted tokens, and tokens are refreshed automatically when they expire.

## 2. Background and relationship to `httpfs`

DuckDB's `httpfs` extension already implements GCS I/O. It registers the `gs://` and `gcs://` schemes and talks to the
GCS **XML API** at `storage.googleapis.com`, which is S3-compatible. It supports reads (ranged GETs), globbing (bucket
listing), writes (multipart upload), requester-pays (`user_project`), and two ways to authenticate a `gcs` secret:

| httpfs `gcs` secret auth | How it works | Limitation |
| --- | --- | --- |
| `KEY_ID` + `SECRET` | HMAC keys, SigV4-style signing | HMAC keys must be created and managed per service account; many orgs disable them by policy |
| `BEARER_TOKEN` | Sends `Authorization: Bearer <token>` | The user must obtain an OAuth2 access token themselves (e.g. `gcloud auth print-access-token`), and it expires after ~1 hour |

`gcs_adc` **does not reimplement any storage I/O**. It is a *credential provider* for the existing `gcs` secret type:
it discovers ADC, exchanges it for an OAuth2 access token, and produces an ordinary httpfs `gcs` secret whose
`bearer_token` is that access token. httpfs keeps doing all reads, writes and globbing.

This mirrors exactly how the official `aws` extension adds a `credential_chain` provider to httpfs's `s3` secret type.

Why this split:

* **Correctness and performance for free.** httpfs's GCS path is battle-tested (retries, connection reuse, caching,
  multipart writes, generation pinning). A second filesystem claiming `gs://` would also conflict with httpfs, since
  DuckDB routes a path to the first filesystem that claims it.
* **Small surface.** The extension's job is exactly the part that is missing: ADC discovery, token minting, and refresh.
* **Composability.** Every existing httpfs `gcs` option (`SCOPE`, `ENDPOINT`, `URL_STYLE`, `USER_PROJECT`,
  `REQUESTER_PAYS`, proxies, extra headers) keeps working, because the output is a normal `gcs` secret.

### 2.1 Token refresh

Access tokens are short-lived (typically 3600 s). httpfs already has a refresh protocol for secrets: if a secret
carries a `refresh_info` struct, then when a request fails with an authentication error httpfs re-runs
`CREATE OR REPLACE SECRET` with the same type, provider, name, scope and the options stored in `refresh_info`, and
retries the request with the new secret. `gcs_adc` populates `refresh_info` with its own creation options, so an
expired token is transparently re-minted from ADC. (The setting `httpfs_enable_credential_refresh`, default `true`,
controls this on the httpfs side.)

### 2.2 Provider name

The provider is named **`adc`**. The name `credential_chain` is deliberately avoided because the `aws` extension
already registers a `credential_chain` provider on the `gcs` secret type (for AWS-style credentials); reusing the name
would make the outcome depend on extension load order.

## 3. Goals and non-goals

### Goals

1. `CREATE SECRET (TYPE gcs, PROVIDER adc)` resolves credentials using the standard ADC order (section 5) and works
   for reads, globs and writes through httpfs.
2. Support every credential source a typical GCP user has:
   * gcloud user credentials (`authorized_user`),
   * service-account key files (`service_account`),
   * service-account impersonation (`impersonated_service_account` files, and an explicit
     `IMPERSONATE_SERVICE_ACCOUNT` option),
   * the GCE metadata server (GCE, GKE incl. Workload Identity, Cloud Run, Cloud Functions, App Engine, Dataproc,
     Vertex AI workbench),
   * Workload Identity Federation (`external_account`) with file- and URL-sourced subject tokens.
3. Transparent token refresh for long-running sessions and queries.
4. No new storage code paths; no conflict with httpfs or the `aws` extension.
5. Never print or log access tokens, refresh tokens or private keys.
6. Build, test and distribute with the official DuckDB extension template / `extension-ci-tools` conventions, on all
   platforms the template builds for (Linux, macOS, Windows; WASM is out of scope, see below).

### Non-goals (v1)

* A new filesystem, or any change to how httpfs reads or writes data.
* The GCS JSON API (resumable uploads, object metadata APIs, `gs://` listing beyond what httpfs does).
* Bucket/object management functions (create bucket, delete object, set ACLs).
* `external_account` with AWS-sourced or executable-sourced credentials (planned for a later version, section 12).
* Running interactive OAuth flows (`gcloud auth login` equivalents). The user obtains ADC with `gcloud`.
* DuckDB-WASM (no filesystem access to credential files, no metadata server).

## 4. User-facing interface

### 4.1 Secret provider

```sql
CREATE [OR REPLACE] [PERSISTENT | TEMPORARY] SECRET [name] (
    TYPE gcs,
    PROVIDER adc,
    -- all optional:
    CHAIN 'env;gcloud;metadata',
    CREDENTIALS_FILE '/path/to/credentials.json',
    IMPERSONATE_SERVICE_ACCOUNT 'sa@project.iam.gserviceaccount.com',
    DELEGATES ['sa1@p.iam.gserviceaccount.com', ...],
    OAUTH_SCOPES ['https://www.googleapis.com/auth/devstorage.read_write'],
    USE_QUOTA_PROJECT false,
    VALIDATION 'exists',
    REFRESH 'auto',
    -- passed through unchanged to the httpfs gcs secret:
    SCOPE 'gs://my-bucket',
    ENDPOINT 'storage.googleapis.com',
    URL_STYLE 'path',
    USE_SSL true,
    USER_PROJECT 'billing-project',
    REQUESTER_PAYS false,
    HTTP_PROXY '...', HTTP_PROXY_USERNAME '...', HTTP_PROXY_PASSWORD '...',
    EXTRA_HTTP_HEADERS MAP {'k': 'v'}
);
```

#### Options

| Option | Type | Default | Meaning |
| --- | --- | --- | --- |
| `CHAIN` | VARCHAR | `env;gcloud;metadata` | Ordered, `;`-separated list of sources to try (section 5). Unknown names are an error. |
| `CREDENTIALS_FILE` | VARCHAR | none | Use this credential JSON file and nothing else. Mutually exclusive with `CHAIN`. Supports `~` expansion. |
| `IMPERSONATE_SERVICE_ACCOUNT` | VARCHAR | none | After resolving the source credential, call IAM Credentials `generateAccessToken` for this service account and use that token. |
| `DELEGATES` | VARCHAR[] | `[]` | Delegation chain for impersonation. Only valid with `IMPERSONATE_SERVICE_ACCOUNT`. |
| `OAUTH_SCOPES` | VARCHAR[] | `['https://www.googleapis.com/auth/devstorage.read_write']` | OAuth scopes requested where the source supports scopes (service account, metadata server, impersonation, external account). Ignored for `authorized_user`, whose scopes were fixed at `gcloud` login. Named `OAUTH_SCOPES` because `SCOPE` is DuckDB's secret path-prefix option. |
| `USE_QUOTA_PROJECT` | BOOLEAN | `false` | If `true` and the credential file has `quota_project_id`, set the secret's `USER_PROJECT` to it (unless `USER_PROJECT` is given explicitly). Off by default because billing a quota project requires `serviceusage.services.use` on it and would turn working reads into 403s for many users. |
| `VALIDATION` | VARCHAR | `exists` | `exists`: `CREATE SECRET` fails if no source in the chain yields a token. `none`: create the secret even if minting fails now (the token is minted on first refresh). |
| `REFRESH` | VARCHAR | `auto` | `auto`: store `refresh_info` so httpfs re-mints on auth failure. `disabled`: never refresh (token expires after ~1 h). |
| `SCOPE`, `ENDPOINT`, `URL_STYLE`, `USE_SSL`, `USER_PROJECT`, `REQUESTER_PAYS`, `HTTP_PROXY*`, `EXTRA_HTTP_HEADERS` | as httpfs | as httpfs | Copied verbatim into the resulting `gcs` secret. |

#### Resulting secret

The provider returns a `KeyValueSecret` of type `gcs`, provider `adc`, containing:

| Key | Value | Redacted |
| --- | --- | --- |
| `bearer_token` | the OAuth2 access token | yes |
| `refresh_info` | struct of the user's creation options (when `REFRESH 'auto'`) | yes |
| `credential_source` | which source produced the token: `env`, `gcloud`, `metadata`, `file` | no |
| `credential_type` | `authorized_user`, `service_account`, `impersonated_service_account`, `external_account`, `gce_metadata` | no |
| `principal` | service-account email or `client_email` when known; empty for `authorized_user` | no |
| `token_expires_at` | expiry timestamp of the minted token (ISO 8601, UTC) | no |
| pass-through options | as given | as httpfs does |

`FROM duckdb_secrets()` therefore shows where the credential came from and when the token expires, but never shows a
token.

### 4.2 Diagnostic table function

```sql
SELECT * FROM gcs_adc_credential_info(
    chain := 'env;gcloud;metadata',            -- optional, same meaning as CHAIN
    credentials_file := '/path/key.json',      -- optional
    impersonate_service_account := 'sa@...'    -- optional
);
```

Runs the same resolution as the provider, mints a token, and returns one row:

| Column | Type |
| --- | --- |
| `credential_source` | VARCHAR |
| `credential_type` | VARCHAR |
| `credentials_path` | VARCHAR (NULL for metadata) |
| `principal` | VARCHAR |
| `project_id` | VARCHAR |
| `quota_project_id` | VARCHAR |
| `scopes` | VARCHAR[] |
| `token_expires_at` | TIMESTAMP WITH TIME ZONE |
| `attempts` | VARCHAR[] (one human-readable line per chain step that was tried and skipped, with the reason) |

It never returns the token. If no source works it raises the same error `CREATE SECRET` would (section 7).

### 4.3 Settings

| Setting | Type | Default | Meaning |
| --- | --- | --- | --- |
| `gcs_adc_metadata_timeout_ms` | UBIGINT | `500` | Connect/read timeout for the metadata-server probe. |
| `gcs_adc_token_timeout_ms` | UBIGINT | `10000` | Timeout for token-endpoint calls (OAuth2, STS, IAM Credentials). |

Standard environment variables honoured (same semantics as Google client libraries):

| Variable | Effect |
| --- | --- |
| `GOOGLE_APPLICATION_CREDENTIALS` | Path to credential JSON, used by the `env` step. |
| `CLOUDSDK_CONFIG` | gcloud config directory used by the `gcloud` step. |
| `GCE_METADATA_HOST` | Host[:port] of the metadata server (default `metadata.google.internal`, fallback `169.254.169.254`). |
| `NO_GCE_CHECK` | If `true`, the `metadata` step is skipped. |

## 5. Credential resolution

### 5.1 Chain steps

Default order matches `google.auth.default()` / `gcloud`:

1. **`env`**: if `GOOGLE_APPLICATION_CREDENTIALS` is set and non-empty, load that file. If it is set but the file is
   missing or invalid, this is an **error** (not a skip), matching client-library behaviour, so a typo is not silently
   masked by a different identity.
2. **`gcloud`**: the well-known file
   * Linux/macOS: `$CLOUDSDK_CONFIG/application_default_credentials.json`, else
     `$HOME/.config/gcloud/application_default_credentials.json`
   * Windows: `%CLOUDSDK_CONFIG%\application_default_credentials.json`, else
     `%APPDATA%\gcloud\application_default_credentials.json`

   Missing file → skip. Present but invalid → error.
3. **`metadata`**: unless `NO_GCE_CHECK=true`, probe the metadata server with
   `GET http://$HOST/computeMetadata/v1/instance/service-accounts/default/email` and header
   `Metadata-Flavor: Google` (timeout `gcs_adc_metadata_timeout_ms`). Response must carry `Metadata-Flavor: Google`.
   Unreachable → skip.

`CREDENTIALS_FILE` replaces the chain with a single step named `file`.

### 5.2 Credential file types

The JSON `type` field selects the flow:

| `type` | Flow |
| --- | --- |
| `authorized_user` | `POST https://oauth2.googleapis.com/token` (or the file's `token_uri`) with `grant_type=refresh_token`, `client_id`, `client_secret`, `refresh_token`. |
| `service_account` | Build a JWT (`RS256`, header `kid` = `private_key_id`), claims `iss`=`sub`=`client_email`, `aud`=`token_uri`, `scope`=space-joined scopes, `iat`=now, `exp`=now+3600; sign with `private_key` (PKCS#8 PEM); `POST token_uri` with `grant_type=urn:ietf:params:oauth:grant-type:jwt-bearer&assertion=<jwt>`. |
| `impersonated_service_account` | Resolve `source_credentials` (recursively one of the above, depth limit 1), then impersonate the account in `service_account_impersonation_url` (with `delegates`). |
| `external_account` | Read the subject token from `credential_source` (`file`, optionally `format.type=json` + `subject_token_field_name`; or `url` with `headers`, same `format` options). Exchange at `token_url` (STS, `grant_type=urn:ietf:params:oauth:grant-type:token-exchange`, `requested_token_type=urn:ietf:params:oauth:token-type:access_token`, `subject_token_type`, `audience`, `scope`). If `service_account_impersonation_url` is present, impersonate with the STS token. `workforce_pool_user_project` is sent as `options.userProject` when present. `credential_source.aws` and `credential_source.executable` are rejected with a clear "not supported yet" error. |
| anything else | Error naming the type and the file. |

### 5.3 Metadata server

`GET http://$HOST/computeMetadata/v1/instance/service-accounts/default/token?scopes=<comma-joined>` with
`Metadata-Flavor: Google`. On GKE Workload Identity and Cloud Run the `scopes` query is honoured or ignored by the
server; both are fine. `principal` comes from `.../default/email`.

### 5.4 Impersonation

`POST https://iamcredentials.googleapis.com/v1/projects/-/serviceAccounts/{sa}:generateAccessToken` with body
`{"scope": [...], "delegates": ["projects/-/serviceAccounts/..."], "lifetime": "3600s"}` and the source token as
bearer. Response `accessToken` / `expireTime` (RFC 3339).

### 5.5 Token handling

* Every flow yields `(access_token, expires_at)`. `expires_in` responses are converted to an absolute time.
* A process-wide, mutex-protected cache keyed by the resolved credential identity (file path + mtime, or metadata
  host, plus scopes and impersonation target) holds the last token. A cached token is reused if it has at least
  **5 minutes** of validity left; otherwise a new one is minted. This keeps `CREATE SECRET` cheap when many secrets
  share the same credential, and makes httpfs refresh storms (many threads hitting 401 at once) mint once.
* Tokens, refresh tokens, client secrets and private keys are never logged, never included in error messages, and
  are zeroed from temporary buffers after use where practical.

### 5.6 HTTP transport

All token-endpoint and metadata calls go through DuckDB's `HTTPUtil` (the transport httpfs installs), so proxy
settings, CA bundles and logging behave like the rest of DuckDB. Because core `HTTPUtil` only implements GET, the
extension autoloads `httpfs` on first use; if that fails it raises
`"gcs_adc requires the httpfs extension: INSTALL httpfs; LOAD httpfs;"`. Since the secret type `gcs` is itself
registered by httpfs, the extension also autoloads httpfs in its `Load` when autoloading is enabled.

### 5.7 Crypto and parsing

* JWT signing: OpenSSL `EVP_DigestSign` with SHA-256 over an RSA key read by `PEM_read_bio_PrivateKey`.
  OpenSSL is a vcpkg dependency, as in the template.
* JSON: DuckDB's bundled `yyjson` (available because C++ extensions statically link DuckDB).
* Base64url and URL-encoding: small internal helpers.

## 6. Interaction with the rest of DuckDB

* **Secret selection.** DuckDB picks the secret with the longest matching `SCOPE`. Users can mix ADC secrets with
  HMAC secrets per bucket: `CREATE SECRET a (TYPE gcs, PROVIDER adc, SCOPE 'gs://bucket-a')`.
* **Persistent secrets.** Allowed. The persisted secret contains the (short-lived) bearer token and the
  `refresh_info`; after a restart the first request gets 401 and httpfs re-mints. The docs recommend temporary
  secrets plus an init script, because persisting writes a live token to `~/.duckdb/stored_secrets`.
* **`aws` extension.** No interaction, since provider names differ.
* **Concurrency.** Token minting is thread-safe (section 5.5). The provider function can run concurrently from
  multiple connections.

## 7. Errors

All errors are `IOException` or `InvalidInputException` with a message that says what was tried and what to do:

```
gcs_adc: could not find Application Default Credentials. Tried:
  - env: GOOGLE_APPLICATION_CREDENTIALS is not set
  - gcloud: /home/me/.config/gcloud/application_default_credentials.json does not exist
  - metadata: metadata.google.internal did not respond within 500 ms
Run 'gcloud auth application-default login', set GOOGLE_APPLICATION_CREDENTIALS, or pass CREDENTIALS_FILE.
```

Token-endpoint failures include the HTTP status and the OAuth `error` / `error_description` fields (never the
request body). Example: `gcs_adc: token request to https://oauth2.googleapis.com/token failed (400 invalid_grant:
Token has been expired or revoked). Run 'gcloud auth application-default login' again.`

## 8. Security considerations

* No secret material in logs, errors, `duckdb_secrets()` or `gcs_adc_credential_info()`.
* Credential files are read only when a secret is created or refreshed; they are not cached beyond the parsed
  fields needed for refresh, and never written.
* Default OAuth scope is `devstorage.read_write`, not `cloud-platform`, to limit blast radius of a leaked token.
* TLS verification is always on for Google endpoints; only the metadata server uses plain HTTP (link-local, as
  every Google client library does).
* Persistent secrets store a live token on disk (section 6); this is documented.

## 9. Repository layout

Following `duckdb/extension-template`:

```
CMakeLists.txt, Makefile, extension_config.cmake, vcpkg.json
duckdb/                  (submodule, pinned to v2.0.1)
extension-ci-tools/      (submodule, branch v2.0-cyanoptera)
src/
  gcs_adc_extension.cpp          extension entry, registration, settings
  adc/credential_chain.cpp       chain parsing and step execution
  adc/credential_file.cpp        JSON credential file loading
  adc/oauth_client.cpp           token endpoint calls (refresh_token, jwt-bearer, STS, IAM)
  adc/metadata_client.cpp        metadata server probe/token
  adc/jwt.cpp                    RS256 JWT signing
  adc/token_cache.cpp            process-wide token cache
  secret/adc_secret_provider.cpp CREATE SECRET provider
  functions/credential_info.cpp  gcs_adc_credential_info()
  include/...
test/
  sql/*.test                     sqllogictests
  mock/mock_google.py            fake OAuth2/STS/IAM/metadata + minimal GCS XML API
  keys/                          throwaway RSA test key
docs/SPEC.md, docs/UPDATING.md
.github/workflows/MainDistributionPipeline.yml
```

## 10. Testing strategy

No real GCP is needed for CI.

1. **Unit-level sqllogictests** (`test/sql/`), runnable with `make test`:
   * provider registration, option validation (unknown chain step, `CHAIN` + `CREDENTIALS_FILE`, bad `VALIDATION`),
   * error message when nothing is found (env vars pointed at empty temp dirs, `NO_GCE_CHECK=true`),
   * malformed / unsupported credential files.
2. **Mock-server integration tests**: `test/mock/mock_google.py` (Python stdlib only) serves:
   * an OAuth2 token endpoint that validates `refresh_token` grants and verifies `jwt-bearer` assertions against the
     test public key,
   * an STS endpoint and an IAM Credentials `generateAccessToken` endpoint,
   * a metadata server (`Metadata-Flavor` checks),
   * a minimal GCS XML API (GET with Range, HEAD, PUT, list-objects-v2, multipart upload) that rejects requests whose
     bearer token is unknown or expired.

   Credential fixtures point `token_uri` / `token_url` / `service_account_impersonation_url` at the mock; the secret
   uses `ENDPOINT 'localhost:<port>'`, `USE_SSL false`. Tests cover read, glob, write, and **refresh**: the mock issues
   tokens with a 2-second lifetime, and a query after expiry must succeed through httpfs's refresh path.
   Tests that need the mock are guarded by `require-env GCS_ADC_MOCK_PORT` and run via `make test_mock`, which starts
   and stops the server.
3. **Optional live test** (`require-env GCS_ADC_LIVE_BUCKET`): read/write a real bucket with real ADC. Not in CI by
   default.

## 11. Build and distribution

* Built with `extension-ci-tools` (`_extension_distribution.yml@v2.0-cyanoptera`), DuckDB `v2.0.1`.
* vcpkg dependency: `openssl` only (httpfs brings its own curl).
* Excluded platforms: `wasm_mvp;wasm_eh;wasm_threads` (see non-goals).
* Distribution target: DuckDB Community Extensions (`INSTALL gcs_adc FROM community`), via a descriptor PR to
  `duckdb/community-extensions` once v1 is complete. Until then, users build from source or download CI artifacts and
  `LOAD` with `allow_unsigned_extensions`.

## 12. Roadmap

| Version | Scope |
| --- | --- |
| v0.1 | Scaffold; `authorized_user` and `service_account` via `env` / `gcloud` / `CREDENTIALS_FILE`; secret provider with refresh; mock tests. |
| v0.2 | Metadata server; impersonation (option and file type); `gcs_adc_credential_info()`; token cache. |
| v0.3 | `external_account` (file and URL sources); quota project; Windows paths. |
| v1.0 | Docs, live test, community-extensions submission. |
| later | `external_account` AWS and executable sources; optional auto-created default secret on `LOAD`. |

## 13. Open questions

* Should `LOAD gcs_adc` create a default temporary `gcs` secret automatically when ADC is found? Convenient, but
  surprising if a user also has HMAC settings. v1: no, explicit `CREATE SECRET` only.
* Proactive refresh (re-mint before expiry) would avoid one failed request per hour, but httpfs has no hook for it
  today; the 401-then-refresh path is sufficient.
