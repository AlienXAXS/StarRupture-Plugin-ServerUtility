#include "ingame_admin.h"
#include "admin_ops.h"
#include "claim_model.h"
#include "host_rules.h"
#include "ini_file.h"
#include "player_ids.h"
#include "player_list.h"
#include "../admin_wire/admin_wire.h"
#include "../plugin_helpers.h"

#include "SDK/Engine_classes.hpp"
#include "object_ref.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

namespace InGameAdmin
{
	namespace
	{
		using InGameAdminModel::ConnId;

		struct Connection
		{
			ConnId                            id = 0;
			ObjectRef<SDK::APlayerController> ref;   // weak: Get() before every use
		};

		std::mutex                                g_mutex;   // the model pointer and the connection list
		std::unique_ptr<InGameAdminModel::Server> g_model;
		std::vector<Connection>                   g_conns;
		ConnId                                    g_nextConn = 1;
		int                                       g_listener = 0;
		bool                                      g_handlerRegistered = false;
		bool                                      g_worldRegistered = false;

		double NowSeconds()
		{
			using namespace std::chrono;
			return duration<double>(steady_clock::now().time_since_epoch()).count();
		}

		InGameAdminModel::Server* ModelPtr()
		{
			std::lock_guard<std::mutex> lk(g_mutex);
			return g_model.get();
		}

		// Game thread. The connection number for this controller (made on first sight);
		// entries whose controller is gone are dropped first.
		ConnId ConnectionFor(SDK::APlayerController* pc, std::vector<ConnId>& dropped)
		{
			std::lock_guard<std::mutex> lk(g_mutex);
			for (size_t i = 0; i < g_conns.size();)
			{
				if (g_conns[i].ref.Get() == nullptr)
				{
					dropped.push_back(g_conns[i].id);
					g_conns.erase(g_conns.begin() + i);
				}
				else
					++i;
			}

			const ObjectRef<SDK::APlayerController> ref(pc);
			for (const Connection& c : g_conns)
				if (c.ref == ref)
					return c.id;

			Connection c;
			c.id = g_nextConn++;
			c.ref = ref;
			g_conns.push_back(c);
			return c.id;
		}

		void SendRaw(SDK::APlayerController* pc, const std::string& message)
		{
			auto* hooks = GetHooks();
			if (!hooks || !hooks->Network || !GetSelf() || !pc)
				return;
			hooks->Network->SendPacketToClient(pc, GetSelf(), AdminWire::kPacketTag,
				reinterpret_cast<const uint8_t*>(message.data()), message.size());
		}

		std::string NameOf(const std::string& id)
		{
			PlayerList::Entry e;
			if (PlayerList::Find(id, e) && !e.label.empty())
				return e.label;
			return id;
		}

		// The loader calls this on the game thread with the sender's controller; the
		// pointer is used only inside this call.
		void OnPacket(void* sender, const char*, const char*, const uint8_t* data, size_t size)
		{
			auto* hooks = GetHooks();
			if (!hooks || !hooks->Engine || !hooks->Engine->IsGameThread() || !sender || !data)
				return;
			InGameAdminModel::Server* model = ModelPtr();
			if (!model)
				return;

			auto* pc = static_cast<SDK::APlayerController*>(sender);
			std::vector<ConnId> dropped;
			const ConnId conn = ConnectionFor(pc, dropped);
			for (ConnId d : dropped)
				model->OnDisconnect(d);

			const std::string id = PlayerIds::OfController(sender);
			const std::string name = id.empty() ? std::string() : NameOf(id);
			for (const std::string& reply : model->OnPacket(conn, id, name, data, size))
				SendRaw(pc, reply);
		}

		void OnWorldEnd(SDK::UWorld*, const char*)
		{
			// Travel: proven state and connection numbers start over.
			if (InGameAdminModel::Server* model = ModelPtr())
				model->OnWorldEnd();
			std::lock_guard<std::mutex> lk(g_mutex);
			g_conns.clear();
		}

		std::string StorePath()
		{
			const std::wstring& iniPath = IniFile::Path();
			if (iniPath.empty())
				return std::string();
			return (std::filesystem::path(iniPath).parent_path().parent_path() / L"ServerUtility" / L"admin-pairing.txt").string();
		}
	}

	bool Enabled()
	{
		return ClaimModel::InGameAdminOn(HostRules::Get().policyKept);
	}

