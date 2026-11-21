#include "ep_inventory_give.h"
#include "../admin_json.h"
#include "../session/session_store.h"
#include "../admin_gamethread.h"

#if defined(MODLOADER_SERVER_BUILD) || defined(MODLOADER_CLIENT_BUILD)
#  include "SDK/Engine_classes.hpp"
#  include "SDK/Chimera_classes.hpp"
#  include "SDK/AuItems_classes.hpp"
#  include "server_utility_fields.h"
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
// Separated from the SEH helper so std::string objects don't live in the __try frame.
// An exact asset_path match wins. Otherwise a substring match is used only when it
// names exactly one item. On failure nullptr is returned and err holds the error JSON.
static SDK::UAuItemDataBase* FindItemByPath(SDK::UAuItemDataBaseSubsystem* itemSubsys, const char* assetPath,
                                            char* err, int errCap)
{
	const auto* items = ServerUtilityFields::ItemDataBaseArray.In(itemSubsys);
	if (!items)
	{
		snprintf(err, errCap, R"({"ok":false,"error":"item not found"})");
		return nullptr;
	}

	SDK::UAuItemDataBase* single = nullptr;
	int hits = 0;

	const int32_t itemCount = items->Num();
	for (int32_t i = 0; i < itemCount; ++i)
	{
		SDK::UAuItemDataBase* item = (*items)[i];
		if (!item) continue;

		const std::string fullName = item->GetFullName(); // "<Class> <path>.<name>"

		// The path without the class prefix, and the bare name after its last '.' or ':'.
		const size_t space = fullName.find(' ');
		const std::string path = (space == std::string::npos) ? fullName : fullName.substr(space + 1);
		const size_t sep = path.find_last_of(".:");
		const std::string shortName = (sep == std::string::npos) ? path : path.substr(sep + 1);
		if (path == assetPath || shortName == assetPath)
			return item;

		if (fullName.find(assetPath) != std::string::npos)
		{
			++hits;
			single = item;
		}
	}

	if (hits == 1)
		return single;

	if (hits == 0)
		snprintf(err, errCap, R"({"ok":false,"error":"item not found"})");
	else
		snprintf(err, errCap, R"({"ok":false,"error":"%d items match, use the full asset_path"})", hits);
	return nullptr;
}

// No C++ objects with destructors in this frame — SEH is legal.
// err is caller-owned storage for the error JSON FindItemByPath may produce.
// playerIndex is a players/list index; expectedName is the name the list showed at it.
static const char* GiveItem_SEH(int playerIndex, const wchar_t* expectedName, int count, const char* assetPath,
                                char* err, int errCap)
{
	__try
	{
		auto* world = static_cast<SDK::UWorld*>(PluginSdkRuntime::GetWorld());
		if (!world) return R"({"ok":false,"error":"world unavailable"})";

		SDK::AGameStateBase* gs = world->GameState;
		if (!gs) return R"({"ok":false,"error":"game state unavailable"})";
		SDK::APlayerState* ps = ListedPlayers::Find(gs, playerIndex, expectedName);
		if (!ps) return R"({"ok":false,"error":"player not found"})";

		SDK::APlayerController* pc = static_cast<SDK::APlayerController*>(ps->GetOwner());
		if (!pc) return R"({"ok":false,"error":"player has no controller"})";

		SDK::APawn* pawn = pc->K2_GetPawn();
		if (!pawn) return R"({"ok":false,"error":"player has no pawn"})";

		if (!pawn->IsA(SDK::ACrCharacterPlayerBase::StaticClass()))
			return R"({"ok":false,"error":"player pawn is not a player character"})";

		auto* character = static_cast<SDK::ACrCharacterPlayerBase*>(pawn);

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

		const int playerIndex = AdminJson::ExtractInt(req->body, req->bodyLen, "player_index");
		if (playerIndex < 0)
		{
			AdminJson::SetError(resp, 400, "player_index required", body);
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

		const std::string result = AdminGT::Dispatch([playerIndex, count, assetPath]() -> std::string
		{
#if INV_HAS_SDK
			PlayerInfo listed;
			if (!ListedPlayers::Cached(playerIndex, listed))
				return R"({"ok":false,"error":"player not found"})";

			const std::wstring expectedName = ListedPlayers::ToWide(listed.name);
			char err[160] = {};
			return GiveItem_SEH(playerIndex, expectedName.c_str(), count, assetPath.c_str(), err,
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
