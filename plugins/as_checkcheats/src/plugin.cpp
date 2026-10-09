// as_checkcheats — Plugify port of the Pisex Metamod:Source module "CheckCheats" (as_checkcheats).
//
// A "cheat check" workflow: an admin opens the check menu (!check / !cheats or the admin menu
// item), picks a player, the suspect submits a social-network contact with !contact, the admin
// starts the check and ends it with a verdict from checkcheats.ini (optionally banning the
// suspect). Commands, menu items, phrases, config keys and the checkcheats_stats table are kept
// from the original; Metamod/SourceHook/SchemaEntity/HH are replaced by s2sdk + pisexcompat and
// the module's own MySQL connection by plgdb.
//
// Deliberate differences to the Metamod original (see also the port report):
//   * The CheckTransmit hook that hid the overlay particles from other players is not ported: the
//     original's particle code is commented out, g_hOverlays was never populated and the hook was
//     a pure no-op. The `overlay` config key is still read for compatibility.
//   * The config is parsed into typed values with the shipped defaults instead of raw const char*
//     globals, so a missing/partial checkcheats.ini logs and falls back to sane defaults (the
//     original left the globals uninitialised, and RegCommand then dereferenced NULL).
//   * checkcheats_stats is written through plgdb with prepared statements instead of
//     GetMySQLConnection()->Escape() + string interpolation; table name, columns and stored
//     values (steam ids as decimal text in the VARCHAR columns) are identical.
//   * The player_disconnect handler reads the player slot through the s2sdk accessor instead of
//     the raw "userid" int, and every array access is bounds checked (the original indexed
//     g_iTimer[-1] when no target was set).
//   * The one-second timer no longer drives the overlay, only the timers and the auto-ban.

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>

#include <pisex/pisex.hpp>

#include <admin_api.hpp>
#include <plgdb/db.hpp>

#include <s2sdk/s2sdk.hpp>

#include <array>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The Pisex spelling: g_pUtils / g_pMenus / g_pPlayers and the free helpers, unqualified.
using namespace pisex;

