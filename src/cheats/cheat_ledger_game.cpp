#include "cheat_ledger_game.h"
#include "cheat_authority.h"
#include "host_rules/admin_ops.h"
#include "host_rules/host_rules.h"
#include "host_rules/ingame_admin.h"
#include "host_rules/ini_file.h"
#include "host_rules/player_ids.h"
#include "host_rules/player_list.h"
#include "object_serial.h"
#include "plugin_helpers.h"

#include "SDK/Engine_classes.hpp"

#include <Windows.h>
#include <cstdio>
#include <map>
#include <memory>

namespace CheatLedgerGame
{
	using namespace CheatLedger;

	namespace
	{
		std::unique_ptr<Ledger>         g_ledger;
		std::map<std::string, Accessor> g_accessors;   // "feature/slot"
		int                             g_listener = 0;

		bool OnGameThread()
		{
			auto* hooks = GetHooks();
			return hooks && hooks->Engine && hooks->Engine->IsGameThread();
		}

		const Accessor* AccessorFor(const Entry& e)
		{
			const auto it = g_accessors.find(e.feature + "/" + e.slot);
			return it == g_accessors.end() ? nullptr : &it->second;
		}

		// One line per commit, with the local time. The file is opened, appended and closed each
		// time: commits are rare, and nothing stays open for a crash to lose.
		void WriteLog(const std::string& line)
		{
			FILE* f = nullptr;
			if (_wfopen_s(&f, IniFile::DataPath(L"cheats.log").c_str(), L"ab") != 0 || !f)
				return;
			SYSTEMTIME t;
			GetLocalTime(&t);
			std::fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, line.c_str());
			std::fclose(f);
		}

		Env MakeEnv()
		{
			Env env;
			env.read = [](const Entry& e) -> Value
			{
				const Accessor* a = AccessorFor(e);
				if (!a || !a->read)
					return Value();
				SDK::UObject* obj = e.target.Valid() ? Resolve(e.target) : nullptr;
				if (e.target.Valid() && !obj)
					return Value();
				return a->read(obj, e);
			};
			env.write = [](const Entry& e, const Value& v) -> bool
			{
				const Accessor* a = AccessorFor(e);
				if (!a || !a->write)
					return false;
				SDK::UObject* obj = e.target.Valid() ? Resolve(e.target) : nullptr;
				if (e.target.Valid() && !obj)
					return false;
				return a->write(obj, e, v);
			};
			env.alive = [](const Weak& w) { return Resolve(w) != nullptr; };
			env.allowed = [](Entry& e) -> Allowance
			{
				// The owner is here: ask with the live proof and keep that state. The owner is
				// gone (an Override outlives them): the state they last had decides.
				SDK::UObject* pc = Resolve(e.conn);
				if (pc)
					e.ownerProven = InGameAdmin::IsProven(pc, e.playerId);
				const bool proven = e.ownerProven;

				CheatAuthority::Env a;
				a.findPlayer = [](const std::string& who, PlayerList::Entry& out) { return PlayerList::Find(who, out); };
				a.isProven   = [proven](const std::string&) { return proven; };
				a.model      = [] { return HostRules::Get(); };
				return CheatAuthority::Decide(a, e.playerId, "BetterCheats", e.feature.c_str()).allowed ? Allowance::Allowed : Allowance::Denied;
			};
			env.nowMs        = [] { return static_cast<uint64_t>(GetTickCount64()); };
			env.isGameThread = [] { return OnGameThread(); };
			env.log          = &WriteLog;
			return env;
		}
	}

	void RegisterAccessor(const std::string& feature, const std::string& slot, Accessor accessor)
	{
		g_accessors[feature + "/" + slot] = accessor;
	}

	Weak MakeWeak(SDK::UObject* obj)
	{
		Weak w;
		if (!obj)
			return w;
		w.index  = obj->Index;
		w.serial = GetObjectSerialNumber(obj->Index);
		w.cls    = reinterpret_cast<uint64_t>(obj->Class);
		w.addr   = reinterpret_cast<uint64_t>(obj);
		return w;
	}

	SDK::UObject* Resolve(const Weak& w)
	{
		if (!w.Valid() || !w.addr || !SDK::UObject::GObjects)
			return nullptr;
		// The checks ObjectRef makes, plus the serial: the slot still holds this object, of this
		// class, and the engine has not started to destroy it.
		SDK::UObject* obj = SDK::UObject::GObjects->GetByIndex(w.index);
		if (reinterpret_cast<uint64_t>(obj) != w.addr)
			return nullptr;
		if (reinterpret_cast<uint64_t>(obj->Class) != w.cls)
			return nullptr;
		if (GetObjectSerialNumber(w.index) != w.serial)
			return nullptr;
		constexpr uint32_t kDeadFlags =
			static_cast<uint32_t>(SDK::EObjectFlags::BeginDestroyed) |
			static_cast<uint32_t>(SDK::EObjectFlags::FinishDestroyed) |
			static_cast<uint32_t>(SDK::EObjectFlags::MirroredGarbage);
		if (static_cast<uint32_t>(obj->Flags) & kDeadFlags)
			return nullptr;
		return obj;
	}

	Ledger& Get()
	{
		if (!g_ledger)
			g_ledger = std::make_unique<Ledger>(MakeEnv());
		return *g_ledger;
	}

	void Init()
	{
		Get();
		// A change to the rules or the player list is checked at the next tick.
		g_listener = AdminOps::AddListener([](uint64_t, unsigned sections)
		{
			if ((sections & (AdminOps::kSectionRules | AdminOps::kSectionPlayers)) && g_ledger)
				g_ledger->MarkReconcileDue();
		});
	}

	void Shutdown()
	{
		if (g_listener)
		{
			AdminOps::RemoveListener(g_listener);
			g_listener = 0;
		}
		if (g_ledger)
			g_ledger->RestoreAll();
	}

	void OnPlayerLeft(void* playerController)
	{
		const std::string id = PlayerIds::OfController(playerController);
		if (!id.empty())
			Get().RestorePlayer(id);
	}

	void OnPlayerJoined(void* playerController)
	{
		const std::string id = PlayerIds::OfController(playerController);
		if (!id.empty())
			Get().SweepRejoined(id);
	}

	void OnWorldEnd() { if (g_ledger) g_ledger->OnWorldEnd(); }
	void Tick()       { if (g_ledger) g_ledger->Tick(); }

	void ReconcilePlayer(const std::string& playerId)
	{
		if (g_ledger)
			g_ledger->ReconcilePlayer(playerId);
	}
}
