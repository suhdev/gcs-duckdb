# Troubleshooting

Start with:

```sql
SELECT * FROM gcs_adc_credential_info();
```

It shows which credential was found, for whom, and which sources were skipped.

## "could not find Application Default Credentials"

None of the chain steps found anything. The error lists what was checked. Fix it with one of:

* `gcloud auth application-default login`
* `export GOOGLE_APPLICATION_CREDENTIALS=/path/to/key.json`
* `CREATE SECRET (TYPE gcs, PROVIDER adc, CREDENTIALS_FILE '/path/to/key.json')`

On Google Cloud compute, a skipped `metadata` step usually means the probe timed out. Raise
`gcs_adc_metadata_timeout_ms`.

## "could not open credential file"

`GOOGLE_APPLICATION_CREDENTIALS` or `CREDENTIALS_FILE` points at a file that doesn't exist or can't be read. When the
environment variable is set, a missing file is always an error and gcloud is not tried as a fallback. Unset the
variable to use your gcloud login.

Credential files are read through DuckDB's file system, so settings that restrict file access, such as
`allowed_directories` with `enable_external_access = false`, apply to them too.

## `400 invalid_grant: Token has been expired or revoked`

Your gcloud login expired, was revoked, or your password changed. Run `gcloud auth application-default login` again.
For service account keys, the key was deleted or disabled.

## `401` / `403` from GCS

* The identity lacks a role on the bucket, for example `roles/storage.objectViewer` to read or
  `roles/storage.objectUser` to write. Check with `gcs_adc_credential_info()` which principal you are.
* `OAUTH_SCOPES` is narrower than the operation, for example `devstorage.read_only` while writing.
* With `USE_QUOTA_PROJECT true` or `USER_PROJECT`, the identity also needs `serviceusage.services.use` on that
  project.
* `REFRESH 'disabled'` and the token is more than an hour old.

## `403 PERMISSION_DENIED` on impersonation

The source identity needs `roles/iam.serviceAccountTokenCreator` on the target service account. With `DELEGATES`, each
account in the chain needs it on the next.

## "uses a 'aws' credential source, which is not supported yet"

The Workload Identity Federation file uses the AWS or executable token source. Only `file` and `url` sources are
supported for now.

## "gcs_adc requires the httpfs extension"

Token requests use POST, which needs httpfs's HTTP client. Run `INSTALL httpfs; LOAD httpfs;`, or enable
autoloading.

## Secrets created but queries are unauthenticated

Check that the path is inside the secret's `SCOPE` and that no other `gcs` secret has a longer matching scope:

```sql
SELECT name, provider, scope FROM duckdb_secrets() WHERE type = 'gcs';
SELECT * FROM which_secret('gs://my-bucket/file.parquet', 'gcs');
```
