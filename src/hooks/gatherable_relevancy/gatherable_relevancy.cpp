#include "gatherable_relevancy.h"
#include "scan_helpers.h"
#include "server_utility_fields.h"
#include "plugin_helpers.h"
#include "game_signatures.h"
#include "policy_map.h"
#include "object_serial.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
using RepGraphActorFn = void(__fastcall*)(void* graph, void* actor);

static RepGraphActorFn g_addNetworkActor    = nullptr;
static RepGraphActorFn g_removeNetworkActor = nullptr;

static bool g_installed   = false;
static bool g_livePending = false;   // a world came up; the live-graph half still has to run
static int  g_liveTicks   = 0;       // ticks spent waiting for the rep actor to appear
static bool g_seedResync  = true;    // config: push the loaded seed onto the rep actor
static int  g_resyncTicks = -1;      // countdown to the seed resync; -1 = none scheduled
static int  g_resyncTries = 0;       // seed resync attempts made for this world
static std::string g_worldName;      // world the pending work belongs to, for the completion line

// The rep actor is spawned in UCrGatherableSpawnersSubsystem::OnWorldBeginPlay
// and the graph adds it on spawn. Our world-begin-play hook is on a different
// subsystem, so on a given frame it may not exist yet; a few seconds of ticks
// is far more than it needs and bounds the retry if something is genuinely
// missing (a world with no gatherable subsystem, say).
static constexpr int kMaxLiveTicks = 600;

// Ticks between the live re-route and the seed resync. The subsystem's own
// TryLoading retries on a 3 s timer until the rep actor exists, and the
// resync must run after it has restored the saved seed; a couple of seconds
// of ticks after the actor showed up comfortably covers that.
static constexpr int kResyncDelayTicks = 240;

// The PCG spawner actors are not guaranteed to be loaded by the time the rep
// actor exists, so an empty scan is retried every kResyncDelayTicks. This many
// attempts (~a minute or more of ticks) before concluding they never will be.
static constexpr int kMaxResyncTries = 15;

