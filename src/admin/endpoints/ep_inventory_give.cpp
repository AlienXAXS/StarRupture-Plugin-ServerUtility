#include "ep_inventory_give.h"
#include "../admin_json.h"
#include "../session/session_store.h"
#include "../admin_gamethread.h"

#if defined(MODLOADER_SERVER_BUILD) || defined(MODLOADER_CLIENT_BUILD)
#  include "SDK/Engine_classes.hpp"
#  include "SDK/Chimera_classes.hpp"
#  include "SDK/AuItems_classes.hpp"
#  include "server_utility_fields.h"
#  include "game_access.h"
#  include "item_lookup.h"
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

// No C++ objects with destructors in this frame — SEH is legal.
// err is caller-owned storage for the error JSON FindItemByPath may produce.
// playerIndex is a players/list index; expectedName and expectedId are what the list showed at it.
static const char* GiveItem_SEH(int playerIndex, const wchar_t* expectedName, const char* expectedId, int count,
                                const char* assetPath,
                                char* err, int errCap)
{
	__try
	{
		auto* world = static_cast<SDK::UWorld*>(PluginSdkRuntime::GetWorld());
		if (!world) return R"({"ok":false,"error":"world unavailable"})";

		if (!ServerUtilityFields::PlayerActions.On()) return R"({"ok":false,"error":"player actions unavailable"})";

		SDK::AGameStateBase* gs = ListedPlayers::GameStateOf(world);
		if (!gs) return R"({"ok":false,"error":"game state unavailable"})";
		SDK::APlayerState* ps = ListedPlayers::Find(gs, playerIndex, expectedName, expectedId);
		if (!ps) return R"({"ok":false,"error":"player not found"})";

		SDK::APlayerController* pc = static_cast<SDK::APlayerController*>(ListedPlayers::OwnerOf(ps));
		if (!pc) return R"({"ok":false,"error":"player has no controller"})";

		const GameAccess::PlayerPawn held = GameAccess::PawnOf_SEH(pc);
		if (held.kind == PawnKind::None) return R"({"ok":false,"error":"player has no pawn"})";
		if (held.kind != PawnKind::Character)
			return R"({"ok":false,"error":"player pawn is not a player character"})";

		auto* character = static_cast<SDK::ACrCharacterPlayerBase*>(held.pawn);

		auto* itemSubsys = static_cast<SDK::UAuItemDataBaseSubsystem*>(
			SDK::USubsystemBlueprintLibrary::GetWorldSubsystem(
				world, SDK::UAuItemDataBaseSubsystem::StaticClass()));
		if (!itemSubsys) return R"({"ok":false,"error":"item subsystem unavailable"})";

		SDK::UAuItemDataBase* foundItem = FindItemByPath(itemSubsys, assetPath, err, errCap);
		if (!foundItem) return err;

		character->ServerAddItem(foundItem, static_cast<int32_t>(count));
		return R"({"ok":true})";
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return R"({"ok":false,"error":"exception giving item"})";
	}
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
			return GiveItem_SEH(position, expectedName.c_str(), listed.id.c_str(), count, assetPath.c_str(), err,
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
