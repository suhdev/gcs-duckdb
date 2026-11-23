# Settings

| Setting | Type | Default | Meaning |
| --- | --- | --- | --- |
| `gcs_adc_metadata_timeout_ms` | `UBIGINT` | `500` | How long to wait for the metadata server before the `metadata` step is skipped. Raise it if a slow metadata server is being skipped on Google Cloud; lower it to speed up the chain off Google Cloud (or set `NO_GCE_CHECK=true`). |
| `gcs_adc_token_timeout_ms` | `UBIGINT` | `10000` | Timeout for each OAuth2, STS and IAM Credentials request. Failed requests are retried twice. |
| `gcs_adc_iam_endpoint` | `VARCHAR` | `https://iamcredentials.googleapis.com` | Base URL of the IAM Credentials API, used by `IMPERSONATE_SERVICE_ACCOUNT`. Change it only for private endpoints or testing. Impersonation URLs inside credential files are used as they are. |

```sql
SET gcs_adc_metadata_timeout_ms = 2000;
```

The settings apply when a secret is created or refreshed.

## Relevant httpfs settings

| Setting | Effect on `gcs_adc` |
| --- | --- |
| `httpfs_enable_credential_refresh` (default `true`) | When `false`, httpfs never re-runs the provider after a 401/403, so tokens are not refreshed. |
| `http_proxy`, `http_proxy_username`, `http_proxy_password` | Also used for token requests. |
| `ca_cert_file` | CA bundle used for token requests too. |
