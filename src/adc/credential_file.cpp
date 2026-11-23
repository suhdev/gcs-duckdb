#include "adc/credential_file.hpp"

#include "adc/json.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/types/timestamp.hpp"

namespace duckdb {
namespace gcs_adc {

static constexpr const char *DEFAULT_TOKEN_URI = "https://oauth2.googleapis.com/token";
static constexpr const char *DEFAULT_STS_URI = "https://sts.googleapis.com/v1/token";

string CredentialTypeToString(CredentialType type) {
	switch (type) {
	case CredentialType::AUTHORIZED_USER:
		return "authorized_user";
	case CredentialType::SERVICE_ACCOUNT:
		return "service_account";
	case CredentialType::IMPERSONATED_SERVICE_ACCOUNT:
		return "impersonated_service_account";
	case CredentialType::EXTERNAL_ACCOUNT:
		return "external_account";
	case CredentialType::GCE_METADATA:
		return "gce_metadata";
	default:
		throw InternalException("Unknown gcs_adc credential type");
	}
}

string ReadCredentialFile(ClientContext &context, const string &path, int64_t &mtime) {
	auto &fs = FileSystem::GetFileSystem(context);
	unique_ptr<FileHandle> handle;
	try {
		handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	} catch (std::exception &ex) {
		ErrorData error(ex);
		throw IOException("gcs_adc: could not open credential file '%s': %s", path, error.RawMessage());
	}
	auto size = handle->GetFileSize();
	if (size > idx_t(1024) * 1024) {
		throw IOException("gcs_adc: credential file '%s' is larger than 1 MiB", path);
	}
	string result;
	result.resize(size);
	idx_t read = 0;
	while (read < size) {
		auto n = handle->Read(&result[read], size - read);
		if (n <= 0) {
			break;
		}
		read += NumericCast<idx_t>(n);
	}
	result.resize(read);
	try {
		mtime = Timestamp::GetEpochSeconds(fs.GetLastModifiedTime(*handle));
	} catch (...) {
		mtime = 0;
	}
	return result;
}

static string Require(const JsonDocument &doc, const string &key, const string &type, const string &path) {
	auto value = doc.GetString(key);
	if (value.empty()) {
		throw InvalidInputException("gcs_adc: %s credential file '%s' is missing the '%s' field", type, path, key);
	}
	return value;
}

static void ParseExternalSource(const JsonDocument &doc, Credential &cred, const string &path) {
	JsonDocument source;
	if (!doc.GetObject("credential_source", source)) {
		throw InvalidInputException("gcs_adc: external_account credential file '%s' is missing 'credential_source'",
		                            path);
	}
	auto &result = cred.credential_source;
	if (source.Has("environment_id") || source.Has("regional_cred_verification_url")) {
		result.unsupported_source = "aws";
		return;
	}
	if (source.Has("executable")) {
		result.unsupported_source = "executable";
		return;
	}
	result.file = source.GetString("file");
	result.url = source.GetString("url");
	if (result.file.empty() && result.url.empty()) {
		throw InvalidInputException(
		    "gcs_adc: external_account credential file '%s' has a credential_source with neither 'file' nor 'url'",
		    path);
	}
	JsonDocument headers;
	if (source.GetObject("headers", headers)) {
		result.headers = headers.GetStringMembers();
	}
	JsonDocument format;
	if (source.GetObject("format", format)) {
		auto format_type = StringUtil::Lower(format.GetString("type"));
		if (!format_type.empty()) {
			result.format_type = format_type;
		}
		result.subject_token_field_name = format.GetString("subject_token_field_name");
	}
	if (result.format_type != "text" && result.format_type != "json") {
		throw InvalidInputException("gcs_adc: external_account credential file '%s' has unsupported format type '%s'",
		                            path, result.format_type);
	}
	if (result.format_type == "json" && result.subject_token_field_name.empty()) {
		throw InvalidInputException(
		    "gcs_adc: external_account credential file '%s' uses format 'json' without 'subject_token_field_name'",
		    path);
	}
}

//! Parses the two "leaf" credential types; returns false when the type is neither
static bool TryParseLeafCredential(const JsonDocument &doc, const string &type, const string &what, const string &path,
                                   Credential &result) {
	if (type == "authorized_user") {
		result.type = CredentialType::AUTHORIZED_USER;
		result.client_id = Require(doc, "client_id", what, path);
		result.client_secret = Require(doc, "client_secret", what, path);
		result.refresh_token = Require(doc, "refresh_token", what, path);
	} else if (type == "service_account") {
		result.type = CredentialType::SERVICE_ACCOUNT;
		result.client_email = Require(doc, "client_email", what, path);
		result.private_key = Require(doc, "private_key", what, path);
		result.private_key_id = doc.GetString("private_key_id");
	} else {
		return false;
	}
	result.token_uri = doc.GetString("token_uri");
	if (result.token_uri.empty()) {
		result.token_uri = DEFAULT_TOKEN_URI;
	}
	return true;
}

shared_ptr<Credential> ParseCredentialJson(const string &json, const string &path) {
	JsonDocument doc;
	if (!doc.Parse(json)) {
		throw InvalidInputException("gcs_adc: credential file '%s' is not a valid JSON object", path);
	}
	auto result = make_shared_ptr<Credential>();
	result->path = path;
	result->quota_project_id = doc.GetString("quota_project_id");
	result->project_id = doc.GetString("project_id");

	auto type = doc.GetString("type");
	if (TryParseLeafCredential(doc, type, type, path, *result)) {
		return result;
	}
	if (type == "impersonated_service_account") {
		result->type = CredentialType::IMPERSONATED_SERVICE_ACCOUNT;
		result->service_account_impersonation_url = Require(doc, "service_account_impersonation_url", type, path);
		result->delegates = doc.GetStringArray("delegates");
		JsonDocument source;
		if (!doc.GetObject("source_credentials", source)) {
			throw InvalidInputException("gcs_adc: %s credential file '%s' is missing the 'source_credentials' field",
			                            type, path);
		}
		auto nested = make_shared_ptr<Credential>();
		nested->path = path;
		nested->quota_project_id = source.GetString("quota_project_id");
		auto source_type = source.GetString("type");
		if (!TryParseLeafCredential(source, source_type, "source_credentials", path, *nested)) {
			throw InvalidInputException(
			    "gcs_adc: credential file '%s' has source_credentials of unsupported type '%s' (expected "
			    "authorized_user or service_account)",
			    path, source_type);
		}
		result->source_credentials = std::move(nested);
	} else if (type == "external_account") {
		result->type = CredentialType::EXTERNAL_ACCOUNT;
		result->audience = Require(doc, "audience", type, path);
		result->subject_token_type = Require(doc, "subject_token_type", type, path);
		result->token_uri = doc.GetString("token_url");
		if (result->token_uri.empty()) {
			result->token_uri = DEFAULT_STS_URI;
		}
		result->service_account_impersonation_url = doc.GetString("service_account_impersonation_url");
		result->workforce_pool_user_project = doc.GetString("workforce_pool_user_project");
		ParseExternalSource(doc, *result, path);
	} else if (type.empty()) {
		throw InvalidInputException("gcs_adc: credential file '%s' has no 'type' field", path);
	} else {
		throw InvalidInputException("gcs_adc: credential file '%s' has unsupported credential type '%s'", path, type);
	}
	return result;
}

shared_ptr<Credential> LoadCredentialFile(ClientContext &context, const string &path) {
	int64_t mtime = 0;
	auto json = ReadCredentialFile(context, path, mtime);
	auto result = ParseCredentialJson(json, path);
	result->path_mtime = mtime;
	return result;
}

} // namespace gcs_adc
} // namespace duckdb
