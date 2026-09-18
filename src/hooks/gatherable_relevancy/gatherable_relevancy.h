#pragma once

// ---------------------------------------------------------------------------
// Gatherable respawn fix for remote clients
//
// Gatherables (plants, eggs, golden balloons -- anything that goes through
// UCrGatherableSpawnersSubsystem::RegisterDepletedGatherable) only regrow on
// enviro-wave stage transitions: the server rolls a new PCG seed, destroys the
// Mass entities for that stage and re-runs the spawner volumes. A *client*
// never binds to the wave delegates; it generates its own gatherables and its
// only trigger to regenerate is ACrGatherableSpawnersRepActor::
// OnRepGlobalGatherablePCGSeed, which carries the seed and the wave stage.
//
// That actor is spawned with bReplicates only -- no bAlwaysRelevant and no
// RootComponent, so it sits at (0,0,0). The packed DefaultGame.ini does not
// list it in UCrReplicationGraphSettings.ClassSettings, so the replication
// graph classifies it Spatialize_Dynamic at world origin, and it is only ever
// sent to a client standing within the default net cull distance of the
// origin. Everyone else keeps their pre-wave layout forever: gathered plants
// never come back, and after the first heat wave the client's seed no longer
// matches the server's. (A listen server's host is unaffected -- it *is* the
// server -- which is why single player and hosting look fine.)
//
// Two halves, because plugin init runs after FEngineLoop::Init and the game
// world may already be up:
//
//   1. bAlwaysRelevant on the class default object. Every replication graph
//      built from now on (each map load creates a fresh net driver and graph)
//      resolves the class to RelevantAllConnections in GetClassNodeMapping,
//      and spawned instances inherit the flag, which also covers the plain
//      AActor::IsNetRelevantFor path if the graph is ever disabled.
//
//   2. The graph that already exists cached the class as Spatialize_Dynamic in
//      its ClassRepNodePolicies map when it was built. That entry is rewritten
//      to RelevantAllConnections and the live rep actor is removed from and
//      re-added to the graph so RouteAddNetworkActorToNodes puts it in the
//      AlwaysRelevantNode. Done on the game thread once the actor exists
//      (it is spawned in the gatherable subsystem's OnWorldBeginPlay, whose
//      order relative to our own world-begin-play hook is not guaranteed, so
//      it retries for a few seconds of ticks).
//
// And a third step, because a relevant actor carrying the wrong seed is no
// better: after a save load the rep actor still holds its default seed (the
// saved one only lands in the subsystem, and only a wave stage change copies
// it across). The seed the server actually generated with is read back from
// its PCG spawner actors' LocalSeed and written onto the rep actor together
// with the Heat/Moving stage, which is what makes a client regenerate from
// it. See ResyncSeed().
//
// Verify with the engine console: `CrRepGraph.PrintRouting` prints
// `CrGatherableSpawnersRepActor --> RelevantAllConnections` once applied.
// ---------------------------------------------------------------------------
struct IPluginSelf;
struct IPluginHookScanner;

namespace GatherableRelevancyFix
{
	// Resolve UReplicationGraph::AddNetworkActor / RemoveNetworkActor. Callable
	// only from OnPluginLoadHooks. Both optional: without them only the live
	// graph patch is skipped; the CDO half still applies to every later map.
	void Resolve(IPluginSelf* self, IPluginHookScanner* scanner);

	// Apply the CDO half and register the world/tick callbacks for the live
	// half. Call from PluginInit. seedResync additionally forces one generation
	// pass per world so the loaded seed is pushed onto the rep actor.
	void Install(bool seedResync);

	// Unregister callbacks. Call from engine shutdown.
	void Remove();
}
