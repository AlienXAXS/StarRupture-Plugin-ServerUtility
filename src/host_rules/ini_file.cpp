#include "ini_file.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>

namespace
{
	bool IsAnyHeader(const std::string& line)
	{
		const std::string t = IniFile::Trim(line);
		return !t.empty() && t[0] == '[';
	}

	bool IsHeaderOf(const std::string& line, const std::string& name)
	{
		return IniFile::Lower(IniFile::Trim(line)) == "[" + IniFile::Lower(name) + "]";
	}

	std::wstring FindPath()
	{
		wchar_t module[MAX_PATH] = {};
		HMODULE self = nullptr;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                        reinterpret_cast<LPCWSTR>(&IniFile::Path), &self)
		    || GetModuleFileNameW(self, module, MAX_PATH) == 0)
			return {};

		std::wstring dir(module);
		dir.resize(dir.find_last_of(L"\\/"));
		return dir + L"\\config\\HostRules.ini";
	}
}

namespace IniFile
{
	const std::wstring& Path()
	{
		static const std::wstring s_path = FindPath();
		return s_path;
	}

	std::mutex& FileMutex()
	{
		static std::mutex s_mutex;
		return s_mutex;
	}

	std::string Lower(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return s;
	}

	std::string Trim(const std::string& s)
	{
		const size_t a = s.find_first_not_of(" \t\r\n");
		if (a == std::string::npos) return {};
		return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
	}

	bool IsCommentOrBlank(const std::string& line)
	{
		const std::string t = Trim(line);
		return t.empty() || t[0] == ';' || t[0] == '#';
	}

	bool ReadLines(const std::wstring& path, std::vector<std::string>& lines, bool& exists, bool& crlf)
	{
		lines.clear();
		crlf = true;

		const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
		                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
		{
			const DWORD err = GetLastError();
			exists = !(err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND);
			return !exists;
		}
		exists = true;

		std::string data;
		char buf[8192];
		DWORD got = 0;
		bool ok = true;
		while (true)
		{
			if (!ReadFile(h, buf, sizeof(buf), &got, nullptr)) { ok = false; break; }
			if (got == 0) break;
			data.append(buf, got);
		}
		CloseHandle(h);
		if (!ok) return false;

		if (data.compare(0, 3, "\xEF\xBB\xBF") == 0) data.erase(0, 3);
		crlf = data.find("\r\n") != std::string::npos || data.find('\n') == std::string::npos;

		size_t s = 0;
		while (s < data.size())
		{
			size_t e = data.find('\n', s);
			if (e == std::string::npos) e = data.size();
			std::string line = data.substr(s, e - s);
			if (!line.empty() && line.back() == '\r') line.pop_back();
			lines.push_back(std::move(line));
			s = e + 1;
		}
		return true;
	}

	bool WriteLines(const std::wstring& path, const std::vector<std::string>& lines, bool crlf)
	{
		const std::wstring dir = path.substr(0, path.find_last_of(L"\\/"));
		CreateDirectoryW(dir.c_str(), nullptr);

		std::string text;
		for (const std::string& l : lines) { text += l; text += crlf ? "\r\n" : "\n"; }

		const std::wstring tmp = path + L".tmp";
		const HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE) return false;
		DWORD put = 0;
		const bool wroteOk = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &put, nullptr) && put == text.size()
		                     && FlushFileBuffers(h);
		CloseHandle(h);
		if (!wroteOk || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			DeleteFileW(tmp.c_str());
			return false;
		}
		return true;
	}

	void ReplaceSection(std::vector<std::string>& lines, const std::string& name, const std::vector<std::string>& body)
	{
		std::vector<std::string> out;
		bool wrote = false;
		bool inSection = false;
		auto emit = [&]()
		{
			out.insert(out.end(), body.begin(), body.end());
			wrote = true;
		};
		std::vector<std::string> keep; // comment lines of the old section
		for (const std::string& line : lines)
		{
			if (IsAnyHeader(line))
			{
				if (inSection) { out.insert(out.end(), keep.begin(), keep.end()); keep.clear(); emit(); }
				inSection = IsHeaderOf(line, name) && !wrote;
				out.push_back(line);
				continue;
			}
			if (!inSection) { out.push_back(line); continue; }
			if (IsCommentOrBlank(line)) keep.push_back(line);
		}
		if (inSection) { out.insert(out.end(), keep.begin(), keep.end()); emit(); }
		if (!wrote)
		{
			if (!out.empty() && !Trim(out.back()).empty()) out.push_back({});
			out.push_back("[" + name + "]");
			emit();
		}
		lines = std::move(out);
	}
}
