// lr_unusual_kills — Plugify C++ port of the Pisex LR module "[LR] Unusual Kills"
// (ref/src/cs2-lvl_ranks_modules/LR_UnusualKills/lr_unusual_kills.cpp, 488 lines; the production
// plugin addons/lr_modules/lr_unusual_kills, vdf lr_unusual_kills.vdf).
//
// What it does (identical to the original): counts "unusual" kills (first kill of the round,
// penetration, no-scope, running, jump, flash-blind, through smoke, last clip, long distance) into
// the `<lr_table>_unusualkills` table, and either awards the configured experience per kill type
// (a separate message per type), adds it to the kill's own experience or only records the kill,
// depending on Exp_Mode of unusual_kills.ini. The messages use the module's phrase file.
//
// Port notes (the rest is parity):
//   * pEvent->GetPlayerController("attacker") + m_steamID() become pisex::GetControllerEntity() /
//     GetPawnEntity() / g_pPlayers->GetSteamID64(); the pawn's m_vecAbsVelocity() is s2sdk's
//     GetClientAbsVelocity (the same field) and m_hActiveWeapon()->m_iClip1() is the active weapon
//     handle of s2sdk::GetClientActiveWeapon plus a schema read of CBasePlayerWeapon::m_iClip1.
//   * The original refreshed the entity-system globals from IUtilsApi::StartupServer; s2sdk has no
//     entity-system object, so that part is gone (every read goes through the bindings instead).
//   * The original's round_start hook (g_bOPKill = false) is pisex::HookEvent("round_start", ...).
//   * A kill whose attacker has no readable steam id no longer writes the UPDATE with an empty
//     SteamID (the original's ConvertSteamID(nullptr) crash guard, and no row could match anyway).
//   * The TypeKills key of an unknown name ("invalid selection" of the original, which then also
//     wrote one int past g_iExp[9]) is logged once and dropped; the production config has none.
//   * The ProhibitedWeapons list is compared verbatim with the event's weapon string, as in the
//     original; the production file lists classname-style names ("weapon_hegrenade"), see the
//     report - this needs a live-server check, the port keeps the original's behaviour.
//   * Own plgdb connection (GetDatabaseConfig(), "lr.sqlite3" fallback) and the same table.

#include <pisex/pisex.hpp>

#include <plg/plugin.hpp>

#include <s2sdk/s2sdk.hpp>

#include <lr_api.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "unusual_core.hpp"

namespace {

// GetLogTag() of the original.
constexpr char kLogTag[] = "[LR-UK]";

// The original's g_pLRCore.
levels_ranks::LrApi* g_pLRCore = &levels_ranks::LrApi::Get();

using namespace lr_unusual;

class LrUnusualKillsPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

private:
	void LoadPhrases();
	void LoadSettings();
	void OnCoreIsReady();
	void OnPlayerLoaded(int slot, const char* steamid);
	void OnResetPlayerStats(int slot, const char* steamid);
	void OnPlayerKilledPre(pisex::IGameEvent* event, int& exp_caused);
	void OnDatabaseCleanup(int type);
	void OnRoundStart();

	void RunStatement(Statement statement);
	void ResetSlot(int slot);
	std::string SteamIdOf(int slot) const;
	bool PlayerUsable(int slot) const;
	// pAttacker->GetPlayerPawn()->m_vecAbsVelocity().Length()
	float AttackerSpeed(int slot) const;
	// pAttacker->GetPlayerPawn()->m_pWeaponServices()->m_hActiveWeapon()->m_iClip1()
	int ActiveClip(int slot);
	// CBasePlayerWeapon::m_iClip1 resolved once (-1 until the schema system answers).
	int m_clip1_offset = -1;

	Config m_cfg;
	State m_counters{};
	std::array<std::string, kMaxSlots> m_steamids{};
	std::map<std::string, std::string> m_phrases;
	std::unique_ptr<plgdb::Database> m_db;
	std::string m_table; // <lr_table>_unusualkills
	bool m_op_kill = false;
};

LrUnusualKillsPlugin g_plugin;

// --- configuration -------------------------------------------------------------------------------

void LrUnusualKillsPlugin::LoadPhrases() {
	// AllPluginsLoaded() of the original: addons/translations/lr_unusualkills.phrases.txt, with
	// g_pUtils->GetLanguage() (= pisex::PhrasesLanguage(), the normalised server language).
	pisex::PhraseMap phrases;
	if (!pisex::LoadPhrasesFile("addons/translations/lr_unusualkills.phrases.txt", phrases)) {
		pisex::ErrorLog("%s Failed to load addons/translations/lr_unusualkills.phrases.txt", kLogTag);
		return;
	}
	m_phrases.clear();
	m_phrases.insert(phrases.begin(), phrases.end());
}

