#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace gcs_adc {

//! Unpadded base64url (RFC 4648 section 5), as JWTs use
string Base64UrlEncode(const string &input);
//! Percent-encodes everything except RFC 3986 unreserved characters
string UrlEncode(const string &input);
//! Builds an application/x-www-form-urlencoded body
string FormEncode(const vector<pair<string, string>> &fields);
//! Quotes and escapes a string as a JSON string literal
string JsonQuote(const string &input);
//! A JSON array of strings
string JsonStringArray(const vector<string> &values);
//! Overwrites a string's contents before it is released (best effort for secret material)
void ScrubString(string &value);

} // namespace gcs_adc
} // namespace duckdb
