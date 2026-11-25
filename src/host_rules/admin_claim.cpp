#include "admin_claim.h"
#include "ingame_admin.h"
#include "admin_ops.h"
#include "claim_model.h"
#include "host_rules.h"
#include "line_text.h"
#include "player_ids.h"
#include "player_list.h"
#include "../plugin_helpers.h"
#include "../rcon/rcon.h"
#include "../rcon/state/server_state.h"
#include "../rcon/listed_players.h"

#include "SDK/Engine_classes.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <mutex>
#include <set>
#include <string>

#pragma comment(lib, "bcrypt.lib")

namespace
{
	std::mutex             g_mutex;
	std::string            g_code;        // empty = no code
	ClaimModel::Attempts   g_attempts;
	std::set<std::string>  g_open;        // players with an open prompt
	std::set<std::string>  g_declined;    // answered "Not me": not asked again until restart
	bool                   g_registered = false;

	struct Wiper { std::string& s; ~Wiper() { ClaimModel::Wipe(s); } };

	bool SystemRandom(unsigned char* buf, size_t size)
	{
		return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, buf, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
	}

	bool ListHasAdmin()
	{
		for (const PlayerList::Entry& e : PlayerList::All())
			if (e.admin)
				return true;
		return false;
	}

	IPluginHostRules* Api()
	{
		auto* hooks = GetHooks();
		return hooks ? hooks->HostRules : nullptr;
	}

	// g_mutex held. Keeps the code in step with the list: none once an admin
	// exists, a new one (logged for the host) while there is none. True when a
	// code is active.
	bool EnsureCodeLocked(const char* why)
	{
		if (ListHasAdmin())
		{
			ClaimModel::Wipe(g_code);
			return false;
		}
		if (!g_code.empty())
			return true;

		if (!ClaimModel::GenerateCode(SystemRandom, g_code))
		{
			LOG_ERROR("[Admin] Could not make a claim code (the system random generator failed). Use RCON: admin add");
			return false;
		}
		LOG_INFO("[Admin] No admins yet%s. Claim code: %s (shown only while the list is empty; RCON: admin code)", why, g_code.c_str());
		return true;
	}

	std::string NameOf(const std::string& id)
	{
		Rcon::RefreshPlayers();
		for (const PlayerInfo& p : ServerState::Get().GetPlayers())
			if (p.id == id)
				return p.name;
		return std::string();
	}

	void Say(void* pc, const char* text, bool close)
	{
		IPluginHostRules* api = Api();
		if (api && !api->AdminMessage(pc, text, close))
			LOG_DEBUG("[Admin] The loader did not send a prompt line");
	}

	enum class Result { NotMe, NoCode, Taken, Locked, Wrong, Claimed, SaveFailed };

