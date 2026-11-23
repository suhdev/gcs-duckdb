#pragma once

#include "adc/adc_types.hpp"

namespace duckdb {
namespace gcs_adc {

//! Reads gcs_adc_* settings into options (timeouts, IAM endpoint)
void ApplySettings(ClientContext &context, AdcOptions &options);

//! Runs the chain (spec section 5.1). Throws, listing every attempt, when no step yields a credential.
ResolvedCredential ResolveCredential(ClientContext &context, const AdcOptions &options);

//! The gcloud well-known ADC file path for this platform and environment
string GcloudCredentialsPath();

//! Mints (or reuses from the process-wide cache) a token for a resolved credential. force_refresh skips the cache
//! lookup but still stores the new token.
AccessToken GetAccessToken(ClientContext &context, const ResolvedCredential &resolved, const AdcOptions &options,
                           bool force_refresh);

//! Number of tokens minted (not served from cache) since load; used by tests
idx_t TokenMintCount();

} // namespace gcs_adc
} // namespace duckdb
