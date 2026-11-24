// ServerUtility, client half: the in-game admin tool. Reads no game values and
// touches no UObjects; it only exchanges plugin packets with the server half.
#include "plugin.h"
#include "plugin_helpers.h"
#include "client/admin_client.h"
#include "client/admin_window.h"
#include "admin_wire/admin_wire.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <windows.h>

static IPluginSelf* g_self = nullptr;
static std::unique_ptr<AdminClient::Client> g_client;

// The state model for the window code (render callback): GetView() copies it out under its lock.
AdminClient::Client* GetAdminClient() { return g_client.get(); }

static double NowSeconds()
{
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Plugins\ServerUtility\admin-pairing.txt, next to this DLL.
static std::string PairingStorePath()
{
	HMODULE self = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        reinterpret_cast<LPCWSTR>(&PairingStorePath), &self))
		return std::string();
	wchar_t module[MAX_PATH] = {};
	if (GetModuleFileNameW(self, module, MAX_PATH) == 0) return std::string();
	return (std::filesystem::path(module).parent_path() / L"ServerUtility" / L"admin-pairing.txt").string();
}

static void OnAdminPacket(const char*, const char*, const uint8_t* data, size_t size)
{
	if (g_client) g_client->OnPacket(data, size);
}

// Game thread: the only place the client sends, so the render callback makes no loader calls.
static void OnTick(float)
{
	if (g_client) g_client->Flush();
}

static void OnServerReady(const char*)
{
	if (g_client) g_client->OnServerReady();   // hello only once the server can receive it
}

static void OnWorldEnd(SDK::UWorld*, const char*)
{
	if (g_client) g_client->OnWorldEnd();
}

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

	if (!self->hooks || !self->hooks->Network || !self->hooks->World || !self->hooks->Engine)
	{
		LOG_ERROR("Admin client: network or world interface missing");
		return false;
	}

	AdminClient::Env env;
	env.send = [](const std::string& msg)
	{
		IPluginSelf* s = g_self;
		if (s && s->hooks && s->hooks->Network)
			s->hooks->Network->SendPacketToServer(s, AdminWire::kPacketTag,
				reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
	};
	env.now = &NowSeconds;
	env.storePath = PairingStorePath();
	g_client = std::make_unique<AdminClient::Client>(env);

	self->hooks->Network->RegisterMessageHandler(self, AdminWire::kPacketTag, &OnAdminPacket);
	self->hooks->World->RegisterOnAfterWorldEndPlay(&OnWorldEnd);
	self->hooks->Engine->RegisterOnTick(&OnTick);
	// Fires at once when the server is already ready (hot reload).
	self->hooks->Network->RegisterServerReadyCallback(self, &OnServerReady);

	AdminWindow::Init(self);

	LOG_INFO("Admin client initialising...");
	return true;
}

__declspec(dllexport) void PluginShutdown()
{
	IPluginSelf* self = g_self;
	if (self) AdminWindow::Shutdown(self);   // the panel reads g_client: unregister it first
	if (self && self->hooks)
	{
		if (self->hooks->Network)
		{
			self->hooks->Network->UnregisterServerReadyCallback(self, &OnServerReady);
			self->hooks->Network->UnregisterMessageHandler(self, AdminWire::kPacketTag, &OnAdminPacket);
		}
		if (self->hooks->World) self->hooks->World->UnregisterOnAfterWorldEndPlay(&OnWorldEnd);
		if (self->hooks->Engine) self->hooks->Engine->UnregisterOnTick(&OnTick);
	}
	g_client.reset();
	g_self = nullptr;
}
}
