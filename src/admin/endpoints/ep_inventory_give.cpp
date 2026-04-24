#include "ep_inventory_give.h"
#include "../admin_json.h"
#include "../session/session_store.h"
#include "../admin_gamethread.h"

#if defined(MODLOADER_SERVER_BUILD) || defined(MODLOADER_CLIENT_BUILD)
#  include "SDK/Engine_classes.hpp"
#  include "SDK/Chimera_classes.hpp"
#  include "SDK/AuItems_classes.hpp"
#  define INV_HAS_SDK 1
#else
#  define INV_HAS_SDK 0
#endif

#include <string>

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

		const int playerIndex = AdminJson::ExtractInt(req->body, req->bodyLen, "player_index");
		if (playerIndex < 0)
		{
			AdminJson::SetError(resp, 400, "player_index required", body);
			return;
		}

		const int count = AdminJson::ExtractInt(req->body, req->bodyLen, "count", 1);
		if (count <= 0)
		{
			AdminJson::SetError(resp, 400, "count must be > 0", body);
			return;
		}

		const std::string assetPath = AdminJson::ExtractString(req->body, req->bodyLen, "asset_path");
		if (assetPath.empty())
		{
			AdminJson::SetError(resp, 400, "asset_path required", body);
			return;
		}

		const std::string result = AdminGT::Dispatch([playerIndex, count, assetPath]() -> std::string
		{
#if INV_HAS_SDK
			__try
			{
				auto* world = static_cast<SDK::UWorld*>(SDK::UWorld::GetWorld());
				if (!world) return R"({"ok":false,"error":"world unavailable"})";

				// Find player
				SDK::AGameStateBase* gs = world->GameState;
				if (!gs) return R"({"ok":false,"error":"game state unavailable"})";
				if (playerIndex >= gs->PlayerArray.Num()) return R"({"ok":false,"error":"player not found"})";

				SDK::APlayerState* ps = gs->PlayerArray[playerIndex];
				if (!ps) return R"({"ok":false,"error":"player not found"})";

				SDK::APlayerController* pc = ps->GetOwningPlayerController();
				if (!pc) return R"({"ok":false,"error":"player has no controller"})";

				SDK::APawn* pawn = pc->K2_GetPawn();
				if (!pawn) return R"({"ok":false,"error":"player has no pawn"})";

				auto* character = static_cast<SDK::ACrCharacterPlayerBase*>(pawn);

				// Find item in UAuItemDataBaseSubsystem
				auto* itemSubsys = static_cast<SDK::UAuItemDataBaseSubsystem*>(
					SDK::USubsystemBlueprintLibrary::GetWorldSubsystem(
						world, SDK::UAuItemDataBaseSubsystem::StaticClass()));

				if (!itemSubsys) return R"({"ok":false,"error":"item subsystem unavailable"})";

				SDK::UAuItemDataBase* foundItem = nullptr;
				const int32_t itemCount = itemSubsys->ItemDataBaseArray.Num();
				for (int32_t i = 0; i < itemCount; ++i)
				{
					SDK::UAuItemDataBase* item = itemSubsys->ItemDataBaseArray[i];
					if (!item) continue;

					// GetFullName returns "ClassName /Package/Path.ObjectName"
					// Match against the provided asset_path substring
					std::string fullName = item->GetFullName();
					if (fullName.find(assetPath) != std::string::npos)
					{
						foundItem = item;
						break;
					}
				}

				if (!foundItem) return R"({"ok":false,"error":"item not found"})";

				character->ServerAddItem(foundItem, static_cast<int32_t>(count));
				return R"({"ok":true})";
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return R"({"ok":false,"error":"exception giving item"})";
			}
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
