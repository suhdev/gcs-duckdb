# Diagnostics

## `gcs_adc_credential_info()`

Shows which identity a secret would get, without creating one:

```sql
SELECT * FROM gcs_adc_credential_info();
```

It runs the same resolution as `CREATE SECRET (TYPE gcs, PROVIDER adc)`, mints a token, and returns one row:

| Column | Type | Meaning |
| --- | --- | --- |
| `credential_source` | `VARCHAR` | `env`, `gcloud`, `metadata` or `file` |
| `credential_type` | `VARCHAR` | `authorized_user`, `service_account`, `impersonated_service_account`, `external_account`, `gce_metadata` |
| `credentials_path` | `VARCHAR` | The file used; `NULL` for the metadata server |
| `principal` | `VARCHAR` | The service account the token is for; `NULL` for user logins |
| `project_id` | `VARCHAR` | From the credential file, when present |
| `quota_project_id` | `VARCHAR` | From the credential file, when present |
| `scopes` | `VARCHAR[]` | Scopes requested (empty for user logins) |
| `token_expires_at` | `TIMESTAMP WITH TIME ZONE` | When the minted token expires |
| `attempts` | `VARCHAR[]` | One line for each chain step that was tried and skipped |

It never returns the token.

It takes the same options as the secret provider, in lower case:

```sql
SELECT principal, token_expires_at
FROM gcs_adc_credential_info(
    chain := 'metadata',
    oauth_scopes := ['https://www.googleapis.com/auth/devstorage.read_only']
);

SELECT * FROM gcs_adc_credential_info(credentials_file := '~/keys/etl.json');

SELECT principal FROM gcs_adc_credential_info(
    impersonate_service_account := 'etl@my-project.iam.gserviceaccount.com',
    delegates := ['hop@my-project.iam.gserviceaccount.com']
);
```

When nothing resolves, it raises the same error `CREATE SECRET` would, listing every attempt.

## `duckdb_secrets()`

After creating a secret, see where its token came from and when it expires:

```sql
SELECT name, scope, secret_string FROM duckdb_secrets() WHERE provider = 'adc';
```

## HTTP logs

DuckDB's logging shows every HTTP request, including the token requests `gcs_adc` makes. Header values are redacted
by default.

```sql
CALL enable_logging('HTTP');
SELECT * FROM 'gs://my-bucket/file.parquet' LIMIT 1;
SELECT request.url, response.status FROM duckdb_logs_parsed('HTTP');
```

## `gcs_adc_token_mint_count()`

Returns how many tokens this process has minted, not counting cache hits. It exists for the token-cache tests, and
can also tell you whether a workload is re-minting more often than you expect.
