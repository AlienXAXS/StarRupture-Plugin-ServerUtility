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
// AGameStateBase::PlayerArray, AActor::Owner and APlayerController::PlayerState,
// as are the player name and ping, AActor::bAlwaysRelevant and the UObject class
// and index. No game or engine member is read at an offset compiled into the
// plugin: what is not in this table is the SDK's own inline code.

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
	// The player list and the admin player actions read the same game state.
	inline auto WorldGameState         = SERVER_UTILITY_FIELD(UWorld, GameState);
	inline auto GameStatePlayerArray   = SERVER_UTILITY_FIELD(AGameStateBase, PlayerArray);
	inline auto ActorOwner             = SERVER_UTILITY_FIELD(AActor, Owner);
	inline auto ControllerPlayerState  = SERVER_UTILITY_FIELD(APlayerController, PlayerState);

	// What the player list shows of a player: the name, and the ping the query
	// protocol reports.
	inline auto PlayerStateName = SERVER_UTILITY_FIELD(APlayerState, PlayerNamePrivate);
	inline auto PlayerStatePing = SERVER_UTILITY_FIELD(APlayerState, CompressedPing);

	// The gatherable relevancy fix: AActor::bAlwaysRelevant on the rep actor and
	// its class default object, and the class and index of an object, which the
	// fix walks GObjects by and keys the replication graph's policy map on. The
	// SDK names them Class and Index; the map names them as the engine does
	// (ClassPrivate, InternalIndex).
	inline auto ActorAlwaysRelevant = SR_BITFIELD(AActor, bAlwaysRelevant);
	inline auto ObjectClass = SERVER_UTILITY_NATIVE(SDK::UClass*, UObject, ClassPrivate, offsetof(SDK::UObject, Class));
	inline auto ObjectIndex = SERVER_UTILITY_NATIVE(int32_t,      UObject, InternalIndex, offsetof(SDK::UObject, Index));

	// The part of the plugin that can be off while the rest runs: finding the
	// connected players and the ID of each. Host rules, the player list, the
	// admin claim and the in-game admin tool all start from it. When the loader
	// has no row for one of its keys on the running game build it turns this
	// feature off and keeps the plugin; every key not listed here still refuses
	// the plugin. While it is off, the lookup returns nothing: no controllers
	// are listed and no player's ID is readable.
	inline PluginMap::Feature PlayerLookup{ "player-lookup", "Player lookup", PluginMap::KeysOf(
		WorldGameState, GameStatePlayerArray, ActorOwner, ControllerPlayerState) };

	// The player list the RCON and query protocols and the admin panel show:
	// each connected player's name and ping. While it is off the list is empty.
	inline PluginMap::Feature PlayerList{ "player-list", "Player list", PluginMap::KeysOf(
		WorldGameState, GameStatePlayerArray, PlayerStateName, PlayerStatePing) };

	// The admin panel's kick, ban and give: a player is found in the list by
	// name and then taken to its controller. While it is off each answers
	// "player not found".
	inline PluginMap::Feature PlayerActions{ "player-actions", "Admin player actions", PluginMap::KeysOf(
		WorldGameState, GameStatePlayerArray, PlayerStateName, ActorOwner) };

	// Keeping the gatherable spawners' replication actor relevant to every
	// client (see gatherable_relevancy.cpp). While it is off none of that runs.
	inline PluginMap::Feature GatherableRelevancy{ "gatherable-relevancy", "Gatherable relevancy fix", PluginMap::KeysOf(
		ActorAlwaysRelevant, ObjectClass, ObjectIndex) };

	// The RCON save command finds the save subsystem among the live objects by
	// its class. While it is off the command says so.
	inline PluginMap::Feature SaveCommand{ "save-command", "RCON save command", PluginMap::KeysOf(
		ObjectClass) };

	// Asks the loader for every member above. False when a key that no feature
	// lists did not resolve; the loader has then recorded the failure and
	// refuses the plugin. A miss on a key a feature lists only turns that
	// feature off.
	inline bool Resolve(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		PluginMap::DeclareFeatures(scanner, self, PlayerLookup, PlayerList, PlayerActions, GatherableRelevancy, SaveCommand);

		// A miss only turns off the features that list the key. Each group is
		// asked even after a miss, so the loader's log lists them all.
		const bool lookup = PluginMap::ResolveFields(scanner, self,
			WorldGameState, GameStatePlayerArray, ActorOwner, ControllerPlayerState);
		if (!lookup)
			LOG_WARN("[PlayerLookup] The loader has no row for one of its engine members on this game build - "
			         "host rules and player IDs are off");

		const bool names = PluginMap::ResolveFields(scanner, self, PlayerStateName, PlayerStatePing);
		if (!names)
			LOG_WARN("[PlayerList] The loader has no row for a player's name or ping on this game build - "
			         "the player list and the admin player actions are off");

		const bool relevancy = PluginMap::ResolveFields(scanner, self, ActorAlwaysRelevant, ObjectClass, ObjectIndex);
		if (!relevancy)
			LOG_WARN("[GatherableFix] The loader has no row for AActor::bAlwaysRelevant or the UObject class and "
			         "index on this game build - the gatherable relevancy fix and the RCON save command are off");

		return PluginMap::ResolveFields(scanner, self,
			SessionName, SaveGameName, SaveGameInterval, StartNewGame, LoadSavedGame,
			ClassRepNodePolicies, ObjectItemSerialNumber,
			RepGlobalGatherablePCGSeed, RepEnviroWaveTypeChange, RepEnviroWaveStageChange,
			PCGActorBaseLocalSeed, PCGGatherableSpawnerLocalSeed, PCGVolumeLocalSeed,
			ItemDataBaseArray);
	}
}