void LrUnusualKillsPlugin::LoadSettings() {
	// LoadSettings() of the original: KeyValues("LR_UnusualKills")
	// -> addons/configs/levels_ranks/unusual_kills.ini.
	pisex::KeyValues kv("LR_UnusualKills");
	if (!kv.LoadFromFile("addons/configs/levels_ranks/unusual_kills.ini")) {
		pisex::ErrorLog("%s Failed to load addons/configs/levels_ranks/unusual_kills.ini", kLogTag);
		return;
	}
	// g_bMessages = g_pLRCore->GetSettingsValue(LR_ShowUsualMessage) == 1;
	const bool messages =
		g_pLRCore->GetSettingsValue(levels_ranks::LR_ShowUsualMessage) == 1;
	ParseConfig(kv, m_cfg, messages);
	if (!m_cfg.problem.empty()) {
		pisex::ErrorLog("%s unusual_kills.ini: %s", kLogTag, m_cfg.problem.c_str());
	}
}

// --- database ------------------------------------------------------------------------------------

void LrUnusualKillsPlugin::RunStatement(Statement statement) {
	if (m_db == nullptr || statement.sql.empty()) {
		return;
	}
	m_db->Query(std::move(statement.sql), std::move(statement.params));
}

void LrUnusualKillsPlugin::OnCoreIsReady() {
	// OnCoreIsReadyHook() of the original: connection, table, hooks, then the settings.
	m_table = TableName(g_pLRCore->GetTableName());
	m_db = plgdb::Database::Create(
		levels_ranks::MakeDatabaseConfig(g_pLRCore->GetDatabaseConfig(), "lr.sqlite3"));
	if (m_db == nullptr) {
		pisex::ErrorLog("%s the database configuration is invalid - unusual kills are off", kLogTag);
		return;
	}
	m_db->Query(CreateTableSql(m_table, m_db->GetDriver()), {}); // queued before every later query

	g_pLRCore->HookOnPlayerLoaded([this](int slot, const char* steamid) { OnPlayerLoaded(slot, steamid); });
	g_pLRCore->HookOnResetPlayerStats(
		[this](int slot, const char* steamid) { OnResetPlayerStats(slot, steamid); });
	g_pLRCore->HookOnPlayerKilledPre([this](pisex::IGameEvent* event, int& exp_caused, int, int) {
		OnPlayerKilledPre(event, exp_caused);
	});
	g_pLRCore->HookOnDatabaseCleanup([this](int type) { OnDatabaseCleanup(type); });

	LoadSettings();
}

void LrUnusualKillsPlugin::OnPlayerLoaded(int slot, const char* steamid) {
	if (slot < 0 || slot >= kMaxSlots || m_db == nullptr) {
		return;
	}
	const std::string id = steamid != nullptr ? steamid : "";
	m_steamids[static_cast<std::size_t>(slot)] = id;
	Statement select = LoadPlayerStatement(m_table, id);
	m_db->Query(std::move(select.sql), std::move(select.params), [this, slot, id](const plgdb::Result& result) {
		Counters& counters = m_counters[static_cast<std::size_t>(slot)];
		if (result.Ok() && result.RowCount() > 0) {
			ApplyLoadedCounters(counters, result);
			return;
		}
		// No row: the original created it and zeroed the counters.
		for (int i = 0; i < kMaxTypes; ++i) {
			counters[static_cast<std::size_t>(i)] = 0;
		}
		RunStatement(CreateRowStatement(m_table, id));
	});
}

void LrUnusualKillsPlugin::ResetSlot(int slot) {
	for (int i = 0; i < kMaxTypes; ++i) {
		m_counters[static_cast<std::size_t>(slot)][static_cast<std::size_t>(i)] = 0;
	}
}

void LrUnusualKillsPlugin::OnResetPlayerStats(int slot, const char* steamid) {
	// OnResetLoadedHook() of the original: zero the counters, then the UPDATE.
	if (slot < 0 || slot >= kMaxSlots) {
		return;
	}
	ResetSlot(slot);
	if (m_db == nullptr) {
		return;
	}
	const std::string id = steamid != nullptr ? steamid : "";
	m_steamids[static_cast<std::size_t>(slot)] = id;
	RunStatement(ResetPlayerStatement(m_table, id));
}

