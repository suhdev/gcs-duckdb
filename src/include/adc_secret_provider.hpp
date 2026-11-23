#pragma once

#include "adc/adc_types.hpp"

namespace duckdb {
class ExtensionLoader;

namespace gcs_adc {

//! Registers CREATE SECRET (TYPE gcs, PROVIDER adc) (spec section 4.1)
void RegisterAdcSecretProvider(ExtensionLoader &loader);

//! Registers gcs_adc_credential_info() (spec section 4.2)
void RegisterCredentialInfoFunction(ExtensionLoader &loader);

//! Builds AdcOptions from chain / credentials_file / impersonation / scope inputs, validating combinations
AdcOptions BuildAdcOptions(ClientContext &context, const string &chain, const string &credentials_file,
                           const string &impersonate_service_account, const vector<string> &delegates,
                           const vector<string> &scopes);

string FormatExpiry(int64_t expires_at);

} // namespace gcs_adc
} // namespace duckdb
