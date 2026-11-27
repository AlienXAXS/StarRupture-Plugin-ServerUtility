#include "op_give_game.h"
#include "op_give.h"
#include "cheat_ledger_game.h"
#include "host_rules/player_list.h"
#include "admin/endpoints/item_lookup.h"
#include "plugin_helpers.h"
#include "game_access.h"
#include "plugin_map.h"
#include "server_utility_fields.h"

#include "SDK/Engine_classes.hpp"
#include "SDK/Chimera_classes.hpp"
#include "SDK/AuItems_classes.hpp"

#include <Windows.h>
#include <cstdint>
#include <cwchar>
#include <string>

namespace OpGiveGame
{
	namespace
	{
		uintptr_t g_addNewItem = 0;

		using AddNewItemFn = SDK::TArray<SDK::FAuAddedItem>* (__fastcall*)(
			SDK::UAuItemsComponent*, SDK::TArray<SDK::FAuAddedItem>*, const SDK::UAuItemDataBase*, uint32_t);

		constexpr int kNameChars = 128;

		struct Located
		{
			SDK::UAuItemsComponent*     inventory = nullptr;
			const SDK::UAuItemDataBase* grant     = nullptr;
			wchar_t                     name[kNameChars] = {};
			AddStatus                   failure   = AddStatus::Fault;
		};

		// The item's display name into buf. The text is engine-owned; call inside the caller's guard.
		void CopyName(const SDK::UAuItemDataBase* item, wchar_t* buf)
		{
			const SDK::FText* text = ServerUtilityFields::ItemName.In(item);
			if (!text || !text->TextData) return;
			const SDK::FString& s = text->GetStringRef();
			if (s.Num() <= 0 || !s.CStr()) return;
			wcsncpy_s(buf, kNameChars, s.CStr(), _TRUNCATE);
		}

		// The object AddNewItem takes: the item's blueprint class default object when it has one
		// that is an item, else the entry itself.
		const SDK::UAuItemDataBase* GrantObject(SDK::UAuItemDataBase* entry)
		{
			SDK::UClass** classAt = ServerUtilityFields::ObjectClass.In(entry);
			SDK::UClass* cls = classAt ? *classAt : nullptr;
			if (cls && cls != SDK::UAuItemDataBase::StaticClass())
			{
				SDK::UObject* cdo = cls->ClassDefaultObject;
				if (cdo && static_cast<void*>(cdo) != static_cast<void*>(entry) &&
				    cdo->IsA(SDK::UAuItemDataBase::StaticClass()))
					return static_cast<const SDK::UAuItemDataBase*>(cdo);
			}
			return entry;
		}

		// No C++ objects with destructors in this frame: SEH is legal. err is caller-owned.
		bool Locate_SEH(void* controller, const char* assetPath, Located* out, char* err, int errCap)
		{
			__try
			{
				auto* world = static_cast<SDK::UWorld*>(PluginSdkRuntime::GetWorld());
				auto* pc = static_cast<SDK::APlayerController*>(controller);
				if (!world || !pc) { out->failure = AddStatus::Fault; return false; }

				auto* itemSubsys = static_cast<SDK::UAuItemDataBaseSubsystem*>(
					SDK::USubsystemBlueprintLibrary::GetWorldSubsystem(world, SDK::UAuItemDataBaseSubsystem::StaticClass()));
				if (!itemSubsys) { out->failure = AddStatus::NoSubsystem; return false; }

				SDK::UAuItemDataBase* entry = FindItemByPath(itemSubsys, assetPath, err, errCap);
				if (!entry) { out->failure = AddStatus::NoItem; return false; }

				SDK::ACrCharacterPlayerBase* character = GameAccess::CharacterOf(pc);
				if (!character)
				{ out->failure = AddStatus::NoCharacter; return false; }

				SDK::UCrInventoryComponent** invAt = ServerUtilityFields::InventoryComponent.In(character);
				out->inventory = invAt ? *invAt : nullptr;
				if (!out->inventory) { out->failure = AddStatus::NoCharacter; return false; }

				out->grant = GrantObject(entry);
				CopyName(entry, out->name);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				out->failure = AddStatus::Fault;
				return false;
			}
		}

