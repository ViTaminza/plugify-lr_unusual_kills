#pragma once
// Engine-free core of the lr_unusual_kills port (the Pisex LR module "[LR] Unusual Kills",
// ref/src/cs2-lvl_ranks_modules/LR_UnusualKills/lr_unusual_kills.cpp, 488 lines): the
// unusual_kills.ini model, the flag evaluation of the kill handler, the per-slot counters and the
// SQL of the `<lr_table>_unusualkills` table.
//
// The engine-side half (the event reads, the LR core hooks, the messages) lives in src/plugin.cpp.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pisex/keyvalues.hpp>
#include <plgdb/db.hpp>

namespace lr_unusual {

inline constexpr int kMaxSlots = 64;

// Entity HANDLES are int32 views of the engine's uint32 CEntityHandle (index | serial << 15):
// valid <=> h != INVALID_EHANDLE_INDEX (-1) && h != 0 (index 0 is the world; GetClientActiveWeapon
// answers 0/-1 when there is no active weapon). The sign means nothing - once the engine's serial
// counter passes 65535 bit 31 is set; the clip falls back to -1 then instead of reading the
// weapon's m_iClip1.
inline bool IsValidHandle(int32_t h) { return h != -1 && h != 0; }

// MAX_UKTYPES of the original: the number of unusual-kill kinds and columns.
inline constexpr int kMaxTypes = 9;

// The UnusualKill_* flags of the original.
enum KillFlag : int {
	kUnusualNone = 0,
	kUnusualOpenFrag = 1 << 0,
	kUnusualPenetrated = 1 << 1,
	kUnusualNoScope = 1 << 2,
	kUnusualRun = 1 << 3,
	kUnusualJump = 1 << 4,
	kUnusualFlash = 1 << 5,
	kUnusualSmoke = 1 << 6,
	kUnusualLastClip = 1 << 7,
	kUnusualDistance = 1 << 8,
};

// g_sNameUK: the TypeKills key (phrase key), the database column and the loop order of the
// original - one entry per flag bit.
inline constexpr const char* kTypeNames[kMaxTypes] = {
	"OP", "Penetrated", "NoScope", "Run", "Jump", "Flash", "Smoke", "LastClip", "Distance",
};

struct Config {
	// "Exp_Mode" (default 1): 0 = database records only, 1 = a separate exp message per kill type,
	// 2 = the exp is added to the kill's own experience.
	int exp_mode = 1;
	// "ProhibitedWeapons": the comma separated list, as written (no trimming - SplitString() of
	// the original split on ',' and kept everything else verbatim).
	std::vector<std::string> prohibited_weapons;
	// "TypeKills" -> "exp" per type; 0 for every type when Exp_Mode is 0 (g_iExp of the original).
	int exp[kMaxTypes] = {};
	float min_distance = 0.0f; // "distance" -> "min_distance" (g_flDistance)
	float min_speed = 100.0f;  // "run" -> "min_speed" (g_flSpeed; 100.0 is the original's GetFloat default for a missing key - its zero-initialised global was only the missing-file fallback)
	// g_bMessages: lr_show_usual_message of the core (read by LoadSettings on core ready).
	bool messages = false;
	// Invalid "TypeKills" names (the original printed "invalid selection" and then wrote past its
	// g_iExp[9] array; the port drops them and reports them here).
	std::string problem;
};

// LoadSettings() of the original (without the core lookup of g_bMessages, passed in instead).
void ParseConfig(const pisex::KeyValues& kv, Config& out, bool messages);

// The first letter of a TypeKills key selects the type (iUKSymbolTypes of the original);
// kInvalidType is the 127 code (an unknown letter).
inline constexpr int kInvalidType = 127;
int TypeOfKeyName(std::string_view name);

// The engine-free view of the player_death event the flag evaluation needs.
struct KillContext {
	std::string weapon;          // pEvent->GetString("weapon")
	bool penetrated = false;     // GetInt("penetrated")
	bool noscope = false;        // GetInt("noscope")
	bool attacker_in_air = false; // GetInt("attackerinair")
	bool attacker_blind = false; // GetInt("attackerblind")
	bool through_smoke = false;  // GetInt("thrusmoke")
	float distance = 0.0f;       // GetFloat("distance")
	float speed = 0.0f;          // the pawn's m_vecAbsVelocity.Length()
	int clip = -1;               // the active weapon's m_iClip1 (-1 = none/unknown)
};

// `isWeaponProhibited` of the original: the raw event weapon string is compared to the config
// entries verbatim (the production config lists "weapon_hegrenade" style names).
bool IsWeaponProhibited(const Config& cfg, std::string_view weapon);

// `szWeapon[0] != 'k' && szWeapon[2] != 'y'`: knife names and "bayonet" never count.
bool IsExcludedWeapon(std::string_view weapon);

// Everything the handler sets before/after the "first kill of the round" flag.
int EvaluateFlags(const Config& cfg, const KillContext& ctx);

// `(!g_bOPKill) { flags |= UnusualKill_OpenFrag; g_bOPKill = true; }`
int AddOpenFrag(int flags);

// The flagged type indices in ascending order (the loop order of the original).
std::vector<int> FlaggedTypes(int flags);

// engine->GetPlayerNetworkIDString() + ConvertSteamID() of the original: CS2's "[U:1:accountid]"
// becomes "STEAM_1:x:y" (the core's conversion, so the `SteamID` column matches the core's
// `steam`); a value that already is a STEAM_ id is passed through, anything unusable becomes "".
std::string ConvertSteamId(std::string_view network_id);

// --- per-slot counters (the original's g_iUK[64][9]) ---------------------------------------------

using Counters = std::array<int, kMaxTypes>;
using State = std::array<Counters, kMaxSlots>;

// --- SQL -----------------------------------------------------------------------------------------

std::string TableName(std::string_view base);
std::string CreateTableSql(std::string_view table, plgdb::Driver driver);

struct Statement {
	std::string sql;
	plgdb::Params params;
};

Statement LoadPlayerStatement(std::string_view table, std::string_view steamid);
// INSERT INTO `<t>_unusualkills` (`SteamID`) VALUES (?) - the empty-row creation of the original.
Statement CreateRowStatement(std::string_view table, std::string_view steamid);
// The nine columns of a SELECT result, in kTypeNames order (empty when the shape is unexpected).
void ApplyLoadedCounters(Counters& counters, const plgdb::Result& result);

// The `UPDATE ... SET `X` = ? ...` of the kill handler; columns carries (kTypeNames index, value).
Statement UpdateCountersStatement(std::string_view table, std::string_view steamid,
								  const std::vector<std::pair<int, int>>& columns);

// OnResetLoadedHook(): every column back to 0.
Statement ResetPlayerStatement(std::string_view table, std::string_view steamid);

// OnDatabaseCleanupHook() for LR_AllData / LR_StatsData.
std::string TruncateSql(std::string_view table, plgdb::Driver driver);

} // namespace lr_unusual
