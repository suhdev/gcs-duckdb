#pragma once

#include "adc/adc_types.hpp"

namespace duckdb {
namespace gcs_adc {

//! Reads a credential file through DuckDB's file system (so enable_external_access and friends apply).
//! Throws IOException when it cannot be read. Sets mtime to the file's modification time in epoch seconds.
string ReadCredentialFile(ClientContext &context, const string &path, int64_t &mtime);

//! Parses the JSON of an ADC credential file (spec section 5.2). path is only used in error messages.
//! Throws InvalidInputException when the JSON is malformed, the type is unknown, or a required field is missing.
//! Error messages never contain field values.
shared_ptr<Credential> ParseCredentialJson(const string &json, const string &path);

//! Reads and parses a credential file
shared_ptr<Credential> LoadCredentialFile(ClientContext &context, const string &path);

} // namespace gcs_adc
} // namespace duckdb