namespace {

constexpr const char* kLogTag = "CheckCheats";
constexpr int kSlots = 64;

// A constexpr-filled std::array (the original used plain C arrays with the same contents).
template <typename T, std::size_t N>
constexpr std::array<T, N> Filled(T value) {
	std::array<T, N> out{};
	out.fill(value);
	return out;
}

// ---------------------------------------------------------------------------------------------
// Config (checkcheats.ini) — the same keys with the same semantics as the original.
// ---------------------------------------------------------------------------------------------
struct Reasons {
	std::string sReason;
	std::string sReason2;
	int iTime = 0;
	int iMin = -1;
	int iMax = -1;
	bool bShow = false;
};

struct Socials {
	std::string sName;
	int iMin = 0;
	int iMax = 0;
	std::string sExample;
};

struct TimerConfig {
	int iTime = 0;
	int iMin = 0;
	int iMax = 0;
	bool bAutoBan = false;
	int iBanReason = 0;
};

struct LeaveConfig {
	bool bAutoBan = false;
	int iBanReason = 0;
	bool bLeaveMenu = false;
	int iMin = 0;
	int iMax = 0;
};

std::map<int, Reasons> g_mReasons;
std::map<std::string, Socials> g_mSocials;
TimerConfig g_Timer;
LeaveConfig g_Leave;

std::string g_szSound;
std::string g_szOverlay; // read for compatibility; the overlay feature is disabled (see header)
bool g_bAutoMove = false;

int g_iStart[kSlots] = {};

[[maybe_unused]] int g_iImmunityType = 0;
std::string g_szImmunityFlag = "@admin/uncheck";
[[maybe_unused]] bool g_bAdminMenuType = false;
std::string g_szAdminMenuCategory = "players";
std::string g_szAdminMenuFlag = "@admin/checkcheats";
std::string g_szContactCommand = "!contact";
std::vector<std::string> g_vecCommands{"!check", "!cheats"};

bool g_bDB = false;
int g_iServerID = 0;

// ---------------------------------------------------------------------------------------------
// Per-slot user data (the original's globals; strings replace the leaking const char* slots).
// ---------------------------------------------------------------------------------------------
std::array<int, kSlots> g_iTimer = Filled<int, kSlots>(0);
std::array<std::string, kSlots> g_szSocial;
std::array<int, kSlots> g_iTarget = Filled<int, kSlots>(-1);
std::array<int, kSlots> g_iStage = Filled<int, kSlots>(-1);
std::array<std::string, kSlots> g_szContact;
std::array<bool, kSlots> g_bBlockTeamChange = Filled<bool, kSlots>(false);
std::array<bool, kSlots> g_bAdmin = Filled<bool, kSlots>(false);

pisex::CTimer* g_pTimer = nullptr;

// ---------------------------------------------------------------------------------------------
// Interface handles. g_pAdmin is the admin_system consumer API (IAdminApi replacement).
// ---------------------------------------------------------------------------------------------
admin_system::AdminApi* g_pAdmin = nullptr;
std::unique_ptr<plgdb::Database> g_db;
bool g_bCoreLoaded = false;

bool ValidSlot(int iSlot) {
	return iSlot >= 0 && iSlot < kSlots;
}

// Tagged error log (pisex PrintToConsole/ErrorLog style). The format is checked by the compiler.
void LogError(const char* format, ...) __attribute__((format(printf, 1, 2)));
void LogError(const char* format, ...) {
	std::array<char, 2048> buffer{};
	va_list args;
	va_start(args, format);
	std::vsnprintf(buffer.data(), buffer.size(), format, args);
	va_end(args);
	pisex::ErrorLog("[%s] %s", kLogTag, buffer.data());
}

void LogInfo(const char* format, ...) __attribute__((format(printf, 1, 2)));
void LogInfo(const char* format, ...) {
	std::array<char, 2048> buffer{};
	va_list args;
	va_start(args, format);
	std::vsnprintf(buffer.data(), buffer.size(), format, args);
	va_end(args);
	if (plg::Log) {
		plg::Log(std::string_view(buffer.data()), plg::Severity::Info, plg::source_location::current());
	}
}

const char* T(const char* key) {
	return g_pAdmin ? g_pAdmin->GetTranslation(key) : key;
}

// g_mReasons[reason] of the original (a default entry when the reason is unknown).
const Reasons& Reason(int iReason) {
	const auto it = g_mReasons.find(iReason);
	if (it != g_mReasons.end()) {
		return it->second;
	}
	static const Reasons kEmpty;
	return kEmpty;
}

// g_mSocials[name] of the original (a default entry when the social is unknown).
const Socials& Social(const std::string& sSocial) {
	const auto it = g_mSocials.find(sSocial);
	if (it != g_mSocials.end()) {
		return it->second;
	}
	static const Socials kEmpty;
	return kEmpty;
}

// ---------------------------------------------------------------------------------------------
// checkcheats_stats (the original wrote it through GetMySQLConnection()).
// ---------------------------------------------------------------------------------------------
void InsertCheckStats(int iSuspect, int iAdmin, int iStartTime, const std::string& sVerdict,
					  const std::string& sContact) {
	if (!g_bDB || !g_db) {
		return;
	}
	const std::string sSuspectSteamID = std::to_string(pisex::GetSteamID64(iSuspect));
	const std::string sSuspectName = pisex::GetPlayerName(iSuspect);
	const std::string sAdminSteamID = std::to_string(pisex::GetSteamID64(iAdmin));
	const std::string sAdminName = pisex::GetPlayerName(iAdmin);

	g_db->Query(
		"INSERT INTO `checkcheats_stats` (`server_id`, `player_steamid`, `player_name`, `admin_steamid`, "
		"`admin_name`, `datestart`, `date_end`, `verdict`, `suspect_discord`) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
		plgdb::Params{static_cast<int64_t>(g_iServerID), sSuspectSteamID, sSuspectName, sAdminSteamID, sAdminName,
					  static_cast<int64_t>(iStartTime), static_cast<int64_t>(std::time(nullptr)), sVerdict, sContact},
		[](const plgdb::Result& result) {
			if (!result.Ok()) {
				LogError("insert into checkcheats_stats failed: %s", result.Error().c_str());
			}
		});
}

void CreateStatsTable() {
	if (!g_db) {
		return;
	}
	// Same columns as the original's CREATE TABLE; only the AUTO_INCREMENT spelling differs per
	// driver (plgdb supports MySQL and SQLite).
	static constexpr const char* kCreateMySQL =
		"CREATE TABLE IF NOT EXISTS `checkcheats_stats` ("
		"`id` INT PRIMARY KEY AUTO_INCREMENT,"
		"`server_id` INT DEFAULT NULL,"
		"`player_steamid` VARCHAR(32) CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci NULL DEFAULT NULL,"
		"`player_name` VARCHAR(64) CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci NULL DEFAULT NULL,"
		"`admin_steamid` VARCHAR(32) CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci NULL DEFAULT NULL,"
		"`admin_name` VARCHAR(64) CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci NULL DEFAULT NULL,"
		"`datestart` INT UNSIGNED NULL DEFAULT NULL,"
		"`date_end` INT UNSIGNED NULL DEFAULT NULL,"
		"`verdict` VARCHAR(128) CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci NULL DEFAULT NULL,"
		"`suspect_discord` VARCHAR(32) CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci NULL DEFAULT NULL"
		") ENGINE = InnoDB CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;";
	static constexpr const char* kCreateSQLite =
		"CREATE TABLE IF NOT EXISTS `checkcheats_stats` ("
		"`id` INTEGER PRIMARY KEY AUTOINCREMENT,"
		"`server_id` INTEGER DEFAULT NULL,"
		"`player_steamid` TEXT NULL DEFAULT NULL,"
		"`player_name` TEXT NULL DEFAULT NULL,"
		"`admin_steamid` TEXT NULL DEFAULT NULL,"
		"`admin_name` TEXT NULL DEFAULT NULL,"
		"`datestart` INTEGER NULL DEFAULT NULL,"
		"`date_end` INTEGER NULL DEFAULT NULL,"
		"`verdict` TEXT NULL DEFAULT NULL,"
		"`suspect_discord` TEXT NULL DEFAULT NULL"
		");";
	const bool bMySQL = g_db->GetDriver() == plgdb::Driver::MySQL;
	g_db->Query(bMySQL ? kCreateMySQL : kCreateSQLite, {}, [](const plgdb::Result& result) {
		if (!result.Ok()) {
			LogError("CREATE TABLE checkcheats_stats failed: %s", result.Error().c_str());
		}
	});
}

// Opens the module's own plgdb connection with the settings the admin core exports (the original
// used the core's IMySQLConnection; a Plugify plugin cannot share another plugin's Database).
void OpenDatabase() {
	if (g_db || !g_bDB || !g_pAdmin) {
		return;
	}
	const admin_system::DatabaseConfig config = g_pAdmin->GetDatabaseConfig();
	std::string sqlitePath;
	if (config.IsSqlite()) {
		// The core hands the SQLite file path in the host slot (admin_system/API.md).
		sqlitePath = config.host;
	}
	g_db = plgdb::Database::Create(admin_system::MakeDatabaseConfig(config, std::move(sqlitePath)));
	if (!g_db) {
		LogError("could not open the database (driver '%s')", config.driver.c_str());
		return;
	}
	CreateStatsTable();
	LogInfo("%s: database connection opened (driver %s)", kLogTag, config.driver.c_str());
}

// ---------------------------------------------------------------------------------------------
// User data helpers (verbatim ports of the original's functions).
// ---------------------------------------------------------------------------------------------
void CheckMenu(int iSlot);

void SetStage(int iSlot, int iStage) {
	if (!ValidSlot(iSlot)) {
		return;
	}
	g_iStage[iSlot] = iStage;
	const int iTarget = g_iTarget[iSlot];
	if (ValidSlot(iTarget)) {
		g_iStage[iTarget] = iStage;
	}

	if (iStage == g_Timer.iMin) {
		g_iTimer[iSlot] = g_Timer.iTime;
		if (ValidSlot(iTarget)) {
			g_iTimer[iTarget] = g_Timer.iTime;
		}
	} else if (iStage == g_Timer.iMax) {
		g_iTimer[iSlot] = 0;
		if (ValidSlot(iTarget)) {
			g_iTimer[iTarget] = 0;
		}
	}
	if (iStage != -1 && ValidSlot(iTarget)) {
		CheckMenu(g_bAdmin[iSlot] ? iSlot : iTarget);
	}
	if (iStage == 1) {
		const char* szExample = Social(g_szSocial[iSlot]).sExample.c_str();
		if (ValidSlot(iTarget)) {
			pisex::PrintToChat(iTarget, T("CC_Input_Contact"), g_szContactCommand.c_str(), szExample);
		}
		pisex::PrintToChat(iSlot, T("CC_Input_Contact"), g_szContactCommand.c_str(), szExample);
	}
}

void SetSocial(int iSlot, const std::string& szSocial) {
	if (!ValidSlot(iSlot)) {
		return;
	}
	pisex::PrintToChat(iSlot, T("CC_Select_Social"), szSocial.c_str());
	const int iTarget = g_iTarget[iSlot];
	if (ValidSlot(iTarget)) {
		pisex::PrintToChat(iTarget, T("CC_Select_Social"), szSocial.c_str());
		g_szSocial[iTarget] = szSocial;
		g_szContact[iTarget].clear();
	}
	g_szSocial[iSlot] = szSocial;
	g_szContact[iSlot].clear();
	SetStage(iSlot, 1);
}

void ResetUserData(int iSlot) {
	if (!ValidSlot(iSlot)) {
		return;
	}
	g_iTimer[iSlot] = 0;
	g_iTarget[iSlot] = -1;
	g_szContact[iSlot].clear();
	g_szSocial[iSlot].clear();
	g_bBlockTeamChange[iSlot] = false;
	g_bAdmin[iSlot] = false;
	SetStage(iSlot, -1);
	pisex::ClosePlayerMenu(iSlot);
}

void SetContact(int iSlot, const std::string& szContact) {
	if (!ValidSlot(iSlot)) {
		return;
	}
	g_szContact[iSlot] = szContact;
	const int iTarget = g_iTarget[iSlot];
	if (ValidSlot(iTarget)) {
		g_szContact[iTarget] = szContact;
	}
	SetStage(iSlot, 2);
}

void SetTarget(int iSlot, int iTarget) {
	if (!ValidSlot(iSlot) || !ValidSlot(iTarget)) {
		return;
	}
	g_iTarget[iSlot] = iTarget;
	g_iTarget[iTarget] = iSlot;
	SetStage(iSlot, 0);
}

// ---------------------------------------------------------------------------------------------
// Menus
// ---------------------------------------------------------------------------------------------
std::string JoinReasonParam(int iTarget, int iTime, const std::string& sReason2) {
	return std::to_string(iTarget) + " " + std::to_string(iTime) + " " + sReason2;
}

void EndCheckMenu(int iSlot) {
	if (!ValidSlot(iSlot)) {
		return;
	}
	pisex::Menu hMenu;
	g_pMenus->SetTitleMenu(hMenu, T("CC_Title_EndCheck"));
	for (const auto& it : g_mReasons) {
		if (it.second.bShow) {
			const bool bActive = (it.second.iMin == -1 || it.second.iMin <= g_iStage[iSlot]) &&
								 (it.second.iMax == -1 || g_iStage[iSlot] < it.second.iMax);
			g_pMenus->AddItemMenu(hMenu, std::to_string(it.first).c_str(), it.second.sReason.c_str(),
								  bActive ? pisex::ITEM_DEFAULT : pisex::ITEM_DISABLED);
		}
	}
	g_pMenus->SetBackMenu(hMenu, true);
	g_pMenus->SetExitMenu(hMenu, true);
	g_pMenus->SetCallback(hMenu, [](const char* szBack, const char*, int iItem, int iSlot) {
		if (iItem >= 7 || !ValidSlot(iSlot)) {
			return;
		}
		const int iTarget = g_iTarget[iSlot];
		if (!ValidSlot(iTarget)) {
			return;
		}
		const int iReason = std::atoi(szBack ? szBack : "0");
		const Reasons& sReason = Reason(iReason);
		if (g_mReasons.find(iReason) == g_mReasons.end()) {
			return;
		}
		if ((sReason.iMin <= g_iStage[iSlot] || sReason.iMin == -1) &&
			(sReason.iMax > g_iStage[iSlot] || sReason.iMax == -1)) {
			InsertCheckStats(iTarget, iSlot, g_iStart[iSlot], sReason.sReason, g_szContact[iSlot]);
			const std::string sParam = JoinReasonParam(iTarget, sReason.iTime, sReason.sReason2);
			g_pAdmin->SendAction(iSlot, "checkcheats", sParam.c_str());
			if (sReason.iTime == -1) {
				pisex::PrintToChat(iSlot, T("CC_End_Good_Admin"));
				pisex::PrintToChat(iTarget, T("CC_End_Good"));
			} else {
				g_pAdmin->AddPlayerPunishment(iTarget, admin_system::RT_BAN, sReason.iTime, sReason.sReason2.c_str(),
											  iSlot);
			}
			ResetUserData(iTarget);
			ResetUserData(iSlot);
		}
	});
	g_pMenus->DisplayPlayerMenu(hMenu, iSlot);
}

void CheckMenu(int iSlot) {
	if (!ValidSlot(iSlot)) {
		return;
	}
	pisex::Menu hMenu;
	g_pMenus->SetTitleMenu(hMenu, T("CC_Title_Main"));
	if (g_iStage[iSlot] < 1) {
		g_pMenus->AddItemMenu(hMenu, "0", T("CC_Notify_Socials"));
	}
	if (g_iStage[iSlot] < 2 && g_iStage[iSlot] > 0) {
		g_pMenus->AddItemMenu(hMenu, "1", T("CC_Notify_Contact"));
	}
	g_pMenus->AddItemMenu(hMenu, "3", T("CC_Stage_Start"),
						  (g_iStage[iSlot] < 3 && g_iStage[iSlot] >= 2) ? pisex::ITEM_DEFAULT : pisex::ITEM_DISABLED);
	if (!g_bAutoMove) {
		g_pMenus->AddItemMenu(hMenu, "2", T("CC_ChangeSpecItem"));
		g_pMenus->AddItemMenu(hMenu, "5", T("CC_BlockChangeTeam_Item"));
	}
	g_pMenus->AddItemMenu(hMenu, "4", T("CC_EndCheck_Item"));
	g_pMenus->SetExitMenu(hMenu, true);
	g_pMenus->SetCallback(hMenu, [](const char* szBack, const char*, int iItem, int iSlot) {
		if (iItem >= 7 || !ValidSlot(iSlot)) {
			return;
		}
		const int iTarget = g_iTarget[iSlot];
		switch (std::atoi(szBack ? szBack : "0")) {
		case 0:
			pisex::PrintToChat(iSlot, T("CC_Notify_Socials_Admin"));
			if (ValidSlot(iTarget)) {
				pisex::PrintToChat(iTarget, T("CC_Notify_Socials_User"));
			}
			break;
		case 1:
			pisex::PrintToChat(iSlot, T("CC_Notify_Contact_Admin"));
			if (ValidSlot(iTarget)) {
				pisex::PrintToChat(iTarget, T("CC_Input_Contact"), g_szContactCommand.c_str(),
								   Social(g_szSocial[iSlot]).sExample.c_str());
			}
			break;
		case 2:
			pisex::PrintToChat(iSlot, T("CC_Notify_Spec_Admin"));
			if (ValidSlot(iTarget)) {
				pisex::PrintToChat(iTarget, T("CC_Notify_Spec_User"));
				pisex::CommitSuicide(iTarget, false, true);
				pisex::ChangeTeam(iTarget, 1);
			}
			break;
		case 3:
			SetStage(iSlot, 3);
			pisex::PrintToChat(iSlot, T("CC_Notify_Start_Admin"));
			if (ValidSlot(iTarget)) {
				pisex::PrintToChat(iTarget, T("CC_Notify_Start_User"));
			}
			break;
		case 4:
			EndCheckMenu(iSlot);
			break;
		case 5:
			pisex::PrintToChat(iSlot, T("CC_Notify_Block_Team_Admin"));
			if (ValidSlot(iTarget)) {
				pisex::PrintToChat(iTarget, T("CC_Notify_Block_Team_User"));
			}
			g_bBlockTeamChange[iSlot] = true;
			break;
		default:
			break;
		}
	});
	g_pMenus->DisplayPlayerMenu(hMenu, iSlot);
}

void StartCheck(int iSlot, int iTarget) {
	if (!ValidSlot(iSlot) || !ValidSlot(iTarget)) {
		return;
	}
	g_iStart[iSlot] = static_cast<int>(std::time(nullptr));
	g_iStart[iTarget] = static_cast<int>(std::time(nullptr));
	// ClientCommand is g_pEngineServer->ClientCommand, which does not validate the slot: the
	// suspect is picked from a menu, so he may be gone (or replaced) by the time the admin
	// selects him. pisex::g_pPlayers->IsConnected is pisexcompat's "there is a live client in
	// this slot" guard (map lifecycle included), so only a slot that still carries a client
	// gets the sound.
	if (!g_szSound.empty() && __s2sdk_ClientCommand && pisex::g_pPlayers->IsConnected(iTarget)) {
		try {
			s2sdk::ClientCommand(iTarget, plg::string(std::string("play ") + g_szSound));
		} catch (...) {
			LogError("could not play the check sound for slot %d", iTarget);
		}
	}
	// The original's overlay creation (info_particle_system + FollowEntity) is commented out there
	// as well; g_szOverlay is only read to keep the config key alive.
	if (g_bAutoMove) {
		pisex::PrintToChat(iSlot, T("CC_Notify_SpecAndBlock_Admin"));
		pisex::PrintToChat(iTarget, T("CC_Notify_SpecAndBlock_User"));
		pisex::CommitSuicide(iTarget, false, true);
		pisex::ChangeTeam(iTarget, 1);
		g_bBlockTeamChange[iSlot] = true;
	}
	if (g_mSocials.size() == 1) {
		SetSocial(iSlot, g_mSocials.begin()->first);
	} else {
		pisex::Menu hMenu;
		g_pMenus->SetTitleMenu(hMenu, T("CC_Title_Socials"));
		for (const auto& it : g_mSocials) {
			g_pMenus->AddItemMenu(hMenu, it.first.c_str(), it.second.sName.c_str());
		}
		g_pMenus->SetBackMenu(hMenu, false);
		g_pMenus->SetExitMenu(hMenu, false);
		g_pMenus->SetCallback(hMenu, [](const char* szBack, const char*, int iItem, int iSlot) {
			if (iItem < 7 && ValidSlot(iSlot)) {
				SetSocial(iSlot, szBack ? szBack : "");
				pisex::ClosePlayerMenu(iSlot);
			}
		});
		g_pMenus->DisplayPlayerMenu(hMenu, iTarget);
	}
	if (iSlot != iTarget) {
		CheckMenu(iSlot);
	}
}

void ShowMainMenu(int iSlot, bool bCommand = false) {
	if (!ValidSlot(iSlot)) {
		return;
	}
	if (g_iTarget[iSlot] != -1) {
		CheckMenu(iSlot);
		return;
	}
	pisex::Menu hMenu;
	g_pMenus->SetTitleMenu(hMenu, T("CC_SelectPlayer"));
	bool bFound = false;
	for (int i = 0; i < kSlots; ++i) {
		if (g_iTarget[i] != -1) {
			continue;
		}
		if (!pisex::IsConnected(i)) {
			continue;
		}
		if (pisex::IsFakeClient(i)) {
			continue;
		}
		if (!pisex::IsInGame(i)) {
			continue;
		}
		if (g_pAdmin->HasPermission(i, g_szImmunityFlag.c_str())) {
			continue;
		}
		if (i == iSlot) {
			continue;
		}
		g_pMenus->AddItemMenu(hMenu, std::to_string(i).c_str(), pisex::GetPlayerName(i));
		bFound = true;
	}
	if (!bFound) {
		pisex::PrintToChat(iSlot, T("CC_NoPlayers"));
		return;
	}
	if (!bCommand) {
		g_pMenus->SetBackMenu(hMenu, true);
	}
	g_pMenus->SetExitMenu(hMenu, true);
	g_pMenus->SetCallback(hMenu, [bCommand](const char* szBack, const char*, int iItem, int iSlot) {
		if (!ValidSlot(iSlot)) {
			return;
		}
		if (iItem < 7) {
			const int iTarget = std::atoi(szBack ? szBack : "-1");
			if (!ValidSlot(iTarget)) {
				return;
			}
			ResetUserData(iSlot);
			ResetUserData(iTarget);
			g_bAdmin[iSlot] = true;
			SetTarget(iSlot, iTarget);
			StartCheck(iSlot, iTarget);
		} else if (iItem == 7 && !bCommand) {
			g_pAdmin->ShowAdminLastCategoryMenu(iSlot);
		}
	});
	g_pMenus->DisplayPlayerMenu(hMenu, iSlot);
}

// ---------------------------------------------------------------------------------------------
// Config loading
// ---------------------------------------------------------------------------------------------
std::vector<std::string> Split(const std::string& s, const std::string& delimiter) {
	std::vector<std::string> res;
	std::size_t posStart = 0;
	const std::size_t delimLen = delimiter.length();
	std::size_t posEnd = 0;
	while ((posEnd = s.find(delimiter, posStart)) != std::string::npos) {
		res.push_back(s.substr(posStart, posEnd - posStart));
		posStart = posEnd + delimLen;
	}
	res.push_back(s.substr(posStart));
	return res;
}

void OnCheckCheatsItemSelect(int iSlot, const char*, const char*, const char*) {
	ShowMainMenu(iSlot);
}

// The original's contact handler: the suspect submits their social-network contact.
bool OnContactCommand(int iSlot, const char* szContent) {
	if (!ValidSlot(iSlot) || g_iTarget[iSlot] == -1 || g_szSocial[iSlot].empty() || !g_szContact[iSlot].empty() ||
		g_bAdmin[iSlot]) {
		return true;
	}
	pisex::CommandArgs args;
	args.Tokenize(szContent);
	if (args.ArgC() < 2) {
		pisex::PrintToChat(iSlot, T("Usage_Contact"), args.Arg(0) ? args.Arg(0) : g_szContactCommand.c_str(),
						   Social(g_szSocial[iSlot]).sExample.c_str());
		return true;
	}
	// Everything after the command name, without the surrounding whitespace (the original removed
	// one trailing space that pisexcompat's command content does not carry).
	std::string sContact = szContent ? szContent : "";
	const std::size_t iCommandEnd = sContact.find(' ');
	if (iCommandEnd == std::string::npos) {
		return true;
	}
	sContact.erase(0, iCommandEnd);
	while (!sContact.empty() && sContact.front() == ' ') {
		sContact.erase(0, 1);
	}
	while (!sContact.empty() && sContact.back() == ' ') {
		sContact.pop_back();
	}
	if (sContact.empty()) {
		return true;
	}
	const Socials& sSocial = Social(g_szSocial[iSlot]);
	if (static_cast<int>(sContact.size()) < sSocial.iMin || static_cast<int>(sContact.size()) > sSocial.iMax) {
		pisex::PrintToChat(iSlot, T("CC_Contact_Size"), sSocial.iMin, sSocial.iMax);
		return true;
	}
	SetContact(iSlot, sContact);
	pisex::PrintToChat(iSlot, T("CC_SetContact_User"), sContact.c_str());
	const int iTarget = g_iTarget[iSlot];
	if (ValidSlot(iTarget)) {
		pisex::PrintToChat(iTarget, T("CC_SetContact_Admin"), sContact.c_str());
	}
	return true;
}

bool OnJoinTeamCommand(int iSlot, const char*) {
	if (!ValidSlot(iSlot)) {
		return false;
	}
	const int iTarget = g_iTarget[iSlot];
	if (ValidSlot(iTarget) && (g_bBlockTeamChange[iTarget] || (g_bAutoMove && g_bAdmin[iTarget]))) {
		pisex::PrintToChat(iSlot, T("CC_BlockChangeTeam"));
		return true;
	}
	return false;
}

bool OnCheckCommand(int iSlot, const char*) {
	if (g_pAdmin->HasPermission(iSlot, g_szAdminMenuFlag.c_str())) {
		ShowMainMenu(iSlot, true);
	}
	return true;
}

void LoadConfig() {
	pisex::KeyValues hKv("Config");
	const char* pszPath = "addons/configs/admin_system/checkcheats.ini";
	if (hKv.LoadFromFile(pszPath)) {
		if (pisex::KeyValues* hReasons = hKv.FindKey("end")) {
			FOR_EACH_SUBKEY(hReasons, pValue) {
				const int iIndex = std::atoi(pValue->GetName());
				Reasons sReason;
				sReason.sReason = pValue->GetString("reason");
				sReason.sReason2 = pValue->GetString("reason2");
				sReason.iTime = pValue->GetInt("time");
				sReason.iMin = pValue->GetInt("min");
				sReason.iMax = pValue->GetInt("max");
				sReason.bShow = pValue->GetBool("show");
				g_mReasons[iIndex] = std::move(sReason);
			}
		}
		if (pisex::KeyValues* hSocials = hKv.FindKey("socials")) {
			FOR_EACH_SUBKEY(hSocials, pValue) {
				Socials sSocial;
				sSocial.sName = pValue->GetName();
				sSocial.iMin = pValue->GetInt("min");
				sSocial.iMax = pValue->GetInt("max");
				sSocial.sExample = pValue->GetString("example");
				g_mSocials[sSocial.sName] = std::move(sSocial);
			}
		}
		if (pisex::KeyValues* hTimer = hKv.FindKey("timer")) {
			g_Timer.iTime = hTimer->GetInt("time");
			g_Timer.iMin = hTimer->GetInt("min");
			g_Timer.iMax = hTimer->GetInt("max");
			g_Timer.bAutoBan = hTimer->GetBool("auto_ban_status");
			g_Timer.iBanReason = hTimer->GetInt("auto_ban_reason");
		}
		if (pisex::KeyValues* hAdmin = hKv.FindKey("admin")) {
			g_iImmunityType = hAdmin->GetInt("immunity_type");
			g_szImmunityFlag = hAdmin->GetString("immunity_flag", g_szImmunityFlag.c_str());
			g_szAdminMenuFlag = hAdmin->GetString("check_flag", g_szAdminMenuFlag.c_str());
		}
		if (pisex::KeyValues* hMenu = hKv.FindKey("admin_menu")) {
			g_bAdminMenuType = hMenu->GetBool("type");
			g_szAdminMenuCategory = hMenu->GetString("category", g_szAdminMenuCategory.c_str());
		}
		if (pisex::KeyValues* hLeave = hKv.FindKey("leave")) {
			g_Leave.bAutoBan = hLeave->GetBool("player_ban_status");
			g_Leave.iBanReason = hLeave->GetInt("player_ban_reason");
			g_Leave.bLeaveMenu = hLeave->GetBool("player_menu");
			g_Leave.iMin = hLeave->GetInt("player_ban_min");
			g_Leave.iMax = hLeave->GetInt("player_ban_max");
		}
		g_szOverlay = hKv.GetString("overlay", "");
		g_szSound = hKv.GetString("sound", "");
		g_bAutoMove = hKv.GetBool("auto_move");
		g_bDB = hKv.GetBool("db_use", false);
		g_iServerID = hKv.GetInt("server_id", 0);
		const std::string sCommands = hKv.GetString("commands", "!check|!cheats");
		std::vector<std::string> vecCommands = Split(sCommands, "|");
		if (!vecCommands.empty()) {
			g_vecCommands = std::move(vecCommands);
		}
		g_szContactCommand = hKv.GetString("contact", g_szContactCommand.c_str());
	} else {
		LogError("Failed to load %s - using the built-in defaults", pszPath);
	}

	// Commands (the original registered both inside LoadConfig).
	g_pUtils->RegCommand({}, g_vecCommands, OnCheckCommand);
	if (!g_szContactCommand.empty()) {
		g_pUtils->RegCommand({}, {g_szContactCommand}, OnContactCommand);
	}

	// Admin menu: the original either creates its own category or attaches to the "players" one.
	if (g_bAdminMenuType) {
		g_pAdmin->RegisterCategory(g_szAdminMenuCategory.c_str(), "Category_CheckCheats", nullptr);
	} else {
		g_pAdmin->RegisterCategory("players", "Category_Players", nullptr);
	}
	g_pAdmin->RegisterItem("checkcheats", "Item_CheckCheats", g_szAdminMenuCategory.c_str(),
						   g_szAdminMenuFlag.c_str(), nullptr, OnCheckCheatsItemSelect);
}

// ---------------------------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------------------------
void OnPlayerDisconnect(const char*, pisex::IGameEvent* pEvent, bool) {
	if (!pEvent) {
		return;
	}
	const int iSlot = pEvent->GetPlayerSlot("userid");
	const int iReason = pEvent->GetInt("reason");
	if (!ValidSlot(iSlot)) {
		return;
	}
	const int iTarget = g_iTarget[iSlot];
	if (iTarget == -1) {
		return;
	}
	const bool bAdmin = g_bAdmin[iSlot];

	if (iReason == 0 || iReason == 1 || iReason == 41 || iReason == 54) {
		ResetUserData(iSlot);
		ResetUserData(iTarget);
		return;
	}

	// Capture the suspect's identity and the verdict data before the state is reset.
	const int iSuspect = bAdmin ? iTarget : iSlot;
	const int iAdmin = bAdmin ? iSlot : iTarget;
	if (!ValidSlot(iSuspect) || !ValidSlot(iAdmin)) {
		return;
	}
	const std::string sName = pisex::GetPlayerName(iSuspect);
	const uint64_t iSteamID = pisex::GetSteamID64(iSuspect);
	const int iStage = g_iStage[iSlot];
	const std::string sContact = g_szContact[iSlot];
	ResetUserData(iSlot);
	ResetUserData(iTarget);

	const Reasons& sReason = Reason(g_Leave.iBanReason);
	if (g_Leave.bAutoBan && (g_Leave.iMin <= iStage || g_Leave.iMin == -1) &&
		(g_Leave.iMax > iStage || g_Leave.iMax == -1)) {
		g_pAdmin->AddPlayerPunishment(iSuspect, admin_system::RT_BAN, sReason.iTime, sReason.sReason2.c_str(), iAdmin);
		if (g_bDB && g_db) {
			// Steam id/name of the target and of the admin (the original read them again here).
			InsertCheckStats(iSuspect, iAdmin, g_iStart[iSlot], sReason.sReason, sContact);
		}
		const std::string sParam = JoinReasonParam(iSuspect, sReason.iTime, sReason.sReason2);
		g_pAdmin->SendAction(iAdmin, "checkcheats", sParam.c_str());
	} else if (g_Leave.bLeaveMenu && !bAdmin) {
		const uint64_t iSteamIDCopy = iSteamID;
		const std::string sNameCopy = sName;
		const std::string sContactCopy = sContact;
		pisex::Menu hMenu;
		g_pMenus->SetTitleMenu(hMenu, T("CC_Title_Leave"));
		for (const auto& it : g_mReasons) {
			if (it.second.bShow) {
				const bool bActive = (it.second.iMin == -1 || it.second.iMin <= iStage) &&
									 (it.second.iMax == -1 || iStage < it.second.iMax);
				g_pMenus->AddItemMenu(hMenu, std::to_string(it.first).c_str(), it.second.sReason.c_str(),
									  bActive ? pisex::ITEM_DEFAULT : pisex::ITEM_DISABLED);
			}
		}
		g_pMenus->SetBackMenu(hMenu, false);
		g_pMenus->SetExitMenu(hMenu, true);
		// The menu is the admin's (bAdmin ? iSlot : iTarget); the original read g_iStart of the
		// menu owner when the verdict was picked.
		g_pMenus->SetCallback(hMenu, [iSteamIDCopy, sNameCopy, sContactCopy, iStart = g_iStart[iAdmin]](
										 const char* szBack, const char*, int iItem, int iSlot) {
			if (iItem >= 7 || !ValidSlot(iSlot)) {
				return;
			}
			const int iReasonIndex = std::atoi(szBack ? szBack : "0");
			if (g_mReasons.find(iReasonIndex) == g_mReasons.end()) {
				return;
			}
			const Reasons sLeaveReason = g_mReasons[iReasonIndex];
			const std::string sSteamID = std::to_string(iSteamIDCopy);
			const std::string sParam = sSteamID + " " + sNameCopy + " " + std::to_string(sLeaveReason.iTime) + " " +
									   sLeaveReason.sReason;
			g_pAdmin->SendAction(iSlot, "checkcheats_offline", sParam.c_str());

			if (g_bDB && g_db) {
				const std::string sAdminSteamID = std::to_string(pisex::GetSteamID64(iSlot));
				const std::string sAdminName = pisex::GetPlayerName(iSlot);
				g_db->Query(
					"INSERT INTO `checkcheats_stats` (`server_id`, `player_steamid`, `player_name`, `admin_steamid`, "
					"`admin_name`, `datestart`, `date_end`, `verdict`, `suspect_discord`) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
					plgdb::Params{static_cast<int64_t>(g_iServerID), sSteamID, sNameCopy, sAdminSteamID, sAdminName,
								  static_cast<int64_t>(iStart), static_cast<int64_t>(std::time(nullptr)),
								  sLeaveReason.sReason, sContactCopy},
					[](const plgdb::Result& result) {
						if (!result.Ok()) {
							LogError("insert into checkcheats_stats failed: %s", result.Error().c_str());
						}
					});
			}
			if (sLeaveReason.iTime != -1) {
				g_pAdmin->AddOfflinePlayerPunishment(sSteamID.c_str(), sNameCopy.c_str(), admin_system::RT_BAN,
													 sLeaveReason.iTime, sLeaveReason.sReason2.c_str(), iSlot);
			}
			pisex::ClosePlayerMenu(iSlot);
		});
		g_pMenus->DisplayPlayerMenu(hMenu, iAdmin);
	}
}

void OnAdminCoreLoaded() {
	if (g_bCoreLoaded) {
		return;
	}
	g_bCoreLoaded = true;
	OpenDatabase();
}

// ---------------------------------------------------------------------------------------------
// The original's one-second timer: expiry messages, the countdown on the centre text and the
// automatic ban.
// ---------------------------------------------------------------------------------------------
float OnTimerTick() {
	for (int i = 0; i < kSlots; ++i) {
		if (g_iTimer[i] <= 0) {
			continue;
		}
		g_iTimer[i]--;
		if (g_iTimer[i] == 0) {
			const int iTarget = g_iTarget[i];
			pisex::PrintToChat(i, T("CC_Timer_Expired"));
			if (ValidSlot(iTarget)) {
				pisex::PrintToChat(iTarget, T("CC_Timer_Expired"));
				g_iTimer[iTarget] = 0;
			}
			if (!g_Timer.bAutoBan || !ValidSlot(iTarget)) {
				continue;
			}
			const Reasons& sReason = Reason(g_Timer.iBanReason);
			const int iSuspect = g_bAdmin[i] ? iTarget : i;
			const int iAdmin = g_bAdmin[i] ? i : iTarget;
			if (g_bDB && g_db) {
				InsertCheckStats(iSuspect, iAdmin, g_iStart[i], sReason.sReason, g_szContact[i]);
			}
			g_pAdmin->AddPlayerPunishment(iSuspect, admin_system::RT_BAN, sReason.iTime, sReason.sReason2.c_str(),
										  iAdmin);
			const std::string sParam = JoinReasonParam(iSuspect, sReason.iTime, sReason.sReason2);
			g_pAdmin->SendAction(iAdmin, "checkcheats", sParam.c_str());
		} else if (g_Timer.iMin <= g_iStage[i] && g_iStage[i] < g_Timer.iMax) {
			pisex::PrintToCenter(i, T("CC_Timer_Seconds"), g_iTimer[i]);
		}
	}
	return 1.0f;
}

// ---------------------------------------------------------------------------------------------
// Plugin glue
// ---------------------------------------------------------------------------------------------
class CheckCheatsPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override {
		try {
			// Never Failed: a plugin that fails OnPluginStart stops every Plugify plugin.
			pisex::Init("admin_system"); // <configs>/admin_system (+ translations)
			g_pAdmin = &admin_system::AdminApi::Get();

			LoadConfig();

			g_pUtils->HookEvent("player_disconnect", OnPlayerDisconnect);
			g_pUtils->RegCommand({"jointeam"}, {}, OnJoinTeamCommand);

			g_pAdmin->OnCoreLoaded(OnAdminCoreLoaded);
			if (g_pAdmin->IsCoreLoaded()) {
				OnAdminCoreLoaded(); // late load: the event will not fire twice
			}

			g_pTimer = g_pUtils->CreateTimer(1.0f, OnTimerTick);
			if (!g_pTimer) {
				LogError("could not create the one-second timer");
			}
			LogInfo("%s: started", kLogTag);
			return plg::PluginResult{};
		} catch (const std::exception& ex) {
			LogError("OnPluginStart failed: %s", ex.what());
			return plg::PluginResult{};
		} catch (...) {
			LogError("OnPluginStart failed with an unknown exception");
			return plg::PluginResult{};
		}
	}

	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds) override {
		try {
			pisex::Poll();
			if (g_db) {
				g_db->Poll();
			}
		} catch (const std::exception& ex) {
			LogError("update failed: %s", ex.what());
		} catch (...) {
			LogError("update failed with an unknown exception");
		}
		return plg::PluginResult{};
	}

	plg::PluginResult OnPluginEnd() override {
		try {
			pisex::Shutdown();
			if (g_pAdmin) {
				g_pAdmin->OnCoreLoadedUnregister();
			}
			g_db.reset(); // flushes pending statements and joins the worker
		} catch (...) {
		}
		return plg::PluginResult{};
	}
};

CheckCheatsPlugin g_checkCheatsPlugin;

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_checkCheatsPlugin)