	void Init()
	{
		auto* hooks = GetHooks();
		if (!hooks || !hooks->Network || !GetSelf())
		{
			LOG_INFO("[Admin] No plugin network on this mod loader - the in-game admin tool is off");
			return;
		}

		InGameAdminModel::Env env;
		env.enabled   = [] { return Enabled(); };
		env.isAdmin   = [](const std::string& id) { PlayerList::Entry e; return PlayerList::Find(id, e) && e.admin; };
		env.revision  = [] { return static_cast<uint32_t>(AdminOps::Revision()); };
		env.now       = &NowSeconds;
		env.audit     = [](const std::string& line) { LOG_INFO("%s", line.c_str()); };
		env.storePath = StorePath();

		auto model = std::make_unique<InGameAdminModel::Server>(env);
		if (!model->Ready())
			LOG_ERROR("[Admin] The in-game admin tool could not make its server id - it stays off");
		{
			std::lock_guard<std::mutex> lk(g_mutex);
			g_model = std::move(model);
		}

		hooks->Network->RegisterServerMessageHandler(GetSelf(), AdminWire::kPacketTag, &OnPacket);
		g_handlerRegistered = true;
		if (hooks->World)
		{
			hooks->World->RegisterOnAfterWorldEndPlay(&OnWorldEnd);
			g_worldRegistered = true;
		}

		g_listener = AdminOps::AddListener([](uint64_t, unsigned sections)
		{
			if ((sections & AdminOps::kSectionPlayers) == 0)
				return;
			if (InGameAdminModel::Server* m = ModelPtr())
				m->OnPlayersChanged();
		});

		// An ID that lost the Admin flag while the server was down loses its secret now.
		if (InGameAdminModel::Server* m = ModelPtr())
			m->OnPlayersChanged();

		if (!Enabled())
			LOG_INFO("[Admin] In-game admin tool is off ([Policy] InGameAdmin=0)");
	}

	void Shutdown()
	{
		auto* hooks = GetHooks();
		if (g_listener)
			AdminOps::RemoveListener(g_listener);
		g_listener = 0;
		if (hooks && GetSelf())
		{
			if (g_handlerRegistered && hooks->Network)
				hooks->Network->UnregisterServerMessageHandler(GetSelf(), AdminWire::kPacketTag, &OnPacket);
			if (g_worldRegistered && hooks->World)
				hooks->World->UnregisterOnAfterWorldEndPlay(&OnWorldEnd);
		}
		g_handlerRegistered = false;
		g_worldRegistered = false;

		std::lock_guard<std::mutex> lk(g_mutex);
		g_conns.clear();
		g_model.reset();
	}

	bool IssueCode(const std::string& playerId, std::string& code, std::string& error)
	{
		InGameAdminModel::Server* model = ModelPtr();
		if (!model) { error = "The in-game admin tool is not running on this server"; return false; }
		if (!Enabled()) { error = "The in-game admin tool is off ([Policy] InGameAdmin=0)"; return false; }
		return model->IssueCode(playerId, code, error);
	}

	bool Unpair(const std::string& playerId)
	{
		InGameAdminModel::Server* model = ModelPtr();
		return model && model->Unpair(playerId);
	}

	void OnClaimed(void* playerController, const std::string& playerId)
	{
		InGameAdminModel::Server* model = ModelPtr();
		if (!model || !playerController) return;
		std::vector<ConnId> dropped;
		const ConnId claimant = ConnectionFor(static_cast<SDK::APlayerController*>(playerController), dropped);
		for (ConnId d : dropped)
			model->OnDisconnect(d);
		for (const InGameAdminModel::Out& out : model->OnClaimed(claimant, playerId))
			SendToConnection(out.conn, out.data);
	}

	void PlayerLeft(void* playerController)
	{
		InGameAdminModel::Server* model = ModelPtr();
		if (!model || !playerController) return;
		const ObjectRef<SDK::APlayerController> ref(static_cast<SDK::APlayerController*>(playerController));
		ConnId gone = 0;
		{
			std::lock_guard<std::mutex> lk(g_mutex);
			for (size_t i = 0; i < g_conns.size(); ++i)
				if (g_conns[i].ref == ref)
				{
					gone = g_conns[i].id;
					g_conns.erase(g_conns.begin() + static_cast<std::ptrdiff_t>(i));
					break;
				}
		}
		if (gone) model->OnDisconnect(gone);
	}

	InGameAdminModel::Server* Model()
	{
		return ModelPtr();
	}

	// Game thread. Looks the controller up again; nothing is sent when it is gone.
	bool SendToConnection(ConnId conn, const std::string& message)
	{
		ObjectRef<SDK::APlayerController> ref;
		{
			std::lock_guard<std::mutex> lk(g_mutex);
			const auto it = std::find_if(g_conns.begin(), g_conns.end(), [&](const Connection& c) { return c.id == conn; });
			if (it == g_conns.end()) return false;
			ref = it->ref;
		}
		SDK::APlayerController* pc = ref.Get();
		if (!pc) return false;
		SendRaw(pc, message);
		return true;
	}

	void SendToProven(const std::string& message)
	{
		InGameAdminModel::Server* model = ModelPtr();
		if (!model) return;
		for (ConnId conn : model->ProvenConnections())
			SendToConnection(conn, message);
	}
}
