#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace gcs_adc {

struct HttpResult {
	//! HTTP status code, or 0 when no response was received (connect/DNS/TLS failure or timeout)
	int32_t status = 0;
	string body;
	case_insensitive_map_t<string> headers;
	//! Transport error when status is 0
	string error;

	bool Ok() const {
		return status >= 200 && status < 300;
	}
};

struct HttpRequestOptions {
	uint64_t timeout_ms = 10000;
	idx_t retries = 2;
	case_insensitive_map_t<string> headers;
};

//! Makes sure an HTTPUtil that supports POST is installed (autoloading httpfs). Throws when that is not possible.
void EnsureHttpTransport(ClientContext &context);

//! GET through DuckDB's HTTPUtil. Never throws for HTTP errors; inspect the result.
HttpResult HttpGet(ClientContext &context, const string &url, const HttpRequestOptions &options);
//! POST through DuckDB's HTTPUtil. Never throws for HTTP errors; inspect the result.
HttpResult HttpPost(ClientContext &context, const string &url, const string &body, const string &content_type,
                    const HttpRequestOptions &options);

} // namespace gcs_adc
} // namespace duckdb
