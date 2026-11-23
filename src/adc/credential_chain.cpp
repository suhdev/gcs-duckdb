#include "adc/credential_chain.hpp"

#include "adc/credential_file.hpp"
#include "adc/http_client.hpp"
#include "adc/token_minter.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/client_context.hpp"

#include <atomic>
#include <cstdlib>
#include <mutex>

namespace duckdb {
namespace gcs_adc {

static constexpr const char *DEFAULT_SCOPE = "https://www.googleapis.com/auth/devstorage.read_write";
static constexpr const char *DEFAULT_METADATA_HOST = "metadata.google.internal";
static constexpr const char *FALLBACK_METADATA_HOST = "169.254.169.254";
//! A cached token is reused only while it has at least this much validity left (spec section 5.5)
static constexpr int64_t MIN_TOKEN_VALIDITY_SECONDS = 300;

vector<string> AdcOptions::DefaultChain() {
	return {"env", "gcloud", "metadata"};
}

vector<string> AdcOptions::DefaultScopes() {
	return {DEFAULT_SCOPE};
}

vector<string> AdcOptions::ParseChain(const string &chain) {
	vector<string> result;
	for (auto &step : StringUtil::Split(chain, ';')) {
		auto name = StringUtil::Lower(step);
		StringUtil::Trim(name);
		if (name.empty()) {
			continue;
		}
		if (name != "env" && name != "gcloud" && name != "metadata") {
			throw InvalidInputException("gcs_adc: unknown CHAIN step '%s' (expected env, gcloud or metadata)", step);
		}
		if (std::find(result.begin(), result.end(), name) != result.end()) {
			throw InvalidInputException("gcs_adc: CHAIN step '%s' is listed more than once", name);
		}
		result.push_back(name);
	}
	if (result.empty()) {
		throw InvalidInputException("gcs_adc: CHAIN must name at least one of env, gcloud, metadata");
	}
	return result;
}

static string GetEnv(const char *name) {
	auto value = std::getenv(name);
	return value ? string(value) : string();
}

void ApplySettings(ClientContext &context, AdcOptions &options) {
	Value value;
	if (context.TryGetCurrentSetting("gcs_adc_metadata_timeout_ms", value) && !value.IsNull()) {
		options.metadata_timeout_ms = value.GetValue<uint64_t>();
	}
	if (context.TryGetCurrentSetting("gcs_adc_token_timeout_ms", value) && !value.IsNull()) {
		options.token_timeout_ms = value.GetValue<uint64_t>();
	}
	if (context.TryGetCurrentSetting("gcs_adc_iam_endpoint", value) && !value.IsNull()) {
		auto endpoint = value.ToString();
		if (!endpoint.empty()) {
			while (StringUtil::EndsWith(endpoint, "/")) {
				endpoint.pop_back();
			}
			options.iam_endpoint = endpoint;
		}
	}
}

string GcloudCredentialsPath() {
	static constexpr const char *FILE_NAME = "application_default_credentials.json";
	auto config_dir = GetEnv("CLOUDSDK_CONFIG");
#ifdef _WIN32
	const char *separator = "\\";
	if (config_dir.empty()) {
		auto appdata = GetEnv("APPDATA");
		if (appdata.empty()) {
			return string();
		}
		config_dir = appdata + "\\gcloud";
	}
#else
	const char *separator = "/";
	if (config_dir.empty()) {
		auto home = GetEnv("HOME");
		if (home.empty()) {
			return string();
		}
		config_dir = home + "/.config/gcloud";
	}
#endif
	return config_dir + separator + FILE_NAME;
}

static bool IsTrue(const string &value) {
	auto lower = StringUtil::Lower(value);
	return lower == "true" || lower == "1" || lower == "yes";
}

//! Probes the metadata server; returns the host that answered, or "" with a reason
static string ProbeMetadataServer(ClientContext &context, const AdcOptions &options, string &reason) {
	vector<string> hosts;
	auto configured = GetEnv("GCE_METADATA_HOST");
	if (!configured.empty()) {
		hosts.push_back(configured);
	} else {
		hosts = {DEFAULT_METADATA_HOST, FALLBACK_METADATA_HOST};
	}
	HttpRequestOptions request_options;
	request_options.timeout_ms = options.metadata_timeout_ms;
	request_options.retries = 0;
	request_options.headers["Metadata-Flavor"] = "Google";
	vector<string> failures;
	for (auto &host : hosts) {
		auto url = "http://" + host + "/computeMetadata/v1/instance/service-accounts/default/email";
		auto result = HttpGet(context, url, request_options);
		if (result.Ok()) {
			auto flavor = result.headers.find("Metadata-Flavor");
			if (flavor != result.headers.end() && flavor->second == "Google") {
				return host;
			}
			failures.push_back(host + " answered without 'Metadata-Flavor: Google'");
		} else if (result.status == 0) {
			failures.push_back(StringUtil::Format("%s did not respond within %llu ms", host,
			                                      static_cast<unsigned long long>(options.metadata_timeout_ms)));
		} else {
			failures.push_back(StringUtil::Format("%s answered HTTP %d", host, result.status));
		}
	}
	reason = StringUtil::Join(failures, "; ");
	return string();
}

ResolvedCredential ResolveCredential(ClientContext &context, const AdcOptions &options) {
	ResolvedCredential result;
	if (!options.credentials_file.empty()) {
		auto &fs = FileSystem::GetFileSystem(context);
		auto path = fs.ExpandPath(options.credentials_file);
		result.source = "file";
		result.credential = LoadCredentialFile(context, path);
		return result;
	}
	auto chain = options.chain.empty() ? AdcOptions::DefaultChain() : options.chain;
	for (auto &step : chain) {
		if (step == "env") {
			auto path = GetEnv("GOOGLE_APPLICATION_CREDENTIALS");
			if (path.empty()) {
				result.attempts.push_back("env: GOOGLE_APPLICATION_CREDENTIALS is not set");
				continue;
			}
			// Set but unreadable or invalid is an error, so a typo is not masked by a different identity
			result.source = "env";
			result.credential = LoadCredentialFile(context, path);
			return result;
		}
		if (step == "gcloud") {
			auto path = GcloudCredentialsPath();
			if (path.empty()) {
				result.attempts.push_back("gcloud: could not determine the gcloud configuration directory");
				continue;
			}
			auto &fs = FileSystem::GetFileSystem(context);
			if (!fs.FileExists(path)) {
				result.attempts.push_back("gcloud: " + path + " does not exist");
				continue;
			}
			result.source = "gcloud";
			result.credential = LoadCredentialFile(context, path);
			return result;
		}
		if (step == "metadata") {
			if (IsTrue(GetEnv("NO_GCE_CHECK"))) {
				result.attempts.push_back("metadata: skipped because NO_GCE_CHECK is set");
				continue;
			}
			string reason;
			auto host = ProbeMetadataServer(context, options, reason);
			if (host.empty()) {
				result.attempts.push_back("metadata: " + reason);
				continue;
			}
			result.source = "metadata";
			result.credential = make_shared_ptr<Credential>();
			result.credential->type = CredentialType::GCE_METADATA;
			result.credential->metadata_host = host;
			return result;
		}
		throw InternalException("gcs_adc: unknown chain step '%s'", step);
	}
	string message = "gcs_adc: could not find Application Default Credentials. Tried:";
	for (auto &attempt : result.attempts) {
		message += "\n  - " + attempt;
	}
	message += "\nRun 'gcloud auth application-default login', set GOOGLE_APPLICATION_CREDENTIALS, or pass "
	           "CREDENTIALS_FILE.";
	throw IOException(message);
}

namespace {
struct CacheEntry {
	std::mutex mint_lock;
	AccessToken token;
	bool has_token = false;
};

struct TokenCache {
	std::mutex lock;
	unordered_map<string, shared_ptr<CacheEntry>> entries;
};

TokenCache &GetTokenCache() {
	static TokenCache cache;
	return cache;
}

std::atomic<idx_t> mint_count {0};
} // namespace

static string CacheKey(const ResolvedCredential &resolved, const AdcOptions &options) {
	auto &cred = *resolved.credential;
	string key = CredentialTypeToString(cred.type) + "|" + cred.path + "|" + to_string(cred.path_mtime) + "|" +
	             cred.metadata_host + "|" + StringUtil::Join(options.scopes, " ") + "|" +
	             options.impersonate_service_account + "|" + StringUtil::Join(options.delegates, ",") + "|" +
	             options.iam_endpoint;
	return key;
}

AccessToken GetAccessToken(ClientContext &context, const ResolvedCredential &resolved, const AdcOptions &options,
                           bool force_refresh) {
	auto key = CacheKey(resolved, options);
	shared_ptr<CacheEntry> entry;
	{
		auto &cache = GetTokenCache();
		std::lock_guard<std::mutex> guard(cache.lock);
		auto &slot = cache.entries[key];
		if (!slot) {
			slot = make_shared_ptr<CacheEntry>();
		}
		entry = slot;
	}
	// One mint per credential at a time: concurrent callers wait and then reuse the fresh token
	std::lock_guard<std::mutex> guard(entry->mint_lock);
	auto now = NowEpochSeconds();
	if (!force_refresh && entry->has_token && entry->token.expires_at - now >= MIN_TOKEN_VALIDITY_SECONDS) {
		return entry->token;
	}
	auto token = MintAccessToken(context, *resolved.credential, options);
	mint_count++;
	entry->token = token;
	entry->has_token = true;
	return token;
}

idx_t TokenMintCount() {
	return mint_count.load();
}

} // namespace gcs_adc
} // namespace duckdb
