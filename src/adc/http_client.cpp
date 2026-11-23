#include "adc/http_client.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/http/http_util.hpp"

namespace duckdb {
namespace gcs_adc {

static constexpr const char *BUILT_IN_HTTP_UTIL = "Built-In";

void EnsureHttpTransport(ClientContext &context) {
	auto &db = DatabaseInstance::GetDatabase(context);
	if (HTTPUtil::Get(db).GetName() != BUILT_IN_HTTP_UTIL) {
		return;
	}
	ExtensionHelper::TryAutoLoadExtension(context, "httpfs");
	if (HTTPUtil::Get(db).GetName() != BUILT_IN_HTTP_UTIL) {
		return;
	}
	throw InvalidConfigurationException("gcs_adc requires the httpfs extension: INSTALL httpfs; LOAD httpfs;");
}

static unique_ptr<HTTPParams> CreateParams(ClientContext &context, HTTPUtil &http_util, const string &url,
                                           const HttpRequestOptions &options) {
	auto params = http_util.InitializeParameters(context, url);
	params->timeout = options.timeout_ms / 1000;
	params->timeout_usec = (options.timeout_ms % 1000) * 1000;
	params->retries = options.retries;
	params->retry_wait_ms = 100;
	params->keep_alive = false;
	// Token endpoints are not data paths; extra headers configured for storage must not leak to them
	params->extra_headers.clear();
	return params;
}

static HTTPHeaders CreateHeaders(ClientContext &context, const HttpRequestOptions &options) {
	HTTPHeaders headers(DatabaseInstance::GetDatabase(context));
	for (auto &header : options.headers) {
		headers[header.first] = header.second;
	}
	return headers;
}

static HttpResult ToResult(unique_ptr<HTTPResponse> &response, string body) {
	HttpResult result;
	if (!response) {
		result.error = "no response received";
		return result;
	}
	if (response->status == HTTPStatusCode::INVALID) {
		result.error = response->GetError();
		return result;
	}
	result.status = static_cast<int32_t>(response->status);
	for (auto &header : response->headers) {
		result.headers[header.first] = header.second;
	}
	result.body = body.empty() ? std::move(response->body) : std::move(body);
	return result;
}

HttpResult HttpGet(ClientContext &context, const string &url, const HttpRequestOptions &options) {
	auto &http_util = HTTPUtil::Get(DatabaseInstance::GetDatabase(context));
	auto params = CreateParams(context, http_util, url, options);
	auto headers = CreateHeaders(context, options);
	string body;
	GetRequestInfo info(
	    url, headers, *params,
	    [&](const HTTPResponse &) {
		    body.clear();
		    return true;
	    },
	    [&](const_data_ptr_t data, idx_t len) {
		    body.append(const_char_ptr_cast(data), len);
		    return true;
	    });
	info.try_request = true;
	unique_ptr<HTTPClient> client;
	unique_ptr<HTTPResponse> response;
	try {
		response = http_util.Request(info, client);
	} catch (std::exception &ex) {
		ErrorData error(ex);
		HttpResult result;
		result.error = error.RawMessage();
		return result;
	}
	if (client) {
		http_util.CloseClient(std::move(client));
	}
	return ToResult(response, std::move(body));
}

HttpResult HttpPost(ClientContext &context, const string &url, const string &body, const string &content_type,
                    const HttpRequestOptions &options) {
	EnsureHttpTransport(context);
	auto &http_util = HTTPUtil::Get(DatabaseInstance::GetDatabase(context));
	auto params = CreateParams(context, http_util, url, options);
	auto headers = CreateHeaders(context, options);
	headers["Content-Type"] = content_type;
	PostRequestInfo info(url, headers, *params, const_data_ptr_cast(body.c_str()), body.size());
	info.try_request = true;
	unique_ptr<HTTPClient> client;
	unique_ptr<HTTPResponse> response;
	try {
		response = http_util.Request(info, client);
	} catch (std::exception &ex) {
		ErrorData error(ex);
		HttpResult result;
		result.error = error.RawMessage();
		return result;
	}
	if (client) {
		http_util.CloseClient(std::move(client));
	}
	return ToResult(response, std::move(info.buffer_out));
}

} // namespace gcs_adc
} // namespace duckdb
