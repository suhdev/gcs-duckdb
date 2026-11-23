#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace gcs_adc {

//! The kinds of credential a chain step can produce (spec section 5.2)
enum class CredentialType : uint8_t {
	AUTHORIZED_USER,
	SERVICE_ACCOUNT,
	IMPERSONATED_SERVICE_ACCOUNT,
	EXTERNAL_ACCOUNT,
	GCE_METADATA
};

string CredentialTypeToString(CredentialType type);

//! Where an external_account subject token comes from
struct ExternalCredentialSource {
	//! Path of a file holding the subject token (credential_source.file)
	string file;
	//! URL returning the subject token (credential_source.url)
	string url;
	//! Headers to send with the url request
	case_insensitive_map_t<string> headers;
	//! "text" (default) or "json"
	string format_type = "text";
	//! The JSON field holding the token when format_type is "json"
	string subject_token_field_name;
	//! Set when the file uses a source this version does not support (aws, executable)
	string unsupported_source;
};

//! A parsed credential, either from a JSON file or the metadata server
struct Credential {
	CredentialType type = CredentialType::AUTHORIZED_USER;
	//! The file it was read from; empty for the metadata server
	string path;
	//! When path is set: the file's modification time, used as part of the token cache key
	int64_t path_mtime = 0;

	string project_id;
	string quota_project_id;

	// authorized_user
	string client_id;
	string client_secret;
	string refresh_token;

	// service_account
	string client_email;
	string private_key;
	string private_key_id;

	//! Token endpoint: authorized_user/service_account token_uri, external_account token_url
	string token_uri;

	// impersonated_service_account (also external_account's optional impersonation)
	string service_account_impersonation_url;
	vector<string> delegates;
	shared_ptr<Credential> source_credentials;

	// external_account
	string audience;
	string subject_token_type;
	string workforce_pool_user_project;
	ExternalCredentialSource credential_source;

	// gce metadata
	string metadata_host;
};

//! The options a CREATE SECRET (PROVIDER adc) or gcs_adc_credential_info() call resolves with
struct AdcOptions {
	//! The chain steps to try in order: env, gcloud, metadata (or just file, when credentials_file is set)
	vector<string> chain;
	string credentials_file;
	string impersonate_service_account;
	vector<string> delegates;
	vector<string> scopes;
	uint64_t metadata_timeout_ms = 500;
	uint64_t token_timeout_ms = 10000;
	//! Base URL of the IAM Credentials API (overridable for testing)
	string iam_endpoint = "https://iamcredentials.googleapis.com";

	static vector<string> DefaultChain();
	static vector<string> DefaultScopes();
	//! Parses and validates a ';'-separated chain
	static vector<string> ParseChain(const string &chain);
};

//! The outcome of running the chain
struct ResolvedCredential {
	//! The chain step that produced it: env, gcloud, metadata or file
	string source;
	shared_ptr<Credential> credential;
	//! One line per step that was tried and skipped
	vector<string> attempts;
};

//! A minted OAuth2 access token. The token string is secret material and must never be logged.
struct AccessToken {
	string token;
	//! Expiry, in seconds since the Unix epoch
	int64_t expires_at = 0;
	//! The identity the token is for, when known (service account email)
	string principal;
	//! The scopes the token was requested with (empty for authorized_user)
	vector<string> scopes;
};

} // namespace gcs_adc
} // namespace duckdb
