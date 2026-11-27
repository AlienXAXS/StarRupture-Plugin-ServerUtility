#include "stack_size_model.h"
#include "host_rules/ini_file.h"

namespace StackSizeModel
{
	bool Validate(const std::string& text, int& out, std::string& error)
	{
		const std::string t = IniFile::Trim(text);
		if (t.empty()) { error = "stack size needs a number from 0 to " + std::to_string(kMaxStack); return false; }
		if (t.size() > 6) { error = "stack size must be from 0 to " + std::to_string(kMaxStack); return false; }
		int n = 0;
		for (char c : t)
		{
			if (c < '0' || c > '9') { error = "stack size must be a whole number from 0 to " + std::to_string(kMaxStack); return false; }
			n = n * 10 + (c - '0');
		}
		if (n > kMaxStack) { error = "stack size must be from 0 to " + std::to_string(kMaxStack); return false; }
		out = n;
		return true;
	}

	int ParseLines(const std::vector<std::string>& lines, std::string& error)
	{
		error.clear();
		const std::string header = "[" + IniFile::Lower(kSection) + "]";
		const std::string key = IniFile::Lower(kKey);
		bool inSection = false;
		for (const std::string& raw : lines)
		{
			const std::string line = IniFile::Trim(raw);
			if (line.empty() || line[0] == ';' || line[0] == '#') continue;
			if (line[0] == '[') { inSection = IniFile::Lower(line) == header; continue; }
			if (!inSection) continue;
			const size_t eq = line.find('=');
			if (eq == std::string::npos || IniFile::Lower(IniFile::Trim(line.substr(0, eq))) != key) continue;
			int n = 0;
			if (!Validate(line.substr(eq + 1), n, error)) return 0;
			return n;
		}
		return 0;
	}

	std::vector<std::string> ComposeBody(int stackSize)
	{
		return { std::string(kKey) + "=" + std::to_string(stackSize) };
	}
}
