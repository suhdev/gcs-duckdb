# gcs_adc

A DuckDB extension that lets DuckDB read and write Google Cloud Storage (`gs://`) using
**Google Application Default Credentials (ADC)**: the credentials `gcloud`, `gsutil` and the Google Cloud client
libraries already use. No HMAC keys, no pasted tokens, and tokens refresh themselves.

```sql
LOAD gcs_adc;

CREATE SECRET (TYPE gcs, PROVIDER adc);

SELECT * FROM 'gs://my-bucket/events/*.parquet';
COPY (SELECT * FROM my_table) TO 'gs://my-bucket/out/my_table.parquet';
```

**Documentation: <https://suhdev.github.io/gcs-duckdb/>**

## How it works

DuckDB's `httpfs` extension already reads, globs and writes `gs://` paths, and its `gcs` secret accepts an OAuth2
bearer token. `gcs_adc` adds a credential provider, `adc`, to that secret type. It finds your credentials, exchanges
them for an access token, and hands httpfs an ordinary `gcs` secret. When the token expires and GCS answers
401/403, httpfs re-creates the secret through `gcs_adc`, which mints a new token, and the request is retried.

The full design is in [docs/SPEC.md](docs/SPEC.md).

## Where credentials come from

With no options, the provider tries these in order and uses the first that exists:

1. **`env`**: the file named by `GOOGLE_APPLICATION_CREDENTIALS`. If the variable is set but the file is missing or
   invalid, that is an error rather than a silent fallback.
2. **`gcloud`**: `~/.config/gcloud/application_default_credentials.json` (or `$CLOUDSDK_CONFIG/...`, or
   `%APPDATA%\gcloud\...` on Windows), written by `gcloud auth application-default login`.
3. **`metadata`**: the GCE metadata server, which also covers GKE Workload Identity, Cloud Run, Cloud Functions,
   App Engine, Dataproc and Vertex AI. Set `NO_GCE_CHECK=true` to skip it, or `GCE_METADATA_HOST` to point elsewhere.

Supported credential file types: `authorized_user` (gcloud user login), `service_account` (key files),
`impersonated_service_account` (`gcloud auth application-default login --impersonate-service-account`) and
`external_account` (Workload Identity Federation with `file` or `url` subject tokens; AWS and executable sources are
not supported yet).

## Usage

```sql
-- Default chain
CREATE SECRET (TYPE gcs, PROVIDER adc);

-- A specific key file
CREATE SECRET (TYPE gcs, PROVIDER adc, CREDENTIALS_FILE '~/keys/reader.json');

-- Only the metadata server
CREATE SECRET (TYPE gcs, PROVIDER adc, CHAIN 'metadata');

-- Impersonate a service account (needs roles/iam.serviceAccountTokenCreator on it)
CREATE SECRET (TYPE gcs, PROVIDER adc, IMPERSONATE_SERVICE_ACCOUNT 'etl@my-project.iam.gserviceaccount.com');

-- Read-only token, scoped to one bucket, billed to a project for requester-pays
CREATE SECRET reader (
    TYPE gcs, PROVIDER adc,
    OAUTH_SCOPES ['https://www.googleapis.com/auth/devstorage.read_only'],
    SCOPE 'gs://shared-bucket',
    USER_PROJECT 'my-billing-project'
);
```

### Options

| Option | Default | Meaning |
| --- | --- | --- |
| `CHAIN` | `'env;gcloud;metadata'` | Which sources to try, in order. |
| `CREDENTIALS_FILE` | | Use this file and nothing else (cannot be combined with `CHAIN`). |
| `IMPERSONATE_SERVICE_ACCOUNT` | | Exchange the found credential for a token of this service account. |
| `DELEGATES` | `[]` | Delegation chain for impersonation. |
| `OAUTH_SCOPES` | `['https://www.googleapis.com/auth/devstorage.read_write']` | Scopes to request. gcloud user logins keep the scopes they consented to. |
| `USE_QUOTA_PROJECT` | `false` | Use the file's `quota_project_id` as `USER_PROJECT`. |
| `VALIDATION` | `'exists'` | `'none'` creates the secret even if no token can be minted yet. |
| `REFRESH` | `'auto'` | `'disabled'` keeps the first token until it expires. |

