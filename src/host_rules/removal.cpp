#include "removal.h"
#include "line_text.h"
#include "player_ids.h"
#include "rules_model.h"
#include "../plugin_helpers.h"

#include "SDK/Engine_classes.hpp"
#include "object_ref.h"
#include "../server_utility_fields.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
	// The game's FText for a reason. Built once per distinct text and kept:
	// an FText is a plain 16-byte handle to engine-owned data, the same
	// pattern BetterDrone uses for its labels. The SDK's FString never frees
	// the buffer it was built from and the engine text is never released by
	// us, so the handle stays valid. Game thread only.
	const SDK::FText* TextFor(const std::string& reason)
	{
		static std::unordered_map<std::string, SDK::FText> s_texts;

		auto it = s_texts.find(reason);
		if (it != s_texts.end())
			return &it->second;

		std::wstring wide;
		const int len = MultiByteToWideChar(CP_UTF8, 0, reason.c_str(), -1, nullptr, 0);
		if (len > 1)
		{
			wide.resize(static_cast<size_t>(len));
			MultiByteToWideChar(CP_UTF8, 0, reason.c_str(), -1, wide.data(), len);
		}
		const SDK::FText text = SDK::UKismetTextLibrary::Conv_StringToText(SDK::FString(wide.c_str()));
		return &s_texts.emplace(reason, text).first->second;
	}

	// One removal per join. A join has its own player controller, so the
	// controller is the key, kept as an ObjectRef and never as a raw pointer.
	// A new controller can take a dead one's slot, so an entry also ends after
	// kGuardSeconds, long after the removed player's connection has closed.
	struct Removed
	{
		ObjectRef<SDK::APlayerController>     pc;
		std::chrono::steady_clock::time_point at;
	};
	constexpr int kGuardSeconds = 30;

	bool AlreadyRemoved(SDK::APlayerController* pc)
	{
		static std::vector<Removed> s_removed;

		const auto now = std::chrono::steady_clock::now();
		for (size_t i = 0; i < s_removed.size();)
		{
			const bool expired = now - s_removed[i].at > std::chrono::seconds(kGuardSeconds);
			if (expired || !s_removed[i].pc.Get())
				s_removed.erase(s_removed.begin() + i);
			else
				++i;
		}

		for (const Removed& r : s_removed)
			if (r.pc.Get() == pc)
				return true;

		s_removed.push_back({ ObjectRef<SDK::APlayerController>(pc), now });
		return false;
	}

	std::string PlayerName(SDK::APlayerController* pc)
	{
		SDK::APlayerState** stateAt = ServerUtilityFields::ControllerPlayerState.In(pc);
		SDK::APlayerState* state = stateAt ? *stateAt : nullptr;
		const SDK::FString* name = ServerUtilityFields::PlayerStateName.In(state);
		if (name && name->Num() > 0)
			return name->ToString();
		return "unknown";
	}

	// The loader's controller pointer can be stale by the time the callback
	// runs, so every read of it sits behind a structured-exception guard, as
	// in the admin kick. Kept apart from the string work because __try cannot
	// share a function with objects that need unwinding.
	void RemoveChecked(SDK::APlayerController* pc, const char* plainReason)
	{
		if (PlayerIds::ExemptFromVersion(pc))
		{
			LOG_INFO("[HostRules] Player '%s' is below a minimum version but exempt - kept", LineText::OneLine(PlayerName(pc)).c_str());
			return;
		}

		if (AlreadyRemoved(pc))
			return;

		LOG_INFO("[HostRules] Player '%s' removed: %s", LineText::OneLine(PlayerName(pc)).c_str(), LineText::OneLine(plainReason).c_str());
		Removal::Kick(pc, plainReason);
	}

	bool RemoveGuarded(SDK::APlayerController* pc, const char* plainReason)
	{
		__try
		{
			RemoveChecked(pc, plainReason);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// A player below a minimum whose ID cannot be read yet is held, as a weak
	// reference and a copy of the reason (the loader's text is valid only for the
	// callback), and tried again each tick: the exemption can only be checked once
	// the ID reads. A rejoin is a new controller, so it starts fresh. Game thread only.
	struct Held
	{
		ObjectRef<SDK::APlayerController> pc;
		std::string                       reason;
		ULONGLONG                         sinceMs;
	};
	std::vector<Held> g_held;
	bool              g_tickRegistered = false;

	void OnTick(float)
	{
		const ULONGLONG now = GetTickCount64();
		for (size_t i = 0; i < g_held.size();)
		{
			SDK::APlayerController* pc = g_held[i].pc.Get();
			const bool idRead = pc && !PlayerIds::OfController(pc).empty();
			const RulesModel::HeldStep step = RulesModel::NextHeldStep(pc != nullptr, idRead, now - g_held[i].sinceMs);
			if (step == RulesModel::HeldStep::Wait)
			{
				++i;
				continue;
			}

			const std::string reason = g_held[i].reason;
			g_held.erase(g_held.begin() + i);
			if (step == RulesModel::HeldStep::Drop)
				continue;
			if (step == RulesModel::HeldStep::ActNoId)
				LOG_WARN("[HostRules] A player's ID was not readable after %d seconds - removed as not exempt",
				         static_cast<int>(RulesModel::kLateIdGraceMs / 1000));
			if (!RemoveGuarded(pc, reason.c_str()))
				LOG_WARN("[HostRules] Exception while removing a player - skipped");
		}
	}

	void OnBelowMinimum(void* playerController, const char* plainReason)
	{
		auto* hooks = GetHooks();
		if (!hooks || !hooks->Engine || !hooks->Engine->IsGameThread())
		{
			LOG_WARN("[HostRules] Below-minimum callback arrived off the game thread - ignored");
			return;
		}

		auto* pc = static_cast<SDK::APlayerController*>(playerController);
		if (!pc || !plainReason)
			return;

		if (PlayerIds::OfController(pc).empty())
		{
			const bool held = std::any_of(g_held.begin(), g_held.end(), [&](const Held& h) { return h.pc.Get() == pc; });
			if (!held)
				g_held.push_back({ ObjectRef<SDK::APlayerController>(pc), plainReason, GetTickCount64() });
			return;
		}

		if (!RemoveGuarded(pc, plainReason))
			LOG_WARN("[HostRules] Exception while removing a player - skipped");
	}

	bool g_registered = false;
}

namespace Removal
{
	void Kick(SDK::APlayerController* pc, const std::string& reason)
	{
		pc->ClientReturnToMainMenuWithTextReason(reason.empty() ? SDK::FText{} : *TextFor(reason));
	}

	void Init()
	{
		auto* hooks = GetHooks();
		if (!hooks || !hooks->HostRules || !GetSelf())
		{
			LOG_INFO("[HostRules] This mod loader has no host rules support - minimum version removal is off");
			return;
		}
		hooks->HostRules->RegisterOnClientBelowMinimum(GetSelf(), OnBelowMinimum);
		g_registered = true;
		if (hooks->Engine)
		{
			hooks->Engine->RegisterOnTick(OnTick);
			g_tickRegistered = true;
		}
	}

	void Shutdown()
	{
		auto* hooks = GetHooks();
		if (g_registered && hooks && hooks->HostRules && GetSelf())
			hooks->HostRules->UnregisterOnClientBelowMinimum(GetSelf(), OnBelowMinimum);
		g_registered = false;
		if (g_tickRegistered && hooks && hooks->Engine)
			hooks->Engine->UnregisterOnTick(OnTick);
		g_tickRegistered = false;
		g_held.clear();
	}
}
