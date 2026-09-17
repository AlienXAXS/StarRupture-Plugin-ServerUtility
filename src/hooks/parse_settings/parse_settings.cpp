#include "parse_settings.h"
#include "plugin_helpers.h"
#include "game_signatures.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <exception>
#include <string>

// ---------------------------------------------------------------------------
// UCrDedicatedServerSettingsComp memory layout
//
// ParseSettings writes five fields on the component object.  The offsets
// are defined in game_signatures.h (GameSig::DedicatedServerSettingsComp)
// and were derived from the IDA decompilation of the original function.
// ---------------------------------------------------------------------------

// Minimal FString mirror so we can manipulate engine strings from outside.
// Layout matches UE4/UE5 TArray<TCHAR> used as FString storage.
struct EngineString
{
	wchar_t* Data; // AllocatorInstance.Data
	int32_t Num; // ArrayNum  (includes null terminator)
	int32_t Max; // ArrayMax
};

// ---------------------------------------------------------------------------
// Field accessor namespace for UCrDedicatedServerSettingsComp
// Offsets come from game_signatures.h.
// ---------------------------------------------------------------------------
namespace FieldAccessor
{
	using namespace GameSig::DedicatedServerSettingsComp;

	inline EngineString* GetSessionName(void* thisPtr)
	{
		return reinterpret_cast<EngineString*>(reinterpret_cast<uint8_t*>(thisPtr) + OFFSET_SESSION_NAME);
	}

	inline EngineString* GetSaveGameName(void* thisPtr)
	{
		return reinterpret_cast<EngineString*>(reinterpret_cast<uint8_t*>(thisPtr) + OFFSET_SAVEGAME_NAME);
	}

	inline int32_t* GetSaveGameInterval(void* thisPtr)
	{
		return reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(thisPtr) + OFFSET_SAVE_INTERVAL);
	}

	inline bool* GetStartNewGame(void* thisPtr)
	{
		return reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(thisPtr) + OFFSET_START_NEW_GAME);
	}

	inline bool* GetLoadSavedGame(void* thisPtr)
	{
		return reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(thisPtr) + OFFSET_LOAD_SAVED_GAME);
	}
}

// ---------------------------------------------------------------------------
// Command-line parameter names
// ---------------------------------------------------------------------------
static constexpr auto PARAM_SESSION_NAME = L"-SessionName=";
static constexpr auto PARAM_SAVE_INTERVAL = L"-SaveGameInterval=";

// Save game name is always AutoSave0
static constexpr auto SAVE_GAME_NAME = L"AutoSave0.sav";

// Default save interval in seconds (5 minutes)
static constexpr int DEFAULT_SAVE_INTERVAL = 300;

