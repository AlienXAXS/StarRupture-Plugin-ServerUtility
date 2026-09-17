#include "gatherable_relevancy.h"
#include "plugin_helpers.h"
#include "game_signatures.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <vector>

#if defined(MODLOADER_SERVER_BUILD) || defined(MODLOADER_CLIENT_BUILD)
#  include "SDK/Engine_classes.hpp"
#  include "SDK/Chimera_classes.hpp"
#  define GATHERABLE_FIX_HAS_SDK 1
#else
#  define GATHERABLE_FIX_HAS_SDK 0
#endif

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
using RepGraphActorFn = void(__fastcall*)(void* graph, void* actor);

static RepGraphActorFn g_addNetworkActor    = nullptr;
static RepGraphActorFn g_removeNetworkActor = nullptr;

static bool g_installed   = false;
static bool g_livePending = false;   // a world came up; the live-graph half still has to run
static int  g_liveTicks   = 0;       // ticks spent waiting for the rep actor to appear

// The rep actor is spawned in UCrGatherableSpawnersSubsystem::OnWorldBeginPlay
// and the graph adds it on spawn. Our world-begin-play hook is on a different
// subsystem, so on a given frame it may not exist yet; a few seconds of ticks
// is far more than it needs and bounds the retry if something is genuinely
// missing (a world with no gatherable subsystem, say).
static constexpr int kMaxLiveTicks = 600;

// ---------------------------------------------------------------------------
// Resolve
// ---------------------------------------------------------------------------
void GatherableRelevancyFix::Resolve(IPluginSelf* self, IPluginHookScanner* scanner)
{
	if (!self || !scanner)
		return;

	g_addNetworkActor = reinterpret_cast<RepGraphActorFn>(scanner->ResolveOptional(
		self, "UReplicationGraph::AddNetworkActor", GameSig::REPGRAPH_ADD_NETWORK_ACTOR));
	g_removeNetworkActor = reinterpret_cast<RepGraphActorFn>(scanner->ResolveOptional(
		self, "UReplicationGraph::RemoveNetworkActor", GameSig::REPGRAPH_REMOVE_NETWORK_ACTOR));
}

#if GATHERABLE_FIX_HAS_SDK

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// FUObjectItem::SerialNumber for an object index. FObjectKey is
// {ObjectIndex, ObjectSerialNumber} and the map is keyed on both, so the
// serial has to come from the same place the engine read it: GUObjectArray.
static int32_t GetObjectSerialNumber(int32_t index)
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

	const auto* item = reinterpret_cast<const uint8_t*>(&items[inChunk]);
	int32_t serial = 0;
	std::memcpy(&serial, item + GameSig::UObjectItem::OFFSET_SERIAL_NUMBER, sizeof(serial));
	return serial;
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
		if (!obj || !obj->Class || obj->IsDefaultObject())
			continue;
		if (obj->Class->GetName() == className)
			out.push_back(static_cast<T*>(obj));
	}
	return out;
}

