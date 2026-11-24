#pragma once

#include <mutex>
#include <string>
#include <vector>

// Plugins\config\HostRules.ini as lines of text, shared by the player list and
// the host rules. Each module owns its own sections and leaves the others as
// they are.
namespace IniFile
{
	// <folder of this DLL>\config\HostRules.ini; empty when the folder is unknown.
	const std::wstring& Path();

	// Plugins\ServerUtility\<name>, where the plugin keeps
	// its own data files; empty when the folder is unknown.
	std::wstring DataPath(const wchar_t* name);

	// Held by a module for the whole read-modify-write of the file, so two
	// modules never write over each other's change.
	std::mutex& FileMutex();

	// Read the file into lines (BOM dropped, line ends removed). A file that
	// does not exist reads as no lines with exists=false and returns true.
	// Returns false when it exists but could not be read. crlf is the line
	// ending to write back.
	bool ReadLines(const std::wstring& path, std::vector<std::string>& lines, bool& exists, bool& crlf);

	// Write the lines to a temp file, then rename it over the file. False when
	// the file could not be written (the old file is then untouched).
	bool WriteLines(const std::wstring& path, const std::vector<std::string>& lines, bool crlf);

	// Write text to path through a temp file renamed over it, creating the
	// folders on the way. False when it could not be written (the old file is
	// then untouched).
	bool WriteTextFile(const std::wstring& path, const std::string& text);

	// Replace the body of [name]: the old section's comment and blank lines are
	// kept, then body follows. A missing section is added at the end.
	void ReplaceSection(std::vector<std::string>& lines, const std::string& name,
	                    const std::vector<std::string>& body);

	std::string Trim(const std::string& s);
	std::string Lower(std::string s);
	bool        IsCommentOrBlank(const std::string& line);
}