// ---------------------------------------------------------------------------
// Helper: assign an FString using the engine's own allocator.
//
// The mod loader exposes FMemory::Malloc / FMemory::Free through
// hooks->Memory->Alloc / Free, so we never need to locate them ourselves.
// Both go through FMallocBinned2, which means the canary values are correct
// and the GC destructor will not crash when it later frees FString::Data.
//
// Because the hook skips the original function entirely when command-line
// params are present, the FString fields are guaranteed to be in their
// default-constructed state (Data=null, Num=0, Max=0) from UObject
// initialisation.  We therefore:
//   1. Free old Data via hooks->Memory->Free only if it looks valid
//   2. Allocate a new buffer via hooks->Memory->Alloc
//   3. Copy the string into the new buffer
//   4. Update Num and Max
// ---------------------------------------------------------------------------
static bool AssignEngineString(EngineString* str, const wchar_t* value)
{
	if (!str)
		return false;

	auto* hooks = GetHooks();
	if (!hooks || !hooks->Memory)
	{
		LOG_ERROR("[AssignEngineString] Memory interface not available!");
		return false;
	}

	if (!hooks->Memory->IsAllocatorAvailable())
	{
		LOG_ERROR("[AssignEngineString] Engine allocator not available via mod loader!");
		return false;
	}

	// Free old allocation if present AND looks valid.
	// Since we skip the original, FStrings should be default-constructed
	// (Data=null), but handle the case defensively.
	if (str->Data)
	{
		bool looksValid = (str->Num > 0 && str->Max > 0 && str->Num <= str->Max && str->Max < 0x100000);

		if (looksValid)
		{
			LOG_DEBUG("[AssignEngineString] Freeing old Data at %p (Num=%d, Max=%d)",
			          static_cast<void*>(str->Data), str->Num, str->Max);
			hooks->Memory->Free(str->Data);
		}
		else
		{
			LOG_WARN("[AssignEngineString] Skipping free of suspicious Data=%p (Num=%d, Max=%d) - likely uninitialized",
			         static_cast<void*>(str->Data), str->Num, str->Max);
		}

		str->Data = nullptr;
		str->Num = 0;
		str->Max = 0;
	}

	if (!value || value[0] == L'\0')
	{
		// Leave as empty/null - already cleared above
		return true;
	}

	const int32_t len = static_cast<int32_t>(wcslen(value));
	const int32_t numElements = len + 1; // include null terminator
	const size_t byteSize = static_cast<size_t>(numElements) * sizeof(wchar_t);

	// Allocate via the engine's FMemory::Malloc with default alignment
	// UE5 FString uses alignment of __STDCPP_DEFAULT_NEW_ALIGNMENT__ which is
	// typically 16 on x64 MSVC.  FMallocBinned2 expects the same alignment
	// that was used at allocation time, so we use 16 to match.
	void* newData = hooks->Memory->Alloc(byteSize, 16);
	if (!newData)
	{
		LOG_ERROR("[AssignEngineString] Memory->Alloc(%zu, 16) returned null!", byteSize);
		return false;
	}

	wmemcpy(static_cast<wchar_t*>(newData), value, static_cast<size_t>(numElements));

	str->Data = static_cast<wchar_t*>(newData);
	str->Num = numElements;
	str->Max = numElements;

	LOG_DEBUG("[AssignEngineString] Allocated new Data at %p (Num=%d, Max=%d)",
	          newData, str->Num, str->Max);

	return true;
}

// ---------------------------------------------------------------------------
// Helper: parse a single command-line parameter value.
// Returns true and fills 'out' when the parameter is found.
// Quoted values (e.g. -SessionName="My Server") are supported.
// ---------------------------------------------------------------------------
static bool GetCommandLineParam(const wchar_t* paramName, std::wstring& out)
{
	const wchar_t* cmdLine = GetCommandLineW();
	if (!cmdLine)
		return false;

	const wchar_t* pos = wcsstr(cmdLine, paramName);
	if (!pos)
		return false;

	pos += wcslen(paramName); // advance past "ParamName=="

	bool quoted = (*pos == L'"');
	if (quoted)
		++pos;

	const wchar_t* end = pos;
	if (quoted)
	{
		while (*end && *end != L'"')
			++end;
	}
	else
	{
		while (*end && *end != L' ' && *end != L'\t')
			++end;
	}

	out.assign(pos, end);
	return !out.empty();
}

// ---------------------------------------------------------------------------
// Check whether the required parameters are present on the command line.
// ---------------------------------------------------------------------------
static bool RequiredParamsPresent()
{
	std::wstring tmp;
	return GetCommandLineParam(PARAM_SESSION_NAME, tmp);
}

