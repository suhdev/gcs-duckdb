#define DUCKDB_EXTENSION_MAIN

#include "gcs_adc_extension.hpp"

#include "adc/credential_chain.hpp"
#include "adc_secret_provider.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_helper.hpp"

namespace duckdb {

static void TokenMintCountFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<int64_t>(result)[0] = NumericCast<int64_t>(gcs_adc::TokenMintCount());
}

static void LoadInternal(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	// The gcs secret type, and the HTTP transport used for token requests, come from httpfs
	ExtensionHelper::TryAutoLoadExtension(db, "httpfs");

	auto &config = DBConfig::GetConfig(db);
	config.AddExtensionOption("gcs_adc_metadata_timeout_ms",
	                          "Timeout in milliseconds for probing the GCE metadata server", LogicalType::UBIGINT,
	                          Value::UBIGINT(500));
	config.AddExtensionOption("gcs_adc_token_timeout_ms",
	                          "Timeout in milliseconds for OAuth2, STS and IAM Credentials token requests",
	                          LogicalType::UBIGINT, Value::UBIGINT(10000));
	config.AddExtensionOption("gcs_adc_iam_endpoint",
	                          "Base URL of the IAM Credentials API used for service account impersonation",
	                          LogicalType::VARCHAR, Value("https://iamcredentials.googleapis.com"));

	gcs_adc::RegisterAdcSecretProvider(loader);
	gcs_adc::RegisterCredentialInfoFunction(loader);

	// Diagnostic: how many tokens this process has minted (cache misses). Used by the token-cache tests.
	ScalarFunction mint_count("gcs_adc_token_mint_count", {}, LogicalType::BIGINT, TokenMintCountFunction);
	mint_count.SetStability(FunctionStability::VOLATILE);
	loader.RegisterFunction(mint_count);
}

void GcsAdcExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string GcsAdcExtension::Name() {
	return "gcs_adc";
}

std::string GcsAdcExtension::Version() const {
#ifdef EXT_VERSION_GCS_ADC
	return EXT_VERSION_GCS_ADC;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(gcs_adc, loader) {
	duckdb::LoadInternal(loader);
}
}
