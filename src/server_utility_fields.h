#pragma once

#include "plugin_interface.h"
#include "plugin_helpers.h"
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
// plugin falls back on are in game_signatures.h. The engine members the player
// lookup reads are here too, under the map's keys UWorld::GameState,
// AGameStateBase::PlayerArray, AActor::Owner and APlayerController::PlayerState.
// A few engine reads elsewhere (UObject::Index, bAlwaysRelevant, the GameState
// reads in rcon.cpp and the admin endpoints) are not in the table yet and stay
// compiled in.

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

// A member of an SDK class. Its key is "Class::Member", and the compiled offset
// is the row for the game build the SDK was dumped for.
#define SERVER_UTILITY_FIELD(Class, Member) SR_FIELD(decltype(SDK::Class::Member), Class, Member, 0)

// A member the SDK dumps as padding, so it has no name or offset to compile in:
// the loader's map holds it under this key, and offset is the row for the build
// this plugin was compiled against. The offsets were typed by hand and checked
// on CL 127004: re-check them whenever the dump moves.
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

	// The engine members that lead from the world to a connected player's
	// controller and from a controller to its player state (the player lookup).
	inline auto WorldGameState         = SERVER_UTILITY_FIELD(UWorld, GameState);
	inline auto GameStatePlayerArray   = SERVER_UTILITY_FIELD(AGameStateBase, PlayerArray);
	inline auto ActorOwner             = SERVER_UTILITY_FIELD(AActor, Owner);
	inline auto ControllerPlayerState  = SERVER_UTILITY_FIELD(APlayerController, PlayerState);

	// The part of the plugin that can be off while the rest runs: finding the
	// connected players and the ID of each. Host rules, the player list, the
	// admin claim and the in-game admin tool all start from it. When the loader
	// has no row for one of its keys on the running game build it turns this
	// feature off and keeps the plugin; every key not listed here still refuses
	// the plugin. While it is off, the lookup returns nothing: no controllers
	// are listed and no player's ID is readable.
	inline PluginMap::Feature PlayerLookup{ "player-lookup", "Player lookup", {
		WorldGameState.key, GameStatePlayerArray.key, ActorOwner.key, ControllerPlayerState.key
	} };

	// Asks the loader for every member above. False when a key that is not in
	// the player lookup did not resolve; the loader has then recorded the
	// failure and refuses the plugin. A miss in the player lookup only turns
	// that feature off.
	inline bool Resolve(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		PluginMap::DeclareFeatures(scanner, self, PlayerLookup);

		const bool lookup = PluginMap::ResolveFields(scanner, self,
			WorldGameState, GameStatePlayerArray, ActorOwner, ControllerPlayerState);
		if (!lookup)
			LOG_WARN("[PlayerLookup] The loader has no row for one of its engine members on this game build - "
			         "host rules, player IDs and the player list are off");

		return PluginMap::ResolveFields(scanner, self,
			SessionName, SaveGameName, SaveGameInterval, StartNewGame, LoadSavedGame,
			ClassRepNodePolicies, ObjectItemSerialNumber,
			RepGlobalGatherablePCGSeed, RepEnviroWaveTypeChange, RepEnviroWaveStageChange,
			PCGActorBaseLocalSeed, PCGGatherableSpawnerLocalSeed, PCGVolumeLocalSeed,
			ItemDataBaseArray);
	}
}
