#include "host_rules.h"
#include "ini_file.h"
#include "known_features.h"
#include "player_ids.h"
#include "player_list.h"
#include "admin_claim.h"
#include "ingame_admin.h"
#include "../plugin_helpers.h"
#include "../rcon/listed_players.h"

#include "SDK/Engine_classes.hpp"
#include "object_ref.h"
#include "plugin_map.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <mutex>
#include <vector>

namespace
{
	std::mutex                g_mutex;
	RulesModel::Model         g_model;
	bool                      g_loadOk = true;    // false: the file exists but was not readable, so it is never written
	std::string               g_pushStatus = "not sent yet";
	std::string               g_minStatus = "not sent yet";

	struct Known { std::string id; HostRules::Reported info; };
	std::vector<Known>        g_known;            // players whose loader reported, by ID
	std::map<std::string, std::string> g_ownRules; // player ID -> own rules text last sent (g_mutex)

	std::string PushText(const std::string& text);

	std::string IdOf(void* playerController)
	{
		return PlayerIds::OfController(playerController);
	}

	// A controller whose ID is not readable yet is kept as a weak reference and
	// tried again each tick, until it resolves, dies or waits too long (RulesModel::kLateIdGraceMs). Game
	// thread only.
	struct Waiting
	{
		ObjectRef<SDK::APlayerController> pc;
		ULONGLONG                         sinceMs;
	};
	std::vector<Waiting>      g_waiting;
	bool                      g_tickRegistered = false;

	// Game thread. The feature text one controller's loader reported; false
	// when there is none.
	bool ReadFeatures(IPluginHostRules* api, void* playerController, std::string& text)
	{
		// Measure, then fetch; a second pass covers a text that grew in between.
		std::string buf;
		int need = api->GetClientFeatures(playerController, nullptr, 0);
		for (int pass = 0; pass < 3 && need >= 0; ++pass)
		{
			buf.assign(static_cast<size_t>(need) + 1, '\0');
			const int got = api->GetClientFeatures(playerController, buf.data(), buf.size());
			if (got < 0)
				return false;
			if (static_cast<size_t>(got) < buf.size())
			{
				buf.resize(static_cast<size_t>(got));
				text = buf;
				return true;
			}
			need = got;
		}
		return false;
	}

	// Note a player whose loader reported its feature list, and remember the
	// features in it. Game thread (where the loader's callback runs).
	void RecordReported(const std::string& id, void* playerController)
	{
		auto* hooks = GetHooks();
		std::string text;
		if (hooks && hooks->HostRules && ReadFeatures(hooks->HostRules, playerController, text))
			KnownFeatures::NoteReportText(text);

		std::lock_guard<std::mutex> lk(g_mutex);
		auto it = std::find_if(g_known.begin(), g_known.end(), [&](const Known& k) { return k.id == id; });
		if (it == g_known.end()) { g_known.push_back({ id, {} }); it = g_known.end() - 1; }
		++it->info.count;
		it->info.lastMs = GetTickCount64();
	}

	bool OnGameThread()
	{
		auto* hooks = GetHooks();
		return hooks && hooks->Engine && hooks->Engine->IsGameThread();
	}

	// A player's own rules are sent when a flag in the player list changes what
	// they get. The loader keeps that set until the player leaves or it is cleared.
	//
	// Game thread only. Send (or clear) one connection's own rules. force sends
	// even when the same text went out before: a loader report is a fresh start.
	void ApplyRules(void* playerController, const std::string& id, bool force)
	{
		auto* hooks = GetHooks();
		IPluginHostRules* api = hooks ? hooks->HostRules : nullptr;
		if (!api)
			return;

		// An ID alone is not proof: another player can claim it. The flags that loosen a rule
		// apply only to a connection that proved the pairing secret as this ID.
		RulesModel::PlayerFlags flags;
		PlayerList::Entry entry;
		if (PlayerList::Find(id, entry) && entry.NeedsProof())
			flags = RulesModel::FlagsFor(entry.ExemptsRules(), entry.sections, InGameAdmin::IsProven(playerController, id));

		const bool needsOwn = RulesModel::NeedsOwnRules(flags);
		std::string text, last;
		bool hadOwn = false;
		{
			std::lock_guard<std::mutex> lk(g_mutex);
			if (needsOwn)
				text = RulesModel::PlayerRulesText(g_model, flags, true);
			auto it = g_ownRules.find(id);
			if (it != g_ownRules.end()) { hadOwn = true; last = it->second; }
		}

		if (!needsOwn)
		{
			if (hadOwn)
			{
				api->ClearConnection(playerController);
				{
					std::lock_guard<std::mutex> lk(g_mutex);
					g_ownRules.erase(id);
				}
				LOG_INFO("[HostRules] Player %s is back on the server rules", id.c_str());
			}
			return;
		}

		if (!force && hadOwn && last == text)
			return;

		char err[256] = {};
		const bool ok = api->SetRulesForConnection(playerController, text.c_str(), err, sizeof(err));
		{
			std::lock_guard<std::mutex> lk(g_mutex);
			// A refused text leaves the loader's earlier set for this player as it was, so
			// the record stays, marked as unknown (it never equals a real text).
			g_ownRules[id] = ok ? text : std::string();
		}
		if (ok)
			LOG_INFO("[HostRules] Player %s has rules of their own", id.c_str());
		else
			LOG_WARN("[HostRules] Could not send rules for player %s: %s", id.c_str(), err);
	}

