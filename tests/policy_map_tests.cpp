// Unit tests of the replication graph's policy map keys (policy_map.h): the compiled
// value is the same-build fallback the plugin hands the loader, a row the loader
// answers with wins, and a key the loader cannot resolve leaves the layout invalid.
// No project file: from a Visual Studio developer prompt, in the repo root, with the
// plugin SDK's include folder and its Tests\PluginMapTests\stubs folder (it stands in
// for the game SDK headers plugin_map.h names):
//   cl /nologo /EHsc /std:c++20 /Isrc /I<sdk>\Tests\PluginMapTests\stubs /I<sdk>\include tests\policy_map_tests.cpp /Fe:policy_map_tests.exe
//   policy_map_tests.exe
#include <Basic.hpp>
#include <plugin_interface.h>
#include "policy_map.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int g_checks = 0, g_failed = 0;

#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

// What the loader answers: no row (it answers with the built value the request
// carries, as it does on the build the plugin was compiled for), a row, or nothing.
enum class Answer { NoRow, Row, Fail };

static Answer g_answer = Answer::NoRow;
static std::vector<PluginFieldRequest> g_requests;

static bool FakeResolveField(const IPluginSelf*, const PluginFieldRequest* request, PluginFieldResult* result)
{
	g_requests.push_back(*request);
	if (g_answer == Answer::Fail)
		return false;

	result->boolMask = 0;
	result->size = request->size;
	if (g_answer == Answer::NoRow)
	{
		result->offset = request->builtOffset;
		return true;
	}
	// A build whose key has a third int before the serial, and whose map sits 8 bytes later.
	if (std::strcmp(request->key, "TClassMap<EClassRepNodeMapping>::Map") == 0)
		result->offset = 0x38;
	else if (std::strcmp(request->key, "FObjectKey::ObjectIndex") == 0)
		result->offset = 0x0;
	else
		result->offset = 0x8;
	return true;
}

static void Resolve()
{
	g_requests.clear();
	// A previous run's answer must not leak through.
	PolicyMap::Map.offset = PolicyMap::KeyIndex.offset = PolicyMap::KeySerial.offset = -1;
	IPluginHookScanner scanner{};
	scanner.ResolveField = &FakeResolveField;
	PluginMap::ResolveFields(&scanner, nullptr, PolicyMap::Map, PolicyMap::KeyIndex, PolicyMap::KeySerial);
}

int main()
{
	// The compiled values are what the plugin asks the loader with.
	Resolve();
	CHECK(g_requests.size() == 3);
	CHECK(std::strcmp(g_requests[0].key, "TClassMap<EClassRepNodeMapping>::Map") == 0);
	CHECK(g_requests[0].builtOffset == 0x30 && g_requests[0].size == 0x50);
	CHECK(std::strcmp(g_requests[1].key, "FObjectKey::ObjectIndex") == 0);
	CHECK(g_requests[1].builtOffset == 0x0 && g_requests[1].size == 4);
	CHECK(std::strcmp(g_requests[2].key, "FObjectKey::ObjectSerialNumber") == 0);
	CHECK(g_requests[2].builtOffset == 0x4 && g_requests[2].size == 4);
	for (const PluginFieldRequest& r : g_requests)
		CHECK((r.flags & PLUGIN_FIELD_NATIVE) != 0);

	// No row: the fallback gives the compiled layout, 20-byte elements with the value at +8.
	g_answer = Answer::NoRow;
	Resolve();
	CHECK(PolicyMap::Map.offset == 0x30 && PolicyMap::KeyIndex.offset == 0 && PolicyMap::KeySerial.offset == 4);
	{
		const PluginMap::ScriptSetLayout layout = PolicyMap::Layout();
		CHECK(layout.valid);
		CHECK(layout.valueOffset == 8);
		CHECK(layout.hashNextIdOffset == 12 && layout.hashIndexOffset == 16);
		CHECK(layout.elementSize == 20 && layout.stride == 20);
	}

	// A row wins over the fallback: the offsets and the layout follow the row.
	g_answer = Answer::Row;
	Resolve();
	CHECK(PolicyMap::Map.offset == 0x38 && PolicyMap::KeyIndex.offset == 0 && PolicyMap::KeySerial.offset == 8);
	{
		const PluginMap::ScriptSetLayout layout = PolicyMap::Layout();
		CHECK(layout.valid);
		CHECK(layout.valueOffset == 12);
		CHECK(layout.hashNextIdOffset == 16 && layout.hashIndexOffset == 20);
		CHECK(layout.elementSize == 24 && layout.stride == 24);
	}

	// A key the loader cannot resolve: no layout, so nothing is walked.
	g_answer = Answer::Fail;
	Resolve();
	CHECK(PolicyMap::KeyIndex.offset < 0 && PolicyMap::KeySerial.offset < 0);
	CHECK(!PolicyMap::Layout().valid);

	std::printf("policy_map_tests: %d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