// ---------------------------------------------------------------------------
// Determine whether a prior session save exists on disk.
// ---------------------------------------------------------------------------
static bool SaveGameExists(const std::wstring& sessionName)
{
	LOG_DEBUG("[SaveGameExists] Checking for existing save file for session: %ls", sessionName.c_str());

	wchar_t exePath[MAX_PATH] = {};
	if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
	{
		LOG_ERROR("[SaveGameExists] GetModuleFileNameW failed with error code: %lu", GetLastError());
		return false;
	}

	LOG_DEBUG("[SaveGameExists] Executable path: %ls", exePath);

	wchar_t* lastSlash = wcsrchr(exePath, L'\\');
	if (!lastSlash)
	{
		LOG_ERROR("[SaveGameExists] No backslash found in exe path: %ls", exePath);
		return false;
	}
	*lastSlash = L'\0';

	LOG_DEBUG("[SaveGameExists] Binary directory: %ls", exePath);

	std::wstring basePath = exePath;
	for (int i = 0; i < 2; ++i)
	{
		const size_t sep = basePath.rfind(L'\\');
		if (sep == std::wstring::npos)
		{
			LOG_ERROR("[SaveGameExists] Could not navigate back %d directories from: %ls", i + 1, basePath.c_str());
			return false;
		}
		basePath.erase(sep);
		LOG_DEBUG("[SaveGameExists] After navigating up %d level(s): %ls", i + 1, basePath.c_str());
	}

	LOG_INFO("[SaveGameExists] Root save path: %ls", basePath.c_str());

	std::wstring savePath = basePath;
	savePath += L"\\Saved\\SaveGames\\";
	savePath += sessionName;
	savePath += L"\\AutoSave0.sav";

	LOG_INFO("[SaveGameExists] Full save file path: %ls", savePath.c_str());

	const DWORD attr = GetFileAttributesW(savePath.c_str());

	if (attr == INVALID_FILE_ATTRIBUTES)
	{
		const DWORD error = GetLastError();
		LOG_DEBUG("[SaveGameExists] File does not exist or is inaccessible (error %lu)", error);
		return false;
	}

	if (attr & FILE_ATTRIBUTE_DIRECTORY)
	{
		LOG_WARN("[SaveGameExists] Path exists but is a directory, not a file");
		return false;
	}

	LOG_INFO("[SaveGameExists] Save file found! Will load existing session.");
	return true;
}

// ---------------------------------------------------------------------------
// Hook state
// ---------------------------------------------------------------------------
using ParseSettings_t = __int64(__fastcall*)(void* thisPtr);
static ParseSettings_t g_originalParseSettings = nullptr;
static HookHandle g_hookHandle = nullptr;