// ---------------------------------------------------------------------------
// Resolve
// ---------------------------------------------------------------------------
void GatherableRelevancyFix::Resolve(IPluginSelf* self, IPluginHookScanner* scanner)
{
	if (!self || !scanner)
		return;

	g_addNetworkActor = reinterpret_cast<RepGraphActorFn>(ScanUtil::ResolveFunction(self, scanner,
		ServerUtilityFields::AddNetworkActor, GameSig::REPGRAPH_ADD_NETWORK_ACTOR));
	g_removeNetworkActor = reinterpret_cast<RepGraphActorFn>(ScanUtil::ResolveFunction(self, scanner,
		ServerUtilityFields::RemoveNetworkActor, GameSig::REPGRAPH_REMOVE_NETWORK_ACTOR));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// The class of an object, read through the loader's row for UObject::ClassPrivate.
static SDK::UClass* ClassOf(SDK::UObject* obj)
{
	SDK::UClass** at = ServerUtilityFields::ObjectClass.In(obj);
	return at ? *at : nullptr;
}

// Live (non-CDO) instances of a class, by class name. Walks GObjects once.
template <typename T>
static std::vector<T*> FindInstances(const char* className)
{
	std::vector<T*> out;
	auto& gobjects = SDK::UObject::GObjects;
	if (!gobjects)
		return out;

	const int32_t count = gobjects->Num();
	for (int32_t i = 0; i < count; ++i)
	{
		SDK::UObject* obj = gobjects->GetByIndex(i);
		SDK::UClass* objClass = obj ? ClassOf(obj) : nullptr;
		if (!objClass || obj->IsDefaultObject())
			continue;
		if (objClass->GetName() == className)
			out.push_back(static_cast<T*>(obj));
	}
	return out;
}

// Live (non-CDO) instances of a class or any subclass of it -- Blueprint
// children included, which an exact class-name match would miss.
template <typename T>
static std::vector<T*> FindInstancesOf(SDK::UClass* cls)
{
	std::vector<T*> out;
	auto& gobjects = SDK::UObject::GObjects;
	if (!gobjects || !cls)
		return out;

	const int32_t count = gobjects->Num();
	for (int32_t i = 0; i < count; ++i)
	{
		SDK::UObject* obj = gobjects->GetByIndex(i);
		if (!obj || !ClassOf(obj) || obj->IsDefaultObject())
			continue;
		if (obj->IsA(cls))
			out.push_back(static_cast<T*>(obj));
	}
	return out;
}

// Rewrites the graph's cached routing policy for `cls`.
//
// UCrReplicationGraph::ClassRepNodePolicies is a TClassMap: a TFunction
// followed by TMap<FObjectKey, EClassRepNodeMapping>. Neither is a UPROPERTY,
// so the SDK shows only padding. The member's offset, the map's position
// inside it and the two ints of the key come from the loader's map (see
// policy_map.h); the SDK's script map view walks the sparse array at the
// stride the key and value sizes give. Returns the number of entries changed.
static int PatchClassPolicy(SDK::UCrReplicationGraph* graph, SDK::UClass* cls, uint32_t newValue)
{
	uint8_t* policies = reinterpret_cast<uint8_t*>(ServerUtilityFields::ClassRepNodePolicies.In(graph));
	if (!policies)
	{
		LOG_ERROR("[GatherableFix] ClassRepNodePolicies did not resolve - cannot patch the live graph");
		return -1;
	}
	// The map has to lie inside the block the loader's row sizes.
	if (PolicyMap::Map.offset < 0 || !PolicyMap::Layout().valid
	    || static_cast<size_t>(PolicyMap::Map.offset) + sizeof(PluginMap::ScriptSetMemory) > sizeof(ClassRepNodePolicyBlock))
	{
		LOG_ERROR("[GatherableFix] The loader has no usable row for the policy map or its key - policy not patched");
		return -1;
	}
	const auto base = policies + PolicyMap::Map.offset;

	const PluginMap::ScriptMapView map(base, PolicyMap::Layout());
	if (!map)
	{
		LOG_ERROR("[GatherableFix] ClassRepNodePolicies map at graph+0x%zX does not look like a TMap (%s) - "
		          "the layout needs re-deriving for this build",
		          static_cast<size_t>(base - reinterpret_cast<uint8_t*>(graph)), map.ReasonText());
		return -1;
	}

	const int32_t* indexAt = ServerUtilityFields::ObjectIndex.In(cls);
	if (!indexAt)
	{
		LOG_ERROR("[GatherableFix] The loader has no row for the object index - policy not patched");
		return -1;
	}
	const int32_t wantIndex  = *indexAt;
	const int32_t wantSerial = GetObjectSerialNumber(wantIndex);
	if (wantSerial == 0)
	{
		// The engine allocated one when it built the FObjectKey, so a zero
		// here means the serial was read from the wrong place, not that the
		// class is unregistered.
		LOG_ERROR("[GatherableFix] No serial number for %s (index %d) - FUObjectItem offset wrong?",
		          cls->GetName().c_str(), wantIndex);
		return -1;
	}

	int changed = 0;
	map.ForEach([&](uint8_t* key, uint8_t* valueAt)
	{
		int32_t keyIndex = 0, keySerial = 0;
		PolicyMap::Value value = 0;
		std::memcpy(&keyIndex,  key + PolicyMap::KeyIndex.offset,  sizeof(keyIndex));
		std::memcpy(&keySerial, key + PolicyMap::KeySerial.offset, sizeof(keySerial));
		std::memcpy(&value,     valueAt,                           sizeof(value));

		if (keyIndex != wantIndex)
			return;
		if (keySerial != wantSerial)
		{
			LOG_WARN("[GatherableFix] Policy entry for object index %d has serial %d, expected %d - skipping",
			         keyIndex, keySerial, wantSerial);
			return;
		}

		LOG_INFO("[GatherableFix] Graph %s: %s policy %u -> %u",
		         graph->GetName().c_str(), cls->GetName().c_str(), value, newValue);
		std::memcpy(valueAt, &newValue, sizeof(newValue));
		++changed;
	});

	return changed;
}

// ---------------------------------------------------------------------------
// The two halves
// ---------------------------------------------------------------------------
static void ApplyToClassDefault()
{
	SDK::ACrGatherableSpawnersRepActor* cdo = SDK::ACrGatherableSpawnersRepActor::GetDefaultObj();
	if (!cdo)
	{
		LOG_ERROR("[GatherableFix] ACrGatherableSpawnersRepActor CDO not found - class missing from this build?");
		return;
	}

	if (ServerUtilityFields::ActorAlwaysRelevant.Get(cdo))
	{
		LOG_INFO("[GatherableFix] CDO already bAlwaysRelevant - the game fixed this? Nothing to do for new graphs");
		return;
	}

	ServerUtilityFields::ActorAlwaysRelevant.Set(cdo, true);
	LOG_INFO("[GatherableFix] ACrGatherableSpawnersRepActor CDO: bAlwaysRelevant set - "
	         "replication graphs built from now on route it RelevantAllConnections");
}

// Returns true when done (applied, or nothing to apply); false to retry next tick.
static bool ApplyToLiveGraph()
{
	auto actors = FindInstances<SDK::ACrGatherableSpawnersRepActor>("CrGatherableSpawnersRepActor");
	if (actors.empty())
		return false;   // not spawned yet

	auto graphs = FindInstances<SDK::UCrReplicationGraph>("CrReplicationGraph");
	if (graphs.empty())
	{
		// No graph means no net driver yet (or the graph is disabled); the CDO
		// half covers whichever of those it turns out to be.
		LOG_INFO("[GatherableFix] Rep actor present but no UCrReplicationGraph instance - nothing to patch live");
		return true;
	}

	SDK::UClass* cls = SDK::ACrGatherableSpawnersRepActor::StaticClass();
	if (!cls)
		return true;

	for (SDK::ACrGatherableSpawnersRepActor* actor : actors)
		ServerUtilityFields::ActorAlwaysRelevant.Set(actor, true);

	const bool canReroute = g_addNetworkActor && g_removeNetworkActor;
	if (!canReroute)
		LOG_WARN("[GatherableFix] AddNetworkActor/RemoveNetworkActor unresolved - policy patched but the live "
		         "actor stays in its current node until the next map load");

	for (SDK::UCrReplicationGraph* graph : graphs)
	{
		const int changed = PatchClassPolicy(graph, cls,
			static_cast<uint32_t>(SDK::EClassRepNodeMapping::RelevantAllConnections));
		if (changed < 0)
			continue;
		if (changed == 0)
			LOG_INFO("[GatherableFix] Graph %s has no cached policy for the class - it will resolve from the CDO",
			         graph->GetName().c_str());

		if (!canReroute)
			continue;

		// Remove/add rebuilds FNewReplicatedActorInfo and routes through
		// RouteAddNetworkActorToNodes with the policy it now finds.
		for (SDK::ACrGatherableSpawnersRepActor* actor : actors)
		{
			g_removeNetworkActor(graph, actor);
			g_addNetworkActor(graph, actor);
			LOG_INFO("[GatherableFix] Re-routed %s in graph %s", actor->GetName().c_str(), graph->GetName().c_str());
		}
	}

	LOG_INFO("[GatherableFix] Live graph patched - verify with 'CrRepGraph.PrintRouting'");
	return true;
}

// ---------------------------------------------------------------------------
// Seed resync
//
// Relevancy gets the rep actor to the client; it does not get the right seed
// onto it. The seed is only written to ACrGatherableSpawnersRepActor::
// RepGlobalGatherablePCGSeed inside OnGlobalSeedChanged, i.e. when a wave
// stage changes. TryLoading restores the saved seed into the *subsystem* and
// never touches the actor, so after loading a save the actor still carries its
// constructor default (777) until the next wave -- and a client joining in
// between generates from 777 while the server runs the saved seed. A new save
// has never rolled the seed, so 777 is right by accident; a long-lived save is
// wrong on every join. That is the "old saves only" symptom.
//
// The seed the server generated with is read back from its own PCG actors:
// every ACrPCGGatherableSpawnerVolume / ACrPCGVolume / ACrPCGActorBase stores
// the seed it last generated from in LocalSeed (a UPROPERTY, unlike the
// subsystem's copy). That is the number the client has to match -- whatever
// order load and generation happened in on the server -- so it is preferred
// over the subsystem's private field, which would also need a per-build
// offset. The majority value wins; a split is logged, because it would mean
// the server itself is not in one consistent state.
//
// The wave type/stage written alongside are Heat/Moving. A client's OnRep
// hands them to OnGlobalSeedChanged with bIsDebugForced=false, so only
// volumes whose SeedChangeEnviroWaveConditions contain that pair regenerate;
// Heat/Moving is the stage the seed itself changes on, so it is the pair a
// volume must list to ever pick up a new seed, and it is what the game's own
// forced-generation path writes (EnviroWave 1, Stage 2 packed as 513).
//
// Deliberately NOT TriggerGenerationWithCurrentSeed: that path also clears
// the depleted octree for the FireWaveMoving stage before regenerating, i.e.
// plants due back at the next heat wave come back at every server restart.
// Writing the actor directly changes nothing on the server; the client is
// simply told the truth.
// ---------------------------------------------------------------------------
template <typename T>
static void CollectLocalSeeds(const PluginField<int32_t>& localSeed, std::map<int32_t, int>& histogram, int& total)
{
	for (T* actor : FindInstancesOf<T>(T::StaticClass()))
	{
		if (const int32_t* seed = localSeed.In(actor))
		{
			++histogram[*seed];
			++total;
		}
	}
}

// Returns true when done (written, or nothing can be written); false to retry.
static bool ResyncSeed()
{
	auto actors = FindInstances<SDK::ACrGatherableSpawnersRepActor>("CrGatherableSpawnersRepActor");
	if (actors.empty())
	{
		LOG_WARN("[GatherableFix] Rep actor gone before the seed resync ran - nothing to write");
		return true;
	}

	std::map<int32_t, int> histogram;
	int total = 0;
	CollectLocalSeeds<SDK::ACrPCGGatherableSpawnerVolume>(ServerUtilityFields::PCGGatherableSpawnerLocalSeed, histogram, total);
	CollectLocalSeeds<SDK::ACrPCGVolume>(ServerUtilityFields::PCGVolumeLocalSeed, histogram, total);
	CollectLocalSeeds<SDK::ACrPCGActorBase>(ServerUtilityFields::PCGActorBaseLocalSeed, histogram, total);

	if (total == 0)
	{
		if (++g_resyncTries < kMaxResyncTries)
		{
			LOG_DEBUG("[GatherableFix] No PCG spawner actors loaded yet (attempt %d/%d) - retrying",
			          g_resyncTries, kMaxResyncTries);
			return false;
		}
		LOG_WARN("[GatherableFix] No PCG spawner actors found after %d attempts - cannot determine the "
		         "server's gatherable seed; clients keep the rep actor's current seed", g_resyncTries);
		return true;
	}

	int32_t seed  = 0;
	int     votes = 0;
	for (const auto& [value, count] : histogram)
	{
		LOG_DEBUG("[GatherableFix]   LocalSeed %d on %d of %d spawner actors", value, count, total);
		if (count > votes) { seed = value; votes = count; }
	}
	if (histogram.size() > 1)
		LOG_WARN("[GatherableFix] Spawner actors disagree on the seed (%zu distinct values) - using %d (%d/%d). "
		         "The server generated some volumes before the save's seed was restored?",
		         histogram.size(), seed, votes, total);

	for (SDK::ACrGatherableSpawnersRepActor* actor : actors)
	{
		int32_t* seedAt  = ServerUtilityFields::RepGlobalGatherablePCGSeed.In(actor);
		auto*    typeAt  = ServerUtilityFields::RepEnviroWaveTypeChange.In(actor);
		auto*    stageAt = ServerUtilityFields::RepEnviroWaveStageChange.In(actor);
		if (!seedAt || !typeAt || !stageAt)
			continue;

		const int32_t before = *seedAt;
		*seedAt  = seed;
		*typeAt  = SDK::EEnviroWave::Heat;
		*stageAt = SDK::EEnviroWaveStage::Moving;
		LOG_INFO("[GatherableFix] %s: RepGlobalGatherablePCGSeed %d -> %d (Heat/Moving) - clients will regenerate from it",
		         actor->GetName().c_str(), before, seed);
	}
	return true;
}

// The per-world work is over, however it ended. Logged unconditionally so a
// warning from a step above is never the last thing in the log for a healthy
// server.
static void FinishWorld()
{
	LOG_INFO("Plugin ServerUtility finished loading on world '%s'",
	         g_worldName.empty() ? "?" : g_worldName.c_str());
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------
static void OnAnyWorldBeginPlay(SDK::UWorld* /*world*/, const char* worldName)
{
	LOG_DEBUG("[GatherableFix] World '%s' began play - scheduling live graph check", worldName ? worldName : "?");
	g_worldName   = worldName ? worldName : "";
	g_livePending = true;
	g_liveTicks   = 0;
	g_resyncTicks = -1;   // a new world: the old countdown is meaningless
	g_resyncTries = 0;
}

static void OnTick(float /*deltaSeconds*/)
{
	if (g_resyncTicks >= 0 && g_resyncTicks-- == 0)
	{
		if (ResyncSeed())
			FinishWorld();
		else
			g_resyncTicks = kResyncDelayTicks;
	}

	if (!g_livePending)
		return;

	if (ApplyToLiveGraph())
	{
		g_livePending = false;
		if (g_seedResync)
			g_resyncTicks = kResyncDelayTicks;
		else
			FinishWorld();
		return;
	}

	if (++g_liveTicks >= kMaxLiveTicks)
	{
		LOG_WARN("[GatherableFix] ACrGatherableSpawnersRepActor never appeared after %d ticks - giving up "
		         "until the next world (the CDO half still applies)", g_liveTicks);
		g_livePending = false;
		FinishWorld();
	}
}

// ---------------------------------------------------------------------------
// Install / Remove
// ---------------------------------------------------------------------------
void GatherableRelevancyFix::Install(bool seedResync)
{
	if (g_installed)
		return;

	// Resolve already logged why the feature is off.
	if (!ServerUtilityFields::GatherableRelevancy.On())
		return;

	g_seedResync = seedResync;

	auto* hooks = GetHooks();
	if (!hooks || !hooks->Engine || !hooks->World)
	{
		LOG_ERROR("[GatherableFix] Engine/World interfaces unavailable - loader version mismatch?");
		return;
	}

	ApplyToClassDefault();

	hooks->World->RegisterOnAnyWorldBeginPlay(OnAnyWorldBeginPlay);
	hooks->Engine->RegisterOnTick(OnTick);

	// A world may already be running (plugin init happens after the startup
	// map); treat it exactly like one that just began play.
	g_livePending = true;
	g_liveTicks   = 0;
	g_resyncTries = 0;
	g_installed   = true;

	LOG_INFO("[GatherableFix] Installed");
}

void GatherableRelevancyFix::Remove()
{
	if (!g_installed)
		return;

	if (auto* hooks = GetHooks())
	{
		if (hooks->World)  hooks->World->UnregisterOnAnyWorldBeginPlay(OnAnyWorldBeginPlay);
		if (hooks->Engine) hooks->Engine->UnregisterOnTick(OnTick);
	}
	g_livePending = false;
	g_resyncTicks = -1;
	g_installed   = false;
}