	void OnAnswer(void* playerController, bool notMe, const char* code)
	{
		std::string typed = code ? code : "";
		Wiper wipeTyped{ typed };

		auto* hooks = GetHooks();
		if (!hooks || !hooks->Engine || !hooks->Engine->IsGameThread() || !playerController)
			return;

		const std::string id = PlayerIds::OfController(playerController);
		if (id.empty())
		{
			LOG_WARN("[Admin] A prompt answer came from a player whose ID is not readable - closed");
			Say(playerController, "Your player ID could not be read. Try again later.", true);
			return;
		}

		const std::string label = notMe ? std::string() : NameOf(id);

		Result result = Result::NoCode;
		ClaimModel::Attempts::Outcome wrong;
		std::set<std::string> others;
		{
			std::lock_guard<std::mutex> lk(g_mutex);
			// Only a player with an open prompt can answer it.
			if (!g_open.count(id))
				return;
			g_open.erase(id);

			std::string norm;
			Wiper wipeNorm{ norm };
			if (notMe)
			{
				g_declined.insert(id);
				result = Result::NotMe;
			}
			else if (!EnsureCodeLocked(""))
				result = ListHasAdmin() ? Result::Taken : Result::NoCode;
			else if (g_attempts.Locked(id))
				result = Result::Locked;
			else if (ClaimModel::NormalizeCode(typed, norm) && ClaimModel::ConstantTimeEquals(norm, g_code))
			{
				std::string error;
				if (PlayerList::Set(id, label.empty() ? "admin" : "admin label=" + label, error))
				{
					ClaimModel::Wipe(g_code);
					others = g_open;
					g_open.clear();
					result = Result::Claimed;
				}
				else
				{
					LOG_ERROR("[Admin] Could not save the admin list: %s", error.c_str());
					g_open.insert(id);
					result = Result::SaveFailed;
				}
			}
			else
			{
				wrong = g_attempts.RecordWrong(id);
				if (wrong.codeReplaced)
				{
					ClaimModel::Wipe(g_code);
					EnsureCodeLocked(" (the code was replaced after wrong tries)");
				}
				if (wrong.playerLocked)
					g_declined.insert(id);
				else
					g_open.insert(id);
				result = wrong.playerLocked ? Result::Locked : Result::Wrong;
			}
		}

		switch (result)
		{
		case Result::NotMe:
			LOG_INFO("[Admin] Player %s chose Not me - not asked again until restart", LineText::OneLine(id).c_str());
			Say(playerController, "Okay. You will not be asked again until the server restarts.", true);
			break;
		case Result::NoCode:
			Say(playerController, "No claim code is available. Ask the host to add you with the admin command.", true);
			break;
		case Result::Taken:
			LOG_INFO("[Admin] Player %s answered after the server already had an admin (expired)", LineText::OneLine(id).c_str());
			Say(playerController, "This server already has an admin.", true);
			break;
		case Result::Locked:
			LOG_WARN("[Admin] Player %s gave too many wrong codes - not asked again until restart", LineText::OneLine(id).c_str());
			Say(playerController, "Too many wrong codes. You will not be asked again until the server restarts.", true);
			break;
		case Result::Wrong:
		{
			LOG_INFO("[Admin] Player %s gave a wrong code (%d tries left)", LineText::OneLine(id).c_str(), wrong.left);
			const std::string line = "That is not the code. " + std::to_string(wrong.left) + " tries left.";
			Say(playerController, line.c_str(), false);
			break;
		}
		case Result::SaveFailed:
			Say(playerController, "The code is right but the admin list could not be saved. Try again.", false);
			break;
		case Result::Claimed:
		{
			LOG_INFO("[Admin] '%s' claimed admin (id %s)", LineText::OneLine(label).c_str(), LineText::OneLine(id).c_str());
			AdminOps::NoteChanged(AdminOps::kSectionPlayers);
			HostRules::RefreshPlayers();
			InGameAdmin::OnClaimed(playerController, id);
			Say(playerController, InGameAdmin::Enabled()
				? "You are the admin. To manage this server in game, install the ServerUtility admin client; RCON works too."
				: "You are now an admin of this server.", true);

			if (!others.empty())
			{
				SDK::APlayerController* controllers[ListedPlayers::kMax];
				const int count = HostRules::ConnectedControllers(controllers, ListedPlayers::kMax);
				for (int i = 0; i < count; ++i)
					if (others.count(PlayerIds::OfController(controllers[i])))
						Say(controllers[i], "Another player already claimed this server.", true);
			}
			break;
		}
		}
	}
}

namespace AdminClaim
{
	void Init()
	{
		auto* hooks = GetHooks();
		if (!hooks || !hooks->HostRules || !GetSelf())
		{
			LOG_INFO("[Admin] This mod loader has no admin prompt - claim code is off, use RCON: admin add");
			return;
		}
		hooks->HostRules->RegisterOnAdminAnswer(GetSelf(), OnAnswer);
		g_registered = true;

		// With the prompt off nobody is asked, so no code is made until the host asks for one.
		if (!ClaimModel::AdminPromptOn(HostRules::Get().policyKept))
			return;
		std::lock_guard<std::mutex> lk(g_mutex);
		EnsureCodeLocked("");
	}

	void Shutdown()
	{
		auto* hooks = GetHooks();
		if (g_registered && hooks && hooks->HostRules && GetSelf())
			hooks->HostRules->UnregisterOnAdminAnswer(GetSelf(), OnAnswer);
		g_registered = false;
		std::lock_guard<std::mutex> lk(g_mutex);
		ClaimModel::Wipe(g_code);
	}

	void AskPlayer(void* playerController, const std::string& playerId)
	{
		IPluginHostRules* api = Api();
		if (!g_registered || !api || !playerController || playerId.empty())
			return;
		if (!ClaimModel::AdminPromptOn(HostRules::Get().policyKept))
			return;

		{
			std::lock_guard<std::mutex> lk(g_mutex);
			if (!EnsureCodeLocked("") || g_declined.count(playerId) || g_open.count(playerId) || g_attempts.Locked(playerId))
				return;
			g_open.insert(playerId);
		}

		if (!api->AskAdmin(playerController))
		{
			std::lock_guard<std::mutex> lk(g_mutex);
			g_open.erase(playerId);   // no prompt on that client (vanilla or an older loader)
		}
		else
			LOG_INFO("[Admin] Asked player %s for the claim code", playerId.c_str());
	}

	void PlayerLeft(const std::string& playerId)
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		g_open.erase(playerId);
	}

	std::string CodeReport()
	{
		std::lock_guard<std::mutex> lk(g_mutex);
		if (!g_registered)
			return "Claim code is off: this mod loader has no admin prompt. Use: admin add <id|number>\n";
		if (!EnsureCodeLocked(""))
			return ListHasAdmin() ? "The server already has an admin, so there is no claim code.\n"
			                      : "No claim code could be made. Use: admin add <id|number>\n";
		return "Claim code: " + g_code + "\n";
	}
}
