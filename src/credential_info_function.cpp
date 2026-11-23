#include "adc/credential_chain.hpp"
#include "adc_secret_provider.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace gcs_adc {

namespace {
struct CredentialInfoBindData : public TableFunctionData {
	string chain;
	string credentials_file;
	string impersonate_service_account;
	vector<string> delegates;
	vector<string> scopes;
};

struct CredentialInfoState : public GlobalTableFunctionState {
	bool done = false;
};
} // namespace

static vector<string> ToStrings(const Value &value) {
	vector<string> result;
	if (value.IsNull()) {
		return result;
	}
	for (auto &child : ListValue::GetChildren(value)) {
		if (!child.IsNull()) {
			result.push_back(child.ToString());
		}
	}
	return result;
}

static unique_ptr<FunctionData> CredentialInfoBind(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<CredentialInfoBindData>();
	for (auto &kv : input.named_parameters) {
		auto name = StringUtil::Lower(kv.first.GetIdentifierName());
		auto &value = kv.second;
		if (name == "chain") {
			result->chain = value.ToString();
		} else if (name == "credentials_file") {
			result->credentials_file = value.ToString();
		} else if (name == "impersonate_service_account") {
			result->impersonate_service_account = value.ToString();
		} else if (name == "delegates") {
			result->delegates = ToStrings(value);
		} else if (name == "oauth_scopes") {
			result->scopes = ToStrings(value);
		}
	}
	names.emplace_back("credential_source");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("credential_type");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("credentials_path");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("principal");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("project_id");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("quota_project_id");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("scopes");
	return_types.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));
	names.emplace_back("token_expires_at");
	return_types.emplace_back(LogicalType::TIMESTAMP_TZ);
	names.emplace_back("attempts");
	return_types.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> CredentialInfoInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<CredentialInfoState>();
}

static Value OptionalString(const string &value) {
	return value.empty() ? Value(LogicalType::VARCHAR) : Value(value);
}

static Value StringList(const vector<string> &values) {
	vector<Value> children;
	for (auto &value : values) {
		children.emplace_back(value);
	}
	return Value::LIST(LogicalType::VARCHAR, std::move(children));
}

static void CredentialInfoFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<CredentialInfoState>();
	if (state.done) {
		return;
	}
	state.done = true;
	auto &bind = data.bind_data->Cast<CredentialInfoBindData>();
	auto options = BuildAdcOptions(context, bind.chain, bind.credentials_file, bind.impersonate_service_account,
	                               bind.delegates, bind.scopes);
	auto resolved = ResolveCredential(context, options);
	auto token = GetAccessToken(context, resolved, options, false);
	auto &cred = *resolved.credential;

	output.SetValue(0, 0, Value(resolved.source));
	output.SetValue(1, 0, Value(CredentialTypeToString(cred.type)));
	output.SetValue(2, 0, OptionalString(cred.path));
	output.SetValue(3, 0, OptionalString(token.principal));
	output.SetValue(4, 0, OptionalString(cred.project_id));
	output.SetValue(5, 0, OptionalString(cred.quota_project_id));
	output.SetValue(6, 0, StringList(token.scopes));
	output.SetValue(7, 0, Value::TIMESTAMPTZ(timestamp_tz_t(Timestamp::FromEpochSeconds(token.expires_at))));
	output.SetValue(8, 0, StringList(resolved.attempts));
	output.SetCardinality(1);
}

void RegisterCredentialInfoFunction(ExtensionLoader &loader) {
	TableFunction function("gcs_adc_credential_info", {}, CredentialInfoFunction, CredentialInfoBind,
	                       CredentialInfoInit);
	function.GetSignature().WithTypedKwargs("options", [](TypedKwargs &kwargs) {
		kwargs.Add("chain", LogicalType::VARCHAR)
		    .Add("credentials_file", LogicalType::VARCHAR)
		    .Add("impersonate_service_account", LogicalType::VARCHAR)
		    .Add("delegates", LogicalType::LIST(LogicalType::VARCHAR))
		    .Add("oauth_scopes", LogicalType::LIST(LogicalType::VARCHAR));
	});
	loader.RegisterFunction(function);
}

} // namespace gcs_adc
} // namespace duckdb