	void OnTick(float)
	{
		KnownFeatures::Tick(GetTickCount64());
		if (g_waiting.empty())
			return;

		const ULONGLONG now = GetTickCount64();
		for (size_t i = 0; i < g_waiting.size();)
		{
			SDK::APlayerController* pc = g_waiting[i].pc.Get();
			const std::string id = pc ? IdOf(pc) : std::string();
			const RulesModel::HeldStep step = RulesModel::NextHeldStep(pc != nullptr, !id.empty(), now - g_waiting[i].sinceMs);
			if (step == RulesModel::HeldStep::Wait)
			{
				++i;
				continue;
			}

			g_waiting.erase(g_waiting.begin() + i);
			if (step == RulesModel::HeldStep::ActNoId)
				LOG_WARN("[HostRules] A player's ID was not readable after %d seconds - the server rules apply to them",
				         static_cast<int>(RulesModel::kLateIdGraceMs / 1000));
			if (step != RulesModel::HeldStep::Act)
				continue;

			RecordReported(id, pc);
			ApplyRules(pc, id, true);
			AdminClaim::AskPlayer(pc, id);
		}
	}

	// Controllers of the connected players, in PlayerArray order. Reads engine
	// memory, so it has a function of its own (no objects with destructors, as
	// __try needs).
	int CollectControllers(void* worldPtr, SDK::APlayerController** out, int max)
	{
		__try
		{
			auto* world = static_cast<SDK::UWorld*>(worldPtr);
			if (!world || !world->GameState)
				return 0;

			const int32_t count = world->GameState->PlayerArray.Num();
			int written = 0;
			for (int32_t i = 0; i < count && written < max; ++i)
			{
				SDK::APlayerState* ps = world->GameState->PlayerArray[i];
				SDK::AActor* owner = ps ? ps->Owner : nullptr;
				if (owner && owner->IsA(SDK::APlayerController::StaticClass()))
					out[written++] = static_cast<SDK::APlayerController*>(owner);
			}
			return written;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
	}

	// Game thread only. Bring every connected player whose loader reported up to
	// date with the rules and the player list.
	void RefreshNow(void*)
	{
		auto* hooks = GetHooks();
		if (!hooks || !hooks->HostRules)
			return;

		SDK::APlayerController* controllers[ListedPlayers::kMax];
		const int count = CollectControllers(PluginSdkRuntime::GetWorld(), controllers, ListedPlayers::kMax);
		for (int i = 0; i < count; ++i)
		{
			const std::string id = IdOf(controllers[i]);
			if (id.empty())
				continue;

			bool reported;
			{
				std::lock_guard<std::mutex> lk(g_mutex);
				reported = std::any_of(g_known.begin(), g_known.end(), [&](const Known& k) { return k.id == id; });
			}
			if (reported)
				ApplyRules(controllers[i], id, false);
		}
	}

	void OnClientFeatures(void* playerController)
	{
		if (!OnGameThread())
		{
			LOG_WARN("[HostRules] Client features callback arrived off the game thread - ignored");
			return;
		}

		const std::string id = IdOf(playerController);
		if (id.empty())
		{
			auto* pc = static_cast<SDK::APlayerController*>(playerController);
			const bool queued = std::any_of(g_waiting.begin(), g_waiting.end(), [&](const Waiting& w) { return w.pc.Get() == pc; });
			if (pc && !queued)
				g_waiting.push_back({ ObjectRef<SDK::APlayerController>(pc), GetTickCount64() });
			return;
		}

		RecordReported(id, playerController);
		ApplyRules(playerController, id, true);
		AdminClaim::AskPlayer(playerController, id);
	}

	void OnPlayerLeft(void* exitingController)
	{
		// Called from the game's Logout, on the game thread, before the controller is torn down.
		auto* pc = static_cast<SDK::APlayerController*>(exitingController);
		g_waiting.erase(std::remove_if(g_waiting.begin(), g_waiting.end(), [&](const Waiting& w) { return w.pc.Get() == pc; }), g_waiting.end());

		InGameAdmin::PlayerLeft(exitingController);

		const std::string id = IdOf(exitingController);
		if (id.empty())
			return;

		AdminClaim::PlayerLeft(id);

		std::lock_guard<std::mutex> lk(g_mutex);
		g_known.erase(std::remove_if(g_known.begin(), g_known.end(), [&](const Known& k) { return k.id == id; }), g_known.end());
		g_ownRules.erase(id);
	}

	// Send the rules to the loader. Game thread only. Returns the status text.
	std::string PushText(const std::string& text)
	{
		std::string status;
		auto* hooks = GetHooks();
		IPluginHostRules* api = hooks ? hooks->HostRules : nullptr;
		if (!api)
		{
			status = "not sent: this mod loader has no host rules support";
		}
		else
		{
			char err[256] = {};
			if (api->SetRules(text.c_str(), err, sizeof(err)))
				status = "sent";
			else
			{
				status = std::string("not sent: the loader refused the rules: ") + err;
				LOG_WARN("[HostRules] The loader refused the rules: %s", err);
			}
		}
		static bool s_logged = false;
		if (!s_logged && status.find("no host rules support") != std::string::npos)
		{
			s_logged = true;
			LOG_INFO("[HostRules] Rules are saved but %s", status.c_str());
		}
		std::lock_guard<std::mutex> lk(g_mutex);
		g_pushStatus = status;
		return status;
	}

	// Send the version minimums to the loader. Game thread only.
	void PushMinimums(const std::string& text)
	{
		std::string status;
		auto* hooks = GetHooks();
		IPluginHostRules* api = hooks ? hooks->HostRules : nullptr;
		if (!api)
		{
			status = "not sent: this mod loader has no host rules support";
		}
		else
		{
			char err[256] = {};
			if (api->SetVersionMinimums(text.c_str(), err, sizeof(err)))
				status = "sent";
			else
			{
				status = std::string("not sent: the loader refused the minimums: ") + err;
				LOG_WARN("[HostRules] The loader refused the version minimums: %s", err);
			}
		}
		std::lock_guard<std::mutex> lk(g_mutex);
		g_minStatus = status;
	}

	void PushNow(void*)
	{
		std::string text, minimums;
		{
			std::lock_guard<std::mutex> lk(g_mutex);
			text = RulesModel::RulesText(g_model, true);
			minimums = RulesModel::VersionMinimumsText(g_model);
		}
		PushText(text);
		PushMinimums(minimums);
		HostRules::RefreshPlayers();
	}

	// Read the three sections from the file into the model; log each warning.
	bool Load(std::string& error)
	{
		const std::wstring& path = IniFile::Path();
		if (path.empty()) { error = "the rules file path is unknown"; return false; }

		std::vector<std::string> lines;
		bool exists = false, crlf = true;
		if (!IniFile::ReadLines(path, lines, exists, crlf))
		{
			std::lock_guard<std::mutex> lk(g_mutex);
			g_loadOk = false;
			error = "the rules file could not be read";
			return false;
		}

		RulesModel::Model model;
		std::vector<std::string> warnings;
		RulesModel::Parse(lines, model, warnings);
		for (const std::string& w : warnings)
			LOG_WARN("[HostRules] %s", w.c_str());
		LOG_INFO("[HostRules] Rules: default %s, %d rules, %d per-mod minimums, %d lines not understood",
		         model.defaultDeny ? "deny" : "allow", static_cast<int>(model.rules.size()),
		         static_cast<int>(model.modMins.size()), static_cast<int>(warnings.size()));

		std::lock_guard<std::mutex> lk(g_mutex);
		g_loadOk = true;
		g_model = std::move(model);
		return true;
	}
}

namespace HostRules
{
	void Init()
	{
		std::string error;
		if (!Load(error))
			LOG_WARN("[HostRules] Rules not read: %s", error.c_str());

		const std::wstring featuresPath = IniFile::DataPath(L"known-features.txt");
		KnownFeatures::SetWarnSink([](const std::string& line) { LOG_WARN("%s", line.c_str()); });
		if (!featuresPath.empty())
			KnownFeatures::Init(featuresPath);

		auto* hooks = GetHooks();
		if (hooks && hooks->HostRules && GetSelf())
			hooks->HostRules->RegisterOnClientFeatures(GetSelf(), OnClientFeatures);
		if (hooks && hooks->Engine)
		{
			hooks->Engine->RegisterOnTick(OnTick);
			g_tickRegistered = true;
		}
		auto* hooks2 = GetHooks();
		if (hooks2 && hooks2->Players)
			hooks2->Players->RegisterOnPlayerLeft(OnPlayerLeft);
	}

