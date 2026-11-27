#include "op_power_game.h"
#include "op_power.h"
#include "cheat_ledger_game.h"
#include "host_rules/ingame_admin.h"
#include "host_rules/player_list.h"
#include "plugin_helpers.h"
#include "server_utility_fields.h"
#include "SDK/Engine_classes.hpp"
#include "SDK/AssetRegistry_classes.hpp"
#include "SDK/AssetRegistry_parameters.hpp"
#include "plugin_map.h"
#include "object_ref.h"

#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace OpPowerGame
{
	namespace
	{
		using namespace ServerUtilityFields;

		// The engine layout constant: an FConstSharedStruct points at an FStructSharedMemory, which is
		// the UScriptStruct pointer and then the struct's data, aligned up to the struct's minimum
		// alignment (the engine's FStructSharedMemory::GetMemory). The header is one pointer by the
		// engine's own definition, not a game value, so it has no version-map row.
		constexpr size_t kSharedMemoryHeader = sizeof(void*);
		constexpr size_t kMaxHistory         = 64;   // values one building has been set to this world

		using StaticFindObjectByNameFn = SDK::UObject*  (__fastcall*)(SDK::UClass*, SDK::UObject*, const wchar_t*, bool);
		using FindPackageFn            = SDK::UPackage* (__fastcall*)(SDK::UObject*, const wchar_t*);
		using PackageFullyLoadFn       = void           (__fastcall*)(SDK::UPackage*);
		using LoadPackageFn            = SDK::UPackage* (__fastcall*)(SDK::UPackage*, const wchar_t*, uint32_t, void*, const void*);
		using TemplateFn               = const void*    (__fastcall*)(void* config, const SDK::UWorld*);
		using FindOrAddFn              = void*          (__fastcall*)(void* container, uint32_t hash, const SDK::UScriptStruct*, const uint8_t*);
		using Crc32Fn                  = uint32_t       (__fastcall*)(const SDK::UScriptStruct*, const uint8_t*, uint32_t);

		StaticFindObjectByNameFn g_findObject = nullptr;
		FindPackageFn            g_findPackage = nullptr;
		PackageFullyLoadFn       g_fullyLoad = nullptr;
		LoadPackageFn            g_loadPackage = nullptr;
		TemplateFn               g_destroyTemplate = nullptr;
		TemplateFn               g_getOrCreateTemplate = nullptr;
		FindOrAddFn              g_findOrAdd = nullptr;
		Crc32Fn                  g_crc32 = nullptr;
		ULONGLONG                g_nextEngineTry = 0;

		PluginMap::Feature& Feature()
		{
			static PluginMap::Feature feature = CheatPower;
			return feature;
		}

		// Game thread only from here on. Names and the game's own values only; no object pointer.
		struct Known { std::string package; uint8_t type = 0; };
		std::map<std::string, Known>              g_known;      // config asset name -> where it lives, this world
		std::map<std::string, float>              g_pristine;   // the game's own value, read before any edit
		std::map<std::string, float>              g_applied;    // the value the ledger holds, for the re-apply
		std::map<std::string, std::vector<float>> g_history;    // values whose shared block placed buildings may hold
		ObjectRef<SDK::UWorld>                    g_scanWorld;
		ObjectRef<SDK::UWorld>                    g_tickWorld;
		int                                       g_reapplyLeft = 0;
		ULONGLONG                                 g_nextReapply = 0;
		bool                                      g_tickRegistered = false;

		bool OnGameThread()
		{
			IPluginHooks* hooks = GetHooks();
			return hooks && hooks->Engine && hooks->Engine->IsGameThread();
		}

		// The four engine lookups, from the loader. Asked again (at most every 5 s) until all four are there.
		bool EngineLookups()
		{
			if (g_findObject && g_findPackage && g_fullyLoad && g_loadPackage)
				return true;
			const ULONGLONG now = GetTickCount64();
			if (now < g_nextEngineTry)
				return false;
			g_nextEngineTry = now + 5000;
			IPluginHooks* hooks = GetHooks();
			IPluginEngineEvents* engine = hooks ? hooks->Engine : nullptr;
			if (!engine)
				return false;
			if (uintptr_t a = engine->GetStaticFindObjectByNameAddress()) g_findObject  = reinterpret_cast<StaticFindObjectByNameFn>(a);
			if (uintptr_t a = engine->GetFindPackageAddress())            g_findPackage = reinterpret_cast<FindPackageFn>(a);
			if (uintptr_t a = engine->GetPackageFullyLoadAddress())       g_fullyLoad   = reinterpret_cast<PackageFullyLoadFn>(a);
			if (uintptr_t a = engine->GetLoadPackageAddress())            g_loadPackage = reinterpret_cast<LoadPackageFn>(a);
			return g_findObject && g_findPackage && g_fullyLoad && g_loadPackage;
		}

		bool Available()
		{
			return Feature().On() && EngineLookups()
				&& g_destroyTemplate && g_getOrCreateTemplate && g_findOrAdd && g_crc32
				&& MassConfigAssetConfig.offset >= 0 && MassConfigParent.offset >= 0 && MassConfigTraits.offset >= 0
				&& ElectricityParameters.offset >= 0 && ElectricityValue.offset >= 0 && ElectricityType.offset >= 0
				&& ElectricityParametersSize.offset >= 0 && ConstSharedFragmentsContainer.offset >= 0
				&& PluginMap::MassEntityManagerField.offset >= 0;
		}

		// Null off the game thread: every engine and UObject access in this file starts here.
		SDK::UWorld* World()
		{
			if (!OnGameThread())
				return nullptr;
			return static_cast<SDK::UWorld*>(PluginSdkRuntime::GetWorld());
		}

		std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

		// "DA_WindPowerGeneratorTier2" -> "Wind Power Generator Tier 2", for reply text only.
		std::string Prettify(const std::string& asset)
		{
			std::string s = asset;
			for (const char* prefix : { "DA_", "BD_" })
			{
				const size_t len = std::strlen(prefix);
				if (s.size() > len && s.compare(0, len, prefix) == 0) { s = s.substr(len); break; }
			}
			std::string out;
			for (size_t i = 0; i < s.size(); ++i)
			{
				const unsigned char c = static_cast<unsigned char>(s[i]);
				if (i > 0)
				{
					const unsigned char p = static_cast<unsigned char>(s[i - 1]);
					if ((std::isupper(c) && std::islower(p)) || (std::isdigit(c) && std::isalpha(p)) || (std::isalpha(c) && std::isdigit(p)))
						out += ' ';
				}
				out += s[i];
			}
			return out;
		}

		const char* TypeName(uint8_t type)
		{
			switch (static_cast<SDK::ECrMassElectricityAgentType>(type))
			{
			case SDK::ECrMassElectricityAgentType::Producer:  return "producer";
			case SDK::ECrMassElectricityAgentType::Consumer:  return "consumer";
			case SDK::ECrMassElectricityAgentType::Conductor: return "conductor";
			default:                                          return "none";
			}
		}

		// ---- Engine and game calls. Each _SEH function holds no object with a destructor, so the
		// frame can catch a fault; a pointer it returns is used in the same tick and dropped.

		SDK::UObject* LoadAsset_SEH(const wchar_t* package, const wchar_t* name)
		{
			__try
			{
				SDK::UPackage* pkg = g_findPackage(nullptr, package);
				if (pkg)
					g_fullyLoad(pkg);
				else
					pkg = g_loadPackage(nullptr, package, 0, nullptr, nullptr);
				return pkg ? g_findObject(SDK::UMassEntityConfigAsset::StaticClass(), pkg, name, false) : nullptr;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return nullptr;
			}
		}

		SDK::UScriptStruct* FindParamsStruct_SEH()
		{
			__try
			{
				SDK::UPackage* pkg = g_findPackage(nullptr, L"/Script/Chimera");
				return pkg ? static_cast<SDK::UScriptStruct*>(g_findObject(SDK::UScriptStruct::StaticClass(), pkg, L"CrElectricityParameters", false)) : nullptr;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return nullptr;
			}
		}

		// The electricity trait's parameters in the config, or in the parent config it inherits from.
		SDK::FCrElectricityParameters* FindParams(SDK::UObject* asset, int depth = 0)
		{
			if (!asset || depth > 8)
				return nullptr;
			auto* config = MassConfigAssetConfig.In(asset);
			if (!config)
				return nullptr;
			if (auto* traits = MassConfigTraits.In(config))
			{
				for (int32_t i = 0; i < traits->Num(); ++i)
				{
					SDK::UMassEntityTraitBase* trait = (*traits)[i];
					if (trait && trait->Class && trait->Class->GetName() == "CrElectricityTrait")
						return ElectricityParameters.In(trait);
				}
			}
			auto* parent = MassConfigParent.In(config);
			return FindParams(parent ? *parent : nullptr, depth + 1);
		}

		struct Params
		{
			bool                          found = false;
			uint8_t                       type  = 0;
			float                         value = 0;
			SDK::FCrElectricityParameters raw{};
		};

		void Inspect_SEH(SDK::UObject* asset, Params* out)
		{
			__try
			{
				SDK::FCrElectricityParameters* p = FindParams(asset);
				const SDK::ECrMassElectricityAgentType* type = ElectricityType.In(p);
				const float* value = ElectricityValue.In(p);
				if (p && type && value)
				{
					out->found = true;
					out->type  = static_cast<uint8_t>(*type);
					out->value = *value;
					out->raw   = *p;
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				out->found = false;
			}
		}

		bool WriteValue_SEH(SDK::UObject* asset, float value)
		{
			__try
			{
				float* v = ElectricityValue.In(FindParams(asset));
				if (!v)
					return false;
				*v = value;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool Rebuild_SEH(SDK::UObject* asset, SDK::UWorld* world)
		{
			__try
			{
				void* config = MassConfigAssetConfig.In(asset);
				if (!config)
					return false;
				g_destroyTemplate(config, world);
				g_getOrCreateTemplate(config, world);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// Sets the shared block the placed buildings hold to `newValue`, once per value in `olds` (each
		// hashes to its own block). Needs the block to start with the struct pointer we expect; when
		// it does not, nothing is written. 0 = done, otherwise the step that failed.
		int Patch_SEH(SDK::UWorld* world, SDK::UScriptStruct* st, const SDK::FCrElectricityParameters* base,
		              const float* olds, int count, float newValue)
		{
			__try
			{
				auto* subsystem = SDK::USubsystemBlueprintLibrary::GetWorldSubsystem(world, SDK::UMassEntitySubsystem::StaticClass());
				void* manager = subsystem ? PluginMap::MassEntityManagerOf(subsystem) : nullptr;
				void* container = manager ? ConstSharedFragmentsContainer.In(manager) : nullptr;
				if (!container)
					return 1;
				const size_t align = st->MinAlignment > 0 ? static_cast<size_t>(st->MinAlignment) : 0;
				if (!align || (align & (align - 1)))
					return 2;
				const size_t dataOffset = (kSharedMemoryHeader + align - 1) & ~(align - 1);
				for (int i = 0; i < count; ++i)
				{
					SDK::FCrElectricityParameters old = *base;
					float* oldValue = ElectricityValue.In(&old);
					if (!oldValue)
						return 3;
					*oldValue = olds[i];
					const uint32_t hash = g_crc32(st, reinterpret_cast<const uint8_t*>(&old), 0);
					void** element = static_cast<void**>(g_findOrAdd(container, hash, st, reinterpret_cast<const uint8_t*>(&old)));
					uint8_t* block = element ? static_cast<uint8_t*>(*element) : nullptr;
					if (!block || *reinterpret_cast<SDK::UScriptStruct**>(block) != st)
						return 4;
					float* live = ElectricityValue.In(block + dataOffset);
					if (!live)
						return 5;
					*live = newValue;
				}
				return 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return 6;
			}
		}

		// ---- The registry scan: every config asset with an electricity trait the game uses.

		bool Inspect(const std::string& asset, const std::string& package, Params& out)
		{
			if (!EngineLookups())
				return false;
			const std::wstring pkgW = Wide(package), nameW = Wide(asset);
			SDK::UObject* obj = LoadAsset_SEH(pkgW.c_str(), nameW.c_str());
			if (!obj)
				return false;
			Inspect_SEH(obj, &out);
			return out.found;
		}

		void ScanRegistry()
		{
			SDK::UAssetRegistryHelpers* helpers = SDK::UAssetRegistryHelpers::GetDefaultObj();
			SDK::UFunction* getRegistry = SDK::UAssetRegistryHelpers::StaticClass()->GetFunction("AssetRegistryHelpers", "GetAssetRegistry");
			SDK::UFunction* byClass = SDK::IAssetRegistry::StaticClass()->GetFunction("AssetRegistry", "GetAssetsByClass");
			if (!helpers || !getRegistry || !byClass)
				return;

			SDK::Params::AssetRegistryHelpers_GetAssetRegistry registryParams{};
			const auto flags1 = getRegistry->FunctionFlags;
			getRegistry->FunctionFlags |= 0x400;   // EFunctionFlags::Native, as the engine expects for a static call
			helpers->ProcessEvent(getRegistry, &registryParams);
			getRegistry->FunctionFlags = flags1;
			SDK::UObject* registry = registryParams.ReturnValue.GetObjectRef();
			if (!registry)
				return;

			SDK::FString packagePath(L"/Script/MassSpawner");
			SDK::FString className(L"MassEntityConfigAsset");
			SDK::Params::AssetRegistry_GetAssetsByClass byClassParams{};
			byClassParams.ClassPathName     = SDK::UKismetSystemLibrary::MakeTopLevelAssetPath(packagePath, className);
			byClassParams.bSearchSubClasses = true;
			const auto flags2 = byClass->FunctionFlags;
			byClass->FunctionFlags |= 0x400;
			registry->ProcessEvent(byClass, &byClassParams);
			byClass->FunctionFlags = flags2;

			for (int i = 0; i < byClassParams.OutAssetData.Num(); ++i)
			{
				const std::string asset   = byClassParams.OutAssetData[i].AssetName.ToString();
				const std::string package = byClassParams.OutAssetData[i].PackageName.GetRawString();
				Params p;
				if (package.empty() || !Inspect(asset, package, p) ||
				    static_cast<SDK::ECrMassElectricityAgentType>(p.type) == SDK::ECrMassElectricityAgentType::None)
					continue;
				// The first value read is the game's own: nothing was edited before the first scan.
				const float pristine = g_pristine.emplace(asset, p.value).first->second;
				if (pristine == 0.0f)
					continue;   // neither produces nor draws power: nothing to edit
				g_known[asset] = Known{ package, p.type };
			}
		}

		// One scan per world, from the game thread. A hitch on the first call (it loads the packages).
		bool EnsureScan(SDK::UWorld* world)
		{
			if (g_scanWorld == ObjectRef<SDK::UWorld>(world) && !g_known.empty())
				return true;
			g_known.clear();
			g_scanWorld.Set(world);
			try { ScanRegistry(); }
			catch (...) { LOG_WARN("[cheat.power] The building scan failed"); }
			LOG_INFO("[cheat.power] %zu buildings with a power value", g_known.size());
			return !g_known.empty();
		}

		// Puts `value` on the building: the shared block the placed buildings hold (every value this
		// world has seen, so an earlier edit's block follows too), the trait, and the template for new
		// ones. The same value twice does nothing. False when nothing was written.
		bool ApplyValue(const std::string& asset, float value)
		{
			SDK::UWorld* world = World();
			if (!world || !Available() || !EnsureScan(world))
				return false;
			const auto known = g_known.find(asset);
			const auto pristine = g_pristine.find(asset);
			if (known == g_known.end() || pristine == g_pristine.end())
				return false;

			Params cur;
			if (!Inspect(asset, known->second.package, cur))
				return false;
			if (cur.value == value)
				return true;

			std::vector<float>& history = g_history[asset];
			std::vector<float> olds = history;
			for (float v : { pristine->second, cur.value })
				if (std::find(olds.begin(), olds.end(), v) == olds.end())
					olds.push_back(v);

			SDK::UScriptStruct* st = FindParamsStruct_SEH();
			if (!st)
				return false;
			const int patched = Patch_SEH(world, st, &cur.raw, olds.data(), static_cast<int>(olds.size()), value);
			if (patched != 0)
			{
				LOG_WARN("[cheat.power] %s: the shared block patch stopped at step %d - nothing changed", asset.c_str(), patched);
				return false;
			}

			const std::wstring pkgW = Wide(known->second.package), nameW = Wide(asset);
			SDK::UObject* obj = LoadAsset_SEH(pkgW.c_str(), nameW.c_str());
			if (!obj || !WriteValue_SEH(obj, value))
				return false;
			if (!Rebuild_SEH(obj, world))
				LOG_WARN("[cheat.power] %s: the template rebuild failed - new buildings keep the old value", asset.c_str());
			if (history.size() < kMaxHistory && std::find(history.begin(), history.end(), value) == history.end())
				history.push_back(value);
			return true;
		}

		// ---- The ledger's way to read and write one building's value.

		std::string AssetOf(const CheatLedger::Entry& e)
		{
			const std::string prefix = OpPower::Slot(std::string());
			return e.slot.compare(0, prefix.size(), prefix) == 0 ? e.slot.substr(prefix.size()) : std::string();
		}

		CheatLedger::Value ReadValue(SDK::UObject*, const CheatLedger::Entry& e)
		{
			SDK::UWorld* world = World();
			const std::string asset = AssetOf(e);
			if (!world || !Available() || !EnsureScan(world))
				return CheatLedger::Value();
			const auto known = g_known.find(asset);
			Params p;
			if (known == g_known.end() || !Inspect(asset, known->second.package, p))
				return CheatLedger::Value();
			return CheatLedger::Value(std::in_place_type<float>, p.value);
		}

		bool WriteValueSlot(SDK::UObject*, const CheatLedger::Entry& e, const CheatLedger::Value& v)
		{
			const float* value = std::get_if<float>(&v);
			const std::string asset = AssetOf(e);
			if (!value || asset.empty() || !ApplyValue(asset, *value))
				return false;
			g_applied[asset] = *value;
			return true;
		}

		bool Overridden(const std::string& asset)
		{
			return CheatLedgerGame::Get().Has(CheatLedger::Kind::Override, std::string(), OpPower::kFeature, OpPower::Slot(asset));
		}

		// The world changed: the game's config assets may be loaded fresh and the Mass state is new,
		// so every edit the ledger still holds is applied again (a second apply does nothing).
		void OnTick(float)
		{
			SDK::UWorld* world = World();
			if (!world)
				return;
			if (!(g_tickWorld == ObjectRef<SDK::UWorld>(world)))
			{
				g_tickWorld.Set(world);
				g_history.clear();
				g_reapplyLeft = 30;
				g_nextReapply = 0;
			}
			const ULONGLONG now = GetTickCount64();
			if (g_reapplyLeft <= 0 || now < g_nextReapply)
				return;
			for (auto it = g_applied.begin(); it != g_applied.end();)
				it = Overridden(it->first) ? std::next(it) : g_applied.erase(it);
			g_nextReapply = now + 2000;
			if (g_applied.empty())
			{
				g_reapplyLeft = 0;
				return;
			}
			--g_reapplyLeft;
			bool all = true;
			for (const auto& entry : g_applied)
				all = ApplyValue(entry.first, entry.second) && all;
			if (all)
			{
				g_reapplyLeft = 0;
				LOG_INFO("[cheat.power] %zu edited buildings applied to the new world", g_applied.size());
			}
		}

		// ---- OpPower::Env

		bool EnvAvailable() { return Available(); }

		bool Fill(const std::string& asset, const Known& known, OpPower::Building& out)
		{
			Params p;
			const auto pristine = g_pristine.find(asset);
			if (pristine == g_pristine.end() || !Inspect(asset, known.package, p))
				return false;
			out.asset = asset;
			out.display = Prettify(asset);
			out.type = TypeName(p.type);
			out.value = p.value;
			out.defaultValue = pristine->second;
			return true;
		}

		OpPower::Lookup Find(const std::string& asset, OpPower::Building& out)
		{
			SDK::UWorld* world = World();
			if (!world)
				return OpPower::Lookup::NoWorld;
			EnsureScan(world);
			const auto known = g_known.find(asset);
			return known != g_known.end() && Fill(asset, known->second, out) ? OpPower::Lookup::Found : OpPower::Lookup::NotFound;
		}

		std::optional<std::vector<OpPower::Building>> List()
		{
			SDK::UWorld* world = World();
			if (!world)
				return std::nullopt;
			EnsureScan(world);
			std::vector<OpPower::Building> all;
			for (const auto& k : g_known)
			{
				OpPower::Building b;
				if (Fill(k.first, k.second, b))
					all.push_back(std::move(b));
			}
			return all;
		}

		int Edited()
		{
			int n = 0;
			for (const auto& entry : g_applied)
				if (Overridden(entry.first))
					++n;
			return n;
		}

		bool Set(const CheatDispatch::Context& ctx, uint32_t reqId, const std::string& asset, float value)
		{
			const std::string id = ctx.playerId ? *ctx.playerId : std::string();
			PlayerList::Entry who;
			PlayerList::Find(id, who);

			CheatLedger::Request req;
			req.playerId    = id;
			req.playerName  = who.label;
			req.conn        = CheatLedgerGame::MakeWeak(static_cast<SDK::UObject*>(ctx.controller));
			req.feature     = OpPower::kFeature;
			req.slot        = OpPower::Slot(asset);
			req.kind        = CheatLedger::Kind::Override;
			req.value       = CheatLedger::Value(std::in_place_type<float>, value);
			req.reqId       = reqId;
			req.ownerProven = InGameAdmin::IsProven(ctx.controller, id);
			return CheatLedgerGame::Get().Apply(req);
		}

		bool Reset(const CheatDispatch::Context&, uint32_t, const std::string& asset)
		{
			return !Overridden(asset) ||
				CheatLedgerGame::Get().Release(CheatLedger::Kind::Override, std::string(), OpPower::kFeature, OpPower::Slot(asset));
		}

		bool ResetAll(const CheatDispatch::Context& ctx, uint32_t reqId)
		{
			std::vector<std::string> assets;
			for (const auto& entry : g_applied)
				assets.push_back(entry.first);
			bool ok = true;
			for (const std::string& asset : assets)
				ok = Reset(ctx, reqId, asset) && ok;
			return ok;
		}
	}

	void Resolve(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		auto fn = [&](const char* key) { return PluginMap::Function(scanner, self, key, nullptr, PLUGIN_SCAN_FUNCTION_START, PLUGIN_SCAN_FLAG_OPTIONAL); };
		g_destroyTemplate     = reinterpret_cast<TemplateFn>(fn(DestroyEntityTemplate));
		g_getOrCreateTemplate = reinterpret_cast<TemplateFn>(fn(GetOrCreateEntityTemplate));
		g_findOrAdd           = reinterpret_cast<FindOrAddFn>(fn(SharedFragmentsFindOrAdd));
		g_crc32               = reinterpret_cast<Crc32Fn>(fn(GetStructInstanceCrc32));
	}

	void Init()
	{
		CheatLedgerGame::RegisterAccessor(OpPower::kFeature, OpPower::Slot(std::string()), { &ReadValue, &WriteValueSlot });

		OpPower::Env env;
		env.available = &EnvAvailable;
		env.find      = &Find;
		env.list      = &List;
		env.edited    = &Edited;
		env.set       = &Set;
		env.reset     = &Reset;
		env.resetAll  = &ResetAll;
		env.log       = [](const std::string& line) { LOG_INFO("[cheat.power] %s", line.c_str()); };
		OpPower::SetEnv(std::move(env));

		IPluginHooks* hooks = GetHooks();
		if (hooks && hooks->Engine)
		{
			hooks->Engine->RegisterOnTick(OnTick);
			g_tickRegistered = true;
		}
	}

	void Shutdown()
	{
		IPluginHooks* hooks = GetHooks();
		if (hooks && hooks->Engine && g_tickRegistered)
			hooks->Engine->UnregisterOnTick(OnTick);
		g_tickRegistered = false;
	}
}
