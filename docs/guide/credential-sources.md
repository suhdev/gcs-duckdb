# Credential sources

`CREATE SECRET (TYPE gcs, PROVIDER adc)` looks for credentials the way `google.auth.default()` and `gcloud` do. It
walks a **chain** of sources and uses the first one that exists.

## The default chain

| Step | Looks at | If it isn't there |
| --- | --- | --- |
| `env` | The file named by `GOOGLE_APPLICATION_CREDENTIALS` | Skipped when the variable is unset. If the variable is set but the file is missing or invalid, that is an **error**, so a typo can't quietly switch you to another identity. |
| `gcloud` | `$CLOUDSDK_CONFIG/application_default_credentials.json`, else `~/.config/gcloud/application_default_credentials.json` (Linux and macOS) or `%APPDATA%\gcloud\application_default_credentials.json` (Windows) | Skipped when the file doesn't exist. If it exists but is invalid, that is an error. |
| `metadata` | The metadata server at `metadata.google.internal` (then `169.254.169.254`) | Skipped when it doesn't answer within `gcs_adc_metadata_timeout_ms` (500 ms by default), or when `NO_GCE_CHECK=true`. |

If nothing is found, the error lists every step and why it was skipped:

```text
IO Error: gcs_adc: could not find Application Default Credentials. Tried:
  - env: GOOGLE_APPLICATION_CREDENTIALS is not set
  - gcloud: /home/me/.config/gcloud/application_default_credentials.json does not exist
  - metadata: metadata.google.internal did not respond within 500 ms; 169.254.169.254 did not respond within 500 ms
Run 'gcloud auth application-default login', set GOOGLE_APPLICATION_CREDENTIALS, or pass CREDENTIALS_FILE.
```

## Choosing the sources

Use `CHAIN` to pick which steps run, and in what order:

```sql
-- Only the metadata server (e.g. inside GKE, so a stray key file is never used)
CREATE SECRET (TYPE gcs, PROVIDER adc, CHAIN 'metadata');

-- gcloud first, then the environment variable
CREATE SECRET (TYPE gcs, PROVIDER adc, CHAIN 'gcloud;env');
```

Or skip the chain entirely and name a file with `CREDENTIALS_FILE`. A leading `~` is expanded.

```sql
CREATE SECRET (TYPE gcs, PROVIDER adc, CREDENTIALS_FILE '~/keys/etl-reader.json');
```

`CHAIN` and `CREDENTIALS_FILE` can't be used together.

## Credential file types

The `type` field of the JSON file decides how a token is obtained.

### `authorized_user`: your gcloud login

Written by:

```sh
gcloud auth application-default login
```

The file holds an OAuth client ID, client secret and refresh token. `gcs_adc` exchanges the refresh token for an
access token at `https://oauth2.googleapis.com/token`.

!!! note "Scopes"
    A user login keeps the scopes you consented to at login, so `OAUTH_SCOPES` is ignored for this type. The default
    `gcloud` login includes `cloud-platform`, which covers GCS.

### `service_account`: a key file

Created in the console or with:

```sh
gcloud iam service-accounts keys create key.json --iam-account=reader@my-project.iam.gserviceaccount.com
```

`gcs_adc` signs a short-lived RS256 JWT with the private key and exchanges it for an access token with the
`jwt-bearer` grant. The token gets the scopes in `OAUTH_SCOPES` (default `devstorage.read_write`).

### `impersonated_service_account`

Written by:

```sh
gcloud auth application-default login --impersonate-service-account=etl@my-project.iam.gserviceaccount.com
```

`gcs_adc` gets a token for the source credential in the file (a user login or a key), then calls the IAM Credentials
`generateAccessToken` API for the target account. The source identity needs
`roles/iam.serviceAccountTokenCreator` on the target.

### `external_account`: Workload Identity Federation

For workloads outside Google Cloud that hold an OIDC or SAML token, such as GitHub Actions, Azure, Kubernetes, or any
other OIDC provider. Create the configuration with:

```sh
gcloud iam workload-identity-pools create-cred-config \
  projects/123/locations/global/workloadIdentityPools/my-pool/providers/my-provider \
  --service-account=etl@my-project.iam.gserviceaccount.com \
  --credential-source-file=/var/run/secrets/token \
  --output-file=wif.json
```

Supported subject token sources:

| `credential_source` | Supported |
| --- | --- |
| `file`, as plain text or as JSON (`format.type: json`, `subject_token_field_name`) | Yes |
| `url` with optional `headers`, as plain text or JSON | Yes |
| `aws` (`environment_id: aws1`) | Not yet |
| `executable` | Not yet |

`gcs_adc` reads the subject token, exchanges it at the STS endpoint (`token_url`), and, when
`service_account_impersonation_url` is present, impersonates that service account with the STS token.
`workforce_pool_user_project` is sent for workforce pools.

## The metadata server

On Google Cloud compute (GCE, GKE, Cloud Run, Cloud Functions, App Engine, Dataproc, Vertex AI) the metadata server
issues tokens for the attached service account. On GKE with Workload Identity this is the Kubernetes service
account's mapped Google service account.

`gcs_adc` probes `http://metadata.google.internal/computeMetadata/v1/instance/service-accounts/default/email` with
`Metadata-Flavor: Google`. It only accepts responses that carry the same header, then fetches
`.../default/token?scopes=...`.

| Environment variable | Effect |
| --- | --- |
| `GCE_METADATA_HOST` | Use this `host[:port]` instead of `metadata.google.internal` / `169.254.169.254`. |
| `NO_GCE_CHECK=true` | Never probe the metadata server. Useful on machines where the probe would be slow. |

## Impersonating on top of any source

`IMPERSONATE_SERVICE_ACCOUNT` works with every source. The found credential gets a `cloud-platform` token, which is
then exchanged for a token of the target account:

```sql
CREATE SECRET (
    TYPE gcs, PROVIDER adc,
    IMPERSONATE_SERVICE_ACCOUNT 'etl@my-project.iam.gserviceaccount.com',
    DELEGATES ['hop@my-project.iam.gserviceaccount.com']   -- optional delegation chain
);
```

## Environment variables, summarized

| Variable | Used by |
| --- | --- |
| `GOOGLE_APPLICATION_CREDENTIALS` | `env` step |
| `CLOUDSDK_CONFIG` | `gcloud` step |
| `HOME` / `APPDATA` | `gcloud` step default path |
| `GCE_METADATA_HOST` | `metadata` step |
| `NO_GCE_CHECK` | `metadata` step |
