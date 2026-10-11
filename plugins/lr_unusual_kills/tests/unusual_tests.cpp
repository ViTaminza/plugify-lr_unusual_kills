// Tests for plugins/lr_unusual_kills/src/unusual_core.hpp (engine-free core of the ported
// "[LR] Unusual Kills" module).
//
//   * unusual_kills.ini parsing against the shipped file, including the first-letter
//     type table (iUKSymbolTypes),
//   * the flag evaluation of the kill handler (weapon exclusions, speed/distance/clip thresholds),
//   * the steam id conversion,
//   * the SQL layer: CREATE TABLE / INSERT / SELECT / UPDATE / reset / truncate against SQLite
//     ":memory:" always and against MySQL when MYSQL_HOST is set (test.sh provides
//     MYSQL_HOST/MYSQL_PORT/MYSQL_USER/MYSQL_PASSWORD/MYSQL_DATABASE). Rows created in MySQL are
//     deleted at the end.

#include <plg/plugin.hpp>

#include <pisex/keyvalues.hpp>
#include <pisex/paths.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "unusual_core.hpp"

// A real plugin gets these from PLUGIFY_PLUGIN; the test leaves them null. The path resolver is
// driven through pisex::SetPluginName/SetConfigDir instead.
namespace plg {
GetBaseDirFn GetBaseDir{nullptr};
GetExtensionsDirFn GetExtensionsDir{nullptr};
GetConfigsDirFn GetConfigsDir{nullptr};
GetDataDirFn GetDataDir{nullptr};
GetLogsDirFn GetLogsDir{nullptr};
GetCacheDirFn GetCacheDir{nullptr};
IsLoadedFn IsLoaded{nullptr};
LogFn Log{nullptr};
BeginZoneFn BeginZone{nullptr};
EndZoneFn EndZone{nullptr};
} // namespace plg

