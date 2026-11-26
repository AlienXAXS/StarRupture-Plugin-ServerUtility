#include "ingame_admin.h"
#include "admin_ops.h"
#include "claim_model.h"
#include "host_rules.h"
#include "ini_file.h"
#include "known_features.h"
#include "rules_model.h"
#include "player_ids.h"
#include "player_list.h"
#include "../admin_wire/admin_wire.h"
#include "../plugin_helpers.h"
#ifdef MODLOADER_SERVER_BUILD
#include "../cheats/cheat_ledger_game.h"
#endif

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
#include <map>
#include <set>
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
		std::shared_ptr<InGameAdminModel::Server> g_model;
		std::vector<Connection>                   g_conns;
		ConnId                                    g_nextConn = 1;
		int                                       g_listener = 0;
		bool                                      g_handlerRegistered = false;
		bool                                      g_worldRegistered = false;
		KnownFeatures::PushGate                   g_catalogGate;   // game thread only

		double NowSeconds()
		{
			using namespace std::chrono;
			return duration<double>(steady_clock::now().time_since_epoch()).count();
		}

		// A copy of the owning pointer: a thread that is inside the model (RCON, the panel)
		// keeps it alive, so Shutdown cannot free it under that call.
		std::shared_ptr<InGameAdminModel::Server> ModelPtr()
		{
			std::lock_guard<std::mutex> lk(g_mutex);
			return g_model;
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

		constexpr int kMaxControllers = 128;

		std::string FlagsText(const PlayerList::Entry& e)
		{
			std::string flags;
			auto add = [&](const std::string& f) { flags += (flags.empty() ? "" : " ") + f; };
			if (e.admin) add("admin");
			if (e.exemptVersion) add("exemptversion");
			if (e.exemptRules) add("exemptrules");
			const std::string sections = PlayerList::SectionsFlag("sections", e.sections);
			if (!sections.empty()) add(sections);
			return flags;
		}

		// Game thread. The state a snapshot carries: rules, minimums, the mods the
		// server has heard of, and the players (listed and connected).
		AdminWire::Snapshot BuildSnapshot()
		{
			AdminWire::Snapshot snap;
			const RulesModel::Model model = AdminOps::HostBackend().getModel();
			snap.defaultPolicy = model.defaultDeny ? "deny" : "allow";
			snap.message = model.message;
			for (const RulesModel::Rule& r : model.rules)
				snap.rules.push_back({ r.plugin + "." + r.id, r.adminsOnly ? "admins" : r.deny ? "deny" : "allow" });
			for (const RulesModel::DefaultRow& d : RulesModel::DefaultRows())
				snap.defaults.push_back(std::string(d.plugin) + "." + d.id);
			if (!model.minLoaderVersion.empty())
				snap.mins.push_back({ std::string(), model.minLoaderVersion });
			for (const RulesModel::ModMin& m : model.modMins)
				snap.mins.push_back({ m.plugin, m.version });

			std::map<std::string, size_t> modIndex;   // plugin -> index in snap.mods
			std::vector<std::vector<KnownFeatures::Entry>> modRows;   // the same order
			for (const KnownFeatures::Entry& e : KnownFeatures::Snapshot())
			{
				const auto it = modIndex.find(e.plugin);
				size_t at;
				if (it == modIndex.end())
				{
					at = snap.mods.size();
					modIndex[e.plugin] = at;
					AdminWire::ModInfo info;
					info.plugin = e.plugin;
					snap.mods.push_back(info);
					modRows.emplace_back();
				}
				else
					at = it->second;
				AdminWire::ModInfo& info = snap.mods[at];
				modRows[at].push_back(e);
				info.features.push_back({ e.id, e.title.empty() ? e.id : e.title });
			}
			for (size_t i = 0; i < snap.mods.size(); ++i)
				snap.mods[i].version = KnownFeatures::ModVersion(modRows[i]);

			std::set<std::string> online;
			SDK::APlayerController* controllers[kMaxControllers] = {};
			const int count = HostRules::ConnectedControllers(controllers, kMaxControllers);
			for (int i = 0; i < count; ++i)
				if (controllers[i])
				{
					const std::string id = PlayerIds::OfController(controllers[i]);
					if (!id.empty()) online.insert(id);
				}

			std::set<std::string> listed;
			auto addPlayer = [&](const std::string& id, const std::string& label, const std::string& flags)
			{
				AdminWire::PlayerInfo p;
				p.id = id;
				p.name = label.empty() ? id : label;
				p.flags = flags;
				p.online = online.count(id) > 0;
				if (p.online)
				{
					std::string text;
					if (HostRules::ClientFeatures(id, text) == HostRules::FeaturesState::Ok)
					{
						std::map<std::string, std::string> seen;   // plugin -> version
						for (const KnownFeatures::Report& r : KnownFeatures::ParseReportText(text, std::string()))
						{
							std::string& v = seen[r.plugin];
							if (v.empty()) v = r.version;
						}
						for (const auto& kv : seen)
							p.mods.push_back({ kv.first, kv.second });
					}
				}
				snap.players.push_back(p);
			};
			for (const PlayerList::Entry& e : PlayerList::All())
			{
				listed.insert(e.id);
				addPlayer(e.id, e.label, FlagsText(e));
			}
			for (const std::string& id : online)
				if (!listed.count(id))
					addPlayer(id, std::string(), std::string());
			return snap;
		}

		InGameAdminModel::OpsOutcome RunOps(const std::string& id, const std::string& name,
		                                    const std::vector<AdminWire::Op>& wireOps, uint32_t baseRevision)
		{
			std::vector<AdminOps::Op> ops;
			for (const AdminWire::Op& o : wireOps)
				ops.push_back({ o.name, o.args });
			const AdminOps::Result r = AdminOps::Run(AdminOps::Caller::FromPlayer(id, name), ops, baseRevision);
			InGameAdminModel::OpsOutcome out;
			out.ok = r.ok;
			out.stale = r.stale;
			out.text = r.text;
			out.revision = static_cast<uint32_t>(r.revision);
			return out;
		}

		std::string SectionsText(unsigned sections)
		{
			std::string text;
			auto add = [&](const char* s) { text += (text.empty() ? "" : ",") + std::string(s); };
			if (sections & AdminOps::kSectionRules) add("rules");
			if (sections & AdminOps::kSectionMins) add("mins");
			if (sections & AdminOps::kSectionPlayers) add("players");
			if (sections & AdminOps::kSectionMods) add("mods");
			return text.empty() ? std::string(AdminWire::kSectionAll) : text;
		}

		// Game thread: a change was made, tell every proven admin connection.
		void PushChanged(uint32_t revision, unsigned sections, const std::vector<InGameAdminModel::Out>& challenges)
		{
			const std::shared_ptr<InGameAdminModel::Server> model = ModelPtr();
			if (!model) return;
			for (const InGameAdminModel::Out& out : challenges)
				SendToConnection(out.conn, out.data);
			for (const InGameAdminModel::Out& out : model->OnChanged(SectionsText(sections), revision))
				SendToConnection(out.conn, out.data);
		}

		struct ChangedJob { uint32_t revision; unsigned sections; std::vector<InGameAdminModel::Out> challenges; };

		void ChangedOnGameThread(void* context)
		{
			std::unique_ptr<ChangedJob> job(static_cast<ChangedJob*>(context));
			PushChanged(job->revision, job->sections, job->challenges);
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
			const std::shared_ptr<InGameAdminModel::Server> model = ModelPtr();
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
#ifdef MODLOADER_SERVER_BUILD
			CheatLedgerGame::OnWorldEnd();
#endif
			// Travel: proven state and connection numbers start over.
			if (const auto model = ModelPtr())
				model->OnWorldEnd();
			std::lock_guard<std::mutex> lk(g_mutex);
			g_conns.clear();
		}

		std::string StorePath()
		{
			return std::filesystem::path(IniFile::DataPath(L"admin-pairing.txt")).string();
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
		env.canProve  = [](const std::string& id) { PlayerList::Entry e; return PlayerList::Find(id, e) && e.NeedsProof(); };
		env.proofChanged = [] { HostRules::RefreshPlayers(); };
		env.proofRejected = [](ConnId conn)
		{
			ObjectRef<SDK::APlayerController> ref;
			{
				std::lock_guard<std::mutex> lk(g_mutex);
				const auto it = std::find_if(g_conns.begin(), g_conns.end(), [&](const Connection& c) { return c.id == conn; });
				if (it == g_conns.end()) return;
				ref = it->ref;
			}
			if (SDK::APlayerController* pc = ref.Get())
				HostRules::EndAdminHold(pc);
		};
		env.revision  = [] { return static_cast<uint32_t>(AdminOps::Revision()); };
		env.now       = &NowSeconds;
		env.audit     = [](const std::string& line) { LOG_INFO("%s", line.c_str()); };
		env.storePath = StorePath();
		env.snapshot  = &BuildSnapshot;
		env.runOps    = &RunOps;
		env.catalogPush = [](const std::vector<std::string>& rows)
		{
			// The mods list only; the ops revision stays. A paced batch is many messages, so the
			// admins are told at most once per KnownFeatures::PushGate::kIntervalMs.
			if (KnownFeatures::NoteDeclaredRows(rows) && g_catalogGate.Changed(GetTickCount64()))
				AdminOps::NoteCatalogChanged();
		};

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

		g_listener = AdminOps::AddListener([](uint64_t revision, unsigned sections)
		{
			std::vector<InGameAdminModel::Out> challenges;
			if (sections & AdminOps::kSectionPlayers)
				if (const auto m = ModelPtr())
					challenges = m->OnPlayersChanged();

			// The push touches the controllers, so it runs on the game thread.
			auto* hooks = GetHooks();
			if (!hooks || !hooks->Engine) return;
			if (hooks->Engine->IsGameThread())
				PushChanged(static_cast<uint32_t>(revision), sections, challenges);
			else
				hooks->Engine->PostToGameThread(&ChangedOnGameThread, new ChangedJob{ static_cast<uint32_t>(revision), sections, std::move(challenges) });
		});

		// An ID that lost the Admin flag while the server was down loses its secret now.
		if (const auto m = ModelPtr())
			m->OnPlayersChanged();

		if (!Enabled())
			LOG_INFO("[Admin] In-game admin tool is off ([Policy] InGameAdmin=0)");
	}

	void Tick(uint64_t nowMs)
	{
		if (g_catalogGate.Due(nowMs))
			AdminOps::NoteCatalogChanged();
	}

	void Shutdown()
	{
		auto* hooks = GetHooks();
		g_catalogGate = KnownFeatures::PushGate();
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
		const std::shared_ptr<InGameAdminModel::Server> model = ModelPtr();
		if (!model) { error = "The in-game admin tool is not running on this server"; return false; }
		if (!Enabled()) { error = "The in-game admin tool is off ([Policy] InGameAdmin=0)"; return false; }
		return model->IssueCode(playerId, code, error);
	}

	bool Unpair(const std::string& playerId)
	{
		const std::shared_ptr<InGameAdminModel::Server> model = ModelPtr();
		return model && model->Unpair(playerId);
	}

	bool IsProven(void* playerController, const std::string& playerId)
	{
		const std::shared_ptr<InGameAdminModel::Server> model = ModelPtr();
		if (!model || !playerController || playerId.empty() || !Enabled()) return false;
		const ObjectRef<SDK::APlayerController> ref(static_cast<SDK::APlayerController*>(playerController));
		ConnId conn = 0;
		{
			std::lock_guard<std::mutex> lk(g_mutex);
			for (const Connection& c : g_conns)
				if (c.ref == ref)
				{
					conn = c.id;
					break;
				}
		}
		return conn && model->IsProvenAs(conn, playerId);
	}

	bool HasSecret(const std::string& playerId)
	{
		const std::shared_ptr<InGameAdminModel::Server> model = ModelPtr();
		return model && !playerId.empty() && model->HasSecret(playerId);
	}

	void OnClaimed(void* playerController, const std::string& playerId)
	{
		const std::shared_ptr<InGameAdminModel::Server> model = ModelPtr();
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
		const std::shared_ptr<InGameAdminModel::Server> model = ModelPtr();
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
}
