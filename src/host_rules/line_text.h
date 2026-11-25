#pragma once

#include <string>

// Text that goes into a log line, an error or an RCON reply.
namespace LineText
{
	// No line breaks or control characters, so a name or reason cannot start a
	// line of its own.
	inline std::string OneLine(const std::string& s)
	{
		std::string out = s;
		for (char& c : out)
			if (static_cast<unsigned char>(c) < 0x20) c = ' ';
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
