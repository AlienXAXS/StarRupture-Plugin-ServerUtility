#pragma once

#include "pawn_kind.h"
#include "server_utility_fields.h"

#include <Windows.h>

// Game access every cheat and admin endpoint needs, in one place. Engine and UObject access is
// for the game thread only; each function says what it checks.
namespace GameAccess
{
	inline bool OnGameThread()
	{
		IPluginHooks* hooks = GetHooks();
		return hooks && hooks->Engine && hooks->Engine->IsGameThread();
	}

	// Null off the game thread: every engine and UObject access starts here.
	inline SDK::UWorld* World()
	{
		if (!OnGameThread())
			return nullptr;
		return static_cast<SDK::UWorld*>(PluginSdkRuntime::GetWorld());
	}

	// Calls one reflected function through the loader's ProcessEvent path. params is the call's
	// parameter block. No C++ objects needing unwinding in this frame: SEH is legal.
	inline bool Invoke_SEH(IPluginHooks* hooks, SDK::UObject* object, const char* cls, const char* function, void* params)
	{
		__try
		{
			void* resolved = hooks->ObjectWalker->ResolveUFunction(cls, function);
			return resolved && hooks->ObjectWalker->InvokeResolvedUFunction(object, resolved, params);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// False when the loader's walker is missing, off the game thread or on a fault.
	inline bool Invoke(SDK::UObject* object, const char* cls, const char* function, void* params)
	{
		IPluginHooks* hooks = GetHooks();
		if (!object || !hooks || !hooks->ObjectWalker || !hooks->ObjectWalker->ResolveUFunction
			|| !hooks->ObjectWalker->InvokeResolvedUFunction || !OnGameThread())
			return false;
		return Invoke_SEH(hooks, object, cls, function, params);
	}

	// A controller's pawn and what it is. pawn is for the call that fetched it, never kept; it is
	// null unless kind is Character or Other. One K2_GetPawn call. Game thread. No C++ objects
	// needing unwinding in this frame: SEH is legal.
	struct PlayerPawn
	{
		PawnKind    kind = PawnKind::None;
		SDK::APawn* pawn = nullptr;
	};

	inline PlayerPawn PawnOf_SEH(void* controller)
	{
		PlayerPawn out;
		__try
		{
			auto* pc = static_cast<SDK::APlayerController*>(controller);
			if (!pc)
				return out;
			SDK::APawn* pawn = pc->K2_GetPawn();
			const bool isCharacter = pawn && pawn->IsA(SDK::ACrCharacterPlayerBase::StaticClass());
			const bool isSpectator = pawn && !isCharacter && pawn->IsA(SDK::ASpectatorPawn::StaticClass());
			out.kind = ClassifyPawn(pawn != nullptr, isCharacter, isSpectator);
			if (out.kind != PawnKind::None)
				out.pawn = pawn;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			out = PlayerPawn();
		}
		return out;
	}

	// The controller's live player character; null when it has no pawn, a spectator pawn or a
	// vehicle. For the call that fetched it, never kept. Game thread.
	inline SDK::ACrCharacterPlayerBase* CharacterOf(void* controller)
	{
		const PlayerPawn p = PawnOf_SEH(controller);
		return p.kind == PawnKind::Character ? static_cast<SDK::ACrCharacterPlayerBase*>(p.pawn) : nullptr;
	}
}
