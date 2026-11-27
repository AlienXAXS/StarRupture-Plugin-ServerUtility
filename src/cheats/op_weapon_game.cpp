#include "op_weapon_game.h"
#include "op_weapon.h"
#include "plugin_helpers.h"

namespace OpWeaponGame
{
	namespace
	{
		// The weapon attribute calls are not written yet: the feature reports off, so `cap` lists the
		// op off and a request is refused with "Not available on this server's game build."
		bool Available() { return false; }

		bool InWorld() { return false; }

		bool HasWeapon(const CheatDispatch::Context&) { return false; }

		float Current(const std::string&, OpWeapon::Slot) { return 1.0f; }

		bool Set(const CheatDispatch::Context&, uint32_t, OpWeapon::Slot, bool, float) { return false; }
	}

	void Init()
	{
		OpWeapon::Env env;
		env.available = &Available;
		env.inWorld   = &InWorld;
		env.hasWeapon = &HasWeapon;
		env.current   = &Current;
		env.set       = &Set;
		env.log       = [](const std::string& line) { LOG_INFO("[cheat.weapon] %s", line.c_str()); };
		OpWeapon::SetEnv(std::move(env));
	}
}
