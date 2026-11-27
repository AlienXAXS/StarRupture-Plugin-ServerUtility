#include "plugin.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "game_signatures.h"
#include "scan_helpers.h"
#include "server_utility_fields.h"
#include "hooks/parse_settings/parse_settings.h"
#include "hooks/max_players/max_players.h"
#include "hooks/auto_profession/auto_profession.h"
#include "hooks/http_connection/http_connection.h"
#include "hooks/gatherable_relevancy/gatherable_relevancy.h"
#include "rcon/rcon.h"
#include "rcon/commands/command_handler.h"
#include "rcon/commands/cmd_players.h"
#include "rcon/commands/cmd_save.h"
#include "rcon/commands/cmd_player.h"
#include "rcon/commands/cmd_hostrules.h"
#include "rcon/commands/cmd_stacksize.h"
#include "admin/admin_panel.h"
#include "host_rules/removal.h"
#include "host_rules/admin_claim.h"
#include "host_rules/ingame_admin.h"
#include "host_rules/player_list.h"
#include "host_rules/host_rules.h"
#ifdef MODLOADER_SERVER_BUILD
#include "cheats/cheat_net.h"
#include "cheats/cheat_ledger_game.h"
#include "cheats/op_give_game.h"
#include "cheats/op_wave_game.h"
#include "cheats/op_stack_game.h"
#endif

// -----------------------------------------------------------------------
// Global plugin self pointer
// -----------------------------------------------------------------------
static IPluginSelf* g_self = nullptr;

// Getter used by plugin_helpers.h macros and hook implementations
IPluginSelf* GetSelf() { return g_self; }

// -----------------------------------------------------------------------
// Plugin metadata
// -----------------------------------------------------------------------
#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "0.4"
#endif

static PluginInfo s_pluginInfo = {
	"ServerUtility",
	MODLOADER_BUILD_TAG,
	"AlienX",
	"Provides dedicated-server settings via command-line parameters, bypassing DSSettings.txt as well as other various fixes",
	PLUGIN_DECLARED_INTERFACE,
	PLUGIN_TARGET_SERVER,
	PLUGIN_GAME_BUILT_CL,
	PLUGIN_MAP_MIGRATED
};

// Resolved during OnPluginLoadHooks; 0 means the pattern missed on this build.
static uintptr_t g_parseSettingsAddr = 0;

// -----------------------------------------------------------------------
// Engine lifecycle callbacks
// -----------------------------------------------------------------------
static void OnEngineInit()
{
	// Start the RCON / Steam Query subsystem
	Rcon::Init();

	// Auto-assign professions for players joining when MaxPlayers is patched.
	// Here, not in PluginInit: it reads the profession by name from the loaded
	// enum, which needs the game's objects loaded: the engine init callback runs
	// on the loader's init thread, with the game thread parked until plugins load.
	if (ServerUtilityConfig::Config::GetMaxPlayers() > 0)
		AutoProfessionHook::Install();

	// Install FHttpConnection::ProcessRequest hook
	HttpConnectionHook::Install();

	// The rules are sent from the game thread, which runs once the plugins are loaded.
	HostRules::PushWhenGameThreadRuns();

	// Initialise admin web panel (HTTP routes)
	// This isnt ready yet.
	//AdminPanel::Init(g_self);
}

static void OnEngineShutdown()
{
	LOG_INFO("Engine shutting down - removing hooks...");
	AdminPanel::Shutdown(g_self);
#ifdef MODLOADER_SERVER_BUILD
	CheatNet::Shutdown();
	CheatLedgerGame::Shutdown();
	OpStackGame::Shutdown();
#endif
	InGameAdmin::Shutdown();
	AdminClaim::Shutdown();
	Removal::Shutdown();
	HostRules::Shutdown();
	ParseSettingsHook::Remove();
	MaxPlayersHook::Remove();
	AutoProfessionHook::Remove();
	HttpConnectionHook::Remove();
	GatherableRelevancyFix::Remove();

	// Shut down RCON / Steam Query (must happen before UObject teardown)
	Rcon::Shutdown();
}

static bool IsServerBinary()
{
	return g_self->hooks->Network->IsServer();
}

#ifdef MODLOADER_SERVER_BUILD
// Lets a loader with interface 83 or newer report its level, so the cheat reply send turns on.
PLUGIN_EXPORT_HOST_INFO()
#endif

