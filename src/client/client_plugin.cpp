// ServerUtility, client half: the in-game admin tool. Reads no game values and
// touches no UObjects; it only exchanges plugin packets with the server half.
#include "plugin.h"
#include "plugin_helpers.h"

static IPluginSelf* g_self = nullptr;

// Used by the LOG_* macros in plugin_helpers.h
IPluginSelf* GetSelf() { return g_self; }

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "0.4"
#endif

static PluginInfo s_pluginInfo = {
	"ServerUtility",
	MODLOADER_BUILD_TAG,
	"AlienX",
	"In-game admin tool for ServerUtility servers",
	PLUGIN_INTERFACE_VERSION,
	PLUGIN_TARGET_CLIENT,
	PLUGIN_GAME_BUILT_CL,
	PLUGIN_MAP_MIGRATED
};

extern "C" {
__declspec(dllexport) PluginInfo* GetPluginInfo()
{
	return &s_pluginInfo;
}

__declspec(dllexport) void OnPluginLoadHooks(IPluginSelf* self, IPluginHookScanner* scanner)
{
	(void)scanner;
	g_self = self;
}

__declspec(dllexport) bool PluginInit(IPluginSelf* self)
{
	g_self = self;

	// First, before any early return: off its game build the loader refuses a
	// migrated plugin whose PluginInit did not ask for the Game-SDK globals.
	if (self->hooks && self->hooks->Engine && self->hooks->Engine->GetSdkGlobals)
		self->hooks->Engine->GetSdkGlobals();

	LOG_INFO("Admin client initialising...");
	return true;
}

__declspec(dllexport) void PluginShutdown()
{
	g_self = nullptr;
}
}
