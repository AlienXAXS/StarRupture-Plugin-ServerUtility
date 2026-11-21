#pragma once

#include "plugin_interface.h"
#include "SDK/Engine_classes.hpp"
#include "SDK/Chimera_classes.hpp"
#include "SDK/AuItems_classes.hpp"
#include "plugin_map.h"

#include <cstdint>

// Every function, member and struct size of the game the plugin takes from the
// loader's map for the running game build (see plugin_map.h), under one name
// each. Resolved once in OnPluginLoadHooks; a member is read on the game thread
// with In(object), which is null when the object is null or the member did not
// resolve. A rename in the map is a one-line edit here. The byte patterns the
// plugin falls back on are in game_signatures.h. Engine members (UObject::Index,
// UWorld::GameState, bAlwaysRelevant) are not game classes and stay compiled in.

// Minimal FString mirror so we can manipulate engine strings from outside.
// Layout matches UE4/UE5 TArray<TCHAR> used as FString storage.
struct EngineString
{
	wchar_t* Data; // AllocatorInstance.Data
	int32_t Num; // ArrayNum  (includes null terminator)
	int32_t Max; // ArrayMax
};

// UCrReplicationGraph::ClassRepNodePolicies as one block: a TClassMap, which is
// a TFunction followed by a TMap. Neither is a UPROPERTY, so the SDK shows
// padding there. GameSig::ReplicationGraph says where the map lies inside it.
struct ClassRepNodePolicyBlock
{
	uint8_t Bytes[0x80];
};

#define SERVER_UTILITY_FIELD(Class, Member) SR_FIELD(decltype(SDK::Class::Member), Class, Member, 0)

// A member reflection does not describe. The offset is the one compiled in,
// which is also this plugin's own row for the build it was verified on.
#define SERVER_UTILITY_NATIVE(T, Class, Member, offset) \
	PluginField<T>{ { #Class "::" #Member, #Class, #Member, PLUGIN_FIELD_NATIVE, sizeof(T), (offset) } }

namespace ServerUtilityFields
{
	// Game functions, "Class::Function" as the map names them.
	inline constexpr const char* ParseSettings          = "UCrDedicatedServerSettingsComp::ParseSettings";
	inline constexpr const char* GetProfessionForNewPlayer = "ACrGameModeBase::GetProfessionForNewPlayer";
	inline constexpr const char* PreLogin               = "ACrGameModeBase::PreLogin";
	inline constexpr const char* SaveNextSaveGame       = "UCrSaveSubsystem::SaveNextSaveGame";
	inline constexpr const char* AddNetworkActor        = "UReplicationGraph::AddNetworkActor";
	inline constexpr const char* RemoveNetworkActor     = "UReplicationGraph::RemoveNetworkActor";

	// UCrDedicatedServerSettingsComp, which ParseSettings fills in.
	inline auto SessionName      = SERVER_UTILITY_NATIVE(EngineString, UCrDedicatedServerSettingsComp, SessionName,      0xB8);
	inline auto SaveGameName     = SERVER_UTILITY_NATIVE(EngineString, UCrDedicatedServerSettingsComp, SaveGameName,     0xC8);
	inline auto SaveGameInterval = SERVER_UTILITY_NATIVE(int32_t,      UCrDedicatedServerSettingsComp, SaveGameInterval, 0xD8);
	inline auto StartNewGame     = SERVER_UTILITY_NATIVE(bool,         UCrDedicatedServerSettingsComp, bStartNewGame,    0xDC);
	inline auto LoadSavedGame    = SERVER_UTILITY_NATIVE(bool,         UCrDedicatedServerSettingsComp, bLoadSavedGame,   0xDD);

	// The replication graph's routing policy per class (Pad_5D8 in the SDK),
	// and the serial number in a FUObjectItem, which the policy map is keyed on.
	inline auto ClassRepNodePolicies = SERVER_UTILITY_NATIVE(ClassRepNodePolicyBlock, UCrReplicationGraph, ClassRepNodePolicies, 0x5E0);
	inline auto ObjectItemSerialNumber = SERVER_UTILITY_NATIVE(int32_t, FUObjectItem, SerialNumber, 0x10);

	// The gatherable replication actor, written by the seed resync.
	inline auto RepGlobalGatherablePCGSeed = SERVER_UTILITY_FIELD(ACrGatherableSpawnersRepActor, RepGlobalGatherablePCGSeed);
	inline auto RepEnviroWaveTypeChange    = SERVER_UTILITY_FIELD(ACrGatherableSpawnersRepActor, RepEnviroWaveTypeChange);
	inline auto RepEnviroWaveStageChange   = SERVER_UTILITY_FIELD(ACrGatherableSpawnersRepActor, RepEnviroWaveStageChange);

	// The seed each kind of PCG spawner actor last generated from.
	inline auto PCGActorBaseLocalSeed        = SERVER_UTILITY_FIELD(ACrPCGActorBase, LocalSeed);
	inline auto PCGGatherableSpawnerLocalSeed = SERVER_UTILITY_FIELD(ACrPCGGatherableSpawnerVolume, LocalSeed);
	inline auto PCGVolumeLocalSeed           = SERVER_UTILITY_FIELD(ACrPCGVolume, LocalSeed);

	// The item catalogue the admin give-item endpoint searches.
	inline auto ItemDataBaseArray = SERVER_UTILITY_FIELD(UAuItemDataBaseSubsystem, ItemDataBaseArray);

	// The game build the compiled offsets above were taken from: the plugin's
	// own row for a member the loader's map has no row for yet. It is the CL the
	// Game-SDK dump is built for, so the row follows the dump.
	static_assert(PLUGIN_GAME_BUILT_CL != 0, "PLUGIN_GAME_BUILT_CL is not set: the Game SDK has no build_cl.txt");
	inline constexpr uint32_t kVerifiedCL = PLUGIN_GAME_BUILT_CL;

	// Asks the loader for one native member, with this plugin's own row as the
	// fallback for a loader or build whose map has none.
	template <class T>
	bool ResolveNative(IPluginHookScanner* scanner, const IPluginSelf* self, PluginField<T>& field)
	{
		const PluginFieldCandidate rows[] = { { kVerifiedCL, field.builtOffset, field.size, 0, nullptr } };
		return PluginMap::ResolveField(scanner, self, field, rows, 1);
	}

	// Asks the loader for every member above. False when any did not resolve;
	// the loader has then recorded the failure and refuses the plugin.
	inline bool Resolve(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		bool ok = ResolveNative(scanner, self, SessionName);
		ok = ResolveNative(scanner, self, SaveGameName) && ok;
		ok = ResolveNative(scanner, self, SaveGameInterval) && ok;
		ok = ResolveNative(scanner, self, StartNewGame) && ok;
		ok = ResolveNative(scanner, self, LoadSavedGame) && ok;
		ok = ResolveNative(scanner, self, ClassRepNodePolicies) && ok;
		ok = ResolveNative(scanner, self, ObjectItemSerialNumber) && ok;
		return PluginMap::ResolveFields(scanner, self,
			RepGlobalGatherablePCGSeed, RepEnviroWaveTypeChange, RepEnviroWaveStageChange,
			PCGActorBaseLocalSeed, PCGGatherableSpawnerLocalSeed, PCGVolumeLocalSeed,
			ItemDataBaseArray) && ok;
	}
}
