#pragma once

#include "cheat_ledger.h"

#include <string>

namespace SDK { class UObject; }

// The game side of the revert ledger: the live ledger, weak references to game objects, the
// accessors that read and write one slot, and the cheats.log file. Server builds only. Everything
// here runs on the game thread, except where a function says otherwise.
namespace CheatLedgerGame
{
	// A slot the ledger can read and write, registered per "feature/slot". obj is the entry's
	// target, resolved and validated just now (null for an Override without a target); it is used
	// inside the call only.
	struct Accessor
	{
		CheatLedger::Value (*read)(SDK::UObject* obj, const CheatLedger::Entry& e) = nullptr;
		bool               (*write)(SDK::UObject* obj, const CheatLedger::Entry& e, const CheatLedger::Value& v) = nullptr;
	};
	void RegisterAccessor(const std::string& feature, const std::string& slot, Accessor accessor);

	// A weak reference to a live object (index, serial, class), and the object back while it is
	// still the same live one; null once it is gone, replaced or being destroyed.
	CheatLedger::Weak MakeWeak(SDK::UObject* obj);
	SDK::UObject*     Resolve(const CheatLedger::Weak& w);

	CheatLedger::Ledger& Get();

	// From PluginInit and OnEngineShutdown / PluginShutdown. Shutdown puts every entry back.
	void Init();
	void Shutdown();

	// Hooks from the host rules and the in-game admin. Game thread.
	void OnPlayerLeft(void* playerController);
	void OnPlayerJoined(void* playerController);
	void OnWorldEnd();
	void Tick();
	// A player's rules were just applied: entries the feature is now denied for are restored.
	void ReconcilePlayer(const std::string& playerId);
}
