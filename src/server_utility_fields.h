#pragma once

#include "plugin_interface.h"
#include "plugin_helpers.h"
#include "SDK/Engine_classes.hpp"
#include "SDK/Engine_parameters.hpp"
#include "SDK/Chimera_classes.hpp"
#include "SDK/Chimera_parameters.hpp"
#include "SDK/AuItems_classes.hpp"
#include "SDK/MassSpawner_classes.hpp"
#include "SDK/MassEntity_classes.hpp"
#include "SDK/MassActors_classes.hpp"
#include "SDK/MassAIPrototypeEnemyRuntime_classes.hpp"
#include "SDK/AIModule_classes.hpp"
#include "SDK/GameplayAbilities_classes.hpp"
#include "SDK/GameplayAbilities_parameters.hpp"
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
	inline constexpr const char* AddNewItem             = "UAuItemsComponent::AddNewItem";

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

	// What the su.cheat give op reads: the item's display name, the player's inventory component,
	// and the entries AddNewItem reports back (a TArray of them is handed to the game, so the size is checked).
	inline auto ItemName            = SERVER_UTILITY_FIELD(UAuItemDataBase, ItemName);
	inline auto InventoryComponent  = SERVER_UTILITY_FIELD(ACrCharacterPlayerBase, InventoryComponent);
	inline auto AuAddedItem_Amount  = SERVER_UTILITY_FIELD(FAuAddedItem, Amount);
	inline auto AuAddedItem_Size    = SR_SIZE_CHECK(FAuAddedItem);

	// The parameter blocks of the wave subsystem's calls the su.cheat wave op makes, as the running
	// game lays them out (reflected, so no map row is needed). Pause, resume and cancel take none.
	inline auto StartWaveSize      = SR_PARAMS_SIZE(CrEnviroWaveSubsystem, StartWave);
	inline auto StartWaveType      = SR_PARAM(CrEnviroWaveSubsystem, StartWave, WaveType);
	inline auto IsWaveInProgressSize   = SR_PARAMS_SIZE(CrEnviroWaveSubsystem, IsWaveInProgress);
	inline auto IsWaveInProgressResult = SR_PARAM(CrEnviroWaveSubsystem, IsWaveInProgress, ReturnValue);
	inline auto IsWavePausedSize   = SR_PARAMS_SIZE(CrEnviroWaveSubsystem, IsWavePaused);
	inline auto IsWavePausedResult = SR_PARAM(CrEnviroWaveSubsystem, IsWavePaused, ReturnValue);

	// What the su.cheat build op needs to unlock every building: the game state's technology keeper, its
	// unlock flag (a member the SDK dumps as padding: its builtOffset is -1, so only the loader's version
	// map can place it) and the call that rebuilds the available buildings from it.
	inline auto TechnologyKeeper = SERVER_UTILITY_FIELD(ACrGameStateBase, TechnologyKeeper);
	inline auto UnlockAllBuildings = PluginField<bool>{ { "ACrTechnologyKeeper::bUnlockAllBuildings", "ACrTechnologyKeeper",
		"bUnlockAllBuildings", PLUGIN_FIELD_NATIVE, sizeof(bool), -1 } };
	inline constexpr const char* CheckAvailableBuildings = "ACrTechnologyKeeper::CheckAvailableBuildings";

	// The su.cheat build op's free-building hooks: the placement checks that decide the cost and the
	// stability of a placement. Each is hooked through the loader by its map row.
	inline constexpr const char* PlacementResourceCondition = "UCrBuildingComponent::GetPlacementResourceConditionResult";
	inline constexpr const char* CheckStabilityCustom       = "ACrAPHelperActorCustom::CheckStability";
	inline constexpr const char* CheckStabilityPillar       = "ACrAPHelperDynamicPillar::CheckStability";

	// The anywhere parts: the two collision checks the placement helper runs (windows and benches),
	// and the socket pick with the stability subsystem's slot check (benches). The helper's building
	// data names the piece being placed; its snapped actors say what the piece sits in.
	inline constexpr const char* CheckMainMeshCollision = "ACrAPHelperActorCustom::CheckMainMeshCollision";
	inline constexpr const char* GetBoxCollisionResult  = "ACrAPHelper::GetBoxCollisionResult";
	inline constexpr const char* GetSnappedSocket       = "UCrSocketSnapPlacementMethod::GetSnappedSocket";
	inline constexpr const char* IsSocketFree           = "UCrBuildingStabilitySubsystem::IsSocketFree";
	inline auto CrAPHelperBuildingData = SERVER_UTILITY_FIELD(ACrAPHelper, BuildingData);
	inline auto PlacementBuildingID    = SERVER_UTILITY_FIELD(UAuActorPlacementData, BuildingID);
	// Absent on some builds (the client build has no such member): found by name on first use, null
	// there, and the anywhere parts then leave collision alone.
	inline PluginLazyField<SDK::TArray<SDK::AActor*>> CrAPHelperSnappedActors{ "SnappedActors" };

	// What the su.cheat corp op reads and writes: the game state's corporations owner and its data points.
	inline auto CorporationsOwner = SERVER_UTILITY_FIELD(ACrGameStateBase, CorporationsOwner);
	inline auto CorporationsDataPoints = SERVER_UTILITY_FIELD(ACrCorporationsOwner, DataPoints);

	// What the su.cheat power op reads and writes. The trait walk (config asset -> Config -> Traits and
	// Parent -> CrElectricityTrait::Parameters) and the parameters' value and type are reflected. The
	// manager's const shared fragment container is not: an engine member the map holds per build, a
	// block of 0xB0 bytes the shared-fragment calls take by address.
	struct ConstSharedBlock
	{
		uint8_t Bytes[0xB0];
	};
	inline auto MassConfigAssetConfig   = SERVER_UTILITY_FIELD(UMassEntityConfigAsset, Config);
	inline auto MassConfigParent        = SERVER_UTILITY_FIELD(FMassEntityConfig, Parent);
	inline auto MassConfigTraits        = SERVER_UTILITY_FIELD(FMassEntityConfig, Traits);
	inline auto ElectricityParameters   = SERVER_UTILITY_FIELD(UCrElectricityTrait, Parameters);
	inline auto ElectricityValue        = SERVER_UTILITY_FIELD(FCrElectricityParameters, ElectricityValue);
	inline auto ElectricityType         = SERVER_UTILITY_FIELD(FCrElectricityParameters, Type);
	inline auto ElectricityParametersSize = SR_SIZE_CHECK(FCrElectricityParameters);
	inline auto ConstSharedFragmentsContainer = SERVER_UTILITY_NATIVE(ConstSharedBlock, FMassEntityManager, ConstSharedFragmentsContainer, 0x120);
	inline constexpr const char* DestroyEntityTemplate     = "FMassEntityConfig::DestroyEntityTemplate";
	inline constexpr const char* GetOrCreateEntityTemplate = "FMassEntityConfig::GetOrCreateEntityTemplate";
	inline constexpr const char* SharedFragmentsFindOrAdd  = "TSharedFragmentsContainer<FConstSharedStruct>::FindOrAdd";
	inline constexpr const char* GetStructInstanceCrc32    = "UE::StructUtils::GetStructInstanceCrc32";

	// What the su.cheat enemy op reads and writes: the enemy character's pool flag and its three attack
	// ranges, the controller and the controller's brain component, and the health fragment's HP, a
	// member of the Mass entity state the SDK dumps as padding (a map row, size 4). The three game
	// functions find an enemy's health fragment: a weak key for the actor, the entity handle from it, and
	// the fragment data of an entity.
	inline auto EnemyIsInPool                = SERVER_UTILITY_FIELD(AMassEnemyCharacterBase, bIsInPool);
	inline auto EnemyAllowedAttackDistance   = SERVER_UTILITY_FIELD(AMassEnemyCharacterBase, AllowedAttackDistance);
	inline auto EnemyAllowedAttackConeHalfAngle = SERVER_UTILITY_FIELD(AMassEnemyCharacterBase, AllowedAttackConeHalfAngle);
	inline auto EnemyAllowedBuildingAttackDistance = SERVER_UTILITY_FIELD(AMassEnemyCharacterBase, AllowedBuildingAttackDistance);
	inline auto PawnController               = SERVER_UTILITY_FIELD(APawn, Controller);
	inline auto AIControllerBrainComponent   = SERVER_UTILITY_FIELD(AAIController, BrainComponent);
	inline auto EnemyHealthHP                = SERVER_UTILITY_NATIVE(float, FMassEnemyHealthFragment, HP, 0x0);

	// What the su.cheat weapon op reads and writes: the player character's weapon attribute set and the
	// damage and fire-rate multipliers on it (both FGameplayAttributeData, so Base and Current values).
	inline auto CharacterWeaponAttributes    = SERVER_UTILITY_FIELD(ACrCharacterPlayerBase, WeaponAttributes);
	inline auto WeaponDamageMod              = SERVER_UTILITY_FIELD(UCrWeaponAttributeSet, DamageModMultiplier);
	inline auto WeaponFireRateMod            = SERVER_UTILITY_FIELD(UCrWeaponAttributeSet, FireRateModMultiplier);

	// What the su.cheat grid op reads and calls: the inventory component's grid size and slot array,
	// the slot struct's size (the array is stepped by the size the game reflects, not a compiled one)
	// and its item id (a slot with an all-zero id is empty), and the parameter block of the
	// resize call. All reflected, so no map row of its own is needed.
	inline auto InventoryGridColumns      = SERVER_UTILITY_FIELD(UCrInventoryComponent, GridColumns);
	inline auto InventoryGridRows         = SERVER_UTILITY_FIELD(UCrInventoryComponent, GridRows);
	inline auto InventorySlots            = SERVER_UTILITY_FIELD(UCrInventoryComponent, Slots);
	inline auto InventorySlotSize         = SR_SIZE(FCrInventorySlot);
	inline auto InventorySlotItemId       = SERVER_UTILITY_FIELD(FCrInventorySlot, ItemId);
	inline auto ResizeInventorySize       = SR_PARAMS_SIZE(CrInventoryComponent, ServerResizeInventory);
	inline auto ResizeInventoryColumns    = SR_PARAM(CrInventoryComponent, ServerResizeInventory, Columns);
	inline auto ResizeInventoryRows       = SR_PARAM(CrInventoryComponent, ServerResizeInventory, Rows);

	// What the su.cheat mine op reads and calls: the player character's mining boost attribute set and
	// its two boost attributes, and the gameplay tag calls for the overheating tag: the ability
	// library's AddLooseGameplayTags and RemoveLooseGameplayTags, GetAbilitySystemComponent, the
	// component's GetGameplayTagCount, and the tag and tag container structs the tag is built from.
	// All reflected, so no map row of its own is needed.
	inline auto CharacterMiningBoost      = SERVER_UTILITY_FIELD(ACrCharacterPlayerBase, MiningBoostAttributes);
	inline auto MiningBoostCurrent        = SERVER_UTILITY_FIELD(UCrMiningBoostAttributeSet, CurrentBoostMultiplierValue);
	inline auto MiningBoostMax            = SERVER_UTILITY_FIELD(UCrMiningBoostAttributeSet, MaxBoostMultiplierValue);
	inline auto GameplayTagSize           = SR_SIZE(FGameplayTag);
	inline auto GameplayTagName           = SERVER_UTILITY_FIELD(FGameplayTag, TagName);
	inline auto TagContainerTags          = SERVER_UTILITY_FIELD(FGameplayTagContainer, GameplayTags);
	inline auto AddTagsSize               = SR_PARAMS_SIZE(AbilitySystemBlueprintLibrary, AddLooseGameplayTags);
	inline auto AddTagsActor              = SR_PARAM(AbilitySystemBlueprintLibrary, AddLooseGameplayTags, Actor);
	inline auto AddTagsTags               = SR_PARAM(AbilitySystemBlueprintLibrary, AddLooseGameplayTags, GameplayTags);
	inline auto AddTagsReplicate          = SR_PARAM(AbilitySystemBlueprintLibrary, AddLooseGameplayTags, bShouldReplicate);
	inline auto RemoveTagsSize            = SR_PARAMS_SIZE(AbilitySystemBlueprintLibrary, RemoveLooseGameplayTags);
	inline auto RemoveTagsActor           = SR_PARAM(AbilitySystemBlueprintLibrary, RemoveLooseGameplayTags, Actor);
	inline auto RemoveTagsTags            = SR_PARAM(AbilitySystemBlueprintLibrary, RemoveLooseGameplayTags, GameplayTags);
	inline auto RemoveTagsReplicate       = SR_PARAM(AbilitySystemBlueprintLibrary, RemoveLooseGameplayTags, bShouldReplicate);
	inline auto GetAscSize                = SR_PARAMS_SIZE(AbilitySystemBlueprintLibrary, GetAbilitySystemComponent);
	inline auto GetAscActor               = SR_PARAM(AbilitySystemBlueprintLibrary, GetAbilitySystemComponent, Actor);
	inline auto GetAscResult              = SR_PARAM(AbilitySystemBlueprintLibrary, GetAbilitySystemComponent, ReturnValue);
	inline auto TagCountSize              = SR_PARAMS_SIZE(AbilitySystemComponent, GetGameplayTagCount);
	inline auto TagCountTag               = SR_PARAM(AbilitySystemComponent, GetGameplayTagCount, GameplayTag);
	inline auto TagCountResult            = SR_PARAM(AbilitySystemComponent, GetGameplayTagCount, ReturnValue);

	// What the su.cheat stat op reads and writes: the 13 attribute sets on the player character and
	// each set's Current and Max attribute, plus Min on the seven that have one (the afflictions).
	// All reflected, so no map row of its own is needed. The lists below (SU_STAT_KEYS_*) name them
	// in the same order.
