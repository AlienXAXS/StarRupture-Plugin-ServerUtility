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

// A policy map as the game lays it out: three slots of `stride` bytes (a key's two ints,
// the value, then HashNextId and HashIndex at the given offsets), the middle one free.
// The slots and offsets are written out here, not taken from PolicyMap::Layout(), so the
// walk checks the layout against the engine's. The map sits in a block at Map.offset.
static void WalkCheck(const char* what, uint32_t stride, uint32_t valueAt, uint32_t nextAt, uint32_t hashAt)
{
	std::vector<uint8_t> slots(3 * stride, 0);
	auto put = [&](uint32_t slot, uint32_t at, int32_t v) { std::memcpy(&slots[slot * stride + at], &v, sizeof(v)); };
	const uint32_t indexAt = static_cast<uint32_t>(PolicyMap::KeyIndex.offset);
	const uint32_t serialAt = static_cast<uint32_t>(PolicyMap::KeySerial.offset);
	put(0, indexAt, 7);  put(0, serialAt, 70); put(0, valueAt, 1); put(0, nextAt, -1); put(0, hashAt, 0);
	put(2, indexAt, 9);  put(2, serialAt, 90); put(2, valueAt, 2); put(2, nextAt, -1); put(2, hashAt, 1);
	put(1, 0, -1); put(1, 4, -1); // the free slot's links

	PluginMap::ScriptSetMemory map{};
	map.data = slots.data();
	map.num = 3;
	map.max = 4;
	map.inlineBits[0] = 0x5;
	map.numBits = 3;
	map.maxBits = 128;
	map.firstFreeIndex = 1;
	map.numFreeIndices = 1;
	map.hashSize = 2;

	std::vector<uint8_t> block(0x100, 0);
	std::memcpy(&block[PolicyMap::Map.offset], &map, sizeof(map));

	const PluginMap::ScriptMapView view(PolicyMap::Map.In(static_cast<const void*>(block.data())), PolicyMap::Layout());
	CHECK(static_cast<bool>(view));
	CHECK(view.Num() == 2);

	int seen = 0;
	view.ForEach([&](uint8_t* key, uint8_t* value)
	{
		int32_t index = 0, serial = 0;
		PolicyMap::Value v = 0;
		std::memcpy(&index, key + PolicyMap::KeyIndex.offset, sizeof(index));
		std::memcpy(&serial, key + PolicyMap::KeySerial.offset, sizeof(serial));
		std::memcpy(&v, value, sizeof(v));
		if (index == 7) { CHECK(serial == 70 && v == 1); }
		else if (index == 9) { CHECK(serial == 90 && v == 2); }
		else { CHECK(false); }
		++seen;
	});
	CHECK(seen == 2);

	// Retarget_SEH on the same map: the entry whose index and serial both match is rewritten and
	// the old value reported; the other entry is untouched.
	uint8_t* mapAt = block.data() + PolicyMap::Map.offset;
	const PluginMap::ScriptSetLayout layout = PolicyMap::Layout();
	auto valueOf = [&](uint32_t slot) { PolicyMap::Value v = 0; std::memcpy(&v, &slots[slot * stride + valueAt], sizeof(v)); return v; };
	{
		PolicyMap::RetargetResult r;
		PolicyMap::Retarget_SEH(mapAt, layout, 7, 70, 5, &r);
		CHECK(r.status == PolicyMap::RetargetResult::Status::Done);
		CHECK(r.changed == 1 && r.oldValue == 1 && r.mismatched == 0);
		CHECK(valueOf(0) == 5 && valueOf(2) == 2);
	}
	// The index is there with another serial (the object died, the slot was reused): nothing is
	// written, the entry is counted and its serial reported.
	{
		PolicyMap::RetargetResult r;
		PolicyMap::Retarget_SEH(mapAt, layout, 9, 91, 6, &r);
		CHECK(r.status == PolicyMap::RetargetResult::Status::Done);
		CHECK(r.changed == 0 && r.mismatched == 1 && r.mismatchSerial == 90);
		CHECK(valueOf(2) == 2);
	}
	// No entry for the index at all.
	{
		PolicyMap::RetargetResult r;
		PolicyMap::Retarget_SEH(mapAt, layout, 8, 80, 6, &r);
		CHECK(r.status == PolicyMap::RetargetResult::Status::Done && r.changed == 0 && r.mismatched == 0);
	}
	// A block whose header counts are inconsistent (more elements than the container holds, as a
	// wrong offset could land on): the view refuses it with a reason, nothing is walked or written.
	{
		std::vector<uint8_t> bad = block;
		PluginMap::ScriptSetMemory header{};
		std::memcpy(&header, &bad[PolicyMap::Map.offset], sizeof(header));
		header.num = header.max + 1;
		std::memcpy(&bad[PolicyMap::Map.offset], &header, sizeof(header));
		PolicyMap::RetargetResult r;
		PolicyMap::Retarget_SEH(bad.data() + PolicyMap::Map.offset, layout, 7, 70, 5, &r);
		CHECK(r.status == PolicyMap::RetargetResult::Status::NotAMap);
		CHECK(r.reason != nullptr && std::strlen(r.reason) > 0 && r.changed == 0);
		CHECK(valueOf(0) == 5);
	}
	// A pointer that is not memory (a wrong offset on a build whose block holds something else):
	// the fault is caught, the result says Fault.
	{
		PolicyMap::RetargetResult r;
		PolicyMap::Retarget_SEH(reinterpret_cast<uint8_t*>(0x10), layout, 7, 70, 5, &r);
		CHECK(r.status == PolicyMap::RetargetResult::Status::Fault && r.changed == 0);
	}
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
	// The engine's layout of that map: 20-byte slots, value +8, HashNextId +12, HashIndex +16.
	WalkCheck("fallback", 20, 8, 12, 16);

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
	// A 12-byte key then: value +12, HashNextId +16, HashIndex +20, 24-byte slots.
	WalkCheck("row", 24, 12, 16, 20);

	// FitsInBlock: the whole map must lie inside the member's block, as the loader sizes it for
	// this build. g_answer is Row here, so the map is at 0x38.
	{
		const size_t end = static_cast<size_t>(PolicyMap::Map.offset) + sizeof(PluginMap::ScriptSetMemory);
		CHECK(PolicyMap::FitsInBlock(end));
		CHECK(PolicyMap::FitsInBlock(end + 8));
		CHECK(!PolicyMap::FitsInBlock(end - 1));
		CHECK(!PolicyMap::FitsInBlock(0));
	}

	// A key the loader cannot resolve: no layout, so nothing is walked.
	g_answer = Answer::Fail;
	Resolve();
	CHECK(PolicyMap::KeyIndex.offset < 0 && PolicyMap::KeySerial.offset < 0);
	CHECK(!PolicyMap::Layout().valid);
	// A map offset that did not resolve fits no block.
	PolicyMap::Map.offset = -1;
	CHECK(!PolicyMap::FitsInBlock(0x1000));

	std::printf("policy_map_tests: %d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
