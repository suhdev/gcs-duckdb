# Creating secrets

```sql
CREATE [OR REPLACE] [PERSISTENT | TEMPORARY] SECRET [name] (
    TYPE gcs,
    PROVIDER adc,
    option value, ...
);
```

The result is an ordinary httpfs `gcs` secret. DuckDB uses it for any `gs://` or `gcs://` path inside its `SCOPE`.

## Options

### Credential options

| Option | Type | Default | Meaning |
| --- | --- | --- | --- |
| `CHAIN` | `VARCHAR` | `'env;gcloud;metadata'` | Sources to try, separated by `;`. See [credential sources](credential-sources.md). |
| `CREDENTIALS_FILE` | `VARCHAR` | | Use only this credential file. Cannot be combined with `CHAIN`. |
| `IMPERSONATE_SERVICE_ACCOUNT` | `VARCHAR` | | Exchange the found credential for a token of this service account. |
| `DELEGATES` | `VARCHAR[]` | `[]` | Delegation chain for impersonation. Requires `IMPERSONATE_SERVICE_ACCOUNT`. |
| `OAUTH_SCOPES` | `VARCHAR[]` | `['https://www.googleapis.com/auth/devstorage.read_write']` | OAuth scopes to request. Ignored for gcloud user logins. |
| `USE_QUOTA_PROJECT` | `BOOLEAN` | `false` | Set `USER_PROJECT` from the credential file's `quota_project_id`, unless you pass `USER_PROJECT` yourself. |
| `VALIDATION` | `VARCHAR` | `'exists'` | `'exists'`: fail now if no token can be minted. `'none'`: create the secret anyway, and mint the token on first use. |
| `REFRESH` | `VARCHAR` | `'auto'` | `'auto'`: re-mint the token when GCS rejects it. `'disabled'`: keep the first token until it expires (about 1 hour). |

!!! tip "Why `OAUTH_SCOPES` and not `SCOPE`"
    `SCOPE` is DuckDB's generic secret option for the path prefixes a secret applies to. OAuth scopes have their own
    name so the two never clash.

### httpfs options (passed through)

These go into the resulting `gcs` secret unchanged and mean exactly what they mean for httpfs:

| Option | Type | Meaning |
| --- | --- | --- |
| `SCOPE` | `VARCHAR` / `VARCHAR[]` | Path prefixes this secret applies to. Default `['gcs://', 'gs://']`. |
| `ENDPOINT` | `VARCHAR` | GCS endpoint. Default `storage.googleapis.com`. |
| `URL_STYLE` | `VARCHAR` | `path` (default for GCS) or `vhost`. |
| `USE_SSL` | `BOOLEAN` | Default `true`. |
| `VERIFY_SSL` | `BOOLEAN` | Default `true`. |
| `USER_PROJECT` | `VARCHAR` | Project billed for requester-pays buckets. |
| `REQUESTER_PAYS` | `BOOLEAN` | Mark requests as requester-pays. |
| `URL_COMPATIBILITY_MODE` | `BOOLEAN` | httpfs URL compatibility mode. |
| `HTTP_PROXY`, `HTTP_PROXY_USERNAME`, `HTTP_PROXY_PASSWORD` | `VARCHAR` | Proxy for storage requests. |
| `EXTRA_HTTP_HEADERS` | `MAP(VARCHAR, VARCHAR)` | Extra headers on storage requests. They are not sent to token endpoints. |

## Examples

### One identity for everything

```sql
CREATE SECRET (TYPE gcs, PROVIDER adc);
```

### Different identities per bucket

DuckDB picks the secret whose `SCOPE` is the longest match for the path:

```sql
CREATE SECRET analytics (TYPE gcs, PROVIDER adc,
    SCOPE 'gs://analytics-prod');

CREATE SECRET partner (TYPE gcs, PROVIDER adc,
    CREDENTIALS_FILE '~/keys/partner-reader.json',
    OAUTH_SCOPES ['https://www.googleapis.com/auth/devstorage.read_only'],
    SCOPE 'gs://partner-drop');
```

### Mixing with HMAC keys

ADC secrets and httpfs's own HMAC secrets can coexist:

```sql
CREATE SECRET legacy (TYPE gcs, KEY_ID 'GOOG1E...', SECRET '...', SCOPE 'gs://legacy-bucket');
CREATE SECRET (TYPE gcs, PROVIDER adc);
```

### Requester-pays buckets

```sql
CREATE SECRET (TYPE gcs, PROVIDER adc, USER_PROJECT 'my-billing-project', REQUESTER_PAYS true);
```

### Read-only access

```sql
CREATE SECRET (TYPE gcs, PROVIDER adc,
    OAUTH_SCOPES ['https://www.googleapis.com/auth/devstorage.read_only']);
```

The token is limited by both the scope and the identity's IAM roles. A read-only scope makes a leaked token less
dangerous even when the account itself can write.

## What the secret contains

```sql
SELECT name, provider, secret_string FROM duckdb_secrets();
```

| Key | Example | Shown |
| --- | --- | --- |
| `bearer_token` | | Always redacted |
| `refresh_info` | | Always redacted |
| `credential_source` | `env`, `gcloud`, `metadata`, `file` | Yes |
| `credential_type` | `authorized_user`, `service_account`, `impersonated_service_account`, `external_account`, `gce_metadata` | Yes |
| `principal` | `etl@my-project.iam.gserviceaccount.com` (empty for user logins) | Yes |
| `token_expires_at` | `2026-11-24 00:31:20Z` | Yes |
| httpfs options | as given | as httpfs shows them |

## Token refresh

Access tokens last about an hour. When GCS rejects a token with 401 or 403, httpfs re-runs the secret's provider, which
mints a new token, and retries the request. Long sessions and long queries keep working without you doing anything.
See [how it works](../how-it-works.md#token-refresh) for the details.

`CREATE OR REPLACE SECRET` always mints a fresh token. A plain `CREATE SECRET` reuses a cached token for the same
credential if it has at least 5 minutes left.

## Persistent secrets

`CREATE PERSISTENT SECRET` works, but it writes the current access token to `~/.duckdb/stored_secrets`. The token
expires within an hour and is refreshed on first use after a restart. Still, prefer a temporary secret created at
startup, for example in `~/.duckdbrc`:

```sql
-- ~/.duckdbrc
LOAD gcs_adc;
CREATE SECRET (TYPE gcs, PROVIDER adc);
```
