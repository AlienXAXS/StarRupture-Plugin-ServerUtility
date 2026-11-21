#include "host_rules.h"
#include "ini_file.h"
#include "player_ids.h"
#include "../plugin_helpers.h"

#include "SDK/Engine_classes.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <mutex>
#include <vector>

namespace
{
	std::mutex                g_mutex;
	RulesModel::Model         g_model;
	bool                      g_loadOk = true;    // false: the file exists but was not readable, so it is never written
	std::string               g_pushStatus = "not sent yet";

	struct Known { std::string id; HostRules::Reported info; };
	std::vector<Known>        g_known;            // players whose loader reported, by ID

	std::string PushText(const std::string& text);

	// Game thread only. A controller read can fault when the object is already
	// going away, so it sits in a function of its own (no objects with
	// destructors, as __try needs).
	SDK::APlayerState* StateOf(void* playerController)
	{
		__try
		{
			auto* pc = static_cast<SDK::APlayerController*>(playerController);
			return pc ? pc->PlayerState : nullptr;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return nullptr;
		}
	}

	std::string IdOf(void* playerController)
	{
		SDK::APlayerState* ps = StateOf(playerController);
		return ps ? PlayerIds::Read(ps) : std::string();
	}

	void OnClientFeatures(void* playerController)
	{
		const std::string id = IdOf(playerController);
		if (id.empty())
			return;

		std::lock_guard<std::mutex> lk(g_mutex);
		auto it = std::find_if(g_known.begin(), g_known.end(), [&](const Known& k) { return k.id == id; });
		if (it == g_known.end()) { g_known.push_back({ id, {} }); it = g_known.end() - 1; }
		++it->info.count;
		it->info.lastMs = GetTickCount64();
	}

	void OnPlayerLeft(void* exitingController)
	{
		const std::string id = IdOf(exitingController);
		if (id.empty())
			return;

		std::lock_guard<std::mutex> lk(g_mutex);
		g_known.erase(std::remove_if(g_known.begin(), g_known.end(), [&](const Known& k) { return k.id == id; }), g_known.end());
	}

	// Send the rules to the loader. Game thread only. Returns the status text.
	std::string PushText(const std::string& text)
	{
		std::string status;
#if PLUGIN_INTERFACE_VERSION >= 79
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
#else
		(void)text;
		status = "not sent: this build has no host rules support";
#endif
		static bool s_logged = false;
		if (status != "sent" && !s_logged)
		{
			s_logged = true;
			LOG_INFO("[HostRules] This mod loader has no host rules support - rules are saved but not sent");
		}
		std::lock_guard<std::mutex> lk(g_mutex);
		g_pushStatus = status;
		return status;
	}

	void PushNow(void*)
	{
		std::string text;
		{
			std::lock_guard<std::mutex> lk(g_mutex);
			text = RulesModel::RulesText(g_model);
		}
		PushText(text);
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

#if PLUGIN_INTERFACE_VERSION >= 79
		auto* hooks = GetHooks();
		if (hooks && hooks->HostRules && GetSelf())
			hooks->HostRules->RegisterOnClientFeatures(GetSelf(), OnClientFeatures);
#endif
		auto* hooks2 = GetHooks();
		if (hooks2 && hooks2->Players)
			hooks2->Players->RegisterOnPlayerLeft(OnPlayerLeft);
	}

	void PushWhenGameThreadRuns()
	{
		auto* hooks = GetHooks();
		if (hooks && hooks->Engine)
			hooks->Engine->PostToGameThread(PushNow, nullptr);
	}

	void Shutdown()
	{
		auto* hooks = GetHooks();
		if (!hooks)
			return;
#if PLUGIN_INTERFACE_VERSION >= 79
		if (hooks->HostRules && GetSelf())
			hooks->HostRules->UnregisterOnClientFeatures(GetSelf(), OnClientFeatures);
#endif
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
#if PLUGIN_INTERFACE_VERSION >= 79
		auto* hooks = GetHooks();
		return hooks && hooks->HostRules;
#else
		return false;
#endif
	}

	std::string PushStatus()
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		return g_pushStatus;
	}

	bool WhoReported(const std::string& playerId, Reported& out)
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		for (const Known& k : g_known)
			if (k.id == playerId) { out = k.info; return true; }
		return false;
	}
}
