#pragma once

#include <string>
#include <vector>

// Taking one word off the front of a console command's arguments. A word ends
// at a space, except inside double quotes, so a mod name with a space is typed
// as "Best Builder" or "Best Builder.feature". The quotes are not part of the
// word. A mod name cannot hold a quote, so there is nothing to escape for
// one; a feature id can, and cannot be typed here.
namespace WordSplit
{
	inline std::string TakeWord(std::string& rest)
	{
		const size_t a = rest.find_first_not_of(' ');
		if (a == std::string::npos) { rest.clear(); return {}; }

		std::string word;
		bool quoted = false;
		size_t i = a;
		for (; i < rest.size(); ++i)
		{
			const char c = rest[i];
			if (c == '"') quoted = !quoted;
			else if (c == ' ' && !quoted) break;
			else word += c;
		}
		rest.erase(0, i);
		const size_t b = rest.find_first_not_of(' ');
		rest.erase(0, b == std::string::npos ? rest.size() : b);
		return word;
	}

	// "<name>=a;b" for a flags text, or <name>="a b.x;c" when a key holds a
	// space so it stays one word; empty when there are no keys.
	inline std::string SectionsFlag(const char* name, const std::vector<std::string>& keys)
	{
		std::string list;
		for (const std::string& s : keys)
		{
			if (!list.empty()) list += ';';
			list += s;
		}
		if (list.empty()) return std::string();
		return std::string(name) + "=" + (list.find(' ') == std::string::npos ? list : "\"" + list + "\"");
	}
}
