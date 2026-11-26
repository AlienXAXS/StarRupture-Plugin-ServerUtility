#pragma once

#include <string>

// Text that goes into a log line, an error or an RCON reply.
namespace LineText
{
	// No line breaks or control characters (C0, DEL, and the Unicode line breaks
	// U+0085, U+2028 and U+2029), so a name or reason cannot start a line of its
	// own. Each one becomes one space.
	inline std::string OneLine(const std::string& s)
	{
		std::string out;
		out.reserve(s.size());
		for (size_t i = 0; i < s.size(); ++i)
		{
			const unsigned char c = static_cast<unsigned char>(s[i]);
			if (c < 0x20 || c == 0x7F)
				out += ' ';
			else if (c == 0xC2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x85)
			{
				out += ' ';
				++i;
			}
			else if (c == 0xE2 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80 &&
			         (static_cast<unsigned char>(s[i + 2]) == 0xA8 || static_cast<unsigned char>(s[i + 2]) == 0xA9))
			{
				out += ' ';
				i += 2;
			}
			else
				out += s[i];
		}
		return out;
	}

	// At most maxBytes, cut on a UTF-8 boundary, with "..." where it was cut.
	inline std::string Clip(const std::string& s, size_t maxBytes)
	{
		if (s.size() <= maxBytes || maxBytes < 4) return s;
		size_t n = maxBytes - 3;
		while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
		return s.substr(0, n) + "...";
	}
}
