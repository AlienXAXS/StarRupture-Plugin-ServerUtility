#include "auto_profession.h"
#include "plugin_helpers.h"
#include "game_signatures.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
// ACrGameModeBase::GetProfessionForNewPlayer
//
// Pattern and patch bytes live in game_signatures.h.  We patch the prologue
// to `mov eax, 1; ret` (return EProfessionType::Soldier).
//
// Total: 6 bytes.  The original prologue is at least 6 bytes long (starts
// with `48 8B C4 48 89 50 xx` - 7 bytes), so this is safe.
// ---------------------------------------------------------------------------

static constexpr const uint8_t* PATCH_BYTES = GameSig::GET_PROFESSION_PATCH_BYTES;
static constexpr size_t PATCH_SIZE = sizeof(GameSig::GET_PROFESSION_PATCH_BYTES);

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static uintptr_t g_patchAddress = 0; // Start of GetProfessionForNewPlayer
static uint8_t g_originalBytes[PATCH_SIZE]{}; // Saved original prologue bytes
static bool g_patched = false;

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------
// Resolved during OnPluginLoadHooks; 0 means the pattern missed on this build.
static uintptr_t g_getProfessionAddr = 0;

void AutoProfessionHook::Resolve(IPluginSelf* self, IPluginHookScanner* scanner)
{
	if (!self || !scanner)
		return;

	// Optional: the profession patch only matters alongside the MaxPlayers
	// patch, and the plugin already carried on without it.
	g_getProfessionAddr = scanner->ResolveOptional(
		self, "ACrGameModeBase::GetProfessionForNewPlayer", GameSig::GAMEMODE_GET_PROFESSION_FOR_NEW_PLAYER);
}

void AutoProfessionHook::Install()
{
	auto* hooks = GetHooks();
	if (!hooks)
	{
		LOG_ERROR("[AutoProfession] Hooks interface not available");
		return;
	}

	uintptr_t addr = g_getProfessionAddr;
	if (addr == 0)
	{
		LOG_ERROR("[AutoProfession] GetProfessionForNewPlayer unresolved");
		return;
	}

	HMODULE mainModule = GetModuleHandleW(nullptr);
	auto base = reinterpret_cast<uintptr_t>(mainModule);

	LOG_INFO("[AutoProfession] Found GetProfessionForNewPlayer at 0x%llX (base+0x%llX)",
	         static_cast<unsigned long long>(addr),
	         static_cast<unsigned long long>(addr - base));

	// Save original bytes so we can restore them on Remove()
	if (!hooks->Memory->Read(addr, g_originalBytes, PATCH_SIZE))
	{
		LOG_ERROR("[AutoProfession] Failed to read original bytes at 0x%llX",
		          static_cast<unsigned long long>(addr));
		return;
	}

	LOG_DEBUG("[AutoProfession] Original prologue: %02X %02X %02X %02X %02X %02X",
	          g_originalBytes[0], g_originalBytes[1], g_originalBytes[2],
	          g_originalBytes[3], g_originalBytes[4], g_originalBytes[5]);

	// Apply the patch: mov eax, 1 ; ret
	if (!hooks->Memory->Patch(addr, PATCH_BYTES, PATCH_SIZE))
	{
		LOG_ERROR("[AutoProfession] PatchMemory failed at 0x%llX",
		          static_cast<unsigned long long>(addr));
		return;
	}

	g_patchAddress = addr;
	g_patched = true;

	LOG_INFO("[AutoProfession] SUCCESS - GetProfessionForNewPlayer patched to always return Soldier (1)");
}

void AutoProfessionHook::Remove()
{
	if (!g_patched || g_patchAddress == 0)
	{
		LOG_DEBUG("[AutoProfession] No patch to restore");
		return;
	}

	auto* hooks = GetHooks();
	if (hooks && hooks->Memory)
	{
		if (hooks->Memory->Patch(g_patchAddress, g_originalBytes, PATCH_SIZE))
		{
			LOG_INFO("[AutoProfession] Restored original GetProfessionForNewPlayer prologue at 0x%llX",
			         static_cast<unsigned long long>(g_patchAddress));
		}
		else
		{
			LOG_WARN("[AutoProfession] Failed to restore original bytes at 0x%llX",
			         static_cast<unsigned long long>(g_patchAddress));
		}
	}

	g_patchAddress = 0;
	g_patched = false;
	std::memset(g_originalBytes, 0, PATCH_SIZE);
}
