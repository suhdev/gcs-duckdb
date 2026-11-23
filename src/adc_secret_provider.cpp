#include "adc_secret_provider.hpp"

#include "adc/credential_chain.hpp"
#include "adc/token_minter.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/secret/secret.hpp"

namespace duckdb {
namespace gcs_adc {

static constexpr const char *GCS_SECRET_TYPE = "gcs";
static constexpr const char *ADC_PROVIDER = "adc";

string FormatExpiry(int64_t expires_at) {
	return Timestamp::ToString(Timestamp::FromEpochSeconds(expires_at)) + "Z";
}

AdcOptions BuildAdcOptions(ClientContext &context, const string &chain, const string &credentials_file,
                           const string &impersonate_service_account, const vector<string> &delegates,
                           const vector<string> &scopes) {
	AdcOptions options;
	ApplySettings(context, options);
	if (!chain.empty() && !credentials_file.empty()) {
		throw InvalidInputException("gcs_adc: CHAIN and CREDENTIALS_FILE cannot be combined");
	}
	options.chain = chain.empty() ? AdcOptions::DefaultChain() : AdcOptions::ParseChain(chain);
	options.credentials_file = credentials_file;
	options.impersonate_service_account = impersonate_service_account;
	if (!delegates.empty() && impersonate_service_account.empty()) {
		throw InvalidInputException("gcs_adc: DELEGATES requires IMPERSONATE_SERVICE_ACCOUNT");
	}
	options.delegates = delegates;
	options.scopes = scopes.empty() ? AdcOptions::DefaultScopes() : scopes;
	return options;
}

static vector<string> ListToStrings(const string &name, const Value &value) {
	if (value.IsNull()) {
		return {};
	}
	if (value.type().id() == LogicalTypeId::VARCHAR) {
		return {value.ToString()};
	}
	if (value.type().id() != LogicalTypeId::LIST) {
		throw InvalidInputException("gcs_adc: %s must be a list of strings", name);
	}
	vector<string> result;
	for (auto &child : ListValue::GetChildren(value)) {
		if (!child.IsNull()) {
			result.push_back(child.ToString());
		}
	}
	return result;
}

static bool GetBool(const string &name, const Value &value) {
	if (value.type().id() != LogicalTypeId::BOOLEAN) {
		throw InvalidInputException("gcs_adc: %s must be a BOOLEAN, found '%s'", name, value.type().ToString());
	}
	return value.GetValue<bool>();
}

//! Options that are copied unchanged into the httpfs gcs secret
static bool IsPassThroughOption(const string &name) {
	static const case_insensitive_set_t PASS_THROUGH = {"endpoint",
	                                                    "url_style",
	                                                    "use_ssl",
	                                                    "verify_ssl",
	                                                    "user_project",
	                                                    "requester_pays",
	                                                    "url_compatibility_mode",
	                                                    "http_proxy",
	                                                    "http_proxy_username",
	                                                    "http_proxy_password",
	                                                    "extra_http_headers"};
	return PASS_THROUGH.find(name) != PASS_THROUGH.end();
}

static unique_ptr<BaseSecret> CreateAdcSecret(ClientContext &context, CreateSecretInput &input) {
	string chain;
	string credentials_file;
	string impersonate;
	vector<string> delegates;
	vector<string> scopes;
	bool use_quota_project = false;
	string validation = "exists";
	string refresh = "auto";

	auto scope = input.scope.empty() ? vector<string> {"gcs://", "gs://"} : input.scope;
	auto secret = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);
	secret->redact_keys = {"bearer_token", "refresh_info", "http_proxy_password"};

