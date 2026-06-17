#define DUCKDB_EXTENSION_MAIN

#include "lttb_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

// OpenSSL linked through vcpkg
#include <openssl/opensslv.h>

namespace duckdb {

inline void LttbScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "...........🦆 " + name.GetString());
	});
}

inline void LttbOpenSSLVersionScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "Lttb " + name.GetString() + ", my linked OpenSSL version is " +
		                                           OPENSSL_VERSION_TEXT);
	});
}

static void LoadInternal(ExtensionLoader &loader) {
	// Register a scalar function
	auto lttb_scalar_function =
	    ScalarFunction("lttb", {LogicalType::VARCHAR}, LogicalType::VARCHAR, LttbScalarFun);

	loader.RegisterFunction(lttb_scalar_function);

	// Register another scalar function
	auto lttb_openssl_version_scalar_function = ScalarFunction("lttb_openssl_version", {LogicalType::VARCHAR},
	                                                             LogicalType::VARCHAR, LttbOpenSSLVersionScalarFun);
	loader.RegisterFunction(lttb_openssl_version_scalar_function);
}

void LttbExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string LttbExtension::Name() {
	return "lttb";
}

std::string LttbExtension::Version() const {
#ifdef EXT_VERSION_LTTB
	return EXT_VERSION_LTTB;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(lttb, loader) {
	duckdb::LoadInternal(loader);
}
}
