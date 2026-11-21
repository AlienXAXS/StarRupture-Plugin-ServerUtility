#pragma once

#include "../rcon.h"
#include "../state/server_state.h"

#include <cctype>
#include <string>

// Reading the arguments of a console command, shared by the player and
// hostrules verbs.
namespace CmdArgs
{
	// Take the first space-separated word off rest.
	inline std::string NextWord(std::string& rest)
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

	inline std::string Lower(std::string s)
	{
		for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return s;
	}

	// A purely numeric argument of up to 3 digits is the list number.
	inline bool IsListNumber(const std::string& s)
	{
		if (s.empty() || s.size() > 3) return false;
		for (char c : s) if (c < '0' || c > '9') return false;
		return true;
	}

	// An ID, or the number of a connected player as `player ids` shows it.
	inline bool ResolveId(const std::string& arg, std::string& id, std::string& error)
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
}
