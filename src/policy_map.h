#pragma once

#include "plugin_map.h"

#include <cstdint>

// UCrReplicationGraph::ClassRepNodePolicies is a TClassMap: a TFunction, then a
// TMap<FObjectKey, EClassRepNodeMapping>. Neither is a UPROPERTY. The loader's
// map gives where the TMap lies inside the member and where the two ints of an
// FObjectKey lie; each has the compiled value as its same-build fallback, as
// ObjectClass does. How a TMap lays out its elements and its used-slot bits is
// the engine's own and the SDK's PluginMap::ScriptMapView owns it, so nothing
// of that is compiled in here.
namespace PolicyMap
{
	// The TMap inside the member, as the SDK's script-set memory (a TSet is 0x50 bytes).
	inline PluginField<PluginMap::ScriptSetMemory> Map{ { "TClassMap<EClassRepNodeMapping>::Map", "TClassMap<EClassRepNodeMapping>", "Map",
		PLUGIN_FIELD_NATIVE, sizeof(PluginMap::ScriptSetMemory), 0x30 } };
	// The two ints of an FObjectKey, which is the map's key.
	inline PluginField<int32_t> KeyIndex{ { "FObjectKey::ObjectIndex", "FObjectKey", "ObjectIndex",
		PLUGIN_FIELD_NATIVE, sizeof(int32_t), 0x0 } };
	inline PluginField<int32_t> KeySerial{ { "FObjectKey::ObjectSerialNumber", "FObjectKey", "ObjectSerialNumber",
		PLUGIN_FIELD_NATIVE, sizeof(int32_t), 0x4 } };

	// The map's value is read and written as a uint32_t, the way the game stores an EClassRepNodeMapping.
	using Value = uint32_t;

	// The element layout of the policy map. A key is its two ints, so its size ends where the
	// later one ends; each int is aligned as an int32 (an engine type definition). Invalid when
	// either int did not resolve.
	inline PluginMap::ScriptSetLayout Layout()
	{
		if (KeyIndex.offset < 0 || KeySerial.offset < 0)
			return PluginMap::ScriptSetLayout{};
		const uint32_t index  = static_cast<uint32_t>(KeyIndex.offset) + KeyIndex.size;
		const uint32_t serial = static_cast<uint32_t>(KeySerial.offset) + KeySerial.size;
		return PluginMap::MapLayout(index > serial ? index : serial, alignof(int32_t), sizeof(Value), alignof(Value));
	}
}