namespace {

int g_checks = 0;
int g_failures = 0;
std::string_view g_section = "?";

void Section(std::string_view name) {
	g_section = name;
	std::printf("-- %.*s\n", static_cast<int>(name.size()), name.data());
}

#define CHECK(expr)                                                                            \
	do {                                                                                       \
		++g_checks;                                                                            \
		if (!(expr)) {                                                                         \
			++g_failures;                                                                      \
			std::printf("FAIL [%.*s] %s:%d: %s\n", static_cast<int>(g_section.size()),         \
						g_section.data(), __FILE__, __LINE__, #expr);                          \
		}                                                                                      \
	} while (false)

#define CHECK_EQ_INT(actual, expected)                                                         \
	do {                                                                                       \
		++g_checks;                                                                            \
		const long long a_ = static_cast<long long>(actual);                                   \
		const long long e_ = static_cast<long long>(expected);                                 \
		if (a_ != e_) {                                                                        \
			++g_failures;                                                                      \
			std::printf("FAIL [%.*s] %s:%d: got %lld, expected %lld\n",                        \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__,       \
						__LINE__, a_, e_);                                                     \
		}                                                                                      \
	} while (false)

#define CHECK_EQ_STR(actual, expected)                                                         \
	do {                                                                                       \
		++g_checks;                                                                            \
		const std::string a_ = (actual);                                                       \
		const std::string e_ = (expected);                                                     \
		if (a_ != e_) {                                                                        \
			++g_failures;                                                                      \
			std::printf("FAIL [%.*s] %s:%d: got \"%s\", expected \"%s\"\n",                    \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__,       \
						__LINE__, a_.c_str(), e_.c_str());                                     \
		}                                                                                      \
	} while (false)

using namespace lr_unusual;

// --- configuration ------------------------------------------------------------------------------

void TestRealConfig() {
	Section("real unusual_kills.ini");
	pisex::SetPluginName("levels_ranks");
	pisex::SetConfigDir(LR_UNUSUAL_TEST_DATA_DIR);

	pisex::KeyValues kv("LR_UnusualKills");
	CHECK(kv.LoadFromFile("addons/configs/levels_ranks/unusual_kills.ini"));

	Config cfg;
	ParseConfig(kv, cfg, /*messages=*/true);
	CHECK(cfg.problem.empty());
	CHECK_EQ_INT(cfg.exp_mode, 1);
	CHECK(cfg.messages);

	// "ProhibitedWeapons": kept verbatim (including the weapon_ prefix of the shipped file).
	CHECK_EQ_INT(cfg.prohibited_weapons.size(), 3);
	CHECK_EQ_STR(cfg.prohibited_weapons[0], "weapon_hegrenade");
	CHECK_EQ_STR(cfg.prohibited_weapons[1], "weapon_molotov");
	CHECK_EQ_STR(cfg.prohibited_weapons[2], "weapon_incgrenade");

	// The exp values of the shipped TypeKills block, in kTypeNames order.
	CHECK_EQ_INT(cfg.exp[0], 2); // OP
	CHECK_EQ_INT(cfg.exp[1], 2); // Penetrated
	CHECK_EQ_INT(cfg.exp[2], 4); // NoScope
	CHECK_EQ_INT(cfg.exp[3], 2); // Run
	CHECK_EQ_INT(cfg.exp[4], 3); // Jump
	CHECK_EQ_INT(cfg.exp[5], 2); // Flash
	CHECK_EQ_INT(cfg.exp[6], 5); // Smoke
	CHECK_EQ_INT(cfg.exp[7], 5); // LastClip
	CHECK_EQ_INT(cfg.exp[8], 2); // Distance
	CHECK_EQ_INT(cfg.min_distance * 10.0f, 250); // "25.0"
	// The "run" block of the shipped file has no min_speed -> the default 100.0 of GetFloat.
	CHECK_EQ_INT(cfg.min_speed, 100);
}

void TestTypeTable() {
	Section("TypeOfKeyName (iUKSymbolTypes)");
	CHECK_EQ_INT(TypeOfKeyName("op"), 0);
	CHECK_EQ_INT(TypeOfKeyName("penetrated"), 1);
	CHECK_EQ_INT(TypeOfKeyName("noscope"), 2);
	CHECK_EQ_INT(TypeOfKeyName("run"), 3);
	CHECK_EQ_INT(TypeOfKeyName("Jump"), 4);       // the original's (pszName[0] | 32)
	CHECK_EQ_INT(TypeOfKeyName("FLASH"), 5);
	CHECK_EQ_INT(TypeOfKeyName("smoke"), 6);
	CHECK_EQ_INT(TypeOfKeyName("lastclip"), 7);
	CHECK_EQ_INT(TypeOfKeyName("distance"), 8);
	// Everything else: "invalid selection" (127 in the original).
	CHECK_EQ_INT(TypeOfKeyName("bhop"), kInvalidType);
	CHECK_EQ_INT(TypeOfKeyName("z"), kInvalidType);
	CHECK_EQ_INT(TypeOfKeyName(""), kInvalidType);
	CHECK_EQ_INT(TypeOfKeyName("1"), kInvalidType);
}

void TestConfigVariants() {
	Section("config variants");
	{
		// Exp_Mode 0 zeroes every exp value (the loader of the original).
		pisex::KeyValues kv("LR_UnusualKills");
		CHECK(kv.LoadFromBuffer("\"LR_UnusualKills\"\n{\n"
								"    \"Exp_Mode\" \"0\"\n"
								"    \"TypeKills\" { \"op\" { \"exp\" \"5\" } }\n}\n"));
		Config cfg;
		ParseConfig(kv, cfg, false);
		CHECK_EQ_INT(cfg.exp_mode, 0);
		CHECK_EQ_INT(cfg.exp[0], 0);
	}
	{
		// An unknown type key is reported, everything else keeps working.
		pisex::KeyValues kv("LR_UnusualKills");
		CHECK(kv.LoadFromBuffer("\"LR_UnusualKills\"\n{\n"
								"    \"TypeKills\" { \"bhop\" { \"exp\" \"5\" } \"op\" { \"exp\" \"1\" } }\n}\n"));
		Config cfg;
		ParseConfig(kv, cfg, false);
		CHECK(!cfg.problem.empty());
		CHECK_EQ_INT(cfg.exp[0], 1);
	}
	{
		// A missing TypeKills block and a missing ProhibitedWeapons keep the defaults.
		pisex::KeyValues kv("LR_UnusualKills");
		CHECK(kv.LoadFromBuffer("\"LR_UnusualKills\"\n{\n}\n"));
		Config cfg;
		ParseConfig(kv, cfg, false);
		CHECK_EQ_INT(cfg.exp_mode, 1);
		CHECK(cfg.prohibited_weapons.empty());
		CHECK_EQ_INT(cfg.min_speed, 100);
		CHECK_EQ_INT(cfg.min_distance, 0);
	}
	{
		// "run" -> "min_speed" and "distance" -> "min_distance".
		pisex::KeyValues kv("LR_UnusualKills");
		CHECK(kv.LoadFromBuffer("\"LR_UnusualKills\"\n{\n"
								"    \"TypeKills\" {\n"
								"        \"run\" { \"exp\" \"1\" \"min_speed\" \"250.5\" }\n"
								"        \"distance\" { \"exp\" \"1\" \"min_distance\" \"1500.0\" }\n"
								"    }\n}\n"));
		Config cfg;
		ParseConfig(kv, cfg, false);
		CHECK_EQ_INT(cfg.min_speed * 10.0f, 2505);
		CHECK_EQ_INT(cfg.min_distance * 10.0f, 15000);
	}
}

// --- the flag evaluation ------------------------------------------------------------------------

void TestWeaponFilters() {
	Section("weapon filters");
	Config cfg;
	cfg.prohibited_weapons = {"weapon_hegrenade", "weapon_molotov", "weapon_incgrenade"};
	// The comparison is verbatim, like the original's std::find over the split list: the event
	// value "hegrenade" does NOT match the shipped "weapon_hegrenade" entry.
	CHECK(!IsWeaponProhibited(cfg, "hegrenade"));
	CHECK(IsWeaponProhibited(cfg, "weapon_hegrenade"));
	CHECK(!IsWeaponProhibited(cfg, "ak47"));

	// The knife/bayonet exclusion of the original.
	CHECK(IsExcludedWeapon("knife"));
	CHECK(IsExcludedWeapon("karambit"));
	CHECK(IsExcludedWeapon("bayonet"));
	CHECK(IsExcludedWeapon(""));
	CHECK(!IsExcludedWeapon("ak47"));
	CHECK(!IsExcludedWeapon("m4a1"));
	CHECK(!IsExcludedWeapon("mac10")); // 'm' 'a' 'c' - index 2 is not 'y'
}

void TestEvaluateFlags() {
	Section("EvaluateFlags");
	Config cfg;
	cfg.min_speed = 100.0f;
	cfg.min_distance = 25.0f;

	KillContext ctx;
	ctx.weapon = "ak47";
	CHECK_EQ_INT(EvaluateFlags(cfg, ctx), kUnusualNone);

	ctx.penetrated = true;
	CHECK_EQ_INT(EvaluateFlags(cfg, ctx), kUnusualPenetrated);
	ctx.noscope = true;
	CHECK_EQ_INT(EvaluateFlags(cfg, ctx), kUnusualPenetrated | kUnusualNoScope);
	ctx.attacker_in_air = true;
	CHECK_EQ_INT(EvaluateFlags(cfg, ctx),
				 kUnusualPenetrated | kUnusualNoScope | kUnusualJump);
	ctx.attacker_blind = true;
	CHECK_EQ_INT(EvaluateFlags(cfg, ctx),
				 kUnusualPenetrated | kUnusualNoScope | kUnusualJump | kUnusualFlash);
	ctx.through_smoke = true;
	CHECK_EQ_INT(EvaluateFlags(cfg, ctx), kUnusualPenetrated | kUnusualNoScope | kUnusualJump |
											  kUnusualFlash | kUnusualSmoke);

	// Speed: `>= g_flSpeed` (100 inclusive).
	ctx = KillContext{};
	ctx.speed = 99.9f;
	CHECK_EQ_INT(EvaluateFlags(cfg, ctx), kUnusualNone);
	ctx.speed = 100.0f;
	CHECK_EQ_INT(EvaluateFlags(cfg, ctx), kUnusualRun);

	// Distance: strict `>`.
	ctx = KillContext{};
	ctx.distance = 25.0f;
	CHECK_EQ_INT(EvaluateFlags(cfg, ctx), kUnusualNone);
	ctx.distance = 25.1f;
	CHECK_EQ_INT(EvaluateFlags(cfg, ctx), kUnusualDistance);

	// LastClip: the active weapon's m_iClip1 == 1 (no weapon / an unknown clip does not count).
	ctx = KillContext{};
	CHECK_EQ_INT(EvaluateFlags(cfg, ctx), kUnusualNone);
	ctx.clip = 0;
	CHECK_EQ_INT(EvaluateFlags(cfg, ctx), kUnusualNone);
	ctx.clip = 1;
	CHECK_EQ_INT(EvaluateFlags(cfg, ctx), kUnusualLastClip);
	ctx.clip = 2;
	CHECK_EQ_INT(EvaluateFlags(cfg, ctx), kUnusualNone);

	// The round's first kill consumes the OP flag (g_bOPKill), the returned flags carry it.
	CHECK_EQ_INT(AddOpenFrag(kUnusualNone), kUnusualOpenFrag);
	CHECK_EQ_INT(AddOpenFrag(kUnusualRun), kUnusualOpenFrag | kUnusualRun);

	// The loop order of the original: ascending type index = kTypeNames order.
	const std::vector<int> flagged = FlaggedTypes(kUnusualSmoke | kUnusualOpenFrag | kUnusualDistance);
	CHECK_EQ_INT(flagged.size(), 3);
	CHECK_EQ_INT(flagged[0], 0); // OP
	CHECK_EQ_INT(flagged[1], 6); // Smoke
	CHECK_EQ_INT(flagged[2], 8); // Distance
	CHECK(FlaggedTypes(kUnusualNone).empty());

	// The column names the UPDATE uses.
	CHECK_EQ_STR(kTypeNames[0], "OP");
	CHECK_EQ_STR(kTypeNames[5], "Flash");
	CHECK_EQ_STR(kTypeNames[8], "Distance");
}

void TestSteamId() {
	Section("ConvertSteamId");
	CHECK_EQ_STR(ConvertSteamId("[U:1:123]"), "STEAM_1:1:61");
	CHECK_EQ_STR(ConvertSteamId("[U:1:124]"), "STEAM_1:0:62");
	CHECK_EQ_STR(ConvertSteamId("STEAM_1:0:62"), "STEAM_1:0:62");
	CHECK_EQ_STR(ConvertSteamId("  STEAM_1:1:5  "), "STEAM_1:1:5");
	CHECK_EQ_STR(ConvertSteamId(""), "");
	CHECK_EQ_STR(ConvertSteamId("BOT"), "BOT");
}

// --- SQL ---------------------------------------------------------------------------------------

plgdb::Config SqliteConfig() {
	plgdb::Config config;
	config.driver = plgdb::Driver::SQLite;
	config.sqlitePath = ":memory:";
	return config;
}

bool MySqlConfig(plgdb::Config& config, std::string& error) {
	const char* host = std::getenv("MYSQL_HOST");
	if (host == nullptr || *host == '\0') {
		return false;
	}
	config.driver = plgdb::Driver::MySQL;
	config.host = host;
	config.user = std::getenv("MYSQL_USER") != nullptr ? std::getenv("MYSQL_USER") : "";
	config.password = std::getenv("MYSQL_PASSWORD") != nullptr ? std::getenv("MYSQL_PASSWORD") : "";
	config.database = std::getenv("MYSQL_DATABASE") != nullptr ? std::getenv("MYSQL_DATABASE") : "";
	const char* port = std::getenv("MYSQL_PORT");
	if (port != nullptr && *port != '\0') {
		try {
			const int value = std::stoi(port);
			if (value < 1 || value > 65535) {
				error = "MYSQL_PORT out of range";
				return false;
			}
			config.port = static_cast<uint16_t>(value);
		} catch (const std::exception&) {
			error = "MYSQL_PORT is not a number";
			return false;
		}
	}
	return true;
}

void TestHandleValidity() {
	Section("entity handle validity");
	// The case seen in practice: index 93, serial 102949 -> 0xC912805D = -921534371, a valid
	// handle the game hands out once its serial counter has passed 65535 (bit 31 set).
	CHECK(IsValidHandle(-921534371));
	CHECK(IsValidHandle(0x7FFFFFFF));
	CHECK(IsValidHandle(1));
	CHECK(!IsValidHandle(0));  // "none" / index 0 (the world)
	CHECK(!IsValidHandle(-1)); // INVALID_EHANDLE_INDEX (0xFFFFFFFF)
}

void TestDatabase(const plgdb::Config& config, std::string_view label, const std::string& table,
				  bool cleanup) {
	Section(std::string("database ") + std::string(label));
	std::unique_ptr<plgdb::Database> db = plgdb::Database::Create(config);
	CHECK(db != nullptr);
	if (db == nullptr) {
		return;
	}

	const plgdb::Result created = db->QuerySync(CreateTableSql(table, db->GetDriver()), {});
	CHECK(created.Ok());

	const std::string steam = "STEAM_1:0:90000002";
	Counters counters{};

	// No row yet: the loader zeros the counters and the module INSERTs the empty row.
	{
		Statement select = LoadPlayerStatement(table, steam);
		const plgdb::Result result = db->QuerySync(select.sql, select.params);
		CHECK(result.Ok());
		CHECK_EQ_INT(result.RowCount(), 0);
		counters[0] = 42; // a stale value must be cleared
		ApplyLoadedCounters(counters, result);
		CHECK_EQ_INT(counters[0], 0);
		Statement insert = CreateRowStatement(table, steam);
		CHECK(db->QuerySync(insert.sql, insert.params).Ok());
	}

	// The kill handler of the shipped config: OP (2), Run (2), Distance (2) - the counters are
	// the incremented ones and land in the named columns.
	{
		const std::vector<int> flagged = FlaggedTypes(kUnusualOpenFrag | kUnusualRun | kUnusualDistance);
		std::vector<std::pair<int, int>> columns;
		for (const int type : flagged) {
			counters[static_cast<std::size_t>(type)]++;
			columns.emplace_back(type, counters[static_cast<std::size_t>(type)]);
		}
		Statement update = UpdateCountersStatement(table, steam, columns);
		CHECK(update.sql.find("`OP` = ?") != std::string::npos);
		CHECK(update.sql.find("`Run` = ?") != std::string::npos);
		CHECK(update.sql.find("`Distance` = ?") != std::string::npos);
		CHECK(update.sql.find("`Jump`") == std::string::npos);
		CHECK(!update.params.empty());
		CHECK(db->QuerySync(update.sql, update.params).Ok());

		// A second OP kill of another round.
		Statement second = UpdateCountersStatement(table, steam, {{0, ++counters[0]}});
		CHECK(db->QuerySync(second.sql, second.params).Ok());
	}

	// Reload: the nine columns come back in kTypeNames order.
	{
		Statement select = LoadPlayerStatement(table, steam);
		const plgdb::Result result = db->QuerySync(select.sql, select.params);
		CHECK(result.Ok());
		CHECK_EQ_INT(result.RowCount(), 1);
		CHECK_EQ_INT(result.ColumnCount(), 9);
		Counters loaded{};
		ApplyLoadedCounters(loaded, result);
		CHECK_EQ_INT(loaded[0], 2);  // OP
		CHECK_EQ_INT(loaded[3], 1);  // Run
		CHECK_EQ_INT(loaded[8], 1);  // Distance
		CHECK_EQ_INT(loaded[4], 0);  // Jump
	}

	// OnResetLoadedHook(): every column back to zero.
	{
		Statement reset = ResetPlayerStatement(table, steam);
		CHECK(db->QuerySync(reset.sql, reset.params).Ok());
		Statement select = LoadPlayerStatement(table, steam);
		const plgdb::Result result = db->QuerySync(select.sql, select.params);
		CHECK(result.Ok());
		CHECK_EQ_INT(result.RowCount(), 1);
		Counters loaded{};
		ApplyLoadedCounters(loaded, result);
		for (int i = 0; i < kMaxTypes; ++i) {
			CHECK_EQ_INT(loaded[static_cast<std::size_t>(i)], 0);
		}
	}

	// OnDatabaseCleanupHook(): the TRUNCATE/DELETE.
	CHECK(db->QuerySync(TruncateSql(table, db->GetDriver()), {}).Ok());
	{
		Statement select = LoadPlayerStatement(table, steam);
		const plgdb::Result result = db->QuerySync(select.sql, select.params);
		CHECK(result.Ok());
		CHECK_EQ_INT(result.RowCount(), 0);
	}

	if (cleanup) {
		db->QuerySync("DROP TABLE `" + table + "`;", {});
	}
}

} // namespace

int main() {
	std::printf("lr_unusual_kills_tests\n");

	std::random_device device;
	std::mt19937_64 rng(device());
	const std::string tag = std::to_string(rng() % 1000000ULL);

	TestRealConfig();
	TestTypeTable();
	TestConfigVariants();
	TestWeaponFilters();
	TestEvaluateFlags();
	TestSteamId();
	TestHandleValidity();

	TestDatabase(SqliteConfig(), "sqlite (:memory:)", "lvl_unusual_sqlite", /*cleanup=*/false);

	plgdb::Config mysql;
	std::string error;
	if (MySqlConfig(mysql, error)) {
		std::printf("== mysql: %s:%u/%s\n", mysql.host.c_str(), mysql.port, mysql.database.c_str());
		TestDatabase(mysql, "mysql", "lvl_unusual_test_" + tag, /*cleanup=*/true);
	} else if (!error.empty()) {
		std::printf("FATAL: %s\n", error.c_str());
		++g_failures;
	}

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
