#include "adc/json.hpp"

#include "yyjson.hpp"

#include <cstdlib>

namespace duckdb {
namespace gcs_adc {

using namespace duckdb_yyjson; // NOLINT

JsonDocument::~JsonDocument() {
	if (doc) {
		yyjson_doc_free(doc);
	}
}

bool JsonDocument::Parse(const string &text) {
	if (doc) {
		yyjson_doc_free(doc);
		doc = nullptr;
		root = nullptr;
	}
	doc = yyjson_read(text.data(), text.size(), YYJSON_READ_NOFLAG);
	if (!doc) {
		return false;
	}
	root = yyjson_doc_get_root(doc);
	return root && yyjson_is_obj(root);
}

yyjson_val *JsonDocument::Get(const string &key) const {
	if (!root || !yyjson_is_obj(root)) {
		return nullptr;
	}
	return yyjson_obj_getn(root, key.data(), key.size());
}

bool JsonDocument::Has(const string &key) const {
	auto val = Get(key);
	return val && !yyjson_is_null(val);
}

string JsonDocument::GetString(const string &key) const {
	auto val = Get(key);
	if (!val || !yyjson_is_str(val)) {
		return string();
	}
	return string(yyjson_get_str(val), yyjson_get_len(val));
}

int64_t JsonDocument::GetInt(const string &key, int64_t fallback) const {
	auto val = Get(key);
	if (!val) {
		return fallback;
	}
	if (yyjson_is_int(val)) {
		return yyjson_get_sint(val);
	}
	if (yyjson_is_real(val)) {
		return static_cast<int64_t>(yyjson_get_real(val));
	}
	if (yyjson_is_str(val)) {
		string s(yyjson_get_str(val), yyjson_get_len(val));
		char *end = nullptr;
		auto parsed = std::strtoll(s.c_str(), &end, 10);
		if (end && *end == '\0' && !s.empty()) {
			return parsed;
		}
	}
	return fallback;
}

vector<string> JsonDocument::GetStringArray(const string &key) const {
	vector<string> result;
	auto val = Get(key);
	if (!val || !yyjson_is_arr(val)) {
		return result;
	}
	size_t idx, max;
	yyjson_val *item;
	yyjson_arr_foreach(val, idx, max, item) {
		if (yyjson_is_str(item)) {
			result.emplace_back(yyjson_get_str(item), yyjson_get_len(item));
		}
	}
	return result;
}

bool JsonDocument::GetObject(const string &key, JsonDocument &result) const {
	auto val = Get(key);
	if (!val || !yyjson_is_obj(val)) {
		return false;
	}
	size_t len = 0;
	char *text = yyjson_val_write(val, YYJSON_WRITE_NOFLAG, &len);
	if (!text) {
		return false;
	}
	string copy(text, len);
	free(text);
	return result.Parse(copy);
}

case_insensitive_map_t<string> JsonDocument::GetStringMembers() const {
	case_insensitive_map_t<string> result;
	if (!root || !yyjson_is_obj(root)) {
		return result;
	}
	size_t idx, max;
	yyjson_val *key, *val;
	yyjson_obj_foreach(root, idx, max, key, val) {
		if (yyjson_is_str(val)) {
			result[string(yyjson_get_str(key), yyjson_get_len(key))] = string(yyjson_get_str(val), yyjson_get_len(val));
		}
	}
	return result;
}

} // namespace gcs_adc
} // namespace duckdb