void LrUnusualKillsPlugin::OnDatabaseCleanup(int type) {
	// OnDatabaseCleanupHook() of the original: LR_AllData / LR_StatsData.
	if (type != levels_ranks::LR_AllData && type != levels_ranks::LR_StatsData) {
		return;
	}
	if (m_db != nullptr) {
		m_db->Query(TruncateSql(m_table, m_db->GetDriver()), {});
	}
	for (int slot = 0; slot < kMaxSlots; ++slot) {
		// "for every connected player with a pawn and a steam id": OnResetLoadedHook of each.
		if (!PlayerUsable(slot)) {
			continue;
		}
		OnResetPlayerStats(slot, SteamIdOf(slot).c_str());
	}
}

// --- game -----------------------------------------------------------------------------------------

std::string LrUnusualKillsPlugin::SteamIdOf(int slot) const {
	// engine->GetPlayerNetworkIDString(slot) + ConvertSteamID, with the id cached at OnPlayerLoaded
	// as the fallback.
	const std::string network_id = pisex::g_pPlayers->GetSteamID(slot);
	std::string id = ConvertSteamId(network_id);
	if (id.empty() && slot >= 0 && slot < kMaxSlots) {
		id = m_steamids[static_cast<std::size_t>(slot)];
	}
	return id;
}

bool LrUnusualKillsPlugin::PlayerUsable(int slot) const {
	if (slot < 0 || slot >= kMaxSlots) {
		return false;
	}
	// pAttacker && pAttacker->GetPlayerPawn() && pAttacker->m_steamID() > 0.
	return pisex::GetControllerEntity(slot) != 0 && pisex::GetPawnEntity(slot) != 0 &&
		   pisex::g_pPlayers->GetSteamID64(slot) > 0;
}

float LrUnusualKillsPlugin::AttackerSpeed(int slot) const {
	// pAttacker->GetPlayerPawn()->m_vecAbsVelocity().Length()
	if (__s2sdk_GetClientAbsVelocity == nullptr) {
		return 0.0f;
	}
	try {
		const plg::vec3 velocity = s2sdk::GetClientAbsVelocity(slot);
		return std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z);
	} catch (...) {
		return 0.0f;
	}
}

int LrUnusualKillsPlugin::ActiveClip(int slot) {
	// pWeaponServices->m_hActiveWeapon() -> m_iClip1 (the weapon of the kill is the active one).
	if (__s2sdk_GetClientActiveWeapon == nullptr || __s2sdk_GetEntData == nullptr) {
		return -1;
	}
	try {
		const int handle = s2sdk::GetClientActiveWeapon(slot);
		if (!IsValidHandle(handle)) {
			return -1; // no active weapon (or INVALID_EHANDLE_INDEX)
		}
		// The field's schema offset is resolved once (pisexcompat caches successful lookups) and the
		// value is read through it: m_iClip1 is the int32 of CBasePlayerWeapon (CCSWeaponBase).
		// pisex::GetEntInt would build two plg::strings and hit the schema table on every kill; the
		// offset read is the pattern the pisexcompat header prescribes for hot paths.
		if (m_clip1_offset < 0) {
			m_clip1_offset = pisex::SchemaOffset("CBasePlayerWeapon", "m_iClip1");
			if (m_clip1_offset < 0) {
				return -1; // not resolvable (a game update renamed it); retried on the next kill
			}
		}
		return static_cast<int>(s2sdk::GetEntData(handle, m_clip1_offset, static_cast<int>(sizeof(int32_t))));
	} catch (...) {
		return -1;
	}
}

void LrUnusualKillsPlugin::OnRoundStart() {
	// OnRoundStart() of the original.
	m_op_kill = false;
}