	int ConnectedControllers(SDK::APlayerController** out, int max)
	{
		return CollectControllers(PluginSdkRuntime::GetWorld(), out, max);
	}

	void PushWhenGameThreadRuns()
	{
		auto* hooks = GetHooks();
		if (hooks && hooks->Engine)
			hooks->Engine->PostToGameThread(PushNow, nullptr);
	}

	void RefreshPlayers()
	{
		auto* hooks = GetHooks();
		if (!hooks || !hooks->Engine || !hooks->HostRules)
			return;
		if (hooks->Engine->IsGameThread())
			RefreshNow(nullptr);
		else
			hooks->Engine->PostToGameThread(RefreshNow, nullptr);
	}

	void Shutdown()
	{
		auto* hooks = GetHooks();
		if (!hooks)
			return;
		if (hooks->HostRules && GetSelf())
			hooks->HostRules->UnregisterOnClientFeatures(GetSelf(), OnClientFeatures);
		if (g_tickRegistered && hooks->Engine)
			hooks->Engine->UnregisterOnTick(OnTick);
		g_tickRegistered = false;
		g_waiting.clear();
		KnownFeatures::Flush();
		if (hooks->Players)
			hooks->Players->UnregisterOnPlayerLeft(OnPlayerLeft);
	}

	bool Reload(std::string& error)
	{
		if (!Load(error))
			return false;
		PushNow(nullptr);
		return true;
	}

