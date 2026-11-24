#include "cmd_player.h"
#include "command_handler.h"
#include "cmd_args.h"
#include "../rcon.h"
#include "../state/server_state.h"
#include "../../host_rules/admin_claim.h"
#include "../../host_rules/admin_ops.h"
#include "../../host_rules/ini_file.h"
#include "../../host_rules/ingame_admin.h"
#include "../../host_rules/player_list.h"

#include <sstream>
#include <vector>

namespace Cmd_Player
{
	namespace
	{
		const char* const kPlayerUsage =
			"Usage:\n"
			"  player [list]                          entries in the player list\n"
			"  player ids                             connected players and their IDs\n"
			"  player set <id|number> <flag>...       add or change an entry\n"
			"  player remove <id|number>              delete an entry\n"
			"  player reload                          re-read HostRules.ini\n"
			"Flags: admin, exemptversion, exemptrules, sections=<key>;<key>, label=<text>\n"
			"       (a leading - takes a flag off). <number> is the number `player ids` shows.\n";

		const char* const kAdminUsage =
			"Usage:\n"
			"  admin add <id|number> [label]\n"
			"  admin remove <id|number>\n"
			"  admin clear                            take Admin off every entry\n"
				"  admin code                             the claim code, while there is no admin\n"
			"  admin pair <id|number>                 a one-time code (10 minutes) to pair that admin's in-game tool\n"
			"  admin unpair <id|number>               forget that admin's in-game tool pairing\n";

		using CmdArgs::NextWord;
		using IniFile::Lower;
		using CmdArgs::ResolveId;

		std::string Ids()
		{
			Rcon::RefreshPlayers();
			const auto players = ServerState::Get().GetPlayers();
			if (players.empty()) return "No players currently connected.\n";

			std::ostringstream oss;
			oss << "Players (" << players.size() << " connected):\n";
			int idx = 1;
			for (const auto& p : players)
				oss << "  [" << idx++ << "] " << p.name << "  "
				    << (p.id.empty() ? "(ID not available yet)" : p.id) << "\n";
			return oss.str();
		}

		std::string List()
		{
			Rcon::RefreshPlayers();
			const auto players = ServerState::Get().GetPlayers();
			const auto entries = PlayerList::All();
			if (entries.empty()) return "The player list is empty.\n";

			std::ostringstream oss;
			oss << "Player list (" << entries.size() << " entries):\n";
			for (const auto& e : entries)
			{
				oss << "  " << PlayerList::FormatLine(e);
				for (size_t i = 0; i < players.size(); ++i)
					if (players[i].id == e.id) { oss << "  (connected, [" << i + 1 << "] " << players[i].name << ")"; break; }
				oss << "\n";
			}
			return oss.str();
		}

		// who is "Name (ID)" from ResolveId, so the host sees whom the change reached.
		std::string Saved(const std::string& id, const std::string& who)
		{
			PlayerList::Entry e;
			if (!PlayerList::Find(id, e)) return "Saved for " + who + ".\n";
			return "Saved for " + who + ": " + PlayerList::FormatLine(e) + "\n";
		}

		std::string Error(const std::string& text) { return "Error: " + text + "\n"; }

		AdminOps::Result Run(const char* op, std::vector<std::string> args = {})
		{
			return AdminOps::Run(AdminOps::Caller::FromRcon(), AdminOps::Op{ op, std::move(args) });
		}

		std::string HandlePlayer(const std::string& args)
		{
			std::string rest = args;
			const std::string sub = Lower(NextWord(rest));

			if (sub.empty() || sub == "list") return List();
			if (sub == "ids") return Ids();

			std::string error;
			if (sub == "reload")
			{
				const AdminOps::Result r = Run("players.reload");
				if (!r.ok) return Error(r.text);
				return "Player list reloaded: " + std::to_string(PlayerList::All().size()) + " entries.\n";
			}

			if (sub == "set" || sub == "remove")
			{
				const std::string who = NextWord(rest);
				if (who.empty() || (sub == "set" && rest.empty())) return kPlayerUsage;

				std::string id, target;
				if (!ResolveId(who, id, error, &target)) return Error(error);

				if (sub == "set")
				{
					const AdminOps::Result r = Run("player.set", { id, rest });
					return r.ok ? Saved(id, target) : Error(r.text);
				}
				const AdminOps::Result r = Run("player.remove", { id });
				if (!r.ok) return Error(r.text);
				return "Removed the entry for " + target + ".\n";
			}

			return kPlayerUsage;
		}

		std::string HandleAdmin(const std::string& args)
		{
			std::string rest = args;
			const std::string sub = Lower(NextWord(rest));

			std::string error;
			if (sub == "clear")
			{
				const AdminOps::Result r = Run("admin.clear");
				return r.ok ? r.text + "\n" : Error(r.text);
			}

			if (sub == "code")
				return AdminClaim::CodeReport();

			if (sub == "pair" || sub == "unpair")
			{
				const std::string who = NextWord(rest);
				if (who.empty()) return kAdminUsage;

				std::string id, target;
				if (!ResolveId(who, id, error, &target)) return Error(error);

				if (sub == "unpair")
					return InGameAdmin::Unpair(id) ? "Removed the in-game tool pairing for " + target + ".\n"
					                               : Error("that player has no pairing");

				// The code goes only into this reply, never into the log.
				std::string code;
				if (!InGameAdmin::IssueCode(id, code, error)) return Error(error);
				return "Pairing code for " + target + ": " + code +
				       "\nGood for 10 minutes, one use. The player types it into the in-game admin tool.\n";
			}

			if (sub == "add" || sub == "remove")
			{
				const std::string who = NextWord(rest);
				if (who.empty()) return kAdminUsage;

				std::string id, target;
				if (!ResolveId(who, id, error, &target)) return Error(error);

				if (sub == "add")
				{
					const std::string flags = rest.empty() ? "admin" : "admin label=" + rest;
					const AdminOps::Result r = Run("player.set", { id, flags });
					return r.ok ? Saved(id, target) : Error(r.text);
				}

				PlayerList::Entry existing;
				if (!PlayerList::Find(id, existing)) return Error("no entry with that ID");
				const AdminOps::Result r = Run("player.set", { id, "-admin" });
				return r.ok ? Saved(id, target) : Error(r.text);
			}

			return kAdminUsage;
		}
	}

	void Register()
	{
		PluginCommands::Register("player", nullptr, "player [list|ids|set|remove|reload]",
			"Show and edit the host's player list (admins and per-player exemptions)",
			HandlePlayer);
		PluginCommands::Register("admin", nullptr, "admin add|remove|clear|code|pair|unpair",
			"Add or remove an admin in the player list",
			HandleAdmin);
	}
}
