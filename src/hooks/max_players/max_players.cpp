#include "max_players.h"
#include "plugin_helpers.h"
#include "game_signatures.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
// ACrGameModeBase::PreLogin pattern
//
// This pattern matches the prologue of the function.  The hardcoded player
// limit lives later in the body as:
//
//   83 FB 04   cmp  ebx, 4
//7C xx      jl   short <allow_connection>
//
// We scan forward from the pattern match to find this `cmp ebx, imm8`
// instruction and patch the immediate byte (04) to the desired value.
//
// The pattern, scan window and instruction encodings live in
// game_signatures.h.
// ---------------------------------------------------------------------------
static constexpr size_t SCAN_WINDOW = GameSig::PRELOGIN_SCAN_WINDOW;
static constexpr uint8_t CMP_EBX_OPCODE = GameSig::PRELOGIN_CMP_EBX_OPCODE;
static constexpr uint8_t CMP_EBX_MODRM = GameSig::PRELOGIN_CMP_EBX_MODRM;
static constexpr uint8_t JL_OPCODE = GameSig::PRELOGIN_JL_OPCODE;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static uintptr_t g_patchAddress = 0; // Address of the immediate byte (the "4")
static uint8_t g_originalValue = 0; // Original value at that address
static bool g_patched = false;

// Resolved during OnPluginLoadHooks; 0 means the pattern missed on this build.
static uintptr_t g_preLoginAddr = 0;

void MaxPlayersHook::Resolve(IPluginSelf* self, IPluginHookScanner* scanner)
{
	if (!self || !scanner)
		return;

	// Optional: a miss leaves the server on the stock 4-player limit rather than
	// refusing the whole plugin, which is what the old scan-at-init path did.
	g_preLoginAddr = scanner->ResolveOptional(
		self, "ACrGameModeBase::PreLogin", GameSig::GAMEMODE_PRELOGIN);
}

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------
void MaxPlayersHook::Install(int maxPlayers)
{
	if (maxPlayers <= 0)
	{
		LOG_INFO("[MaxPlayers] MaxPlayers is 0 or negative - patch disabled");
		return;
	}

	// Clamp to signed byte range (cmp r32, imm8 uses a sign-extended byte)
	if (maxPlayers > 127)
	{
		LOG_WARN("[MaxPlayers] MaxPlayers %d exceeds imm8 range, clamping to 127", maxPlayers);
		maxPlayers = 127;
	}

	LOG_INFO("[MaxPlayers] Attempting to patch max player limit to %d...", maxPlayers);

	auto* hooks = GetHooks();
	if (!hooks)
	{
		LOG_ERROR("[MaxPlayers] Hooks interface not available");
		return;
	}

	// Step 1: PreLogin, resolved back in OnPluginLoadHooks
	uintptr_t preLoginAddr = g_preLoginAddr;
	if (preLoginAddr == 0)
	{
		LOG_ERROR("[MaxPlayers] ACrGameModeBase::PreLogin unresolved");
		return;
	}

	LOG_INFO("[MaxPlayers] Found PreLogin at 0x%llX",
	         static_cast<unsigned long long>(preLoginAddr));

	// Step 2: Scan forward for `83 FB xx 7C xx`
	auto bytes = reinterpret_cast<const uint8_t*>(preLoginAddr);
	uintptr_t cmpAddr = 0;

	for (size_t offset = 0; offset + 4 < SCAN_WINDOW; ++offset)
	{
		if (bytes[offset] == CMP_EBX_OPCODE &&
			bytes[offset + 1] == CMP_EBX_MODRM &&
			bytes[offset + 3] == JL_OPCODE)
		{
			cmpAddr = preLoginAddr + offset;
			uint8_t currentLimit = bytes[offset + 2];

			LOG_INFO("[MaxPlayers] Found `cmp ebx, 0x%02X` at PreLogin+0x%zX (abs 0x%llX)",
			         currentLimit, offset, static_cast<unsigned long long>(cmpAddr));

			// Sanity: the original should be a small player count (1-32 ish)
			if (currentLimit == 0 || currentLimit > 64)
			{
				LOG_WARN(
					"[MaxPlayers] Unexpected original limit %d - this might be the wrong instruction, continuing scan...",
					currentLimit);
				cmpAddr = 0;
				continue;
			}

			break;
		}
	}

	if (cmpAddr == 0)
	{
		LOG_ERROR("[MaxPlayers] Could not find `cmp ebx, imm8; jl` in PreLogin body (scanned 0x%zX bytes)",
		          SCAN_WINDOW);
		return;
	}

	// The immediate byte is at cmpAddr + 2
	uintptr_t immAddr = cmpAddr + 2;

	// Read the original value
	if (!hooks->Memory->Read(immAddr, &g_originalValue, 1))
	{
		LOG_ERROR("[MaxPlayers] Failed to read original byte at 0x%llX",
		          static_cast<unsigned long long>(immAddr));
		return;
	}

	LOG_INFO("[MaxPlayers] Original max players value: %d", g_originalValue);

	if (g_originalValue == static_cast<uint8_t>(maxPlayers))
	{
		LOG_INFO("[MaxPlayers] Value is already %d - no patch needed", maxPlayers);
		return;
	}

	// Step 3: Patch the immediate byte
	uint8_t newValue = static_cast<uint8_t>(maxPlayers);
	if (!hooks->Memory->Patch(immAddr, &newValue, 1))
	{
		LOG_ERROR("[MaxPlayers] PatchMemory failed at 0x%llX",
		          static_cast<unsigned long long>(immAddr));
		return;
	}

	g_patchAddress = immAddr;
	g_patched = true;

	LOG_INFO("[MaxPlayers] SUCCESS - max players patched from %d to %d (at 0x%llX)",
	         g_originalValue, maxPlayers, static_cast<unsigned long long>(immAddr));
}

void MaxPlayersHook::Remove()
{
	if (!g_patched || g_patchAddress == 0)
	{
		LOG_DEBUG("[MaxPlayers] No patch to restore");
		return;
	}

	auto* hooks = GetHooks();
	if (hooks && hooks->Memory)
	{
		if (hooks->Memory->Patch(g_patchAddress, &g_originalValue, 1))
		{
			LOG_INFO("[MaxPlayers] Restored original max players value %d at 0x%llX",
			         g_originalValue, static_cast<unsigned long long>(g_patchAddress));
		}
		else
		{
			LOG_WARN("[MaxPlayers] Failed to restore original byte at 0x%llX",
			         static_cast<unsigned long long>(g_patchAddress));
		}
	}

	g_patchAddress = 0;
	g_originalValue = 0;
	g_patched = false;
}