void LrUnusualKillsPlugin::OnPlayerKilledPre(pisex::IGameEvent* event, int& exp_caused) {
	// OnPlayerKilledPreHook() of the original.
	if (event == nullptr || m_db == nullptr || !g_pLRCore->CheckCountPlayers()) {
		return;
	}
	const char* weapon_text = event->GetString("weapon");
	const std::string weapon = weapon_text != nullptr ? weapon_text : "";
	if (IsWeaponProhibited(m_cfg, weapon) || IsExcludedWeapon(weapon)) {
		return;
	}
	const int attacker = event->GetInt("attacker");
	if (attacker < 0 || attacker >= kMaxSlots) {
		return;
	}
	if (!PlayerUsable(attacker)) {
		return;
	}

	KillContext ctx;
	ctx.weapon = weapon;
	ctx.penetrated = event->GetInt("penetrated") != 0;
	ctx.noscope = event->GetInt("noscope") != 0;
	ctx.attacker_in_air = event->GetInt("attackerinair") != 0;
	ctx.attacker_blind = event->GetInt("attackerblind") != 0;
	ctx.through_smoke = event->GetInt("thrusmoke") != 0;
	ctx.distance = event->GetFloat("distance");
	ctx.speed = AttackerSpeed(attacker);
	ctx.clip = ActiveClip(attacker);

	int flags = EvaluateFlags(m_cfg, ctx);
	if (!m_op_kill) {
		flags = AddOpenFrag(flags);
		m_op_kill = true;
	}
	if (flags == 0) {
		return; // if(iUKFlags) of the original
	}

	Counters& counters = m_counters[static_cast<std::size_t>(attacker)];
	const std::vector<int> types = FlaggedTypes(flags);
	std::vector<std::pair<int, int>> columns;
	columns.reserve(types.size());
	for (const int type : types) {
		const std::size_t index = static_cast<std::size_t>(type);
		// `%s` = %d with ++g_iUK[iAttacker][iType].
		counters[index]++;
		columns.emplace_back(type, counters[index]);

		if (m_cfg.exp_mode == 1) {
			// if(g_bMessages && g_iExp[iType] && g_pLRCore->ChangeClientValue(...))
			if (m_cfg.messages && m_cfg.exp[index] != 0 &&
				g_pLRCore->ChangeClientValue(attacker, m_cfg.exp[index])) {
				char buffer[64];
				std::snprintf(buffer, sizeof(buffer), m_cfg.exp[index] > 0 ? "+%d" : "%d",
							  m_cfg.exp[index]);
				const auto phrase = m_phrases.find(kTypeNames[index]);
				g_pLRCore->PrintToChat(
					attacker, phrase != m_phrases.end() ? phrase->second.c_str() : "",
					g_pLRCore->GetClientInfo(attacker, levels_ranks::ST_EXP), buffer);
			}
		} else {
			// Exp_Mode 0 and 2 both land here; with mode 0 every g_iExp[] is 0, so this adds
			// nothing (the loader zeroed them), exactly like the original.
			exp_caused += m_cfg.exp[index];
		}
	}

	const std::string id = SteamIdOf(attacker);
	if (id.empty()) {
		pisex::ErrorLog("%s no steam id for slot %d - the unusual kill was not saved", kLogTag, attacker);
		return;
	}
	RunStatement(UpdateCountersStatement(m_table, id, columns));
}

// --- lifecycle ------------------------------------------------------------------------------------

plg::PluginResult LrUnusualKillsPlugin::OnPluginStart() {
	try {
		pisex::Init("levels_ranks");
		LoadPhrases();

		// g_pUtils->HookEvent("round_start", OnRoundStart).
		pisex::HookEvent("round_start", [](const char*, pisex::IGameEvent*, bool) {
			g_plugin.OnRoundStart();
		});

		// AllPluginsLoaded() of the original.
		if (g_pLRCore->CoreIsLoaded()) {
			OnCoreIsReady();
		} else {
			g_pLRCore->HookOnCoreIsReady([this] { OnCoreIsReady(); });
		}
		return plg::PluginResult{};
	} catch (const std::exception& ex) {
		pisex::ErrorLog("%s OnPluginStart failed: %s", kLogTag, ex.what());
	} catch (...) {
		pisex::ErrorLog("%s OnPluginStart failed with an unknown exception", kLogTag);
	}
	return plg::PluginResult{}; // never Failed: that stops every plugify plugin
}

plg::PluginResult LrUnusualKillsPlugin::OnPluginUpdate(std::chrono::milliseconds /*dt*/) {
	try {
		pisex::Poll();
		if (m_db != nullptr) {
			m_db->Poll();
		}
	} catch (const std::exception& ex) {
		pisex::ErrorLog("%s OnPluginUpdate failed: %s", kLogTag, ex.what());
	} catch (...) {
		pisex::ErrorLog("%s OnPluginUpdate failed with an unknown exception", kLogTag);
	}
	return plg::PluginResult{};
}

plg::PluginResult LrUnusualKillsPlugin::OnPluginEnd() {
	try {
		pisex::UnhookEvent("round_start");
		// The Unhook* are no-ops when levels_ranks is not loaded (lr_api.hpp Unregister checks the
		// null binding slot); the Available() guard only keeps that intent visible here.
		if (g_pLRCore->Available()) {
			g_pLRCore->UnhookOnPlayerLoaded();
			g_pLRCore->UnhookOnResetPlayerStats();
			g_pLRCore->UnhookOnPlayerKilledPre();
			g_pLRCore->UnhookOnDatabaseCleanup();
			g_pLRCore->UnhookOnCoreIsReady();
		}
		pisex::Shutdown();
		m_db.reset();
	} catch (...) {
		// nothing left to do at unload
	}
	return plg::PluginResult{};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_plugin)
