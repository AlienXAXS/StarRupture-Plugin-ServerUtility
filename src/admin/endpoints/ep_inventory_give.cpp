#include "ep_inventory_give.h"
#include "../admin_json.h"
#include "../session/session_store.h"
#include "../admin_gamethread.h"

#if defined(MODLOADER_SERVER_BUILD)
#  include "SDK/Engine_classes.hpp"
#  include "SDK/Chimera_classes.hpp"
#  include "server_utility_fields.h"
#  include "game_access.h"
#  include "cheats/op_give.h"
#  include "cheats/op_give_game.h"
#  include "../../rcon/listed_players.h"
#  define INV_HAS_SDK 1
#else
#  define INV_HAS_SDK 0
#endif

#include <cstdio>
#include <string>

// Most items one give request may add.
static constexpr int kMaxGiveCount = 9999;

#if INV_HAS_SDK

// The controller of the listed player when it holds a player character; null with *refusal set to
// the reply otherwise. No C++ objects with destructors in this frame: SEH is legal.
// playerIndex is a players/list index; expectedName and expectedId are what the list showed at it.
static void* GiveTarget_SEH(int playerIndex, const wchar_t* expectedName, const char* expectedId, const char** refusal)
{
	__try
	{
		auto* world = static_cast<SDK::UWorld*>(PluginSdkRuntime::GetWorld());
		if (!world) { *refusal = R"({"ok":false,"error":"world unavailable"})"; return nullptr; }

		if (!ServerUtilityFields::PlayerActions.On())
		{ *refusal = R"({"ok":false,"error":"player actions unavailable"})"; return nullptr; }

		SDK::AGameStateBase* gs = ListedPlayers::GameStateOf(world);
		if (!gs) { *refusal = R"({"ok":false,"error":"game state unavailable"})"; return nullptr; }
		SDK::APlayerState* ps = ListedPlayers::Find(gs, playerIndex, expectedName, expectedId);
		if (!ps) { *refusal = R"({"ok":false,"error":"player not found"})"; return nullptr; }

		SDK::APlayerController* pc = static_cast<SDK::APlayerController*>(ListedPlayers::OwnerOf(ps));
		if (!pc) { *refusal = R"({"ok":false,"error":"player has no controller"})"; return nullptr; }

		const GameAccess::PlayerPawn held = GameAccess::PawnOf_SEH(pc);
		if (held.kind == PawnKind::None) { *refusal = R"({"ok":false,"error":"player has no pawn"})"; return nullptr; }
		if (held.kind != PawnKind::Character)
		{ *refusal = R"({"ok":false,"error":"player pawn is not a player character"})"; return nullptr; }

		return pc;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		*refusal = R"({"ok":false,"error":"exception giving item"})";
		return nullptr;
	}
}

// Adds the item through op_give's server path (AddNewItem). err is caller-owned storage for the
// error JSON the item lookup may produce.
static std::string GiveItem(int playerIndex, const wchar_t* expectedName, const char* expectedId, int count,
                            const char* assetPath, char* err, int errCap)
{
	const char* refusal = nullptr;
	void* controller = GiveTarget_SEH(playerIndex, expectedName, expectedId, &refusal);
	if (!controller) return refusal;

	const OpGiveGame::Added added = OpGiveGame::AddItem(controller, assetPath, count, err, errCap);
	switch (added.status)
	{
	case OpGiveGame::AddStatus::Added:         return R"({"ok":true,"added":)" + std::to_string(added.given) + "}";
	case OpGiveGame::AddStatus::NothingAdded:
		return std::string(R"({"ok":false,"error":")") + OpGive::kTextNothingAdded + R"("})";
	case OpGiveGame::AddStatus::NoItem:       return err;
	case OpGiveGame::AddStatus::NoSubsystem:  return R"({"ok":false,"error":"item subsystem unavailable"})";
	case OpGiveGame::AddStatus::NoCharacter:  return R"({"ok":false,"error":"player has no pawn"})";
	case OpGiveGame::AddStatus::NoInventory:  return R"({"ok":false,"error":"player character has no inventory"})";
	case OpGiveGame::AddStatus::Unavailable:
		return std::string(R"({"ok":false,"error":")") + OpGive::kTextOff + R"("})";
	case OpGiveGame::AddStatus::Fault:        break;
	}
	return R"({"ok":false,"error":"exception giving item"})";
}
#endif

namespace Ep_InventoryGive
{
	void Handle(const PluginHttpRequest* req, PluginHttpResponse* resp)
	{
		std::string body;

		if (!AdminJson::RequirePost(req, resp, body)) return;

		const std::string token = AdminJson::ExtractString(req->body, req->bodyLen, "session_token");
		if (!AdminSessionStore::Get().ValidateSession(token))
		{
			AdminJson::SetError(resp, 401, "unauthorized", body);
			return;
		}

		// The player is named by player_id, or by player_index plus player_name; see rcon/listed_players.h.
		const std::string listedId   = AdminJson::ExtractString(req->body, req->bodyLen, "player_id");
		const std::string listedName = AdminJson::ExtractString(req->body, req->bodyLen, "player_name");
		const int playerIndex = AdminJson::ExtractInt(req->body, req->bodyLen, "player_index");
		if (listedId.empty() && playerIndex < 0)
		{
			AdminJson::SetError(resp, 400, "player_id or player_index required", body);
			return;
		}

		const int count = AdminJson::ExtractInt(req->body, req->bodyLen, "count", 1);
		if (count <= 0 || count > kMaxGiveCount)
		{
			AdminJson::SetError(resp, 400, "count must be 1 to 9999", body);
			return;
		}

		const std::string assetPath = AdminJson::ExtractString(req->body, req->bodyLen, "asset_path");
		if (assetPath.empty())
		{
			AdminJson::SetError(resp, 400, "asset_path required", body);
			return;
		}

		const std::string result = AdminGT::Dispatch([playerIndex, listedId, listedName, count, assetPath]() -> std::string
		{
#if INV_HAS_SDK
			PlayerInfo listed;
			int position = 0;
			std::string refusal;
			if (!ListedPlayers::Resolve(playerIndex, listedId, listedName, listed, position, refusal))
				return "{\"ok\":false,\"error\":\"" + AdminJson::Escape(refusal) + "\"}";

			const std::wstring expectedName = ListedPlayers::ToWide(listed.name);
			char err[160] = {};
			return GiveItem(position, expectedName.c_str(), listed.id.c_str(), count, assetPath.c_str(), err,
			                static_cast<int>(sizeof(err)));
#else
			return R"({"ok":false,"error":"SDK not available in this build"})";
#endif
		});

		body = result;
		resp->statusCode  = 200;
		resp->contentType = "application/json";
		resp->body        = body.c_str();
		resp->bodyLen     = body.size();
	}
}
