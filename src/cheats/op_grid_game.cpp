#include "op_grid_game.h"
#include "op_grid.h"
#include "cheat_ledger_game.h"
#include "host_rules/host_rules.h"
#include "host_rules/player_ids.h"
#include "host_rules/player_list.h"
#include "plugin_helpers.h"
#include "game_access.h"
#include "server_utility_fields.h"
#include "plugin_map.h"
#include "object_ref.h"

#include <Windows.h>
#include <cstdint>
#include <optional>
#include <string>

namespace OpGridGame
{
	namespace
	{
		using GameAccess::OnGameThread;
		using GameAccess::World;
		using GameAccess::Invoke;

		constexpr const char* kClass    = "CrInventoryComponent";
		constexpr const char* kFunction = "ServerResizeInventory";
		constexpr const char* kSlot     = "grid";
		constexpr int kMaxControllers   = 128;

		using InventoryRef = ObjectRef<SDK::UCrInventoryComponent>;

		bool Available()
		{
			return ServerUtilityFields::CheatGrid.On();
		}

		// The controller's player character's inventory component, kept as a weak reference: only
		// valid for the call that fetched the controller. Empty when the controller has no player
		// character. No C++ objects needing unwinding in this frame: SEH is legal.
		bool Capture_SEH(void* controller, InventoryRef* out)
		{
			__try
			{
				SDK::ACrCharacterPlayerBase* character = GameAccess::CharacterOf(controller);
				if (!character)
					return false;
				SDK::UCrInventoryComponent** invAt = ServerUtilityFields::InventoryComponent.In(character);
				if (!invAt || !*invAt)
					return false;
				out->Set(*invAt);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// The grid's columns and rows. False on a fault or an unresolved member.
		bool ReadGrid_SEH(SDK::UCrInventoryComponent* inv, int* cols, int* rows)
		{
			__try
			{
				const int32_t* c = ServerUtilityFields::InventoryGridColumns.In(inv);
				const int32_t* r = ServerUtilityFields::InventoryGridRows.In(inv);
				if (!c || !r)
					return false;
				*cols = *c;
				*rows = *r;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// The size of the slot array, and how many slots the items reach: the highest slot that holds
		// an item (an item id that is not all zero) plus one, 0 for an empty grid. The array is
		// stepped by the slot size the game reflects. False when the array or the slot size did not
		// resolve, the array is empty, or on a fault.
		bool ReadSlots_SEH(SDK::UCrInventoryComponent* inv, int* total, int* reach)
		{
			__try
			{
				const auto* slots = ServerUtilityFields::InventorySlots.In(inv);
				if (!slots || slots->Num() <= 0)
					return false;
				const size_t idSize = ServerUtilityFields::InventorySlotItemId.resolvedSize;
				if (idSize == 0)
					return false;
				int highest = 0;
				for (int i = 0; i < slots->Num(); ++i)
				{
					const uint8_t* slot = PluginMap::ElementAt(*slots, i, ServerUtilityFields::InventorySlotSize);
					const SDK::FAuItemId* id = ServerUtilityFields::InventorySlotItemId.In(slot);
					if (!id)
						return false;
					const uint8_t* bytes = reinterpret_cast<const uint8_t*>(id);
					for (size_t b = 0; b < idSize; ++b)
					{
						if (bytes[b] != 0)
						{
							highest = i + 1;
							break;
						}
					}
				}
				*total = slots->Num();
				*reach = highest;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// Asks the game for one resize. False when the call could not be made; the engine refuses
		// silently, so a true result says only that the call ran (the caller reads the grid back).
		bool CallResize(const InventoryRef& ref, int cols, int rows)
		{
			PluginMap::ParamsBlock block = PluginMap::ParamsBlockOf(ServerUtilityFields::ResizeInventorySize,
				{ &ServerUtilityFields::ResizeInventoryColumns, &ServerUtilityFields::ResizeInventoryRows });
			uint8_t* colAt = block.Param("Columns");
			uint8_t* rowAt = block.Param("Rows");
			if (!block || !colAt || !rowAt
				|| ServerUtilityFields::ResizeInventoryColumns.resolvedSize != sizeof(int32_t)
				|| ServerUtilityFields::ResizeInventoryRows.resolvedSize != sizeof(int32_t))
				return false;
			*reinterpret_cast<int32_t*>(colAt) = cols;
			*reinterpret_cast<int32_t*>(rowAt) = rows;

			SDK::UCrInventoryComponent* inv = ref.Get();
			return inv && Invoke(inv, kClass, kFunction, block.Data());
		}

		// One resize, but only when no item sits at or past the new slot count: the slots are read
		// again right before the call, so a step that moved items is seen. False when the slots
		// can't be read, an item would be dropped, or the call could not be made.
		bool CheckedResize(const InventoryRef& ref, int cols, int rows)
		{
			SDK::UCrInventoryComponent* inv = ref.Get();
			int total = 0, reach = 0;
			if (!inv || !ReadSlots_SEH(inv, &total, &reach) || !OpGrid::Fits(reach, cols, rows))
				return false;
			return CallResize(ref, cols, rows);
		}

		// The one connected player with this ID, or null. Game thread; the controller is only used in
		// the call that fetched it.
		SDK::APlayerController* ControllerOf(const std::string& id)
		{
			SDK::APlayerController* controllers[kMaxControllers] = {};
			const int count = HostRules::ConnectedControllers(controllers, kMaxControllers);
			SDK::APlayerController* found = nullptr;
			for (int i = 0; i < count; ++i)
			{
				if (!controllers[i] || PlayerIds::OfController(controllers[i]) != id)
					continue;
				if (found)
					return nullptr;
				found = controllers[i];
			}
			return found;
		}

		// ---- OpGrid::Env

		bool InWorld() { return World() != nullptr; }

		std::optional<OpGrid::Size> Current(const std::string& playerId)
		{
			InventoryRef ref;
			int cols = 0, rows = 0;
			if (!Available() || !World() || !Capture_SEH(ControllerOf(playerId), &ref))
				return std::nullopt;
			SDK::UCrInventoryComponent* inv = ref.Get();
			if (!inv || !ReadGrid_SEH(inv, &cols, &rows))
				return std::nullopt;
			return OpGrid::Size{ cols, rows };
		}

		std::optional<int> Occupied(const CheatDispatch::Context& ctx)
		{
			InventoryRef ref;
			int total = 0, reach = 0;
			if (!Available() || !World() || !Capture_SEH(ctx.controller, &ref))
				return std::nullopt;
			SDK::UCrInventoryComponent* inv = ref.Get();
			if (!inv || !ReadSlots_SEH(inv, &total, &reach))
				return std::nullopt;
			return reach;
		}

		// BetterCheats' resize: when the new grid holds as many slots as the old one the engine does
		// nothing (16x8 and 8x16 are both 128), so a slightly bigger grid goes first (OpGrid::StepSize).
		// Every call goes through CheckedResize, the steps and the roll back included, so none drops
		// an item. The grid is read back afterwards, because the engine refuses without saying so.
		bool Resize(const CheatDispatch::Context& ctx, uint32_t reqId, int cols, int rows)
		{
			InventoryRef ref;
			if (!Available() || !World() || !Capture_SEH(ctx.controller, &ref))
				return false;

			int total = 0, reach = 0, nowCols = 0, nowRows = 0;
			SDK::UCrInventoryComponent* inv = ref.Get();
			if (!inv || !ReadGrid_SEH(inv, &nowCols, &nowRows) || !ReadSlots_SEH(inv, &total, &reach))
				return false;

			const int oldCols = nowCols, oldRows = nowRows;
			bool done = nowCols == cols && nowRows == rows;
			if (!done)
			{
				const bool steps = total == cols * rows;
				if (steps)
				{
					const OpGrid::Size step = OpGrid::StepSize(cols, rows);
					if (!CheckedResize(ref, step.cols, step.rows))
						return false;
				}
				if (!CheckedResize(ref, cols, rows))
					return false;

				inv = ref.Get();
				done = inv && ReadGrid_SEH(inv, &nowCols, &nowRows) && nowCols == cols && nowRows == rows;
				if (!done)
				{
					// Leave the player's grid as it was if only the first step took.
					if (steps)
						CheckedResize(ref, oldCols, oldRows);
					return false;
				}
			}

			const std::string id = ctx.playerId ? *ctx.playerId : std::string();
			PlayerList::Entry who;
			PlayerList::Find(id, who);
			const std::string what = "a " + std::to_string(cols) + "x" + std::to_string(rows) + " inventory grid";
			CheatLedgerGame::Get().Grant(id, who.label, OpGrid::kFeature, kSlot, what, reqId);
			LOG_INFO("[cheat.grid] req %u: %s set %s", reqId, id.c_str(), what.c_str());
			return true;
		}
	}

	void Init()
	{
		OpGrid::Env env;
		env.available = &Available;
		env.inWorld   = &InWorld;
		env.current   = &Current;
		env.occupied  = &Occupied;
		env.resize    = &Resize;
		env.log       = [](const std::string& line) { LOG_INFO("[cheat.grid] %s", line.c_str()); };
		OpGrid::SetEnv(std::move(env));
	}
}