// -----------------------------------------------------------------------
// Plugin exports
// -----------------------------------------------------------------------
extern "C" {
__declspec(dllexport) PluginInfo* GetPluginInfo()
{
	return &s_pluginInfo;
}

// Runs after GetPluginInfo and before PluginInit, and is the only context in
// which the loader lets a plugin pattern scan. Resolve here, install from
// PluginInit - self->hooks is null for the duration of this event.
__declspec(dllexport) void OnPluginLoadHooks(IPluginSelf* self, IPluginHookScanner* scanner)
{
	g_self = self;

	// Every game member the plugin reads, from the loader's map for the running
	// build. A miss is recorded with the loader, which refuses the plugin.
	ServerUtilityFields::Resolve(self, scanner);
#ifdef MODLOADER_SERVER_BUILD
	OpGiveGame::Resolve(self, scanner);
	OpStackGame::Resolve(self, scanner);
#endif

	// Optional: without it the DSSettings command-line bypass is skipped, but the
	// RCON server and the rest of the plugin still work.
	g_parseSettingsAddr = ScanUtil::ResolveFunction(self, scanner,
		ServerUtilityFields::ParseSettings,
		GameSig::DEDSERVER_SETTINGS_COMP_PARSE_SETTINGS);

	MaxPlayersHook::Resolve(self, scanner);
	AutoProfessionHook::Resolve(self, scanner);
	Cmd_Save::Resolve(self, scanner);
	GatherableRelevancyFix::Resolve(self, scanner);
}

__declspec(dllexport) bool PluginInit(IPluginSelf* self)
{
	// First, before any early return: off its game build the loader refuses a
	// migrated plugin whose PluginInit did not ask for the Game-SDK globals.
	PluginSdkRuntime::Init(self->hooks);

	g_self = self;

	LOG_INFO("Plugin initialising...");

	// Initialize config system with schema
	ServerUtilityConfig::Config::Initialize(self);

	if (!ServerUtilityConfig::Config::IsPluginEnabled())
	{
		LOG_INFO("Plugin is disabled in config - skipping initialization");
		return true;
	}

	if (!IsServerBinary())
	{
		LOG_WARN("This plugin is intended for dedicated server use only - skipping initialization");
		return true;
	}

	if (!g_self->hooks->Engine)
	{
		LOG_ERROR("Engine sub-interface not available - loader version mismatch?");
		return false;
	}

	// Console commands go into the mod loader's own registry, so they are
	// listed by `help` and reachable from the -console window as well as RCON.
	// `stop` is not among them: the loader already has one.
	Cmd_Players::Register();
	Cmd_Save::Register();

	PlayerList::Init();
	Cmd_Player::Register();
	HostRules::Init();
	Cmd_HostRules::Register();
	Cmd_StackSize::Register();
	Removal::Init();
	AdminClaim::Init();
	InGameAdmin::Init();
#ifdef MODLOADER_SERVER_BUILD
	CheatLedgerGame::Init();
	OpGiveGame::Init();
	OpWaveGame::Init();
	OpStackGame::Init();
	CheatNet::Init();
#endif

	g_self->hooks->Engine->RegisterOnInit(OnEngineInit);
	LOG_DEBUG("Registered for engine init callback");

	g_self->hooks->Engine->RegisterOnShutdown(OnEngineShutdown);
	LOG_DEBUG("Registered for engine shutdown callback");

	uintptr_t addr = g_parseSettingsAddr;
	if (addr == 0)
	{
		LOG_ERROR("UCrDedicatedServerSettingsComp::ParseSettings unresolved");
	}
	else
	{
		LOG_INFO("Found ParseSettings at 0x%llX", static_cast<unsigned long long>(addr));
		ParseSettingsHook::Install(addr);
	}

	// Apply max players patch if configured
	int maxPlayers = ServerUtilityConfig::Config::GetMaxPlayers();
	if (maxPlayers > 0)
	{
		LOG_INFO("MaxPlayers configured to %d - applying patch...", maxPlayers);
		MaxPlayersHook::Install(maxPlayers);
	}

	// Gatherable regrowth never reaches remote clients without this; see the
	// module header for why.
	if (ServerUtilityConfig::Config::GetGatherableRespawnFix())
		GatherableRelevancyFix::Install(ServerUtilityConfig::Config::GetGatherableSeedResync());
	else
		LOG_INFO("GatherableRespawnFix disabled in config");

	return true;
}

__declspec(dllexport) void PluginShutdown()
{
	LOG_INFO("Plugin shutting down...");

	// Hook removal and RCON shutdown are handled in OnEngineShutdown() which fires
	// before UObject teardown.  By the time PluginShutdown is called (explicit
	// FreeLibrary only) those resources have already been released.

	PluginCommands::UnregisterAll();

#ifdef MODLOADER_SERVER_BUILD
	CheatLedgerGame::Shutdown();
	OpStackGame::Shutdown();
#endif
	g_self = nullptr;
}
} // extern "C"
