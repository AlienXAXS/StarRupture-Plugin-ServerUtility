#pragma once

#include "server_utility_fields.h"

#include <cstdint>

// FUObjectItem::SerialNumber for an object index. FObjectKey is
// {ObjectIndex, ObjectSerialNumber} and the map is keyed on both, so the
// serial has to come from the same place the engine read it: GUObjectArray.
inline int32_t GetObjectSerialNumber(int32_t index)
{
	auto& gobjects = SDK::UObject::GObjects;
	if (!gobjects || index < 0 || index >= gobjects->Num())
		return 0;

	const int32_t chunk   = index / SDK::TUObjectArray::ElementsPerChunk;
	const int32_t inChunk = index % SDK::TUObjectArray::ElementsPerChunk;
	if (chunk >= gobjects->NumChunks)
		return 0;

	SDK::FUObjectItem* items = gobjects->GetDecrytedObjPtr()[chunk];
	if (!items)
		return 0;

	const int32_t* serial = ServerUtilityFields::ObjectItemSerialNumber.In(static_cast<const void*>(&items[inChunk]));
	return serial ? *serial : 0;
}
