#include "stack_size.h"
#include "stack_size_model.h"
#include "../host_rules/ini_file.h"
#include "../plugin_helpers.h"
#include "../server_utility_fields.h"

#include "SDK/Engine_classes.hpp"
#include "SDK/AuItems_classes.hpp"
#include "item_stack_limits.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <mutex>
#include <vector>

static_assert(StackSizeModel::kMaxStack == ItemStackLimits::kMaxStackCap,
              "the accepted size and the helper's cap must agree");

namespace
{
	constexpr ULONGLONG kGuardMs  = 1000;   // the size is written again this often
	constexpr ULONGLONG kRescanMs = 5000;   // wait this long after a scan that found no items

	// The feature the version map can turn off on its own (a loader or game
	// build without the item members): the plugin keeps running without it.
	PluginMap::Feature& Feature()
	{
		static PluginMap::Feature feature{ "stack-size", "Server stack size", ItemStackLimits::Keys(), {} };
		return feature;
	}

	std::atomic<int>  g_target{ 0 };
	std::atomic<bool> g_available{ false };
	std::atomic<int>  g_tracked{ 0 };
	std::atomic<int>  g_raised{ 0 };

	// Game thread only from here on.
	ItemStackLimits::Limits       g_limits;
	ObjectRef<SDK::UWorld>        g_world;          // the world the items were found in
	int                           g_lastApplied = 0;
	ULONGLONG                     g_lastGuardMs = 0;
	ULONGLONG                     g_nextRescanMs = 0;
	bool                          g_tickRegistered = false;

	bool FeatureOn()
	{
		return Feature().On() && ItemStackLimits::Available();
	}

	// Find every item the game has loaded in this world and hand them to the
	// limits. The raw pointers live in this function only.
	void Rescan(SDK::UWorld* world)
	{
		std::vector<SDK::UAuItemDataBase*> items;
		auto* subsystem = static_cast<SDK::UAuItemDataBaseSubsystem*>(
			SDK::USubsystemBlueprintLibrary::GetWorldSubsystem(world, SDK::UAuItemDataBaseSubsystem::StaticClass()));
		if (subsystem)
		{
			const auto* list = ServerUtilityFields::ItemDataBaseArray.In(subsystem);
			if (list)
			{
				const int32_t count = list->Num();
				items.reserve(static_cast<size_t>(count));
				for (int32_t i = 0; i < count; ++i)
					if ((*list)[i])
						items.push_back((*list)[i]);
			}
		}

		g_limits.SetItems(items);
		g_world.Set(world);
		g_lastApplied = 0;
		g_tracked.store(g_limits.Tracked());
		if (items.empty())
		{
			g_nextRescanMs = GetTickCount64() + kRescanMs;
			LOG_WARN("[StackSize] No items found yet - trying again in a few seconds");
		}
		else
		{
			LOG_INFO("[StackSize] target %d, %d items", g_target.load(), g_limits.Tracked());
		}
	}

	void Restore()
	{
		g_limits.SetTarget(0);
		g_limits.RestoreAll();
		g_lastApplied = 0;
		g_raised.store(0);
	}

	// Game thread, once per frame. No file access here.
	void OnTick(float)
	{
		g_available.store(FeatureOn());

		const ULONGLONG now = GetTickCount64();
		const int target = g_target.load();
		const bool guard = now - g_lastGuardMs >= kGuardMs;
		if (guard) g_lastGuardMs = now;

		auto* world = static_cast<SDK::UWorld*>(PluginSdkRuntime::GetWorld());

		StackSizeModel::State s;
		s.isServerBinary   = true;
		s.featureAvailable = g_available.load();
		s.target           = target;
		s.lastApplied      = (guard && target > 0) ? 0 : g_lastApplied;
		s.itemsTracked     = g_limits.Tracked();
		s.worldChanged     = world && !(ObjectRef<SDK::UWorld>(world) == g_world);

		switch (StackSizeModel::Decide(s))
		{
		case StackSizeModel::Action::Rescan:
			if (world && now >= g_nextRescanMs)
				Rescan(world);
			break;
		case StackSizeModel::Action::Apply:
			g_limits.SetTarget(target);
			g_limits.Apply();
			g_lastApplied = target;
			g_raised.store(g_limits.Raised());
			break;
		case StackSizeModel::Action::Restore:
			Restore();
			break;
		case StackSizeModel::Action::None:
			break;
		}
	}
}

namespace StackSize
{
	void Resolve(const IPluginSelf* self, IPluginHookScanner* scanner)
	{
		PluginMap::DeclareFeatures(scanner, self, Feature());
		if (!ItemStackLimits::Resolve(scanner, self))
			LOG_WARN("[StackSize] UAuItemDataBase::MaxStack/StackingType not in this game build's version map - stack size is off");
		g_available.store(FeatureOn());
	}

	void Init()
	{
		const std::wstring& path = IniFile::Path();
		if (!path.empty())
		{
			std::lock_guard<std::mutex> fileLock(IniFile::FileMutex());
			std::vector<std::string> lines;
			bool exists = false, crlf = true;
			if (IniFile::ReadLines(path, lines, exists, crlf))
			{
				std::string error;
				g_target.store(StackSizeModel::ParseLines(lines, error));
				if (!error.empty())
					LOG_WARN("[StackSize] %s - using the game's own sizes", error.c_str());
			}
			else
			{
				LOG_WARN("[StackSize] The rules file could not be read - using the game's own sizes");
			}
		}

		auto* hooks = GetHooks();
		if (hooks && hooks->Engine)
		{
			hooks->Engine->RegisterOnTick(OnTick);
			g_tickRegistered = true;
		}
		LOG_INFO("[StackSize] %s, saved size %d", g_available.load() ? "available" : "off", g_target.load());
	}

	void Shutdown()
	{
		auto* hooks = GetHooks();
		if (!hooks || !hooks->Engine)
			return;
		if (g_tickRegistered)
			hooks->Engine->UnregisterOnTick(OnTick);
		g_tickRegistered = false;

		// Shutdown also runs at process exit: the items are touched on the game thread only.
		if (hooks->Engine->IsGameThread())
			Restore();
		else
			LOG_WARN("[StackSize] Shutdown is not on the game thread - stack sizes left as they are");
	}

	int Get() { return g_target.load(); }
	bool Available() { return g_available.load(); }
	int Tracked() { return g_tracked.load(); }
	int Raised() { return g_raised.load(); }

	bool Set(int size, std::string& error)
	{
		if (size < 0 || size > StackSizeModel::kMaxStack) { error = "the size must be 0 to 9999"; return false; }

		const std::wstring& path = IniFile::Path();
		if (path.empty()) { error = "the rules file path is unknown"; return false; }

		{
			std::lock_guard<std::mutex> fileLock(IniFile::FileMutex());
			std::vector<std::string> lines;
			bool exists = false, crlf = true;
			if (!IniFile::ReadLines(path, lines, exists, crlf)) { error = "the rules file could not be read"; return false; }
			IniFile::ReplaceSection(lines, StackSizeModel::kSection, StackSizeModel::ComposeBody(size));
			if (!IniFile::WriteLines(path, lines, crlf)) { error = "could not write the rules file"; return false; }
		}

		g_target.store(size);
		return true;
	}
}
