#include "unusual_core.hpp"

#include <algorithm>
#include <charconv>
#include <optional>
#include <sstream>

namespace lr_unusual {
namespace {

// iUKSymbolTypes of the original: the first letter of the TypeKills key selects the type
// (a=0 ... v=21; 127 marks every letter the module does not know). d=distance(8), f=flash(5),
// j=jump(4), l=lastclip(7), n=noscope(2), o=op(0), p=penetrated(1), r=run(3), s=smoke(6).
int TypeOfFirstLetter(char letter) {
	static constexpr int kSymbolTypes[22] = {127, 127, 127, 8,	127, 5,	  127, 127, 127, 4, 127,
											 7,	  127, 2,	0,	1,	127, 3,	  6,	127, 127, 127};
	const int index = (letter | 32) - 97;
	if (index < 0 || index >= 22) {
		return kInvalidType;
	}
	return kSymbolTypes[index];
}

// SplitString() of the original: std::getline over ',' - empty tokens are kept (",," gives an
// empty entry, which then matches an empty weapon name; kept for parity).
std::vector<std::string> SplitString(const std::string& text, char delim) {
	std::vector<std::string> parts;
	std::istringstream stream(text);
	std::string item;
	while (std::getline(stream, item, delim)) {
		parts.push_back(item);
	}
	return parts;
}

} // namespace

int TypeOfKeyName(std::string_view name) {
	return name.empty() ? kInvalidType : TypeOfFirstLetter(name[0]);
}

void ParseConfig(const pisex::KeyValues& kv, Config& out, bool messages) {
	out.messages = messages;
	out.prohibited_weapons.clear();
	out.exp_mode = kv.GetInt("Exp_Mode", 1);

	// KeyValues::GetString("ProhibitedWeapons") + SplitString on ','.
	out.prohibited_weapons = SplitString(std::string(kv.GetString("ProhibitedWeapons", "")), ',');

	for (int i = 0; i < kMaxTypes; ++i) {
		out.exp[i] = 0;
	}
	out.min_distance = 0.0f;
	out.min_speed = 100.0f;
	out.problem.clear();

	const pisex::KeyValues* type_kills = kv.FindKey("TypeKills");
	if (type_kills == nullptr) {
		// FOR_EACH_SUBKEY over a missing block simply did not run in the original.
		return;
	}
	for (const pisex::KeyValues* key = type_kills->GetFirstTrueSubKey(); key != nullptr;
		 key = key->GetNextTrueSubKey()) {
		const std::string name = key->GetName() != nullptr ? key->GetName() : "";
		const int type = TypeOfKeyName(name);
		switch (type) {
			case 8: // distance
				out.min_distance = key->GetFloat("min_distance", 0.0f);
				break;
			case 3: // run
				out.min_speed = key->GetFloat("min_speed", 100.0f);
				break;
			case kInvalidType:
				// "invalid selection" of the original. (The original then also wrote
				// g_iExp[127] outside its array; the port stops here.)
				if (!out.problem.empty()) {
					out.problem += ", ";
				}
				out.problem += '"' + name + '"';
				break;
			default:
				break;
		}
		if (type >= 0 && type < kMaxTypes) {
			// g_iExp[iUKType] = g_iExpMode > 0 ? pKey->GetInt("exp") : 0;
			out.exp[type] = out.exp_mode > 0 ? key->GetInt("exp", 0) : 0;
		}
	}
	if (!out.problem.empty()) {
		out.problem = "invalid TypeKills selection: " + out.problem;
	}
}

bool IsWeaponProhibited(const Config& cfg, std::string_view weapon) {
	return std::find(cfg.prohibited_weapons.begin(), cfg.prohibited_weapons.end(),
					 std::string(weapon)) != cfg.prohibited_weapons.end();
}

bool IsExcludedWeapon(std::string_view weapon) {
	// `szWeapon[0] != 'k' && szWeapon[2] != 'y'` of the original; the index-2 read is guarded
	// (the original read past the end of short strings).
	if (weapon.empty()) {
		return true;
	}
	if (weapon[0] == 'k') {
		return true;
	}
	return weapon.size() >= 3 && weapon[2] == 'y'; // "bayonet"
}

int EvaluateFlags(const Config& cfg, const KillContext& ctx) {
	int flags = kUnusualNone;
	if (ctx.penetrated) {
		flags |= kUnusualPenetrated;
	}
	if (ctx.noscope) {
		flags |= kUnusualNoScope;
	}
	if (ctx.attacker_in_air) {
		flags |= kUnusualJump;
	}
	// if(vecVelocity.Length() >= g_flSpeed) iUKFlags |= UnusualKill_Run;
	if (ctx.speed >= cfg.min_speed) {
		flags |= kUnusualRun;
	}
	// if(pEvent->GetFloat("distance") > g_flDistance) iUKFlags |= UnusualKill_Distance;
	if (ctx.distance > cfg.min_distance) {
		flags |= kUnusualDistance;
	}
	if (ctx.attacker_blind) {
		flags |= kUnusualFlash;
	}
	if (ctx.through_smoke) {
		flags |= kUnusualSmoke;
	}
	// if(m_hActiveWeapon && m_hActiveWeapon->m_iClip1() == 1) iUKFlags |= UnusualKill_LastClip;
	if (ctx.clip == 1) {
		flags |= kUnusualLastClip;
	}
	return flags;
}

int AddOpenFrag(int flags) {
	return flags | kUnusualOpenFrag;
}

std::vector<int> FlaggedTypes(int flags) {
	std::vector<int> types;
	for (int i = 0; i < kMaxTypes; ++i) {
		if ((flags & (1 << i)) != 0) {
			types.push_back(i);
		}
	}
	return types;
}

std::string ConvertSteamId(std::string_view network_id) {
	const auto begin = network_id.find_first_not_of(" \t\r\n");
	if (begin == std::string_view::npos) {
		return {};
	}
	const auto end = network_id.find_last_not_of(" \t\r\n");
	std::string trimmed(network_id.substr(begin, end - begin + 1));
	if (trimmed.rfind("STEAM_", 0) == 0) {
		return trimmed; // s2sdk's GetClientAuthId hands out STEAM_ ids already
	}
	trimmed.erase(std::remove(trimmed.begin(), trimmed.end(), '['), trimmed.end());
	trimmed.erase(std::remove(trimmed.begin(), trimmed.end(), ']'), trimmed.end());
	const std::size_t first = trimmed.find(':');
	const std::size_t second =
		first == std::string::npos ? std::string::npos : trimmed.find(':', first + 1);
	if (second == std::string::npos) {
		return trimmed; // the original crashed on such input; the port keeps it verbatim
	}
	// 64 bit: the original's std::stoi threw for an account id above INT_MAX (a valid, if rare,
	// Steam account id), which would have taken the server down.
	int64_t account = 0;
	const std::string_view account_text(trimmed.c_str() + second + 1,
										trimmed.size() - (second + 1));
	const auto result =
		std::from_chars(account_text.data(), account_text.data() + account_text.size(), account);
	if (result.ec != std::errc{}) {
		return trimmed;
	}
	return "STEAM_1:" + std::string(account % 2 == 0 ? "0:" : "1:") + std::to_string(account / 2);
}

// --- SQL -----------------------------------------------------------------------------------------

std::string TableName(std::string_view base) {
	return std::string(base) + "_unusualkills";
}

std::string CreateTableSql(std::string_view table, plgdb::Driver driver) {
	const std::string name(table);
	if (driver == plgdb::Driver::MySQL) {
		// The CREATE TABLE of the original, byte for byte (the production database is shared with
		// the Metamod server; `CHARSET = utf8 COLLATE utf8_general_ci` included).
		return "CREATE TABLE IF NOT EXISTS `" + name + "` ("
			   "`SteamID` varchar(22) PRIMARY KEY, "
			   "`OP` int NOT NULL DEFAULT 0, "
			   "`Penetrated` int NOT NULL DEFAULT 0, "
			   "`NoScope` int NOT NULL DEFAULT 0, "
			   "`Run` int NOT NULL DEFAULT 0, "
			   "`Jump` int NOT NULL DEFAULT 0, "
			   "`Flash` int NOT NULL DEFAULT 0, "
			   "`Smoke` int NOT NULL DEFAULT 0, "
			   "`LastClip` int NOT NULL DEFAULT 0, "
			   "`Distance` int NOT NULL DEFAULT 0"
			   ") CHARSET = utf8 COLLATE utf8_general_ci;";
	}
	// SQLite has no CHARSET clause; the columns and the primary key are the same.
	return "CREATE TABLE IF NOT EXISTS `" + name + "` ("
		   "`SteamID` TEXT PRIMARY KEY, "
		   "`OP` INTEGER NOT NULL DEFAULT 0, "
		   "`Penetrated` INTEGER NOT NULL DEFAULT 0, "
		   "`NoScope` INTEGER NOT NULL DEFAULT 0, "
		   "`Run` INTEGER NOT NULL DEFAULT 0, "
		   "`Jump` INTEGER NOT NULL DEFAULT 0, "
		   "`Flash` INTEGER NOT NULL DEFAULT 0, "
		   "`Smoke` INTEGER NOT NULL DEFAULT 0, "
		   "`LastClip` INTEGER NOT NULL DEFAULT 0, "
		   "`Distance` INTEGER NOT NULL DEFAULT 0"
		   ");";
}

Statement LoadPlayerStatement(std::string_view table, std::string_view steamid) {
	Statement statement;
	statement.sql = "SELECT `OP`, `Penetrated`, `NoScope`, `Run`, `Jump`, `Flash`, `Smoke`, "
					"`LastClip`, `Distance` FROM `" + std::string(table) +
					"` WHERE `SteamID` = ?;";
	statement.params.emplace_back(std::string(steamid));
	return statement;
}

Statement CreateRowStatement(std::string_view table, std::string_view steamid) {
	Statement statement;
	statement.sql = "INSERT INTO `" + std::string(table) + "` (`SteamID`) VALUES (?);";
	statement.params.emplace_back(std::string(steamid));
	return statement;
}

void ApplyLoadedCounters(Counters& counters, const plgdb::Result& result) {
	for (int i = 0; i < kMaxTypes; ++i) {
		counters[i] = 0;
	}
	if (!result.Ok() || result.RowCount() == 0) {
		return; // FetchRow() failed in the original -> the counters were zeroed
	}
	for (int i = 0; i < kMaxTypes; ++i) {
		// result->GetInt(i) of the original reads by column position (the SELECT lists them in
		// kTypeNames order).
		counters[i] = static_cast<int>(result.GetInt(0, static_cast<std::size_t>(i), 0));
	}
}

Statement UpdateCountersStatement(std::string_view table, std::string_view steamid,
								  const std::vector<std::pair<int, int>>& columns) {
	Statement statement;
	if (columns.empty()) {
		return statement;
	}
	std::string sql = "UPDATE `" + std::string(table) + "` SET ";
	for (std::size_t i = 0; i < columns.size(); ++i) {
		if (i != 0) {
			sql += ", ";
		}
		const int type = columns[i].first;
		// The column names are the module's own constants (never user input), the values are bound.
		sql += "`" + std::string(kTypeNames[type >= 0 && type < kMaxTypes ? type : 0]) + "` = ?";
		statement.params.emplace_back(static_cast<int64_t>(columns[i].second));
	}
	sql += " WHERE `SteamID` = ?;";
	statement.params.emplace_back(std::string(steamid));
	statement.sql = std::move(sql);
	return statement;
}

Statement ResetPlayerStatement(std::string_view table, std::string_view steamid) {
	Statement statement;
	statement.sql = "UPDATE `" + std::string(table) +
					"` SET `OP` = 0, `Penetrated` = 0, `NoScope` = 0, `Run` = 0, `Jump` = 0, "
					"`Flash` = 0, `Smoke` = 0, `LastClip` = 0, `Distance` = 0 WHERE `SteamID` = ?;";
	statement.params.emplace_back(std::string(steamid));
	return statement;
}

std::string TruncateSql(std::string_view table, plgdb::Driver driver) {
	// SQLite has no TRUNCATE; the effect (an empty table) is the same.
	return (driver == plgdb::Driver::MySQL ? "TRUNCATE TABLE `" : "DELETE FROM `") +
		   std::string(table) + "`;";
}

} // namespace lr_unusual
