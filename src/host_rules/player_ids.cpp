#include "player_ids.h"
#include "player_list.h"
#include "../plugin_helpers.h"

#if defined(MODLOADER_SERVER_BUILD) || defined(MODLOADER_CLIENT_BUILD)
#  include "SDK/Engine_classes.hpp"
#  include "SDK/Chimera_classes.hpp"
#  include "../server_utility_fields.h"
#  define PLAYER_IDS_HAS_SDK 1
#else
#  define PLAYER_IDS_HAS_SDK 0
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#if PLAYER_IDS_HAS_SDK
namespace
{
	constexpr int kMaxIdChars = 256;

	// Copy the ID into buf (null terminated). The engine hands back a string
	// it allocated and the SDK never frees it, so it is released here through
	// the loader's engine allocator. Normal function: FString work only, no
	// __try (an SEH function cannot hold objects that need unwinding).
	bool ReadWide(SDK::APlayerState* ps, wchar_t* buf)
	{
		buf[0] = L'\0';
		if (!ps || !ps->IsA(SDK::ACrPlayerStateBase::StaticClass()))
			return false;

		SDK::FString id = static_cast<SDK::ACrPlayerStateBase*>(ps)->GetUniqueNetIdAsString();
		wchar_t* data = id.CStr();
		if (!data)
			return false;

		const int len = id.Num() > 0 ? id.Num() - 1 : 0;
		if (len > 0 && len < kMaxIdChars)
		{
			wmemcpy(buf, data, static_cast<size_t>(len));
			buf[len] = L'\0';
		}

		auto* hooks = GetHooks();
		if (hooks && hooks->Memory && hooks->Memory->IsAllocatorAvailable())
			hooks->Memory->Free(data);
		return buf[0] != L'\0';
	}

	// A controller read can fault when the object is already going away, so it
	// sits in a function of its own (no objects with destructors, as __try needs).
	SDK::APlayerState* StateOf(void* playerController)
	{
		__try
		{
			if (!ServerUtilityFields::PlayerLookup.On())
				return nullptr;

			SDK::APlayerState** stateAt = ServerUtilityFields::ControllerPlayerState.In(playerController);
			return stateAt ? *stateAt : nullptr;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return nullptr;
		}
	}

	bool ReadGuarded(SDK::APlayerState* ps, wchar_t* buf)
	{
		__try
		{
			return ReadWide(ps, buf);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			buf[0] = L'\0';
			return false;
		}
	}
}
#endif

namespace PlayerIds
{
	std::string Read(SDK::APlayerState* ps)
	{
#if PLAYER_IDS_HAS_SDK
		wchar_t buf[kMaxIdChars] = {};
		if (!ReadGuarded(ps, buf))
			return {};

		const int bytes = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
		if (bytes <= 1)
			return {};

		std::string out(static_cast<size_t>(bytes), '\0');
		WideCharToMultiByte(CP_UTF8, 0, buf, -1, out.data(), bytes, nullptr, nullptr);
		out.resize(static_cast<size_t>(bytes - 1));
		return out;
#else
		(void)ps;
		return {};
#endif
	}

	std::string OfController(void* playerController)
	{
#if PLAYER_IDS_HAS_SDK
		return Read(StateOf(playerController));
#else
		(void)playerController;
		return {};
#endif
	}

	bool ExemptFromVersion(SDK::APlayerController* pc)
	{
#if PLAYER_IDS_HAS_SDK
		if (!pc)
			return false;

		const std::string id = OfController(pc);
		if (id.empty())
			return false;

		PlayerList::Entry entry;
		return PlayerList::Find(id, entry) && entry.ExemptsVersion();
#else
		(void)pc;
		return false;
#endif
	}
}
