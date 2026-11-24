#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

// The admin claim code, apart from the game and the mod loader (so it can be
// tested alone): making a code, reading what a player typed, comparing, and
// counting wrong tries.
namespace ClaimModel
{
	// Letters and digits a person can tell apart: no 0, O, 1, I or L.
	inline constexpr char    kAlphabet[]   = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";
	constexpr size_t         kAlphabetSize = 31;
	constexpr size_t         kCodeLength   = 8;

	// Fills the buffer with random bytes from the system's secure generator.
	// False when it could not.
	using RandomFill = std::function<bool(unsigned char* buf, size_t size)>;

	// A new code: kCodeLength characters of kAlphabet, each drawn without bias
	// from the random bytes. False when the random source fails (no code is made).
	bool GenerateCode(const RandomFill& fill, std::string& out);

	// What a player typed -> the code form: trimmed, upper case, and only
	// characters of kAlphabet (^[2-9A-HJKMNP-Z]+$). False when it is empty,
	// too long or has any other character. Used before comparing.
	bool NormalizeCode(const std::string& typed, std::string& out);

	// True when the text equals the secret. The time depends on the secret's
	// length only, not on where the two differ.
	bool ConstantTimeEquals(const std::string& text, const std::string& secret);

	// Overwrite the text, then empty it.
	void Wipe(std::string& s);

	// [Policy] AdminPrompt=0 turns the prompt off. The lines are the policy lines
	// the rules model kept unread. Anything else, or no line, leaves it on.
	bool AdminPromptOn(const std::vector<std::string>& policyKept);

	// [Policy] InGameAdmin=0 turns the in-game admin tool off. Same reading as AdminPrompt.
	bool InGameAdminOn(const std::vector<std::string>& policyKept);

	// Wrong tries: per player (then that player is not asked again until the
	// server restarts) and overall (then the code is replaced).
	class Attempts
	{
	public:
		static constexpr int kPerPlayer = 5;
		static constexpr int kOverall   = 5;

		struct Outcome
		{
			int  left         = 0;       // tries this player has left
			bool playerLocked = false;   // this was the player's last try
			bool codeReplaced = false;   // the overall limit was reached: use a new code
		};

		bool    Locked(const std::string& playerId) const;
		Outcome RecordWrong(const std::string& playerId);

	private:
		std::map<std::string, int> m_perPlayer;
		int                        m_overall = 0;
	};
}
