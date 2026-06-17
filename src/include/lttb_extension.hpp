#pragma once

#include "duckdb/main/extension.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

class LttbExtension : public Extension {
public:
	void Load(ExtensionLoader &db) override;
	std::string Name() override;
	std::string Version() const override;
};

} // namespace duckdb
