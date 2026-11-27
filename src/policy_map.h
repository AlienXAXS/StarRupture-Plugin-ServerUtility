#pragma once

#include "plugin_map.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>

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

	// Does the map lie inside the member's block? blockSize is the member's size the loader reports
	// for the running build, not a compiled one. False when the map's offset did not resolve.
	inline bool FitsInBlock(size_t blockSize)
	{
		return Map.offset >= 0 && static_cast<size_t>(Map.offset) + sizeof(PluginMap::ScriptSetMemory) <= blockSize;
	}

	// What one Retarget_SEH walk found.
	struct RetargetResult
	{
		enum class Status { Done, NotAMap, Fault };
		Status status = Status::Done;
		const char* reason = "";        // NotAMap: why the view yields nothing
		int changed = 0;                // entries whose value was rewritten
		Value oldValue = 0;             // the value the last rewritten entry had
		int mismatched = 0;             // entries for the index with another serial, left alone
		int32_t mismatchSerial = 0;     // the serial of the last of them
	};

	// Sets the value of the entry for one object (its index and serial) in the policy map at `map`.
	// The view, the walk and the write all sit in one __try: a map at a wrong offset can fault
	// before the view rejects it. Nothing here needs unwinding, so SEH is legal, and the caller
	// does the logging. Game thread.
	inline void Retarget_SEH(uint8_t* map, const PluginMap::ScriptSetLayout& layout, int32_t index, int32_t serial,
		Value newValue, RetargetResult* out)
	{
		*out = RetargetResult{};
		__try
		{
			const PluginMap::ScriptMapView view(map, layout);
			if (!view)
			{
				out->status = RetargetResult::Status::NotAMap;
				out->reason = view.ReasonText();
				return;
			}
			view.ForEach([&](uint8_t* key, uint8_t* valueAt)
			{
				int32_t keyIndex = 0, keySerial = 0;
				Value value = 0;
				std::memcpy(&keyIndex,  key + KeyIndex.offset,  sizeof(keyIndex));
				std::memcpy(&keySerial, key + KeySerial.offset, sizeof(keySerial));
				std::memcpy(&value,     valueAt,                sizeof(value));
				if (keyIndex != index)
					return;
				if (keySerial != serial)
				{
					++out->mismatched;
					out->mismatchSerial = keySerial;
					return;
				}
				out->oldValue = value;
				std::memcpy(valueAt, &newValue, sizeof(newValue));
				++out->changed;
			});
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			out->status = RetargetResult::Status::Fault;
		}
	}
}
