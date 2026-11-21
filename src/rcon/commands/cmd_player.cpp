#include "cmd_player.h"
#include "command_handler.h"
#include "../rcon.h"
#include "../state/server_state.h"
#include "../../host_rules/player_list.h"

#include <cctype>
#include <sstream>

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
			"  admin clear                            take Admin off every entry\n";

		// Take the first space-separated word off rest.
		std::string NextWord(std::string& rest)
		{
			const size_t a = rest.find_first_not_of(' ');
			if (a == std::string::npos) { rest.clear(); return {}; }
			size_t b = rest.find(' ', a);
			if (b == std::string::npos) b = rest.size();
			std::string word = rest.substr(a, b - a);
			rest.erase(0, b);
			const size_t c = rest.find_first_not_of(' ');
			rest.erase(0, c == std::string::npos ? rest.size() : c);
			return word;
		}

		std::string Lower(std::string s)
		{
			for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return s;
		}

		// A purely numeric argument of up to 3 digits is the list number.
		bool IsListNumber(const std::string& s)
		{
			if (s.empty() || s.size() > 3) return false;
			for (char c : s) if (c < '0' || c > '9') return false;
			return true;
		}

		// An ID, or the number of a connected player as `player ids` shows it.
		bool ResolveId(const std::string& arg, std::string& id, std::string& error)
		{
			if (!IsListNumber(arg)) { id = arg; return true; }

			Rcon::RefreshPlayers();
			const auto players = ServerState::Get().GetPlayers();
			const size_t n = static_cast<size_t>(std::stoi(arg));
			if (n < 1 || n > players.size()) { error = "no connected player with number " + arg; return false; }
			id = players[n - 1].id;
			if (id.empty()) { error = "that player's ID is not available yet"; return false; }
			return true;
		}

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

		std::string Saved(const std::string& id)
		{
			PlayerList::Entry e;
			if (!PlayerList::Find(id, e)) return "Saved.\n";
			return "Saved: " + PlayerList::FormatLine(e) + "\n";
		}

		std::string Error(const std::string& text) { return "Error: " + text + "\n"; }

		std::string HandlePlayer(const std::string& args)
		{
			std::string rest = args;
			const std::string sub = Lower(NextWord(rest));

			if (sub.empty() || sub == "list") return List();
			if (sub == "ids") return Ids();

			std::string error;
			if (sub == "reload")
			{
				if (!PlayerList::Reload(error)) return Error(error);
				return "Player list reloaded: " + std::to_string(PlayerList::All().size()) + " entries.\n";
			}

			if (sub == "set" || sub == "remove")
			{
				const std::string who = NextWord(rest);
				if (who.empty() || (sub == "set" && rest.empty())) return kPlayerUsage;

				std::string id;
				if (!ResolveId(who, id, error)) return Error(error);

				if (sub == "set")
				{
					if (!PlayerList::Set(id, rest, error)) return Error(error);
					return Saved(id);
				}
				if (!PlayerList::Remove(id, error)) return Error(error);
				return "Removed the entry for " + id + ".\n";
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
				int changed = 0;
				if (!PlayerList::ClearAdmins(changed, error)) return Error(error);
				return "Took Admin off " + std::to_string(changed) + " entries.\n";
			}

			if (sub == "add" || sub == "remove")
			{
				const std::string who = NextWord(rest);
				if (who.empty()) return kAdminUsage;

				std::string id;
				if (!ResolveId(who, id, error)) return Error(error);

				if (sub == "add")
				{
					const std::string flags = rest.empty() ? "admin" : "admin label=" + rest;
					if (!PlayerList::Set(id, flags, error)) return Error(error);
					return Saved(id);
				}

				PlayerList::Entry existing;
				if (!PlayerList::Find(id, existing)) return Error("no entry with that ID");
				if (!PlayerList::Set(id, "-admin", error)) return Error(error);
				return Saved(id);
			}

			return kAdminUsage;
		}
	}

	void Register()
	{
		PluginCommands::Register("player", nullptr, "player [list|ids|set|remove|reload]",
			"Show and edit the host's player list (admins and per-player exemptions)",
			HandlePlayer);
		PluginCommands::Register("admin", nullptr, "admin add|remove|clear",
			"Add or remove an admin in the player list",
			HandleAdmin);
	}
}