		// Calls AddNewItem and sums what it reports; frees the out array's data. -1 on a fault.
		// No C++ objects with destructors in this frame.
		int Add_SEH(AddNewItemFn fn, SDK::UAuItemsComponent* inv, const SDK::UAuItemDataBase* grant, int count)
		{
			SDK::TArray<SDK::FAuAddedItem> added{};
			int total = 0;
			__try
			{
				fn(inv, &added, grant, static_cast<uint32_t>(count));
				const int32_t n = added.Num();
				for (int32_t i = 0; i < n; ++i)
				{
					total += PluginMap::ValueOr(ServerUtilityFields::AuAddedItem_Amount, &added[i], 0);
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				total = -1;
			}
			// The buffer came from the engine allocator; return it there.
			auto* hooks = GetHooks();
			if (added.GetDataPtr() && hooks && hooks->Memory && hooks->Memory->IsAllocatorAvailable())
				hooks->Memory->Free(const_cast<SDK::FAuAddedItem*>(added.GetDataPtr()));
			return total;
		}

		std::string Narrow(const wchar_t* w)
		{
			if (!w || !*w) return {};
			const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
			if (n <= 1) return {};
			std::string s(static_cast<size_t>(n - 1), '\0');
			WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
			return s;
		}

		bool Available() { return ServerUtilityFields::CheatGive.On() && g_addNewItem != 0; }

		OpGive::GiveResult Give(const CheatDispatch::Context& ctx, const std::string& item, int count, uint32_t reqId)
		{
			using Outcome = OpGive::GiveResult::Outcome;
			OpGive::GiveResult r;

			char err[160] = {};
			const Added a = AddItem(ctx.controller, item.c_str(), count, err, static_cast<int>(sizeof(err)));
			switch (a.status)
			{
			case AddStatus::Added:        break;
			case AddStatus::NoItem:       r.outcome = Outcome::NoItem;       return r;
			case AddStatus::NoCharacter:  r.outcome = Outcome::NoCharacter;  return r;
			case AddStatus::NothingAdded: r.outcome = Outcome::NothingAdded; return r;
			default:                      return r;
			}

			r.outcome = Outcome::Gave;
			r.name    = a.name.empty() ? item : a.name;
			r.given   = a.given;

			const std::string id = ctx.playerId ? *ctx.playerId : std::string();
			PlayerList::Entry who;
			PlayerList::Find(id, who);
			const std::string what = std::to_string(a.given) + " x " + r.name;
			CheatLedgerGame::Get().Grant(id, who.label, "item-spawner", "give", what, reqId);
			LOG_INFO("[cheat.give] req %u: %s gave %s", reqId, id.c_str(), what.c_str());
			return r;
		}
	}

	Added AddItem(void* controller, const char* assetPath, int count, char* err, int errCap)
	{
		Added a;

		auto* hooks = GetHooks();
		if (!hooks || !hooks->Engine || !hooks->Engine->IsGameThread())
			return a;
		if (!Available())
		{
			a.status = AddStatus::Unavailable;
			return a;
		}

		Located loc;
		if (!Locate_SEH(controller, assetPath, &loc, err, errCap))
		{
			a.status = loc.failure;
			return a;
		}

		const int added = Add_SEH(reinterpret_cast<AddNewItemFn>(g_addNewItem), loc.inventory, loc.grant, count);
		if (added < 0) return a;
		if (added == 0) { a.status = AddStatus::NothingAdded; return a; }

		a.status = AddStatus::Added;
		a.given  = added;
		a.name   = Narrow(loc.name);
		return a;
	}

	void Resolve(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		g_addNewItem = PluginMap::Function(scanner, self, ServerUtilityFields::AddNewItem, nullptr,
			PLUGIN_SCAN_FUNCTION_START, PLUGIN_SCAN_FLAG_OPTIONAL);
	}

	void Init()
	{
		OpGive::Env env;
		env.available = &Available;
		env.give      = &Give;
		env.log       = [](const std::string& line) { LOG_INFO("[cheat.give] %s", line.c_str()); };
		OpGive::SetEnv(std::move(env));
	}
}
