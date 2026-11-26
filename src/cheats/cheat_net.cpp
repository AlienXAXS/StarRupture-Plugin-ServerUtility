#include "cheat_net.h"
#include "cheat_authority.h"
#include "cheat_dispatch.h"
#include "host_rules/player_ids.h"
#include "plugin_helpers.h"

#include <chrono>
#include <memory>

namespace CheatNet
{
	namespace
	{
		std::unique_ptr<CheatDispatch::Server> g_server;   // game thread only
		bool g_registered = false;

		double NowSeconds()
		{
			using namespace std::chrono;
			return duration<double>(steady_clock::now().time_since_epoch()).count();
		}

#if PLUGIN_OPTIONAL_INTERFACE >= 83
		PluginSendToClientPluginFunc g_send = nullptr;

		// The loader runs this on the game thread (UControlChannel::ReceivedBunch detour,
		// network_channel.cpp "Wire receive"), with the sender's controller. The pointer is used
		// inside this call only.
		void OnPacket(void* sender, const char*, const char*, const uint8_t* data, size_t size)
		{
			auto* hooks = GetHooks();
			if (!hooks || !hooks->Engine || !hooks->Engine->IsGameThread() || !sender || !data || !g_server)
				return;

			const std::string id = PlayerIds::OfController(sender);
			if (id.empty())
				return;

			const std::string reply = g_server->Handle(sender, id, data, size);
			if (reply.empty())
				return;
			g_send(sender, GetSelf(), CheatWire::kPeerPlugin, CheatWire::kPacketTag,
				reinterpret_cast<const uint8_t*>(reply.data()), reply.size());
		}
#endif
	}

	void Init()
	{
		auto* hooks = GetHooks();
		if (!hooks || !hooks->Network || !GetSelf())
			return;

#if PLUGIN_OPTIONAL_INTERFACE >= 83
		g_send = PluginOptSendToClientPlugin(hooks->Network);
		if (g_send)
		{
			CheatDispatch::Env env;
			env.check = [](void* pc, const std::string& id, const char* feature) { return CheatAuthority::Check(pc, id, feature); };
			env.now = &NowSeconds;
			env.log = [](const std::string& line) { LOG_INFO("%s", line.c_str()); };
			env.suVersion = GetSelf()->version;
			g_server = std::make_unique<CheatDispatch::Server>(env);

			hooks->Network->RegisterServerMessageHandler(GetSelf(), CheatWire::kPacketTag, &OnPacket);
			g_registered = true;
			return;
		}
#endif
		LOG_INFO("[Cheat] BetterCheats requests need ModLoader 1.28.0 on the server - they are off");
	}

	void Shutdown()
	{
#if PLUGIN_OPTIONAL_INTERFACE >= 83
		auto* hooks = GetHooks();
		if (g_registered && hooks && hooks->Network && GetSelf())
			hooks->Network->UnregisterServerMessageHandler(GetSelf(), CheatWire::kPacketTag, &OnPacket);
#endif
		g_registered = false;
		g_server.reset();
	}
}
