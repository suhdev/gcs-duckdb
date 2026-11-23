# gcs_adc

**Read and write Google Cloud Storage from DuckDB with Google Application Default Credentials.**

`gcs_adc` is a DuckDB extension that authenticates `gs://` and `gcs://` access with the credentials that `gcloud`,
`gsutil` and the Google Cloud client libraries already use. You don't need HMAC keys or pasted tokens, and tokens
refresh on their own.

```sql
LOAD gcs_adc;

CREATE SECRET (TYPE gcs, PROVIDER adc);

SELECT count(*) FROM 'gs://my-bucket/events/*.parquet';
COPY (SELECT * FROM my_table) TO 'gs://my-bucket/out/my_table.parquet';
```

## Why

DuckDB's `httpfs` extension can already talk to GCS, but only with:

* **HMAC keys**, which must be created and rotated for each service account, and which many organizations disable
  by policy, or
* a **bearer token** that you fetch yourself (`gcloud auth print-access-token`), which stops working after about an
  hour.

`gcs_adc` handles that for you. It finds your credentials the same way Google's own tools do, mints an access token,
and mints a new one whenever GCS rejects the old one.

## What it supports

| Where you run | Credential | Works |
| --- | --- | --- |
| Your laptop | `gcloud auth application-default login` | Yes |
| Anywhere | Service account key file (`GOOGLE_APPLICATION_CREDENTIALS`) | Yes |
| GCE, GKE (incl. Workload Identity), Cloud Run, Cloud Functions, App Engine, Dataproc, Vertex AI | Metadata server | Yes |
| Anywhere | Service account impersonation | Yes |
| AWS, Azure, GitHub Actions, any OIDC provider | Workload Identity Federation (file or URL token source) | Yes |
| AWS | Workload Identity Federation with the AWS token source | Not yet |
| Anywhere | Workload Identity Federation with an executable token source | Not yet |

## Next steps

* [Install the extension](installation.md)
* [See which credential it will use](guide/credential-sources.md)
* [Create secrets for your buckets](guide/secrets.md)
* [Learn how it fits together with httpfs](how-it-works.md)
