// Unit tests of the server-wide stack size model: the [Server] text.
// No project file: from a Visual Studio developer prompt, in the repo root:
//   cl /nologo /EHsc /std:c++17 /Isrc tests\stack_size_model_tests.cpp src\server_settings\stack_size_model.cpp src\host_rules\ini_file.cpp /Fe:stack_size_model_tests.exe
//   stack_size_model_tests.exe
#include "server_settings/stack_size_model.h"
#include "host_rules/ini_file.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_checks = 0, g_failed = 0;

#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

using namespace StackSizeModel;

int main()
{
	// Validate: 0..9999, plain digits.
	{
		int n = -1;
		std::string err;
		CHECK(Validate("0", n, err) && n == 0);
		CHECK(Validate("1", n, err) && n == 1);
		CHECK(Validate(" 500 ", n, err) && n == 500);
		CHECK(Validate("9999", n, err) && n == 9999);
		CHECK(Validate("0042", n, err) && n == 42);
		n = 77;
		CHECK(!Validate("10000", n, err) && !err.empty() && n == 77);
		CHECK(!Validate("-1", n, err));
		CHECK(!Validate("+5", n, err));
		CHECK(!Validate("1.5", n, err));
		CHECK(!Validate("abc", n, err));
		CHECK(!Validate("", n, err));
		CHECK(!Validate("   ", n, err));
		CHECK(!Validate("5 0", n, err));
		CHECK(!Validate("99999999999999999999", n, err));
		CHECK(n == 77);
	}

	// [Server] parse and compose; other sections stay as they are.
	{
		std::string err;
		CHECK(ComposeBody(500) == std::vector<std::string>{ "StackSize=500" });
		CHECK(ComposeBody(0) == std::vector<std::string>{ "StackSize=0" });

		std::vector<std::string> file = { "[Policy]", "Default=allow", "", "[Players]", "1=Al|Admin" };
		CHECK(ParseLines(file, err) == 0 && err.empty());   // no [Server]

		IniFile::ReplaceSection(file, kSection, ComposeBody(500));
		CHECK(ParseLines(file, err) == 500 && err.empty());
		CHECK(file.size() == 8 && file[0] == "[Policy]" && file[1] == "Default=allow" && file[3] == "[Players]" && file[4] == "1=Al|Admin");

		IniFile::ReplaceSection(file, kSection, ComposeBody(9999));
		CHECK(ParseLines(file, err) == 9999 && file.size() == 8);
		IniFile::ReplaceSection(file, kSection, ComposeBody(0));
		CHECK(ParseLines(file, err) == 0 && err.empty() && file.size() == 8);

		// The key and header are case-insensitive; comments and spacing are fine.
		CHECK(ParseLines({ "[server]", "; note", "  stacksize = 250  " }, err) == 250 && err.empty());
		// A key in another section does not count.
		CHECK(ParseLines({ "[Policy]", "StackSize=300", "[Server]" }, err) == 0 && err.empty());
		// A section with other keys only.
		CHECK(ParseLines({ "[Server]", "Other=1" }, err) == 0 && err.empty());
		// Bad values give 0 and an error text.
		CHECK(ParseLines({ "[Server]", "StackSize=abc" }, err) == 0 && !err.empty());
		CHECK(ParseLines({ "[Server]", "StackSize=10000" }, err) == 0 && !err.empty());
		CHECK(ParseLines({ "[Server]", "StackSize=" }, err) == 0 && !err.empty());
		CHECK(ParseLines({ "[Server]", "StackSize=-5" }, err) == 0 && !err.empty());
		CHECK(ParseLines({}, err) == 0 && err.empty());
	}

	std::printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