// Rewrites the graph's cached routing policy for `cls`.
//
// UCrReplicationGraph::ClassRepNodePolicies is a TClassMap: a TFunction
// followed by TMap<FObjectKey, EClassRepNodeMapping>. Neither is a UPROPERTY,
// so the SDK shows only padding; the map's position and the 20-byte element
// (FObjectKey 8, value 4, HashNextId 4, HashIndex 4) come from the class
// constructor and PrintRepNodePolicies in the server binary -- see
// game_signatures.h. Returns the number of entries changed.
static int PatchClassPolicy(SDK::UCrReplicationGraph* graph, SDK::UClass* cls, uint32_t newValue)
{
	namespace RG = GameSig::ReplicationGraph;

	const auto base = reinterpret_cast<uint8_t*>(graph);

	uint8_t* elements = nullptr;
	int32_t  arrayNum = 0, arrayMax = 0, numBits = 0;
	std::memcpy(&elements, base + RG::OFFSET_POLICIES_ELEMENTS,  sizeof(elements));
	std::memcpy(&arrayNum, base + RG::OFFSET_POLICIES_ARRAY_NUM, sizeof(arrayNum));
	std::memcpy(&arrayMax, base + RG::OFFSET_POLICIES_ARRAY_MAX, sizeof(arrayMax));
	std::memcpy(&numBits,  base + RG::OFFSET_POLICIES_NUM_BITS,  sizeof(numBits));

	// The one sanity check that catches a wrong offset before it is trusted:
	// a TSparseArray keeps these three consistent, garbage does not.
	if (!elements || arrayNum < 0 || arrayNum > arrayMax || arrayMax > 65536 || numBits != arrayNum)
	{
		LOG_ERROR("[GatherableFix] ClassRepNodePolicies at graph+0x%zX does not look like a TMap "
		          "(data=%p num=%d max=%d bits=%d) - offsets need re-deriving for this build",
		          RG::OFFSET_POLICIES_ELEMENTS, static_cast<void*>(elements), arrayNum, arrayMax, numBits);
		return -1;
	}

	// Allocation bits: TInlineAllocator<4> words, or a heap block once the
	// sparse array outgrows 128 slots.
	const uint32_t* bits = reinterpret_cast<const uint32_t*>(base + RG::OFFSET_POLICIES_BITS_INLINE);
	if (numBits > RG::POLICIES_BITS_INLINE_CAPACITY)
	{
		const uint32_t* heapBits = nullptr;
		std::memcpy(&heapBits, base + RG::OFFSET_POLICIES_BITS_SECONDARY, sizeof(heapBits));
		if (!heapBits)
		{
			LOG_ERROR("[GatherableFix] ClassRepNodePolicies has %d slots but no allocation bits", numBits);
			return -1;
		}
		bits = heapBits;
	}

	const int32_t wantIndex  = cls->Index;
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
	for (int32_t i = 0; i < arrayNum; ++i)
	{
		if ((bits[i / 32] & (1u << (i % 32))) == 0)
			continue;   // free slot

		uint8_t* el = elements + static_cast<size_t>(i) * RG::POLICIES_ELEMENT_SIZE;
		int32_t keyIndex = 0, keySerial = 0;
		uint32_t value = 0;
		std::memcpy(&keyIndex,  el + RG::POLICIES_KEY_OBJECT_INDEX,  sizeof(keyIndex));
		std::memcpy(&keySerial, el + RG::POLICIES_KEY_SERIAL_NUMBER, sizeof(keySerial));
		std::memcpy(&value,     el + RG::POLICIES_VALUE,             sizeof(value));

		if (keyIndex != wantIndex)
			continue;
		if (keySerial != wantSerial)
		{
			LOG_WARN("[GatherableFix] Policy entry for object index %d has serial %d, expected %d - skipping",
			         keyIndex, keySerial, wantSerial);
			continue;
		}

		LOG_INFO("[GatherableFix] Graph %s: %s policy %u -> %u",
		         graph->GetName().c_str(), cls->GetName().c_str(), value, newValue);
		std::memcpy(el + RG::POLICIES_VALUE, &newValue, sizeof(newValue));
		++changed;
	}

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

	if (cdo->bAlwaysRelevant)
	{
		LOG_INFO("[GatherableFix] CDO already bAlwaysRelevant - the game fixed this? Nothing to do for new graphs");
		return;
	}

	cdo->bAlwaysRelevant = 1;
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
		actor->bAlwaysRelevant = 1;

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
// Callbacks
// ---------------------------------------------------------------------------
static void OnAnyWorldBeginPlay(SDK::UWorld* /*world*/, const char* worldName)
{
	LOG_DEBUG("[GatherableFix] World '%s' began play - scheduling live graph check", worldName ? worldName : "?");
	g_livePending = true;
	g_liveTicks   = 0;
}

static void OnTick(float /*deltaSeconds*/)
{
	if (!g_livePending)
		return;

	if (ApplyToLiveGraph())
	{
		g_livePending = false;
		return;
	}

	if (++g_liveTicks >= kMaxLiveTicks)
	{
		LOG_WARN("[GatherableFix] ACrGatherableSpawnersRepActor never appeared after %d ticks - giving up "
		         "until the next world (the CDO half still applies)", g_liveTicks);
		g_livePending = false;
	}
}

#endif // GATHERABLE_FIX_HAS_SDK

// ---------------------------------------------------------------------------
// Install / Remove
// ---------------------------------------------------------------------------
void GatherableRelevancyFix::Install()
{
#if GATHERABLE_FIX_HAS_SDK
	if (g_installed)
		return;

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
	g_installed   = true;

	LOG_INFO("[GatherableFix] Installed");
#else
	LOG_WARN("[GatherableFix] Built without the game SDK - fix unavailable in this configuration");
#endif
}

void GatherableRelevancyFix::Remove()
{
#if GATHERABLE_FIX_HAS_SDK
	if (!g_installed)
		return;

	if (auto* hooks = GetHooks())
	{
		if (hooks->World)  hooks->World->UnregisterOnAnyWorldBeginPlay(OnAnyWorldBeginPlay);
		if (hooks->Engine) hooks->Engine->UnregisterOnTick(OnTick);
	}
	g_livePending = false;
	g_installed   = false;
#endif
}