	bool has_user_project = false;
	for (auto &option : input.options) {
		auto name = StringUtil::Lower(option.first);
		auto &value = option.second;
		if (name == "chain") {
			chain = value.ToString();
		} else if (name == "credentials_file") {
			credentials_file = value.ToString();
		} else if (name == "impersonate_service_account") {
			impersonate = value.ToString();
		} else if (name == "delegates") {
			delegates = ListToStrings("DELEGATES", value);
		} else if (name == "oauth_scopes") {
			scopes = ListToStrings("OAUTH_SCOPES", value);
		} else if (name == "use_quota_project") {
			use_quota_project = GetBool("USE_QUOTA_PROJECT", value);
		} else if (name == "validation") {
			validation = StringUtil::Lower(value.ToString());
			if (validation != "exists" && validation != "none") {
				throw InvalidInputException("gcs_adc: VALIDATION must be 'exists' or 'none', found '%s'",
				                            value.ToString());
			}
		} else if (name == "refresh") {
			refresh = StringUtil::Lower(value.ToString());
			if (refresh != "auto" && refresh != "disabled") {
				throw InvalidInputException("gcs_adc: REFRESH must be 'auto' or 'disabled', found '%s'",
				                            value.ToString());
			}
		} else if (IsPassThroughOption(name)) {
			if (name == "use_ssl" || name == "verify_ssl" || name == "requester_pays" ||
			    name == "url_compatibility_mode") {
				secret->secret_map[Identifier(name)] = Value::BOOLEAN(GetBool(StringUtil::Upper(name), value));
			} else if (name == "url_style") {
				secret->secret_map[Identifier(name)] = StringUtil::Lower(value.ToString());
			} else if (name == "extra_http_headers") {
				secret->secret_map[Identifier(name)] = value;
			} else {
				secret->secret_map[Identifier(name)] = value.ToString();
			}
			if (name == "user_project") {
				has_user_project = true;
			}
		} else {
			throw InvalidInputException("gcs_adc: unknown parameter '%s' for PROVIDER adc", option.first);
		}
	}

	auto options = BuildAdcOptions(context, chain, credentials_file, impersonate, delegates, scopes);

	if (refresh == "auto") {
		// httpfs re-runs CREATE OR REPLACE SECRET with these options when a request fails authentication
		child_list_t<Value> refresh_fields;
		for (auto &option : input.options) {
			refresh_fields.emplace_back(StringUtil::Lower(option.first), option.second);
		}
		bool has_refresh = false;
		for (auto &field : refresh_fields) {
			has_refresh |= field.first == "refresh";
		}
		if (!has_refresh) {
			refresh_fields.emplace_back("refresh", Value("auto"));
		}
		secret->secret_map["refresh"] = Value("auto");
		secret->secret_map["refresh_info"] = Value::STRUCT(refresh_fields);
	}

	// A replacing create is either the user forcing new credentials or httpfs refreshing after a 401/403: in both
	// cases the cached token is not good enough
	bool force_refresh = input.on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT;
	try {
		auto resolved = ResolveCredential(context, options);
		auto token = GetAccessToken(context, resolved, options, force_refresh);
		auto &cred = *resolved.credential;
		secret->secret_map["bearer_token"] = token.token;
		secret->secret_map["credential_source"] = resolved.source;
		secret->secret_map["credential_type"] = CredentialTypeToString(cred.type);
		secret->secret_map["principal"] = token.principal;
		secret->secret_map["token_expires_at"] = FormatExpiry(token.expires_at);
		if (use_quota_project && !has_user_project && !cred.quota_project_id.empty()) {
			secret->secret_map["user_project"] = cred.quota_project_id;
		}
	} catch (std::exception &ex) {
		if (validation == "exists") {
			throw;
		}
		// VALIDATION 'none': keep the secret; the first refresh mints the token
		secret->secret_map["credential_source"] = "";
	}
	return std::move(secret);
}

void RegisterAdcSecretProvider(ExtensionLoader &loader) {
	CreateSecretFunction function = {GCS_SECRET_TYPE, ADC_PROVIDER, CreateAdcSecret};
	auto &params = function.named_parameters;
	params["chain"] = LogicalType::VARCHAR;
	params["credentials_file"] = LogicalType::VARCHAR;
	params["impersonate_service_account"] = LogicalType::VARCHAR;
	params["delegates"] = LogicalType::LIST(LogicalType::VARCHAR);
	params["oauth_scopes"] = LogicalType::LIST(LogicalType::VARCHAR);
	params["use_quota_project"] = LogicalType::BOOLEAN;
	params["validation"] = LogicalType::VARCHAR;
	params["refresh"] = LogicalType::VARCHAR;
	// Passed through to the httpfs gcs secret
	params["endpoint"] = LogicalType::VARCHAR;
	params["url_style"] = LogicalType::VARCHAR;
	params["use_ssl"] = LogicalType::BOOLEAN;
	params["verify_ssl"] = LogicalType::BOOLEAN;
	params["user_project"] = LogicalType::VARCHAR;
	params["requester_pays"] = LogicalType::BOOLEAN;
	params["url_compatibility_mode"] = LogicalType::BOOLEAN;
	params["http_proxy"] = LogicalType::VARCHAR;
	params["http_proxy_username"] = LogicalType::VARCHAR;
	params["http_proxy_password"] = LogicalType::VARCHAR;
	params["extra_http_headers"] = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);
	loader.RegisterFunction(function);
}

} // namespace gcs_adc
} // namespace duckdb