#define SU_STAT_FIELDS_RES(Name) 	inline auto Stat##Name##Set     = SERVER_UTILITY_FIELD(ACrCharacterPlayerBase, Name##Attributes); 	inline auto Stat##Name##Current = SERVER_UTILITY_FIELD(UCr##Name##AttributeSet, Current##Name); 	inline auto Stat##Name##Max     = SERVER_UTILITY_FIELD(UCr##Name##AttributeSet, Max##Name);
#define SU_STAT_FIELDS_AFF(Name) 	SU_STAT_FIELDS_RES(Name) 	inline auto Stat##Name##Min     = SERVER_UTILITY_FIELD(UCr##Name##AttributeSet, Min##Name);
#define SU_STAT_KEYS_RES(Name) Stat##Name##Set, Stat##Name##Current, Stat##Name##Max
#define SU_STAT_KEYS_AFF(Name) SU_STAT_KEYS_RES(Name), Stat##Name##Min
#define SU_STAT_KEYS_ALL 	SU_STAT_KEYS_RES(Health), SU_STAT_KEYS_RES(Energy), SU_STAT_KEYS_RES(Shield), SU_STAT_KEYS_RES(Hydration), 	SU_STAT_KEYS_RES(Calories), SU_STAT_KEYS_RES(Oxygen), SU_STAT_KEYS_AFF(Toxicity), SU_STAT_KEYS_AFF(Radiation), 	SU_STAT_KEYS_AFF(Heat), SU_STAT_KEYS_AFF(Drain), SU_STAT_KEYS_AFF(Corrosion), SU_STAT_KEYS_AFF(Infection), 	SU_STAT_KEYS_AFF(Temperature)
	SU_STAT_FIELDS_RES(Health)
	SU_STAT_FIELDS_RES(Energy)
	SU_STAT_FIELDS_RES(Shield)
	SU_STAT_FIELDS_RES(Hydration)
	SU_STAT_FIELDS_RES(Calories)
	SU_STAT_FIELDS_RES(Oxygen)
	SU_STAT_FIELDS_AFF(Toxicity)
	SU_STAT_FIELDS_AFF(Radiation)
	SU_STAT_FIELDS_AFF(Heat)
	SU_STAT_FIELDS_AFF(Drain)
	SU_STAT_FIELDS_AFF(Corrosion)
	SU_STAT_FIELDS_AFF(Infection)
	SU_STAT_FIELDS_AFF(Temperature)

	// What the su.cheat move op reads and writes: the player character's movement component, and on
	// it the movement mode, the two flying values and the cheat-flying flag, plus the SetMovementMode
	// call. Collision is never touched. All reflected, so no map row of its own is needed.
	inline auto CharacterMovement          = SERVER_UTILITY_FIELD(ACrCharacterPlayerBase, CrCharacterMovementComponent);
	inline auto MovementMode               = SERVER_UTILITY_FIELD(UCharacterMovementComponent, MovementMode);
	inline auto MovementMaxFlySpeed        = SERVER_UTILITY_FIELD(UCharacterMovementComponent, MaxFlySpeed);
	inline auto MovementBrakingFlying      = SERVER_UTILITY_FIELD(UCharacterMovementComponent, BrakingDecelerationFlying);
	inline auto MovementCheatFlying        = SR_BITFIELD(UCharacterMovementComponent, bCheatFlying);
	inline auto SetModeSize                = SR_PARAMS_SIZE(CharacterMovementComponent, SetMovementMode);
	inline auto SetModeMode                = SR_PARAM(CharacterMovementComponent, SetMovementMode, NewMovementMode);
	inline auto SetModeCustom              = SR_PARAM(CharacterMovementComponent, SetMovementMode, NewCustomMode);
	inline constexpr const char* WeakObjectPtrAssign       = "FWeakObjectPtr::operator=(FObjectPtr)";
	inline constexpr const char* GetEntityHandleFromActor  = "UMassActorSubsystem::GetEntityHandleFromActor";
	inline constexpr const char* InternalGetFragmentDataPtr = "FMassEntityManager::InternalGetFragmentDataPtr";

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
	// The object's outer (a component's owning actor), which the build hooks follow to the placing player.
	inline auto ObjectOuter = SERVER_UTILITY_NATIVE(SDK::UObject*, UObject, OuterPrivate, offsetof(SDK::UObject, Outer));

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

	// The su.cheat give op: AddNewItem on the server, and the members around it. While it is off
	// (no server row for AddNewItem yet) `cap` reports the op off and a request is refused.
	inline PluginMap::Feature CheatGive{ "cheat-give", "Item giving", PluginMap::KeysOf(
		AddNewItem, ItemName, InventoryComponent, AuAddedItem_Amount, AuAddedItem_Size) };

	// The su.cheat wave op: the wave subsystem's start call and its two state reads. While it is off
	// `cap` reports the op off and a request is refused.
	inline PluginMap::Feature CheatWave{ "cheat-wave", "Wave control", PluginMap::KeysOf(
		StartWaveSize, StartWaveType, IsWaveInProgressSize, IsWaveInProgressResult, IsWavePausedSize, IsWavePausedResult) };

	// The su.cheat corp op: the corporations owner and its data points. While it is off `cap` reports
	// the op off and a request is refused.
	inline PluginMap::Feature CheatCorp{ "cheat-corp", "Corporation data points", PluginMap::KeysOf(
		CorporationsOwner, CorporationsDataPoints) };

	// The su.cheat power op: the config asset trait walk, the shared fragment block and the four game
	// functions that rebuild a template and find the block. While it is off `cap` reports the op off.
	inline PluginMap::Feature CheatPower{ "cheat-power", "Building power values", PluginMap::KeysOf(
		MassConfigAssetConfig, MassConfigParent, MassConfigTraits, ElectricityParameters, ElectricityValue,
		ElectricityType, ElectricityParametersSize, ConstSharedFragmentsContainer, PluginMap::MassEntityManagerField,
		DestroyEntityTemplate, GetOrCreateEntityTemplate, SharedFragmentsFindOrAdd, GetStructInstanceCrc32) };

	// The su.cheat enemy op: the enemy character's pool flag and attack ranges, the controller walk to the
	// brain component, the health fragment's HP, the three functions that find the fragment, and the
	// Mass entity manager of the entity subsystem. While it is off `cap` reports the op off.
	inline PluginMap::Feature CheatEnemy{ "cheat-enemy", "Enemy cheats", PluginMap::KeysOf(
		EnemyIsInPool, EnemyAllowedAttackDistance, EnemyAllowedAttackConeHalfAngle, EnemyAllowedBuildingAttackDistance,
		PawnController, AIControllerBrainComponent, EnemyHealthHP, PluginMap::MassEntityManagerField,
		WeakObjectPtrAssign, GetEntityHandleFromActor, InternalGetFragmentDataPtr) };

	// The su.cheat weapon op: the character's weapon attribute set and its two multipliers. While it is
	// off `cap` reports the op off.
	inline PluginMap::Feature CheatWeapon{ "cheat-weapon", "Weapon cheats", PluginMap::KeysOf(
		CharacterWeaponAttributes, WeaponDamageMod, WeaponFireRateMod) };

	// The su.cheat grid op: the player's inventory component, its grid size and slots, and the resize
	// call. While it is off `cap` reports the op off.
	inline PluginMap::Feature CheatGrid{ "cheat-grid", "Inventory grid", PluginMap::KeysOf(
		InventoryComponent, InventoryGridColumns, InventoryGridRows, InventorySlots, InventorySlotSize,
		InventorySlotItemId, ResizeInventorySize, ResizeInventoryColumns, ResizeInventoryRows) };

	// The su.cheat mine op: the mining boost attributes and the gameplay tag calls. While it is off
	// `cap` reports the op off.
	inline PluginMap::Feature CheatMine{ "cheat-mine", "Mining tools", PluginMap::KeysOf(
		CharacterMiningBoost, MiningBoostCurrent, MiningBoostMax, GameplayTagSize, GameplayTagName, TagContainerTags,
		AddTagsSize, AddTagsActor, AddTagsTags, AddTagsReplicate, RemoveTagsSize, RemoveTagsActor, RemoveTagsTags,
		RemoveTagsReplicate, GetAscSize, GetAscActor, GetAscResult, TagCountSize, TagCountTag, TagCountResult) };

	// The su.cheat stat op: the 13 attribute sets and their Current, Max and Min attributes. While it is
	// off `cap` reports the op off.
	inline PluginMap::Feature CheatStat{ "cheat-stat", "Player stats", PluginMap::KeysOf(SU_STAT_KEYS_ALL) };

	// The su.cheat move op: the character's movement component, its flying members and the mode
	// call. While it is off `cap` reports the op off.
	inline PluginMap::Feature CheatMove{ "cheat-move", "Flying and speed", PluginMap::KeysOf(
		CharacterMovement, MovementMode, MovementMaxFlySpeed, MovementBrakingFlying, MovementCheatFlying,
		SetModeSize, SetModeMode, SetModeCustom) };

	// The su.cheat build op's unlock rule. While it is off the rule is not in `cap`'s build.rules.
	inline PluginMap::Feature CheatBuildUnlock{ "cheat-build-unlock", "Building unlock", PluginMap::KeysOf(
		TechnologyKeeper, UnlockAllBuildings, CheckAvailableBuildings) };

	// The build op's free rule, one feature per part so a missing row turns off only that part. Both
	// follow a hooked object to the player: its outer, its owner, a pawn's controller.
	inline PluginMap::Feature CheatBuildCost{ "cheat-build-cost", "Free building cost", PluginMap::KeysOf(
		PlacementResourceCondition, ObjectOuter, ActorOwner, PawnController) };
	inline PluginMap::Feature CheatBuildStability{ "cheat-build-stability", "Free building stability", PluginMap::KeysOf(
		CheckStabilityCustom, CheckStabilityPillar, ObjectOuter, ActorOwner, PawnController) };

	// The build op's anywhere rule, one feature per part. Benches also need the collision hooks.
	inline PluginMap::Feature CheatBuildWindows{ "cheat-build-windows", "Building windows anywhere", PluginMap::KeysOf(
		CheckMainMeshCollision, GetBoxCollisionResult, CrAPHelperBuildingData, PlacementBuildingID,
		ObjectOuter, ActorOwner, PawnController) };
	inline PluginMap::Feature CheatBuildBenches{ "cheat-build-benches", "Building benches anywhere", PluginMap::KeysOf(
		CheckMainMeshCollision, GetBoxCollisionResult, GetSnappedSocket, IsSocketFree, CrAPHelperBuildingData,
		PlacementBuildingID, ObjectOuter, ActorOwner, PawnController) };

	// Asks the loader for every member above. False when a key that no feature
	// lists did not resolve; the loader has then recorded the failure and
	// refuses the plugin. A miss on a key a feature lists only turns that
	// feature off.
	inline bool Resolve(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		PluginMap::DeclareFeatures(scanner, self, PlayerLookup, PlayerList, PlayerActions, GatherableRelevancy, SaveCommand, CheatGive, CheatWave, CheatCorp, CheatPower, CheatEnemy, CheatWeapon, CheatGrid, CheatMine, CheatStat, CheatMove, CheatBuildUnlock, CheatBuildCost, CheatBuildStability, CheatBuildWindows, CheatBuildBenches);

		// A miss only turns off the features that list the key. Every row is
		// asked even after a miss, so the loader's log lists them all.
		PluginMap::ResolveFields(scanner, self,
			WorldGameState, GameStatePlayerArray, ActorOwner, ControllerPlayerState,
			PlayerStateName, PlayerStatePing,
			ActorAlwaysRelevant, ObjectClass, ObjectIndex,
			ItemName, InventoryComponent, AuAddedItem_Amount, AuAddedItem_Size,
			StartWaveSize, StartWaveType, IsWaveInProgressSize, IsWaveInProgressResult, IsWavePausedSize, IsWavePausedResult,
			CorporationsOwner, CorporationsDataPoints,
			EnemyIsInPool, EnemyAllowedAttackDistance, EnemyAllowedAttackConeHalfAngle, EnemyAllowedBuildingAttackDistance,
			PawnController, AIControllerBrainComponent, EnemyHealthHP,
			CharacterWeaponAttributes, WeaponDamageMod, WeaponFireRateMod,
			InventoryGridColumns, InventoryGridRows, InventorySlots, InventorySlotSize, InventorySlotItemId,
			ResizeInventorySize, ResizeInventoryColumns, ResizeInventoryRows,
			CharacterMiningBoost, MiningBoostCurrent, MiningBoostMax, GameplayTagSize, GameplayTagName, TagContainerTags,
			AddTagsSize, AddTagsActor, AddTagsTags, AddTagsReplicate, RemoveTagsSize, RemoveTagsActor, RemoveTagsTags,
			RemoveTagsReplicate, GetAscSize, GetAscActor, GetAscResult, TagCountSize, TagCountTag, TagCountResult,
			SU_STAT_KEYS_ALL,
			CharacterMovement, MovementMode, MovementMaxFlySpeed, MovementBrakingFlying, MovementCheatFlying,
			SetModeSize, SetModeMode, SetModeCustom,
			MassConfigAssetConfig, MassConfigParent, MassConfigTraits, ElectricityParameters, ElectricityValue,
			ElectricityType, ElectricityParametersSize, ConstSharedFragmentsContainer,
			TechnologyKeeper, UnlockAllBuildings, ObjectOuter, CrAPHelperBuildingData, PlacementBuildingID);

		// The entity manager's field is shared by the power and enemy ops; this is the one place it is asked.
		PluginMap::ResolveMassEntityManager(scanner, self);

		// One line per feature the loader switched off, so the text names
		// exactly what is off and follows the feature's key list.
		for (const PluginMap::Feature* f : { &PlayerLookup, &PlayerList, &PlayerActions, &GatherableRelevancy, &SaveCommand, &CheatGive, &CheatWave, &CheatCorp, &CheatPower, &CheatEnemy, &CheatWeapon, &CheatGrid, &CheatMine, &CheatStat, &CheatMove, &CheatBuildUnlock, &CheatBuildCost, &CheatBuildStability, &CheatBuildWindows, &CheatBuildBenches })
		{
			if (!f->On())
				LOG_WARN("[%s] The loader has no row for one of its engine members on this game build - %s is off",
				         f->id, f->title);
		}

		return PluginMap::ResolveFields(scanner, self,
			SessionName, SaveGameName, SaveGameInterval, StartNewGame, LoadSavedGame,
			ClassRepNodePolicies, ObjectItemSerialNumber,
			RepGlobalGatherablePCGSeed, RepEnviroWaveTypeChange, RepEnviroWaveStageChange,
			PCGActorBaseLocalSeed, PCGGatherableSpawnerLocalSeed, PCGVolumeLocalSeed,
			ItemDataBaseArray);
	}
}
