#pragma once

#include "adc/adc_types.hpp"

namespace duckdb {
namespace gcs_adc {

//! The scope requested for a source token that is only used to call IAM Credentials or STS on the way to impersonating
extern const char *CLOUD_PLATFORM_SCOPE;

//! Seconds since the Unix epoch
int64_t NowEpochSeconds();

//! Mints a token for the credential itself (spec sections 5.2 to 5.4), ignoring options.impersonate_service_account
AccessToken MintCredentialToken(ClientContext &context, const Credential &credential, const AdcOptions &options);

//! Mints the token a secret will carry: the credential's token, then impersonation when the options ask for it
AccessToken MintAccessToken(ClientContext &context, const Credential &credential, const AdcOptions &options);

//! Calls IAM Credentials generateAccessToken for target_email (or a full generateAccessToken URL) with a source token
AccessToken ImpersonateServiceAccount(ClientContext &context, const string &source_token, const string &target,
                                      const vector<string> &delegates, const vector<string> &scopes,
                                      const AdcOptions &options);

//! Extracts the service account email from a ...serviceAccounts/{email}:generateAccessToken URL
string ServiceAccountFromImpersonationUrl(const string &url);

} // namespace gcs_adc
} // namespace duckdb
