#pragma once

#include "server_utility_fields.h"

#include <cstdio>
#include <string>

// Separated from the SEH helper so std::string objects don't live in the __try frame.
// An exact asset_path match wins. Otherwise a substring match is used only when it
// names exactly one item. On failure nullptr is returned and err holds the error JSON.
inline SDK::UAuItemDataBase* FindItemByPath(SDK::UAuItemDataBaseSubsystem* itemSubsys, const char* assetPath,
                                            char* err, int errCap)
{
	const auto* items = ServerUtilityFields::ItemDataBaseArray.In(itemSubsys);
	if (!items)
	{
		snprintf(err, errCap, R"({"ok":false,"error":"item not found"})");
		return nullptr;
	}

	SDK::UAuItemDataBase* single = nullptr;
	int hits = 0;

	const int32_t itemCount = items->Num();
	for (int32_t i = 0; i < itemCount; ++i)
	{
		SDK::UAuItemDataBase* item = (*items)[i];
		if (!item) continue;

		const std::string fullName = item->GetFullName(); // "<Class> <path>.<name>"

		// The path without the class prefix, and the bare name after its last '.' or ':'.
		const size_t space = fullName.find(' ');
		const std::string path = (space == std::string::npos) ? fullName : fullName.substr(space + 1);
		const size_t sep = path.find_last_of(".:");
		const std::string shortName = (sep == std::string::npos) ? path : path.substr(sep + 1);
		if (path == assetPath || shortName == assetPath)
			return item;

		if (fullName.find(assetPath) != std::string::npos)
		{
			++hits;
			single = item;
		}
	}

	if (hits == 1)
		return single;

	if (hits == 0)
		snprintf(err, errCap, R"({"ok":false,"error":"item not found"})");
	else
		snprintf(err, errCap, R"({"ok":false,"error":"%d items match, use the full asset_path"})", hits);
	return nullptr;
}
