#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace gcs_adc {

//! Signs data with RSASSA-PKCS1-v1_5 / SHA-256 using a PEM (PKCS#8 or PKCS#1) private key. Throws on failure; the
//! error never contains key material.
string SignRS256(const string &pem_private_key, const string &data);

//! Builds a signed RS256 JWT from a JSON header and claim set
string CreateSignedJwt(const string &pem_private_key, const string &header_json, const string &claims_json);

} // namespace gcs_adc
} // namespace duckdb
