#include "claim_model.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace ClaimModel
{
	namespace
	{
		constexpr size_t kMaxTyped = 64;                       // what the loader hands over at most
		constexpr unsigned kUnbiasedLimit = 256 - (256 % kAlphabetSize);   // bytes at or above this are skipped
		constexpr int kMaxFills = 32;

		bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
	}

	bool GenerateCode(const RandomFill& fill, std::string& out)
	{
		out.clear();
		if (!fill)
			return false;

		std::string code;
		for (int round = 0; round < kMaxFills && code.size() < kCodeLength; ++round)
		{
			unsigned char bytes[kCodeLength * 2] = {};
			if (!fill(bytes, sizeof(bytes)))
			{
				Wipe(code);
				return false;
			}
			for (unsigned char b : bytes)
			{
				if (b < kUnbiasedLimit && code.size() < kCodeLength)
					code.push_back(kAlphabet[b % kAlphabetSize]);
			}
			std::memset(bytes, 0, sizeof(bytes));
		}

		if (code.size() != kCodeLength)
		{
			Wipe(code);
			return false;
		}
		out = code;
		Wipe(code);
		return true;
	}

	bool NormalizeCode(const std::string& typed, std::string& out)
	{
		out.clear();

		size_t begin = 0, end = typed.size();
		while (begin < end && IsSpace(typed[begin])) ++begin;
		while (end > begin && IsSpace(typed[end - 1])) --end;
		if (begin == end || end - begin > kMaxTyped)
			return false;

		std::string s;
		for (size_t i = begin; i < end; ++i)
		{
			const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(typed[i])));
			if (!std::memchr(kAlphabet, c, kAlphabetSize))
			{
				Wipe(s);
				return false;
			}
			s.push_back(c);
		}
		out = s;
		Wipe(s);
		return true;
	}

	bool ConstantTimeEquals(const std::string& text, const std::string& secret)
	{
		unsigned diff = static_cast<unsigned>(text.size() ^ secret.size());
		for (size_t i = 0; i < secret.size(); ++i)
		{
			const unsigned char a = i < text.size() ? static_cast<unsigned char>(text[i]) : 0;
			diff |= static_cast<unsigned>(a ^ static_cast<unsigned char>(secret[i]));
		}
		return diff == 0;
	}

	void Wipe(std::string& s)
	{
		volatile char* p = s.empty() ? nullptr : &s[0];
		for (size_t i = 0; i < s.size(); ++i)
			p[i] = 0;
		s.clear();
	}

	// A [Policy] switch kept as an unread line: on unless set to 0, false, off or no.
	static bool PolicySwitchOn(const std::vector<std::string>& policyKept, const char* wantedKey)
	{
		for (const std::string& raw : policyKept)
		{
			const size_t eq = raw.find('=');
			if (eq == std::string::npos)
				continue;

			auto trim = [](std::string s)
			{
				while (!s.empty() && IsSpace(s.front())) s.erase(s.begin());
				while (!s.empty() && IsSpace(s.back())) s.pop_back();
				return s;
			};
			std::string key = trim(raw.substr(0, eq));
			std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (key != wantedKey)
				continue;

			std::string value = trim(raw.substr(eq + 1));
			std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return !(value == "0" || value == "false" || value == "off" || value == "no");
		}
		return true;
	}

	bool AdminPromptOn(const std::vector<std::string>& policyKept)
	{
		return PolicySwitchOn(policyKept, "adminprompt");
	}

	bool InGameAdminOn(const std::vector<std::string>& policyKept)
	{
		return PolicySwitchOn(policyKept, "ingameadmin");
	}

	bool Attempts::Locked(const std::string& playerId) const
	{
		const auto it = m_perPlayer.find(playerId);
		return it != m_perPlayer.end() && it->second >= kPerPlayer;
	}

	Attempts::Outcome Attempts::RecordWrong(const std::string& playerId)
	{
		const int mine = ++m_perPlayer[playerId];

		Outcome o;
		o.left         = std::max(0, kPerPlayer - mine);
		o.playerLocked = mine >= kPerPlayer;
		if (++m_overall >= kOverall)
		{
			o.codeReplaced = true;
			m_overall = 0;
		}
		return o;
	}
}
