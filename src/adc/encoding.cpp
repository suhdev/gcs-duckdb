#include "adc/encoding.hpp"

namespace duckdb {
namespace gcs_adc {

string Base64UrlEncode(const string &input) {
	static const char *ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	string result;
	result.reserve((input.size() + 2) / 3 * 4);
	idx_t i = 0;
	auto data = reinterpret_cast<const unsigned char *>(input.data());
	for (; i + 2 < input.size(); i += 3) {
		uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | uint32_t(data[i + 2]);
		result += ALPHABET[(n >> 18) & 63];
		result += ALPHABET[(n >> 12) & 63];
		result += ALPHABET[(n >> 6) & 63];
		result += ALPHABET[n & 63];
	}
	auto remaining = input.size() - i;
	if (remaining == 1) {
		uint32_t n = uint32_t(data[i]) << 16;
		result += ALPHABET[(n >> 18) & 63];
		result += ALPHABET[(n >> 12) & 63];
	} else if (remaining == 2) {
		uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
		result += ALPHABET[(n >> 18) & 63];
		result += ALPHABET[(n >> 12) & 63];
		result += ALPHABET[(n >> 6) & 63];
	}
	return result;
}

string UrlEncode(const string &input) {
	static const char *HEX = "0123456789ABCDEF";
	string result;
	result.reserve(input.size());
	for (unsigned char c : input) {
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
		    c == '.' || c == '~') {
			result += static_cast<char>(c);
		} else {
			result += '%';
			result += HEX[c >> 4];
			result += HEX[c & 15];
		}
	}
	return result;
}

string FormEncode(const vector<pair<string, string>> &fields) {
	string result;
	for (auto &field : fields) {
		if (!result.empty()) {
			result += '&';
		}
		result += UrlEncode(field.first);
		result += '=';
		result += UrlEncode(field.second);
	}
	return result;
}

string JsonQuote(const string &input) {
	static const char *HEX = "0123456789abcdef";
	string result = "\"";
	for (unsigned char c : input) {
		switch (c) {
		case '"':
			result += "\\\"";
			break;
		case '\\':
			result += "\\\\";
			break;
		case '\n':
			result += "\\n";
			break;
		case '\r':
			result += "\\r";
			break;
		case '\t':
			result += "\\t";
			break;
		default:
			if (c < 0x20) {
				result += "\\u00";
				result += HEX[c >> 4];
				result += HEX[c & 15];
			} else {
				result += static_cast<char>(c);
			}
		}
	}
	result += '"';
	return result;
}

string JsonStringArray(const vector<string> &values) {
	string result = "[";
	for (idx_t i = 0; i < values.size(); i++) {
		if (i > 0) {
			result += ',';
		}
		result += JsonQuote(values[i]);
	}
	result += ']';
	return result;
}

void ScrubString(string &value) {
	volatile char *data = &value[0];
	for (idx_t i = 0; i < value.size(); i++) {
		data[i] = 0;
	}
	value.clear();
}

} // namespace gcs_adc
} // namespace duckdb
