#include "admin_ops.h"
#include "host_rules.h"
#include "player_ids.h"
#include "player_list.h"
#include "removal.h"
#include "../plugin_helpers.h"
#include "../admin/admin_gamethread.h"

#include "SDK/Engine_classes.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace
{
	constexpr int kMaxControllers = 128;

	// The engine call has a function of its own (no objects with destructors,
	// as __try needs).
	bool KickGuarded(SDK::APlayerController* pc, const std::string& reason)
	{
		__try
		{
			Removal::Kick(pc, reason);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// Game thread only. The controller of the one connected player with this ID; null, with
	// the reason in error, when there is none or more than one.
	SDK::APlayerController* FindController(const std::string& id, std::string& error)
	{
		SDK::APlayerController* controllers[kMaxControllers] = {};
		const int count = HostRules::ConnectedControllers(controllers, kMaxControllers);
		std::vector<std::string> ids;
		std::vector<SDK::APlayerController*> live;
		for (int i = 0; i < count; ++i)
			if (controllers[i])
			{
				ids.push_back(PlayerIds::OfController(controllers[i]));
				live.push_back(controllers[i]);
			}
		size_t position = 0;
		return AdminOps::UniqueConnected(ids, id, position, error) ? live[position] : nullptr;
	}

	AdminOps::Backend MakeBackend()
	{
		AdminOps::Backend be;
		be.getModel       = [] { return HostRules::Get(); };
		be.commitModel    = [](const RulesModel::Model& m, std::string& e) { return HostRules::Commit(m, e); };
		be.players        = [] { return PlayerList::All(); };
		be.validId        = [](const std::string& id) { return PlayerList::IsValidId(id); };
		be.applyFlags     = [](PlayerList::Entry& entry, const std::string& flags, std::string& e) { return PlayerList::ApplyFlags(entry, flags, e); };
		be.setPlayer      = [](const std::string& id, const std::string& flags, std::string& e) { return PlayerList::Set(id, flags, e); };
		be.removePlayer   = [](const std::string& id, std::string& e) { return PlayerList::Remove(id, e); };
		be.clearAdmins    = [](int& changed, std::string& e) { return PlayerList::ClearAdmins(changed, e); };
		be.refreshPlayers = [] { HostRules::RefreshPlayers(); };
		be.reloadRules    = [](std::string& e) { return HostRules::Reload(e); };
		be.reloadPlayers  = [](std::string& e) { return PlayerList::Reload(e); };
		be.connected      = [](const std::string& id) { std::string e; return FindController(id, e) != nullptr || e == AdminOps::kAmbiguousIdText; };   // the kick says why it refuses
		be.kick           = [](const std::string& id, const std::string& reason, std::string& e)
		{
			SDK::APlayerController* pc = FindController(id, e);
			if (!pc) return false;
			if (!KickGuarded(pc, reason)) { e = "the kick failed"; return false; }
			return true;
		};
		be.audit          = [](const std::string& line) { LOG_INFO("%s", line.c_str()); };
		return be;
	}

	struct Shared
	{
		AdminOps::Result   result;
		std::atomic<bool>  started{ false };   // the game thread picked the op up
		std::atomic<bool>  ran{ false };
	};
}

namespace AdminOps
{
	Backend& HostBackend()
	{
		static Backend backend = MakeBackend();
		return backend;
	}

	Result Run(const Caller& caller, const std::vector<Op>& ops, uint64_t baseRevision)
	{
		auto shared = std::make_shared<Shared>();
		AdminGT::Dispatch([shared, caller, ops, baseRevision]() -> std::string
		{
			shared->started.store(true, std::memory_order_release);
			shared->result = Apply(HostBackend(), caller, ops, baseRevision);
			shared->ran.store(true, std::memory_order_release);
			return std::string();
		});

		if (shared->ran.load(std::memory_order_acquire))
			return shared->result;

		Result timeout;
		timeout.ok = false;
		timeout.text = shared->started.load(std::memory_order_acquire) ? "May have applied. Refresh to check." : "game thread timeout";
		timeout.revision = Revision();
		return timeout;
	}

	Result Run(const Caller& caller, const Op& op)
	{
		return Run(caller, std::vector<Op>{ op }, 0);
	}
}