	RulesModel::Model Get()
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		return g_model;
	}

	bool Commit(const RulesModel::Model& next, std::string& error)
	{
		const std::wstring& path = IniFile::Path();
		if (path.empty()) { error = "no rules file path"; return false; }
		{
			std::lock_guard<std::mutex> lk(g_mutex);
			if (!g_loadOk) { error = "the rules file could not be read, so it was not changed"; return false; }
		}

		{
			std::lock_guard<std::mutex> fileLock(IniFile::FileMutex());
			std::vector<std::string> lines;
			bool exists = false, crlf = true;
			if (!IniFile::ReadLines(path, lines, exists, crlf)) { error = "the rules file could not be read"; return false; }

			IniFile::ReplaceSection(lines, "Policy", RulesModel::PolicyLines(next));
			IniFile::ReplaceSection(lines, "Rules", RulesModel::RuleLines(next));
			IniFile::ReplaceSection(lines, "MinVersions", RulesModel::ModMinLines(next));
			if (!IniFile::WriteLines(path, lines, crlf)) { error = "could not write the rules file"; return false; }
		}

		{
			std::lock_guard<std::mutex> lk(g_mutex);
			g_model = next;
		}
		PushNow(nullptr);
		return true;
	}

	bool LoaderReceives()
	{
		auto* hooks = GetHooks();
		return hooks && hooks->HostRules;
	}

	std::string PushStatus()
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		return g_pushStatus;
	}

	std::string MinimumsStatus()
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		return g_minStatus;
	}

	FeaturesState ClientFeatures(const std::string& playerId, std::string& text)
	{
		auto* hooks = GetHooks();
		IPluginHostRules* api = hooks ? hooks->HostRules : nullptr;
		if (!api)
			return FeaturesState::NoLoaderSupport;
		if (!OnGameThread())
			return FeaturesState::NotConnected;

		SDK::APlayerController* controllers[ListedPlayers::kMax];
		const int count = CollectControllers(PluginSdkRuntime::GetWorld(), controllers, ListedPlayers::kMax);
		for (int i = 0; i < count; ++i)
		{
			if (IdOf(controllers[i]) != playerId)
				continue;

			return ReadFeatures(api, controllers[i], text) ? FeaturesState::Ok : FeaturesState::NoReport;
		}
		return FeaturesState::NotConnected;
	}

	bool WhoReported(const std::string& playerId, Reported& out)
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		for (const Known& k : g_known)
			if (k.id == playerId) { out = k.info; return true; }
		return false;
	}
}
