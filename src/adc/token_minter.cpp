#include "adc/token_minter.hpp"

#include "adc/credential_file.hpp"
#include "adc/encoding.hpp"
#include "adc/http_client.hpp"
#include "adc/json.hpp"
#include "adc/jwt.hpp"
#include "duckdb/common/types/timestamp.hpp"

#include <chrono>

namespace duckdb {
namespace gcs_adc {

const char *CLOUD_PLATFORM_SCOPE = "https://www.googleapis.com/auth/cloud-platform";

static constexpr const char *FORM_CONTENT_TYPE = "application/x-www-form-urlencoded";
static constexpr const char *JSON_CONTENT_TYPE = "application/json";
static constexpr int64_t DEFAULT_TOKEN_LIFETIME = 3600;

int64_t NowEpochSeconds() {
	return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
	    .count();
}

static string JoinScopes(const vector<string> &scopes, const string &separator) {
	return StringUtil::Join(scopes, separator);
}

static HttpRequestOptions TokenRequestOptions(const AdcOptions &options) {
	HttpRequestOptions result;
	result.timeout_ms = options.token_timeout_ms;
	result.retries = 2;
	return result;
}

//! Describes an error response without echoing anything that could be secret
static string DescribeError(const HttpResult &result) {
	if (result.status == 0) {
		return "no response: " + result.error;
	}
	string description = to_string(result.status);
	JsonDocument doc;
	if (doc.Parse(result.body)) {
		auto error = doc.GetString("error");
		auto error_description = doc.GetString("error_description");
		JsonDocument nested;
		if (error.empty() && doc.GetObject("error", nested)) {
			// Google API style: {"error": {"code": 403, "message": "...", "status": "PERMISSION_DENIED"}}
			error = nested.GetString("status");
			error_description = nested.GetString("message");
		}
		if (!error.empty()) {
			description += " " + error;
		}
		if (!error_description.empty()) {
			description += ": " + error_description;
		}
	}
	return description;
}

[[noreturn]] static void ThrowTokenError(const string &what, const string &url, const HttpResult &result,
                                         const string &hint) {
	throw IOException("gcs_adc: %s request to %s failed (%s).%s", what, url, DescribeError(result),
	                  hint.empty() ? string() : " " + hint);
}

//! Parses a standard OAuth2 token response ({"access_token", "expires_in"})
static AccessToken ParseOAuthTokenResponse(const string &what, const string &url, const HttpResult &result,
                                           const string &token_field = "access_token") {
	JsonDocument doc;
	if (!doc.Parse(result.body)) {
		throw IOException("gcs_adc: %s response from %s is not valid JSON", what, url);
	}
	AccessToken token;
	token.token = doc.GetString(token_field);
	if (token.token.empty()) {
		throw IOException("gcs_adc: %s response from %s has no '%s'", what, url, token_field);
	}
	token.expires_at = NowEpochSeconds() + doc.GetInt("expires_in", DEFAULT_TOKEN_LIFETIME);
	return token;
}

static AccessToken MintAuthorizedUser(ClientContext &context, const Credential &cred, const AdcOptions &options) {
	auto body = FormEncode({{"grant_type", "refresh_token"},
	                        {"client_id", cred.client_id},
	                        {"client_secret", cred.client_secret},
	                        {"refresh_token", cred.refresh_token}});
	auto result = HttpPost(context, cred.token_uri, body, FORM_CONTENT_TYPE, TokenRequestOptions(options));
	ScrubString(body);
	if (!result.Ok()) {
		ThrowTokenError("token", cred.token_uri, result,
		                result.status == 400 ? "Run 'gcloud auth application-default login' again." : "");
	}
	// authorized_user scopes were fixed when the user consented; we cannot request others
	return ParseOAuthTokenResponse("token", cred.token_uri, result);
}

static AccessToken MintServiceAccount(ClientContext &context, const Credential &cred, const vector<string> &scopes,
                                      const AdcOptions &options) {
	auto now = NowEpochSeconds();
	string header = "{\"alg\":\"RS256\",\"typ\":\"JWT\"";
	if (!cred.private_key_id.empty()) {
		header += ",\"kid\":" + JsonQuote(cred.private_key_id);
	}
	header += "}";
	string claims = "{\"iss\":" + JsonQuote(cred.client_email) + ",\"sub\":" + JsonQuote(cred.client_email) +
	                ",\"aud\":" + JsonQuote(cred.token_uri) + ",\"scope\":" + JsonQuote(JoinScopes(scopes, " ")) +
	                ",\"iat\":" + to_string(now) + ",\"exp\":" + to_string(now + DEFAULT_TOKEN_LIFETIME) + "}";
	auto assertion = CreateSignedJwt(cred.private_key, header, claims);
	auto body = FormEncode({{"grant_type", "urn:ietf:params:oauth:grant-type:jwt-bearer"}, {"assertion", assertion}});
	auto result = HttpPost(context, cred.token_uri, body, FORM_CONTENT_TYPE, TokenRequestOptions(options));
	ScrubString(body);
	ScrubString(assertion);
	if (!result.Ok()) {
		ThrowTokenError("token", cred.token_uri, result, "");
	}
	auto token = ParseOAuthTokenResponse("token", cred.token_uri, result);
	token.principal = cred.client_email;
	token.scopes = scopes;
	return token;
}

static string NormalizeDelegate(const string &delegate) {
	if (StringUtil::StartsWith(delegate, "projects/")) {
		return delegate;
	}
	return "projects/-/serviceAccounts/" + delegate;
}

string ServiceAccountFromImpersonationUrl(const string &url) {
	auto marker = string("serviceAccounts/");
	auto start = url.find(marker);
	if (start == string::npos) {
		return string();
	}
	start += marker.size();
	auto end = url.find(':', start);
	return url.substr(start, end == string::npos ? string::npos : end - start);
}

AccessToken ImpersonateServiceAccount(ClientContext &context, const string &source_token, const string &target,
                                      const vector<string> &delegates, const vector<string> &scopes,
                                      const AdcOptions &options) {
	string url;
	string email;
	if (StringUtil::StartsWith(target, "http://") || StringUtil::StartsWith(target, "https://")) {
		url = target;
		email = ServiceAccountFromImpersonationUrl(target);
	} else {
		email = target;
		url = options.iam_endpoint + "/v1/projects/-/serviceAccounts/" + UrlEncode(target) + ":generateAccessToken";
	}
	vector<string> normalized_delegates;
	for (auto &delegate : delegates) {
		normalized_delegates.push_back(NormalizeDelegate(delegate));
	}
	string body = "{\"scope\":" + JsonStringArray(scopes) + ",\"delegates\":" + JsonStringArray(normalized_delegates) +
	              ",\"lifetime\":\"3600s\"}";
	auto request_options = TokenRequestOptions(options);
	request_options.headers["Authorization"] = "Bearer " + source_token;
	auto result = HttpPost(context, url, body, JSON_CONTENT_TYPE, request_options);
	ScrubString(request_options.headers["Authorization"]);
	if (!result.Ok()) {
		ThrowTokenError("impersonation", url, result,
		                result.status == 403
		                    ? "The source identity needs roles/iam.serviceAccountTokenCreator on '" + email + "'."
		                    : "");
	}
	JsonDocument doc;
	if (!doc.Parse(result.body)) {
		throw IOException("gcs_adc: impersonation response from %s is not valid JSON", url);
	}
	AccessToken token;
	token.token = doc.GetString("accessToken");
	if (token.token.empty()) {
		throw IOException("gcs_adc: impersonation response from %s has no 'accessToken'", url);
	}
	token.expires_at = NowEpochSeconds() + DEFAULT_TOKEN_LIFETIME;
	auto expire_time = doc.GetString("expireTime");
	if (!expire_time.empty()) {
		try {
			token.expires_at = Timestamp::GetEpochSeconds(Timestamp::FromString(expire_time, true));
		} catch (...) { // NOLINT: keep the default lifetime when the timestamp is unparseable
		}
	}
	token.principal = email;
	token.scopes = scopes;
	return token;
}

static string ReadSubjectToken(ClientContext &context, const Credential &cred, const AdcOptions &options) {
	auto &source = cred.credential_source;
	if (!source.unsupported_source.empty()) {
		throw NotImplementedException(
		    "gcs_adc: external_account credential file '%s' uses a '%s' credential source, which is not supported yet "
		    "(only 'file' and 'url' sources are)",
		    cred.path, source.unsupported_source);
	}
	string raw;
	string origin;
	if (!source.file.empty()) {
		origin = source.file;
		int64_t mtime;
		raw = ReadCredentialFile(context, source.file, mtime);
	} else {
		origin = source.url;
		HttpRequestOptions request_options;
		request_options.timeout_ms = options.token_timeout_ms;
		request_options.headers = source.headers;
		auto result = HttpGet(context, source.url, request_options);
		if (!result.Ok()) {
			throw IOException("gcs_adc: fetching the external_account subject token from %s failed (%s)", source.url,
			                  DescribeError(result));
		}
		raw = std::move(result.body);
	}
	if (source.format_type == "json") {
		JsonDocument doc;
		if (!doc.Parse(raw)) {
			throw IOException("gcs_adc: the external_account subject token from %s is not a JSON object", origin);
		}
		auto token = doc.GetString(source.subject_token_field_name);
		ScrubString(raw);
		if (token.empty()) {
			throw IOException("gcs_adc: the external_account subject token from %s has no '%s' field", origin,
			                  source.subject_token_field_name);
		}
		return token;
	}
	StringUtil::Trim(raw);
	if (raw.empty()) {
		throw IOException("gcs_adc: the external_account subject token from %s is empty", origin);
	}
	return raw;
}

static AccessToken MintExternalAccount(ClientContext &context, const Credential &cred, const vector<string> &scopes,
                                       const AdcOptions &options) {
	auto subject_token = ReadSubjectToken(context, cred, options);
	bool impersonate = !cred.service_account_impersonation_url.empty();
	vector<pair<string, string>> fields = {
	    {"grant_type", "urn:ietf:params:oauth:grant-type:token-exchange"},
	    {"audience", cred.audience},
	    {"scope", impersonate ? string(CLOUD_PLATFORM_SCOPE) : JoinScopes(scopes, " ")},
	    {"requested_token_type", "urn:ietf:params:oauth:token-type:access_token"},
	    {"subject_token", subject_token},
	    {"subject_token_type", cred.subject_token_type}};
	if (!cred.workforce_pool_user_project.empty() && !impersonate) {
		fields.emplace_back("options", "{\"userProject\":" + JsonQuote(cred.workforce_pool_user_project) + "}");
	}
	auto body = FormEncode(fields);
	ScrubString(subject_token);
	auto result = HttpPost(context, cred.token_uri, body, FORM_CONTENT_TYPE, TokenRequestOptions(options));
	ScrubString(body);
	if (!result.Ok()) {
		ThrowTokenError("STS token exchange", cred.token_uri, result, "");
	}
	auto sts_token = ParseOAuthTokenResponse("STS token exchange", cred.token_uri, result);
	if (!impersonate) {
		sts_token.scopes = scopes;
		return sts_token;
	}
	auto token = ImpersonateServiceAccount(context, sts_token.token, cred.service_account_impersonation_url, {}, scopes,
	                                       options);
	ScrubString(sts_token.token);
	return token;
}

static AccessToken MintMetadata(ClientContext &context, const Credential &cred, const vector<string> &scopes,
                                const AdcOptions &options) {
	auto base = "http://" + cred.metadata_host + "/computeMetadata/v1/instance/service-accounts/default/";
	HttpRequestOptions request_options;
	request_options.timeout_ms = options.token_timeout_ms;
	request_options.headers["Metadata-Flavor"] = "Google";
	auto url = base + "token";
	if (!scopes.empty()) {
		url += "?scopes=" + UrlEncode(JoinScopes(scopes, ","));
	}
	auto result = HttpGet(context, url, request_options);
	if (!result.Ok()) {
		ThrowTokenError("metadata server token", url, result, "");
	}
	auto token = ParseOAuthTokenResponse("metadata server token", url, result);
	token.scopes = scopes;
	auto email = HttpGet(context, base + "email", request_options);
	if (email.Ok()) {
		token.principal = email.body;
		StringUtil::Trim(token.principal);
	}
	return token;
}

AccessToken MintCredentialToken(ClientContext &context, const Credential &credential, const AdcOptions &options) {
	auto &scopes = options.scopes;
	switch (credential.type) {
	case CredentialType::AUTHORIZED_USER:
		return MintAuthorizedUser(context, credential, options);
	case CredentialType::SERVICE_ACCOUNT:
		return MintServiceAccount(context, credential, scopes, options);
	case CredentialType::IMPERSONATED_SERVICE_ACCOUNT: {
		if (!credential.source_credentials) {
			throw InternalException("gcs_adc: impersonated_service_account without source credentials");
		}
		AdcOptions source_options = options;
		source_options.scopes = {CLOUD_PLATFORM_SCOPE};
		auto source = MintCredentialToken(context, *credential.source_credentials, source_options);
		auto token = ImpersonateServiceAccount(context, source.token, credential.service_account_impersonation_url,
		                                       credential.delegates, scopes, options);
		ScrubString(source.token);
		return token;
	}
	case CredentialType::EXTERNAL_ACCOUNT:
		return MintExternalAccount(context, credential, scopes, options);
	case CredentialType::GCE_METADATA:
		return MintMetadata(context, credential, scopes, options);
	default:
		throw InternalException("gcs_adc: unknown credential type");
	}
}

AccessToken MintAccessToken(ClientContext &context, const Credential &credential, const AdcOptions &options) {
	if (options.impersonate_service_account.empty()) {
		return MintCredentialToken(context, credential, options);
	}
	AdcOptions source_options = options;
	source_options.scopes = {CLOUD_PLATFORM_SCOPE};
	source_options.impersonate_service_account.clear();
	auto source = MintCredentialToken(context, credential, source_options);
	auto token = ImpersonateServiceAccount(context, source.token, options.impersonate_service_account,
	                                       options.delegates, options.scopes, options);
	ScrubString(source.token);
	return token;
}

} // namespace gcs_adc
} // namespace duckdb
