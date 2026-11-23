# How it works

## A credential provider, not a filesystem

DuckDB's `httpfs` extension already implements GCS. It owns the `gs://` and `gcs://` schemes and talks to the GCS
**XML API**, which is S3-compatible: ranged reads, bucket listing for globs, multipart uploads for writes, retries and
connection reuse. Its `gcs` secret type accepts either HMAC keys or a `BEARER_TOKEN`.

`gcs_adc` does no storage I/O. It registers one more **provider**, `adc`, for httpfs's `gcs` secret type. That is the
same pattern the official `aws` extension uses to add a `credential_chain` provider to the `s3` type.

```text
CREATE SECRET (TYPE gcs, PROVIDER adc)
        │
        ▼
 gcs_adc ── resolve the chain ──► credential (user login, key, metadata, impersonation, WIF)
        │
        ├── mint an access token (OAuth2 / IAM Credentials / STS / metadata server)
        ▼
 gcs secret { bearer_token, refresh_info, credential_source, principal, token_expires_at, … }
        │
        ▼
 httpfs ── Authorization: Bearer … ──► storage.googleapis.com (XML API)
```

Doing it this way gives three things:

* Every httpfs feature (globbing, Parquet/CSV/JSON readers, `COPY … TO`, requester-pays, proxies) works unchanged.
* There is no second filesystem competing with httpfs for `gs://`.
* The extension is small: the code is credential discovery, token minting and refresh.

The provider is called `adc` rather than `credential_chain` because the `aws` extension already registers
`credential_chain` on the `gcs` type, and two providers with the same name would depend on load order.

## Token refresh

Access tokens last about an hour. httpfs has a built-in refresh protocol: if a secret holds a `refresh_info` struct and
a request fails with **401 or 403**, httpfs runs

```sql
CREATE OR REPLACE SECRET <same name> (TYPE gcs, PROVIDER adc, <options from refresh_info>)
```

and retries the request with the new secret. `gcs_adc` stores its own creation options in `refresh_info`, so the
re-run resolves the same credential and mints a new token. This covers expiry, revoked tokens, and secrets that were
persisted and loaded again after a restart.

httpfs serializes refreshes, so many threads failing at once trigger one re-mint. Set
`httpfs_enable_credential_refresh = false` or `REFRESH 'disabled'` to turn refresh off.

## Token cache

Minted tokens are cached for the whole process. The cache key is the credential (file path and modification time, or
metadata host), the scopes, and the impersonation target. A plain `CREATE SECRET` reuses a cached token with at least
5 minutes left. `CREATE OR REPLACE` (and so every httpfs refresh) always mints a new one, so a token GCS has just
rejected is never handed back. One credential mints one token at a time, and concurrent callers wait for it and share
it.

## Token flows

| Credential | Request |
| --- | --- |
| `authorized_user` | `POST token_uri` with `grant_type=refresh_token` |
| `service_account` | RS256 JWT signed with OpenSSL, then `POST token_uri` with `grant_type=urn:ietf:params:oauth:grant-type:jwt-bearer` |
| metadata server | `GET http://metadata.google.internal/computeMetadata/v1/instance/service-accounts/default/token?scopes=…` |
| impersonation | `POST https://iamcredentials.googleapis.com/v1/projects/-/serviceAccounts/{sa}:generateAccessToken` with the source token |
| `external_account` | read the subject token, then `POST token_url` (STS token exchange), then impersonate if configured |

Every request goes through DuckDB's `HTTPUtil`, which httpfs provides, so DuckDB's proxy, CA bundle and HTTP logging
settings apply. Token requests never carry the storage-only `EXTRA_HTTP_HEADERS`.

## Security

* Tokens, refresh tokens, client secrets and private keys never appear in errors, logs, `duckdb_secrets()` or
  `gcs_adc_credential_info()`. Error messages include the endpoint, the HTTP status and the OAuth/IAM `error` fields,
  and never request bodies.
* Temporary buffers holding secrets (request bodies, JWT assertions, intermediate tokens) are overwritten after use.
* The default scope is `devstorage.read_write`, not `cloud-platform`, so a leaked token can't reach other Google
  APIs. Impersonation and federation source tokens use `cloud-platform` only to call IAM/STS, and those tokens are
  discarded right away.
* TLS verification is always on for Google endpoints. Only the metadata server uses plain HTTP, as every Google client
  library does, because it is link-local.
* Persistent secrets store a live token on disk (see [secrets](guide/secrets.md#persistent-secrets)).

The full design is in the [specification](SPEC.md).