`SCOPE`, `ENDPOINT`, `URL_STYLE`, `USE_SSL`, `VERIFY_SSL`, `USER_PROJECT`, `REQUESTER_PAYS`, `HTTP_PROXY`,
`HTTP_PROXY_USERNAME`, `HTTP_PROXY_PASSWORD` and `EXTRA_HTTP_HEADERS` are passed to the httpfs `gcs` secret unchanged.

`FROM duckdb_secrets()` shows `credential_source`, `credential_type`, `principal` and `token_expires_at`; the token
itself is always redacted.

### Checking which identity you'll get

```sql
SELECT * FROM gcs_adc_credential_info();
```

returns the source, credential type, file path, principal, project, quota project, scopes, token expiry, and one
line for every source that was tried and skipped. It never returns the token. It takes the same `chain`,
`credentials_file`, `impersonate_service_account`, `delegates` and `oauth_scopes` options.

### Settings

| Setting | Default | |
| --- | --- | --- |
| `gcs_adc_metadata_timeout_ms` | `500` | How long to wait for the metadata server before skipping it. |
| `gcs_adc_token_timeout_ms` | `10000` | Timeout for OAuth2, STS and IAM Credentials requests. |
| `gcs_adc_iam_endpoint` | `https://iamcredentials.googleapis.com` | IAM Credentials API base URL. |

### Persistent secrets

`CREATE PERSISTENT SECRET` works, but it writes the current (short-lived) access token to
`~/.duckdb/stored_secrets`. Prefer a temporary secret created at startup, for example in `~/.duckdbrc`.

## Troubleshooting

* **"could not find Application Default Credentials"**: run `gcloud auth application-default login`, set
  `GOOGLE_APPLICATION_CREDENTIALS`, or pass `CREDENTIALS_FILE`. The message lists every place that was checked.
* **`invalid_grant`**: your gcloud login expired or was revoked. Run `gcloud auth application-default login` again.
* **403 on reads with `USE_QUOTA_PROJECT`**: the identity needs `serviceusage.services.use` on the quota project.
* **Impersonation 403**: the source identity needs `roles/iam.serviceAccountTokenCreator` on the target account.

## Building

Requires CMake, Ninja, a C++17 compiler, and OpenSSL and libcurl development headers (or vcpkg).

```sh
git clone --recurse-submodules https://github.com/suhdev/gcs-duckdb
cd gcs-duckdb
GEN=ninja make release
```

The loadable extension is written to `build/release/repository/<duckdb commit>/<platform>/gcs_adc.duckdb_extension`.
Prebuilt binaries for every platform are attached to each [GitHub release](https://github.com/suhdev/gcs-duckdb/releases).
Load either with `duckdb -unsigned` and `LOAD '/path/to/gcs_adc.duckdb_extension'`. See
[Installation](https://suhdev.github.io/gcs-duckdb/installation/).

## Testing

```sh
./build/release/test/unittest "test/sql/*"   # option validation and file errors, no network
make test_mock                                # end-to-end against test/mock/mock_google.py
```

The mock server fakes the OAuth2, STS, IAM Credentials and metadata endpoints plus a small GCS XML API, so reads,
writes, globbing, token refresh, impersonation and Workload Identity Federation are all tested without a GCP account.

## Releasing

Push a `v*` tag; `.github/workflows/Release.yml` builds every platform and publishes a GitHub release. See
[Releasing](https://suhdev.github.io/gcs-duckdb/releasing/).

## Updating DuckDB

See [docs/UPDATING.md](docs/UPDATING.md).
