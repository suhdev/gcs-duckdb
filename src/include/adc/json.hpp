#pragma once

#include "duckdb.hpp"

namespace duckdb_yyjson {
struct yyjson_doc;
struct yyjson_val;
} // namespace duckdb_yyjson

namespace duckdb {
namespace gcs_adc {

//! A read-only view of a parsed JSON document (DuckDB's bundled yyjson)
class JsonDocument {
public:
	JsonDocument() = default;
	JsonDocument(const JsonDocument &) = delete;
	JsonDocument &operator=(const JsonDocument &) = delete;
	~JsonDocument();

	//! Parses text; returns false when it is not a JSON object
	bool Parse(const string &text);

	bool Has(const string &key) const;
	//! Returns the string field, or "" when it is missing or not a string
	string GetString(const string &key) const;
	//! Returns the integer field (also accepts a numeric string), or the fallback
	int64_t GetInt(const string &key, int64_t fallback) const;
	//! Returns the string-array field, or an empty vector
	vector<string> GetStringArray(const string &key) const;
	//! Returns a nested object as its own document; false when missing or not an object
	bool GetObject(const string &key, JsonDocument &result) const;
	//! All string-valued members of this object
	case_insensitive_map_t<string> GetStringMembers() const;

private:
	duckdb_yyjson::yyjson_doc *doc = nullptr;
	duckdb_yyjson::yyjson_val *root = nullptr;

	duckdb_yyjson::yyjson_val *Get(const string &key) const;
};

} // namespace gcs_adc
} // namespace duckdb