// ---------------------------------------------------------------------------
// Detour
//
// Strategy: When command-line parameters are present, SKIP the original
// function entirely and write all five fields ourselves.  This eliminates
// the DSSettings.txt dependency — no file needed, no JSON parsing, no
// risk of garbage FString state from a failed LoadFileToString.
//
// The FString fields at +0xB8 and +0xC8 are guaranteed to be in their
// default-constructed state (Data=null, Num=0, Max=0) since UObject
// zero-inits its memory.  We allocate via the engine's own FMemory::Malloc
// so the GC destructor sees valid FMallocBinned2 canary values.
//
// When command-line params are NOT present, we fall through to the original
// function for normal DSSettings.txt loading.
// ---------------------------------------------------------------------------
static __int64 __fastcall Hook_ParseSettings(void* thisPtr)
{
	// Validate thisPtr
	if (!thisPtr)
	{
		LOG_ERROR("[Hook_ParseSettings] thisPtr is NULL - delegating to original");
		__int64 result = g_originalParseSettings(thisPtr);
		return result;
	}

	MEMORY_BASIC_INFORMATION mbi;
	if (VirtualQuery(thisPtr, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT)
	{
		LOG_ERROR("[Hook_ParseSettings] thisPtr (0x%p) points to invalid memory - delegating to original", thisPtr);
		__int64 result = g_originalParseSettings(thisPtr);
		return result;
	}

	if (!RequiredParamsPresent())
	{
		LOG_DEBUG("[Hook_ParseSettings] Required command-line parameters not present - delegating to DSSettings.txt");
		__int64 result = g_originalParseSettings(thisPtr);
		return result;
	}

	auto* hooks = GetHooks();
	if (!hooks->Memory)
	{
		LOG_ERROR(
			"[Hook_ParseSettings] Engine allocator not resolved - cannot safely set FStrings, delegating to original");
		__int64 result = g_originalParseSettings(thisPtr);
		return result;
	}

	LOG_DEBUG("[Hook_ParseSettings] Command-line parameters detected - bypassing original (no DSSettings.txt needed)");

	// -----------------------------------------------------------------------
	// Step 1: Parse command-line parameters.
	// -----------------------------------------------------------------------
	std::wstring sessionName, saveGameInterval;

	GetCommandLineParam(PARAM_SESSION_NAME, sessionName);

	int saveInterval = DEFAULT_SAVE_INTERVAL;
	if (GetCommandLineParam(PARAM_SAVE_INTERVAL, saveGameInterval))
	{
		saveInterval = _wtoi(saveGameInterval.c_str());
		LOG_INFO("  SessionName  = %ls", sessionName.c_str());
		LOG_INFO("  SaveGameName = %ls", SAVE_GAME_NAME);
		LOG_INFO("  SaveGameInterval = %d seconds", saveInterval);
	}
	else
	{
		LOG_INFO("  SessionName  = %ls", sessionName.c_str());
		LOG_INFO("  SaveGameName = %ls (fixed)", SAVE_GAME_NAME);
		LOG_INFO("  SaveGameInterval = %d seconds", saveInterval);
	}

	const bool hasSave = SaveGameExists(sessionName);
	const bool bStartNew = !hasSave;
	const bool bLoadSaved = hasSave;

	LOG_INFO("  AutoSave found   = %s  ->  %s",
	         hasSave ? "yes" : "no",
	         hasSave ? "loading existing session" : "starting new session");

	// -----------------------------------------------------------------------
	// Step 2: Write all five fields directly onto the component.
	//
	// The original function is NOT called.  FString fields should be in
	// their default-constructed state (zeroed by UObject allocation).
	// We allocate string data via FMemory::Malloc so GC cleanup works.
	//
	// Field layout from IDA pseudocode:
	//   +0xB8  SessionName      (FString — Data/Num/Max)
	//   +0xC8  SaveGameName     (FString — Data/Num/Max)
	//   +0xD8  SaveGameInterval (int32)
	//+0xDC  bStartNewGame    (bool)
	//   +0xDD  bLoadSavedGame   (bool)
	// -----------------------------------------------------------------------
	LOG_DEBUG("[Hook_ParseSettings] Writing fields directly (thisPtr at 0x%p)...", thisPtr);

	try
	{
		if (!AssignEngineString(FieldAccessor::GetSessionName(thisPtr), sessionName.c_str()))
		{
			LOG_ERROR("[Hook_ParseSettings] Failed to assign SessionName");
		}
		else
		{
			LOG_DEBUG("[Hook_ParseSettings] SessionName assigned successfully");
		}

		if (!AssignEngineString(FieldAccessor::GetSaveGameName(thisPtr), SAVE_GAME_NAME))
		{
			LOG_ERROR("[Hook_ParseSettings] Failed to assign SaveGameName");
		}
		else
		{
			LOG_DEBUG("[Hook_ParseSettings] SaveGameName assigned successfully");
		}

		*FieldAccessor::GetSaveGameInterval(thisPtr) = saveInterval;
		LOG_DEBUG("[Hook_ParseSettings] SaveGameInterval set to %d", saveInterval);

		*FieldAccessor::GetStartNewGame(thisPtr) = bStartNew;
		*FieldAccessor::GetLoadSavedGame(thisPtr) = bLoadSaved;
		LOG_DEBUG("[Hook_ParseSettings] bStartNewGame=%s, bLoadSavedGame=%s",
		          bStartNew ? "true" : "false", bLoadSaved ? "true" : "false");
	}
	catch (const std::exception& ex)
	{
		LOG_ERROR("[Hook_ParseSettings] C++ exception while setting fields: %s", ex.what());
		LOG_ERROR("[Hook_ParseSettings] Falling back to original function");
		__int64 result = g_originalParseSettings(thisPtr);
		return result;
	}
	catch (...)
	{
		LOG_ERROR("[Hook_ParseSettings] Unknown exception while setting fields");
		LOG_ERROR("[Hook_ParseSettings] Falling back to original function");
		__int64 result = g_originalParseSettings(thisPtr);
		return result;
	}

	// -----------------------------------------------------------------------
	// Step 3: Read back and verify the assigned values for diagnostics
	// -----------------------------------------------------------------------
	{
		EngineString* sessionStr = FieldAccessor::GetSessionName(thisPtr);
		EngineString* saveStr = FieldAccessor::GetSaveGameName(thisPtr);

		if (sessionStr->Data && sessionStr->Num > 0)
		{
			LOG_DEBUG("[Hook_ParseSettings] Readback SessionName: \"%ls\" (Num=%d, Max=%d)",
			          sessionStr->Data, sessionStr->Num, sessionStr->Max);
		}
		else
		{
			LOG_ERROR("[Hook_ParseSettings] Readback SessionName: EMPTY/NULL!");
		}

		if (saveStr->Data && saveStr->Num > 0)
		{
			LOG_DEBUG("[Hook_ParseSettings] Readback SaveGameName: \"%ls\" (Num=%d, Max=%d)",
			          saveStr->Data, saveStr->Num, saveStr->Max);
		}
		else
		{
			LOG_ERROR("[Hook_ParseSettings] Readback SaveGameName: EMPTY/NULL!");
		}
	}

	LOG_INFO("[Hook_ParseSettings] Settings applied (SaveGameInterval=%d, bStartNewGame=%s, bLoadSavedGame=%s)",
	         *FieldAccessor::GetSaveGameInterval(thisPtr),
	         *FieldAccessor::GetStartNewGame(thisPtr) ? "true" : "false",
	         *FieldAccessor::GetLoadSavedGame(thisPtr) ? "true" : "false");

	// Return 1 (success) — we've populated all five fields.
	return 1;
}

// ---------------------------------------------------------------------------
// Public API: ParseSettingsHook namespace
// ---------------------------------------------------------------------------
void ParseSettingsHook::Install(uintptr_t targetAddress)
{
	LOG_INFO("[ParseSettingsHook::Install] Installing hook at 0x%llX...",
	         static_cast<unsigned long long>(targetAddress));

	if (g_hookHandle)
	{
		LOG_WARN("[ParseSettingsHook::Install] Hook already installed - skipping");
		return;
	}

	// Install the inline hook via the mod loader's hook interface
	auto* hooks = GetHooks();
	if (!hooks || !hooks->Hooks)
	{
		LOG_ERROR("[ParseSettingsHook::Install] Hook interface not available!");
		return;
	}

	g_hookHandle = hooks->Hooks->Install(
		targetAddress,
		reinterpret_cast<void*>(&Hook_ParseSettings),
		reinterpret_cast<void**>(&g_originalParseSettings));

	if (!g_hookHandle)
	{
		LOG_ERROR("[ParseSettingsHook::Install] InstallHook failed!");
		return;
	}

	LOG_INFO("[ParseSettingsHook::Install] Hook installed successfully (handle=%p)", g_hookHandle);
}

void ParseSettingsHook::Remove()
{
	if (!g_hookHandle)
	{
		LOG_DEBUG("[ParseSettingsHook::Remove] No hook installed - nothing to remove");
		return;
	}

	LOG_INFO("[ParseSettingsHook::Remove] Removing hook (handle=%p)...", g_hookHandle);

	auto* hooks = GetHooks();
	if (hooks && hooks->Hooks)
	{
		hooks->Hooks->Remove(g_hookHandle);
	}
	else
	{
		LOG_WARN("[ParseSettingsHook::Remove] Hook interface not available - cannot remove hook cleanly");
	}

	g_hookHandle = nullptr;
	g_originalParseSettings = nullptr;

	LOG_INFO("[ParseSettingsHook::Remove] Hook removed successfully");
}
