#include <WinSock2.h>
#include <Windows.h>
#include <Xinput.h>

#include <string>
#include <vector>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include <fstream>
#include <ctime>
#include <cstdlib>
#include <cstdint>

#include "Accessibility.h"

using namespace moddingApi;

// ---------------------------------------------------------------------------
// Tolk (screen reader bridge) - loaded dynamically so we need no import lib.
// ---------------------------------------------------------------------------
typedef void(*Tolk_Load_t)();
typedef void(*Tolk_Unload_t)();
typedef bool(*Tolk_IsLoaded_t)();
typedef bool(*Tolk_HasSpeech_t)();
typedef bool(*Tolk_Output_t)(const wchar_t*, bool);
typedef bool(*Tolk_Silence_t)();
typedef const wchar_t*(*Tolk_DetectScreenReader_t)();

static HMODULE g_tolk = nullptr;
static Tolk_Load_t p_TolkLoad = nullptr;
static Tolk_Unload_t p_TolkUnload = nullptr;
static Tolk_IsLoaded_t p_TolkIsLoaded = nullptr;
static Tolk_HasSpeech_t p_TolkHasSpeech = nullptr;
static Tolk_Output_t p_TolkOutput = nullptr;
static Tolk_Silence_t p_TolkSilence = nullptr;
static Tolk_DetectScreenReader_t p_TolkDetect = nullptr;

// ---------------------------------------------------------------------------
// XInput (controller) - also loaded dynamically.
// ---------------------------------------------------------------------------
typedef DWORD(WINAPI* XInputGetState_t)(DWORD, XINPUT_STATE*);
static XInputGetState_t p_GetState = nullptr;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
bool Accessibility::DebugLogging = false;  // off by default; toggle with Back+Y (stability)
bool Accessibility::_available = false;
bool Accessibility::_initialized = false;

static std::mutex g_mtx;                          // guards _seen / _lastSpoken / log
static std::unordered_set<std::string> g_loggedThisNav; // ids logged for current nav
static unsigned long long g_logNavTick = 0;       // nav tick the log set belongs to
static std::unordered_set<std::string> g_loggedPopupEver; // popup ids captured this session
static unsigned g_stackCaptures = 0;                      // char-select stack captures done
static std::unordered_set<std::string> g_stackCapturedIds; // c_jyu ids we've captured a stack for
static std::string g_lastSpoken;                  // last announcement (for repeat)
static std::string g_lastPopup;                   // last popup text spoken (dedupe)
static unsigned long long g_consumedNavTick = 0;  // nav tick we've already announced for
static unsigned long long g_questionNavTick = 0;  // nav tick a question already upgraded
static std::ofstream g_log;

// Navigation gating: only auto-announce text decoded shortly after the player
// moves, so background/idle conversions (footer hints, pre-loaded labels of
// other screens) don't interrupt the focused item. Set in Update(), read in
// OnMessageDecoded().
static std::atomic<unsigned long long> g_lastNavTick{ 0 };
static const unsigned long long NAV_WINDOW_MS = 500;

// Character-select context. The roster grid re-decodes the hovered character's
// signature jutsu (c_jyu_*/c_ult_*/c_cha_*) on every move - but those ids ALSO
// flood during real battles, so we only treat them as announceable while this
// timestamp is fresh. Set when MSG_Random / characterselect_* decode (they
// appear in every char-select bundle but never mid-battle).
static std::atomic<unsigned long long> g_charSelectUntil{ 0 };
// Now self-refreshed on ANY char-select info id (c_jyu/c_ult/c_cha/...), not just
// the sparse markers - the old marker-only refresh let the window expire mid-
// navigation (markers can be >20 s apart) which made char-select go silent. Kept
// short so it expires during a battle load; any battle leak (names on jutsu use)
// is bounded to this window. NOTE: because the refresh runs BEFORE the announce
// block, the first decode after an expiry re-opens the context AND announces, so
// navigation is self-healing regardless of window length.
static const unsigned long long CHARSELECT_WINDOW_MS = 6000;

// Battle gate. Char-select decodes (c_jyu/c_ult/...) ALSO happen during a real
// fight whenever a jutsu is used, which used to leak into speech. "league_*"
// ("Battle") decodes at fight start and character-select markers decode on the
// select screen, so we flip this flag between them: while true, ALL char-select
// handling (context refresh + fighter announce) is suppressed. Reset on return
// to character select.
static std::atomic<bool> g_inBattle{ false };

// --- Adventure decode-once dialog cursor (game-memory read) -------------------
// Yes/No + select dialogs keep the highlighted line in a C++ field, so we can
// report it even though moving the cursor re-translates no text. Chain:
//   g_AdvManager global -> [+0xFCB8] active SelectWindow -> [+0xB4] line index.
// SAFETY (this is why it can't repeat the startup crash):
//  1. GATED on g_gameReady - we do NOT read game memory until the game has
//     decoded its first UI string (proof it is fully initialized), never during
//     startup. This is the key fix vs the previous attempt.
//  2. Every dereference is SEH-guarded -> a wrong/garbage pointer yields
//     "nothing", never a crash.
//  3. The index is the game's real field and Yes/No labels are scoped to the
//     open dialog, so we can never announce a wrong option.
static std::mutex g_dlgMtx;
static std::string g_yesText;                  // localized "Yes" (MSG_Yes)
static std::string g_noText;                   // localized "No"  (MSG_No)
static std::atomic<bool> g_gameReady{ false }; // set on first OnMessageDecoded
static uintptr_t g_exeBase = 0;
static const uintptr_t ADVMGR_RVA = 0x15e76b8; // g_AdvManager (VA 0x1415e76b8)
static const uintptr_t WINDOW_OFF = 0xfcb8;    // advMgr -> active SelectWindow
static const uintptr_t CURSOR_OFF = 0xb4;      // window  -> highlighted line idx
static int  g_advCursorPrev = -2;
static bool g_advWindowWasOpen = false;

// Direction-based fallback for Flash yes/no confirms (battle/menu dialogs), whose
// cursor lives in the SWF and has no readable C++ field. While a yes/no dialog is
// open we watch the d-pad/stick: pressing toward an option speaks it. This is
// POSITIONAL (left option vs right option), not a highlight readout, so it can
// never misreport which option is selected - you press toward what you want and
// hear it. Pure input; no game-memory reads. Assumes the common horizontal layout
// (Yes = left, No = right); flip the two lines in Update() if a test shows it
// reversed. Active only briefly after a yes/no question (g_yesNoDialogUntil).
static std::atomic<unsigned long long> g_yesNoDialogUntil{ 0 };
// Generous: the confirm question can take ~7s to read aloud, so a short window
// would expire before the player reacts. Re-armed on each directional press so it
// stays alive while interacting, and expires harmlessly after they leave.
static const unsigned long long YESNO_WINDOW_MS = 30000;
static bool g_dpadLeftPrev = false;
static bool g_dpadRightPrev = false;

// True (and fills *outIndex) only while an Adventure SelectWindow is open AND the
// game is fully up. Reads are exception-guarded so they can never crash.
static bool ReadAdvDialogCursor(int* outIndex)
{
	if (!g_gameReady.load() || !g_exeBase) return false;  // gate: game must be up
	__try
	{
		uintptr_t advMgr = *(uintptr_t*)(g_exeBase + ADVMGR_RVA);
		if (!advMgr) return false;
		uintptr_t window = *(uintptr_t*)(advMgr + WINDOW_OFF);
		if (!window) return false;
		*outIndex = *(int*)(window + CURSOR_OFF);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Controller edge-detection state (only touched in Update()).
static unsigned long long g_lastPollTick = 0;
static bool g_xPrev = false;
static bool g_yPrev = false;

// Message-id prefixes we speak automatically. Safe (non-battle) menu families
// only; expand from the debug log (Back+Y) as new screens are mapped.
static const char* const ANNOUNCE_PREFIXES[] = {
	"gamemodeselect", // main mode-select bar
	"option",         // settings menu
	"PC_option",      // keyboard settings
	"PC_game",        // quit game etc.
	"PC_network_sys", // online system
	"networkmode",    // online mode menu
	"onlineMenu",     // online submenus
	"freebattle",     // free battle
	"freemenu",       // free battle menu
	"practice",       // practice / training
	"tournament",     // tournament mode
	"survival",       // survival menu (note: Survival_sys is risky-blocked)
	"collect",        // collection (covers collect/collection/collectTop)
	"item_list",      // item list
	"advMainMenu",    // story / adventure main menu
	"advStartMenu",   // story / adventure start menu
	"tutorial_check", // tutorial prompts
	"main_outline",   // story chapter / episode names
	"sub_outline",    // adventure (Boruto's Tale) quest / episode names
	"AdvCmn",         // adventure menu categories (Main Event, Side Quest, ...)
	"adv_act",        // adventure action menu (Save, ...)
	"itemcustomize",  // "Edit Ninja Tools" description
	"eventcheck",     // adventure sub-event menu (Sub-event Details)
	"battlestartmenu",// practice / pause menu (overrides the "battle" risky prefix)
	"practice_menu",  // practice pause menu title
	"characterselect",// character-select headers (e.g. jutsu customization)
	"c_sta",          // stage select - stage names
	"map_icon",       // stage select - location names (Training Field, ...)
	"MSG_",           // confirm/cancel dialogs, yes/no, generic prompts
};

// Character-select info families. These share ids with in-battle decoding, so
// they are only announced while the character-select context is fresh (see
// g_charSelectUntil). They identify the hovered fighter by signature jutsu /
// ultimate / version tag, since the character NAME is a portrait graphic.
static const char* const CHARSELECT_INFO_PREFIXES[] = {
	"c_jyu",     // signature jutsu (e.g. "Onyx Chidori")
	"c_ult",     // ultimate jutsu
	"c_cha",     // version / awakening tag (e.g. "(Great Ninja War)")
	"c_union",   // team combination jutsu
	"c_costume", // costume tag
};

static bool IsCharSelectInfo(const char* id)
{
	for (const char* prefix : CHARSELECT_INFO_PREFIXES)
	{
		size_t plen = std::strlen(prefix);
		if (std::strncmp(id, prefix, plen) == 0) return true;
	}
	return false;
}

// Ids whose text is metadata with a number/value the game draws separately, so
// the string alone is useless ("Estimated Play Time:  minutes", "Completion: %",
// "???"). Never announced, so they don't win the "first per navigation" slot.
static const char* const SUPPRESS_IDS[] = {
	"advMainMenu_026", // Estimated Play Time (blank number) + _singular
	"advMainMenu_001", // Yang Path (column header)
	"advMainMenu_002", // Yin Path
	"advMainMenu_003", // Total Score
	"storybode",       // Story/S-Rank Completion: % (blank number)
	"collect_getinfo", // "???" placeholders
};

static bool IsSuppressed(const char* id)
{
	for (const char* prefix : SUPPRESS_IDS)
	{
		size_t plen = std::strlen(prefix);
		if (std::strncmp(id, prefix, plen) == 0) return true;
	}
	return false;
}

// Footer / button-legend chrome: relevant by prefix (option_/MSG_/PC_option_)
// but never a navigable option - it's the persistent "A: Confirm  B: Back"-style
// hint row. It decodes in every bundle and sometimes FIRST, so if announced it
// steals the "first useful per move" slot from the real focused item and then
// dedupes to silence (this is why the jutsu-select list spoke nothing). Skipped
// entirely. NOTE: MSG_Yes / MSG_No are real dialog options - not listed here.
static const char* const CHROME_IDS[] = {
	"MSG_Confirm", "MSG_Cancel", "MSG_Back", "MSG_Close",
	"MSG_Save", "MSG_Comp", "MSG_Random",
	"option_169",     // "Options" button hint
	"PC_option_000",  // "Keyboard Settings" footer hint (PC)
};

static bool IsChrome(const char* id)
{
	for (const char* full : CHROME_IDS)
		if (std::strcmp(id, full) == 0) return true;
	return false;
}

// Message families seen flooding (often from multiple threads) during pre-battle
// / cutscene transitions. The framework's self-unhooking message hook is fragile
// there, so we do NO extra work for these - not even debug logging.
static const char* const RISKY_PREFIXES[] = {
	"battle", "Battle", "Survival_sys", "t_battlecondition",
	"Battlemission", "episode",
	// NOTE: "eventcheck" was here but it is adventure MENU content
	// (eventcheck_008 "Sub-event Details") - removed so it speaks.
	// NOTE: "battlestartmenu" was here but it is the practice / pause MENU
	// (deliberate, low-frequency) - removed and allowlisted so it speaks. The
	// generic "battle" prefix still matches it, so the allowlist must override
	// risky (see OnMessageDecoded) for it to come through.
};

static bool IsRiskyId(const char* id)
{
	for (const char* prefix : RISKY_PREFIXES)
	{
		size_t plen = std::strlen(prefix);
		if (std::strncmp(id, prefix, plen) == 0) return true;
	}
	return false;
}

// Popup / auto-notification families. Unlike menu items, these appear on their
// own (no controller input), so they must be spoken IGNORING the nav gate.
// Populate from the "(popup?)" lines in the debug log (Back+Y captures popups
// even though they decode outside the nav window). Empty until ids are mapped.
static const std::vector<const char*> POPUP_PREFIXES = {
	"MSG_AutoSaveWarn", // boot autosave notice (appears with no input)
	// add more from the "(popup?)" log lines, e.g. login-bonus ids when captured.
};

static bool IsPopup(const char* id)
{
	for (const char* prefix : POPUP_PREFIXES)
	{
		size_t plen = std::strlen(prefix);
		if (std::strncmp(id, prefix, plen) == 0) return true;
	}
	return false;
}

// The main-menu mode titles are graphics, not text - the game only exposes a
// description per mode. Map those ids to short spoken names; the description is
// then queued as follow-up detail.
struct ModeName { const char* id; const char* name; };
static const ModeName MODE_NAMES[] = {
	{ "gamemodeselect_018",    "Story" },
	{ "gamemodeselect_019",    "Adventure" },
	{ "gamemodeselect_020",    "Collection" },
	{ "gamemodeselect_011",    "Free Battle" },
	{ "gamemodeselect_012_pc", "Online Battle" },
	{ "gamemodeselect_026",    "Boruto Story" },
};

// Produce a short spoken label (and optional queued detail) for a message id.
static void ResolveAnnouncement(const char* id, const std::string& cleaned,
	std::string& label, std::string& detail)
{
	for (const ModeName& m : MODE_NAMES)
	{
		if (std::strcmp(id, m.id) == 0)
		{
			label = m.name;
			detail = cleaned; // the mode description, queued after the name
			return;
		}
	}
	label = cleaned; // unmapped ids (e.g. action labels) speak their own text
	detail.clear();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::string LogPath()
{
	char dir[MAX_PATH];
	DWORD n = GetCurrentDirectoryA(MAX_PATH, dir);
	std::string base = (n > 0 && n < MAX_PATH) ? std::string(dir) : std::string(".");
	return base + "\\storm_access.log";
}

static std::string TimeStamp()
{
	std::time_t t = std::time(nullptr);
	std::tm tmv;
	localtime_s(&tmv, &t);
	char buf[32];
	std::strftime(buf, sizeof(buf), "%H:%M:%S", &tmv);
	return std::string(buf);
}

static std::wstring Widen(const std::string& utf8)
{
	if (utf8.empty()) return std::wstring();
	int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
	if (len <= 0)
	{
		// Fall back to the active code page if the bytes are not valid UTF-8.
		len = MultiByteToWideChar(CP_ACP, 0, utf8.c_str(), -1, nullptr, 0);
		if (len <= 0) return std::wstring();
		std::wstring out(len, L'\0');
		MultiByteToWideChar(CP_ACP, 0, utf8.c_str(), -1, &out[0], len);
		if (!out.empty() && out.back() == L'\0') out.pop_back();
		return out;
	}
	std::wstring out(len, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &out[0], len);
	if (!out.empty() && out.back() == L'\0') out.pop_back();
	return out;
}

std::string Accessibility::CleanText(const char* raw)
{
	if (!raw) return std::string();

	std::string out;
	out.reserve(64);
	bool inTag = false;
	for (const char* p = raw; *p; ++p)
	{
		unsigned char c = (unsigned char)*p;
		if (c == '<') { inTag = true; continue; }   // strip <icon .../>, <color ...>, </color>
		if (c == '>') { inTag = false; continue; }
		if (inTag) continue;
		if (c == '\n' || c == '\r' || c == '\t') { out.push_back(' '); continue; }
		out.push_back((char)c);
	}

	// Trim leading / trailing whitespace.
	size_t s = out.find_first_not_of(' ');
	if (s == std::string::npos) return std::string();
	size_t e = out.find_last_not_of(' ');
	return out.substr(s, e - s + 1);
}

bool Accessibility::ShouldAnnounce(const char* messageId)
{
	if (!messageId) return false;
	for (const char* prefix : ANNOUNCE_PREFIXES)
	{
		size_t plen = std::strlen(prefix);
		if (std::strncmp(messageId, prefix, plen) == 0) return true;
	}
	return false;
}

void Accessibility::DebugLog(const std::string& id, const std::string& text)
{
	// Caller holds g_mtx.
	if (!g_log.is_open()) return;
	g_log << "[" << TimeStamp() << "] " << id << " = \"" << text << "\"\n";
	g_log.flush();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void Accessibility::Initialize()
{
	if (_initialized) return;
	_initialized = true;

	g_log.open(LogPath(), std::ios::out | std::ios::trunc);
	if (g_log.is_open())
	{
		g_log << "=== Storm Access log " << TimeStamp() << " ===\n";
		g_log.flush();
	}

	// --- Tolk ---
	g_tolk = LoadLibraryW(L"Tolk.dll");
	if (g_tolk)
	{
		p_TolkLoad = (Tolk_Load_t)GetProcAddress(g_tolk, "Tolk_Load");
		p_TolkUnload = (Tolk_Unload_t)GetProcAddress(g_tolk, "Tolk_Unload");
		p_TolkIsLoaded = (Tolk_IsLoaded_t)GetProcAddress(g_tolk, "Tolk_IsLoaded");
		p_TolkHasSpeech = (Tolk_HasSpeech_t)GetProcAddress(g_tolk, "Tolk_HasSpeech");
		p_TolkOutput = (Tolk_Output_t)GetProcAddress(g_tolk, "Tolk_Output");
		p_TolkSilence = (Tolk_Silence_t)GetProcAddress(g_tolk, "Tolk_Silence");
		p_TolkDetect = (Tolk_DetectScreenReader_t)GetProcAddress(g_tolk, "Tolk_DetectScreenReader");

		if (p_TolkLoad) p_TolkLoad();
		bool loaded = p_TolkIsLoaded && p_TolkIsLoaded();
		bool speech = p_TolkHasSpeech && p_TolkHasSpeech();
		_available = loaded && speech && p_TolkOutput;

		if (g_log.is_open())
		{
			const wchar_t* sr = (_available && p_TolkDetect) ? p_TolkDetect() : nullptr;
			char name[128] = "(none)";
			if (sr) WideCharToMultiByte(CP_UTF8, 0, sr, -1, name, sizeof(name), nullptr, nullptr);
			g_log << "Tolk loaded=" << loaded << " speech=" << speech
				<< " reader=" << name << "\n";
			g_log.flush();
		}
	}
	else if (g_log.is_open())
	{
		g_log << "ERROR: Tolk.dll not found in game folder.\n";
		g_log.flush();
	}

	// Base of the game exe (process main module) for module-base + RVA reads.
	g_exeBase = (uintptr_t)GetModuleHandleA(nullptr);

	// --- XInput (use the already-loaded real proxy target, else a system dll) ---
	HMODULE xi = GetModuleHandleA("xinput9_1_0_o.dll");
	if (!xi) xi = LoadLibraryA("xinput1_4.dll");
	if (!xi) xi = LoadLibraryA("xinput1_3.dll");
	if (xi) p_GetState = (XInputGetState_t)GetProcAddress(xi, "XInputGetState");

	if (_available) Speak("Storm accessibility loaded", true);
}

void Accessibility::Shutdown()
{
	if (!_initialized) return;
	if (p_TolkUnload) p_TolkUnload();
	if (g_tolk) { FreeLibrary(g_tolk); g_tolk = nullptr; }
	if (g_log.is_open()) g_log.close();
	_available = false;
	_initialized = false;
}

void Accessibility::Speak(const std::string& text, bool interrupt)
{
	if (!_available || !p_TolkOutput || text.empty()) return;
	std::wstring w = Widen(text);
	if (w.empty()) return;
	p_TolkOutput(w.c_str(), interrupt);
}

void Accessibility::RepeatLast()
{
	std::string last;
	{
		std::lock_guard<std::mutex> lk(g_mtx);
		last = g_lastSpoken;
	}
	if (!last.empty()) Speak(last, true);
}

// ---------------------------------------------------------------------------
// Char-select reverse-engineering aid (debug only).
// When a fighter's signature-jutsu id (c_jyu_*) decodes, log the call stack so
// we can find WHICH game function is asking for it - that caller is the
// character-select code that holds the cursor / selected slot. Addresses are
// reported as game-module RVAs (game+0x...) so they can be pasted into Ghidra.
// ---------------------------------------------------------------------------
typedef USHORT(WINAPI* CaptureStackBackTrace_t)(ULONG, ULONG, PVOID*, PULONG);

static uintptr_t GetModuleImageSize(uintptr_t base)
{
	if (!base) return 0;
	__try
	{
		IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
		if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
		IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
		return nt->OptionalHeader.SizeOfImage;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static void LogCallStack(const std::string& id)
{
	// Caller holds g_mtx.
	if (!g_log.is_open()) return;

	static CaptureStackBackTrace_t p_Capture = []() -> CaptureStackBackTrace_t {
		HMODULE nt = GetModuleHandleW(L"ntdll.dll");
		return nt ? (CaptureStackBackTrace_t)GetProcAddress(nt, "RtlCaptureStackBackTrace")
			: nullptr;
	}();
	if (!p_Capture) { g_log << "[" << TimeStamp() << "] STACK unavailable\n"; g_log.flush(); return; }

	void* frames[32] = {};
	USHORT n = p_Capture(0, 32, frames, nullptr);
	uintptr_t base = g_exeBase;
	uintptr_t size = GetModuleImageSize(base);

	g_log << "[" << TimeStamp() << "] STACK for " << id << " (game base 0x"
		<< std::hex << base << ", size 0x" << size << std::dec << "):\n";
	for (USHORT i = 0; i < n; ++i)
	{
		uintptr_t a = (uintptr_t)frames[i];
		if (base && size && a >= base && a < base + size)
			g_log << "    game+0x" << std::hex << (a - base) << std::dec << "\n";
		else
			g_log << "    ext   0x" << std::hex << a << std::dec << "\n";
	}
	g_log.flush();
}

// ---------------------------------------------------------------------------
// Character-select fighter naming.
// A fighter's NAME is a portrait graphic (never decoded), so we key off the jutsu
// ids its info panel DOES decode on each hover: signature (c_jyu_/c_com_),
// ultimate (c_ult_), and form tag (c_cha_/c_costume_). CHAR_NAMES maps those ids
// (identified from the jutsu text) to a character name.
//  - Some SIGNATURE jutsu are shared (e.g. "Fire Ball Jutsu" -> several fighters),
//    so the UNIQUE ULTIMATE is the preferred key; the signature is a fallback.
//  - A real fighter hover always decodes an ultimate; the jutsu-CUSTOMIZATION list
//    decodes bare signatures with no ultimate. So "ultimate present" tells a
//    fighter apart from a jutsu-list item (and stops us renaming that screen).
// This table is best-guess from the jutsu and meant to be expanded/corrected from
// debug logs; unmapped fighters just speak their signature jutsu (as before).
// ---------------------------------------------------------------------------
struct CharName { const char* id; const char* name; };
static const CharName CHAR_NAMES[] = {
	// Names use full/clan form (user preference). Title-only characters (Third
	// Raikage, Fourth Kazekage, ...) have no clan and stay as-is.
	// Ultimate ids (unique per fighter - preferred key).
	{ "c_ult_147", "Naruto Uzumaki" }, { "c_ult_178", "Naruto Uzumaki" }, { "c_ult_02", "Naruto Uzumaki" },
	{ "c_ult_00",  "Naruto Uzumaki" }, { "c_ult_28",  "Naruto Uzumaki" },
	{ "c_ult_173", "Sasuke Uchiha" }, { "c_ult_153", "Sasuke Uchiha" }, { "c_ult_59", "Sasuke Uchiha" },
	{ "c_ult_33",  "Itachi Uchiha" }, { "c_ult_152", "Sakura Haruno" }, { "c_ult_116", "Mei Terumi" },
	{ "c_ult_66",  "Onoki" },  { "c_ult_131", "Suigetsu Hozuki" }, { "c_ult_26",  "Chiyo" },
	{ "c_ult_40",  "Suigetsu Hozuki" }, { "c_ult_30", "Orochimaru" },
	{ "c_ult_58",  "Fourth Raikage" }, { "c_ult_135", "Gaara" }, { "c_ult_75", "Sasori" },
	{ "c_ult_18",  "Tenten" }, { "c_ult_36",  "Third Kazekage" }, { "c_ult_38", "Kakuzu" },
	{ "c_ult_106", "Hiruzen Sarutobi" }, { "c_ult_71", "Tobirama Senju" },
	{ "c_ult_157", "Hashirama Senju" }, { "c_ult_06", "Sai" }, { "c_ult_174", "Madara Uchiha" },
	{ "c_ult_169", "Might Guy" }, { "c_ult_68", "Zabuza Momochi" },
	{ "c_ult_12",  "Ino Yamanaka" }, { "c_ult_11", "Choji Akimichi" }, { "c_ult_180", "Kakashi Hatake" },
	{ "c_ult_154", "Madara Uchiha" }, { "c_ult_168", "Obito Uchiha" },
	// Signature ids (fallback / used when the ultimate isn't in the table).
	// NOTE: only map signatures that are UNIQUE to one fighter. "Rasengan"
	// (c_jyu_000) is shared by Naruto AND Minato, so it is deliberately NOT here -
	// those fighters are named by their (unique) ultimate instead, and an unmapped
	// Rasengan user just says "Rasengan" rather than the wrong name.
	{ "c_jyu_002", "Naruto Uzumaki" }, { "c_jyu_323", "Naruto Uzumaki" },
	{ "c_jyu_515", "Naruto Uzumaki" }, { "c_jyu_549", "Sakura Haruno" }, { "c_jyu_557", "Sasuke Uchiha" },
	{ "c_jyu_324", "Sasuke Uchiha" }, { "c_jyu_142", "Sasuke Uchiha" },
	// (c_jyu_007 "Chidori" removed - shared by Sasuke AND Kakashi; keyed by ult.)
	{ "c_jyu_016", "Ino Yamanaka" }, { "c_jyu_015", "Choji Akimichi" }, { "c_jyu_059", "Kakashi Hatake" },
	{ "c_jyu_386", "Madara Uchiha" }, { "c_jyu_519", "Obito Uchiha" },
	{ "c_com_1367", "Madara Uchiha" }, { "c_com_1030", "Hashirama Senju" },
	// --- Added from 2nd battle log (ultimates preferred; ougi codes confirmed
	//     3mnt=Minato, 5kgy=Kaguya, 6ssk=Sasuke). ---
	{ "c_ult_156", "Minato Namikaze" }, { "c_ult_179", "Naruto Uzumaki" }, { "c_ult_119", "Naruto Uzumaki" },
	{ "c_ult_145", "Naruto Uzumaki" }, { "c_ult_09",  "Tenten" }, { "c_ult_144", "Fourth Kazekage" },
	{ "c_ult_99",  "Gaara" },  { "c_ult_118", "Kisame Hoshigaki" }, { "c_ult_29",  "Tsunade Senju" },
	{ "c_ult_139", "Kabuto Yakushi" }, { "c_ult_184", "Sasuke Uchiha" }, { "c_ult_172", "Kaguya Otsutsuki" },
	{ "c_jyu_529", "Kaguya Otsutsuki" }, { "c_jyu_381", "Kushina Uzumaki" }, { "c_jyu_555", "Naruto Uzumaki" },
	{ "c_jyu_034", "Kisame Hoshigaki" }, { "c_jyu_030", "Tsunade Senju" }, { "c_jyu_318", "Fourth Kazekage" },
	{ "c_jyu_013", "Tenten" },
	// --- Identified via web research (jutsu name -> character; see
	//     CHARACTER_JUTSU_MAP.md). ---
	{ "c_ult_41",  "Jugo" },   { "c_jyu_046", "Jugo" },
	{ "c_ult_138", "Fuu" },    { "c_jyu_260", "Fuu" },
	{ "c_ult_125", "Yugito Nii" }, { "c_jyu_257", "Yugito Nii" },
	{ "c_ult_143", "Third Raikage" }, { "c_jyu_316", "Third Raikage" },
	{ "c_ult_121", "Nagato" },
	// --- User-confirmed + log ids (3rd pass) ---
	{ "c_ult_10",  "Shikamaru Nara" }, { "c_jyu_014", "Shikamaru Nara" },
	{ "c_ult_129", "Yagura" },    { "c_jyu_312", "Yagura" },
	{ "c_ult_150", "Shisui Uchiha" }, { "c_jyu_376", "Shisui Uchiha" },
	{ "c_ult_148", "Obito Uchiha" },  { "c_com_1118", "Obito Uchiha" },
	// --- 4th pass (full roster sweep). Keyed by ultimate (unique). ---
	{ "c_ult_72",  "Kakashi Hatake" }, { "c_ult_73", "Sasuke Uchiha" }, { "c_ult_03", "Sasuke Uchiha" },
	{ "c_ult_124", "Utakata" },  { "c_jyu_256", "Utakata" },
	{ "c_ult_182", "Rin Nohara" }, { "c_jyu_526", "Rin Nohara" }, // Three-Tails jinchuriki (not Yagura)
	{ "c_ult_90",  "Neji Hyuga" }, { "c_jyu_012", "Neji Hyuga" },
	{ "c_ult_15",  "Hinata Hyuga" },
	{ "c_ult_13",  "Kiba Inuzuka" }, { "c_jyu_017", "Kiba Inuzuka" },
	{ "c_ult_25",  "Asuma Sarutobi" }, { "c_jyu_027", "Asuma Sarutobi" },
	{ "c_ult_23",  "Yamato" },   { "c_jyu_088", "Yamato" },
	{ "c_ult_149", "Kushina Uzumaki" },
	{ "c_ult_14",  "Shino Aburame" }, { "c_jyu_056", "Shino Aburame" },
	{ "c_ult_64",  "Danzo Shimura" },
	{ "c_ult_130", "Darui" },    { "c_jyu_230", "Darui" },
	{ "c_ult_76",  "Kabuto Yakushi" }, { "c_ult_31", "Kabuto Yakushi" }, { "c_jyu_032", "Kabuto Yakushi" },
	// --- User-confirmed (5th pass) ---
	{ "c_ult_126", "Roshi" },  { "c_jyu_258", "Roshi" },   // Lava Style: Scorching Rocks
	{ "c_ult_127", "Han" },    { "c_jyu_259", "Han" },     // Five-Tails (Eruption Kick)
	{ "c_jyu_268", "Naruto Uzumaki" },                     // Chakra Mode (Tailed Beast Bomb, no ult)
	// --- User-confirmed by ear (6th pass) ---
	{ "c_ult_146", "Iruka Umino" }, { "c_jyu_374", "Iruka Umino" },
	{ "c_jyu_082", "Killer Bee" },                         // Rising Bomber (+ Shark Skin)
	{ "c_com_048", "Rock Lee" },    { "c_jyu_061", "Rock Lee" },
	{ "c_ult_91",  "Tenten" },                             // Ninja Tool: Million Blade Chaos
	{ "c_jyu_125", "Hinata Hyuga" }, { "c_jyu_019", "Hinata Hyuga" },
};

static const char* LookupCharName(const char* id)
{
	for (const CharName& c : CHAR_NAMES)
		if (std::strcmp(id, c.id) == 0) return c.name;
	return nullptr;
}

static std::string StripParens(const std::string& s)
{
	size_t a = 0, b = s.size();
	if (b > 0 && s.front() == '(') a = 1;
	if (b > a && s[b - 1] == ')') --b;
	return s.substr(a, b - a);
}

// One hover's info panel, accumulated across its c_jyu/c_ult/c_cha decodes and
// spoken once when it settles (on the form tag, on the next hover, or on a short
// timeout in Update()). Guarded by g_charMtx.
static std::mutex g_charMtx;
static std::string g_pcName;    // mapped character name (empty if unmapped)
static std::string g_pcJutsu;   // signature jutsu text (fallback / jutsu-list label)
static std::string g_pcForm;    // form tag ("Sage Mode", ...) - fighters only
static bool g_pcHadUlt = false; // an ultimate decoded => fighter, not a jutsu list
static bool g_pcHadForm = false;// a form tag decoded => also a fighter (some forms,
                                // e.g. Chakra-Mode Naruto, decode NO ultimate)
static bool g_pcActive = false;
static unsigned long long g_pcTick = 0;
static std::string g_lastCharSpoken;
// Multiple signatures of ONE hover (e.g. Chakra-Mode Naruto: Rasengan + Tailed
// Beast Bomb) arrive within a frame; merge them instead of speaking each.
static const unsigned long long CHAR_SAME_HOVER_MS = 120;

// Speak the accumulated fighter (name + form) or jutsu-list item (jutsu), deduped
// on consecutive-identical to swallow idle re-decodes. Clears the pending panel.
static void FlushChar()
{
	std::string spoken;
	{
		std::lock_guard<std::mutex> lk(g_charMtx);
		if (!g_pcActive) return;
		bool fighter = g_pcHadUlt || g_pcHadForm;   // ult OR form tag => a fighter
		if (fighter)
			spoken = g_pcName.empty() ? g_pcJutsu : g_pcName;   // fighter: name, else its jutsu
		else
			spoken = g_pcJutsu.empty() ? g_pcName : g_pcJutsu;  // jutsu-list item: the jutsu
		if (fighter && !g_pcForm.empty() && !spoken.empty())
			spoken += ", " + g_pcForm;
		g_pcName.clear(); g_pcJutsu.clear(); g_pcForm.clear();
		g_pcHadUlt = false; g_pcHadForm = false; g_pcActive = false;
		if (spoken.empty() || spoken == g_lastCharSpoken) return;
		g_lastCharSpoken = spoken;
	}
	{
		std::lock_guard<std::mutex> lk(g_mtx);
		g_lastSpoken = spoken;   // Back+X repeat
	}
	Accessibility::Speak(spoken, true);
}

void Accessibility::OnMessageDecoded(const char* messageId, const char* decodedText)
{
	if (!messageId || messageId[0] == '\0') return;

	// The game is decoding UI text, so it is fully initialized. This is the gate
	// that lets the Adventure dialog reader touch game memory (never before now).
	g_gameReady.store(true);

	unsigned long long now = GetTickCount64();
	unsigned long long navTick = g_lastNavTick.load();
	bool recentNav = (now - navTick <= NAV_WINDOW_MS);

	// Battle entry/exit. "league_*" ("Battle") decodes when a fight starts;
	// character-select markers decode on the select screen (incl. returning from a
	// fight). Between them, suppress char-select handling so jutsu-use decodes in
	// the fight aren't spoken (the battle leak).
	if (std::strncmp(messageId, "league", 6) == 0)
	{
		g_inBattle.store(true);
		g_charSelectUntil.store(0);   // kill any lingering context immediately
	}
	else if (std::strncmp(messageId, "MSG_Random", 10) == 0 ||
		std::strncmp(messageId, "characterselect", 15) == 0)
	{
		g_inBattle.store(false);
	}

	// Mark/refresh the character-select context (never during a battle). Markers
	// (MSG_Random, characterselect_*, c_sta) open it; but we ALSO refresh on any
	// char-select info id (c_jyu/c_ult/c_cha/c_union/c_costume/c_com) so it stays
	// alive through deliberate navigation instead of expiring between sparse
	// markers (the old bug that made char-select silent). Runs before the announce
	// block below so a decode after an expiry re-opens the context and is still
	// announced.
	if (!g_inBattle.load() &&
		(std::strncmp(messageId, "MSG_Random", 10) == 0 ||
		 std::strncmp(messageId, "characterselect", 15) == 0 ||
		 std::strncmp(messageId, "c_sta", 5) == 0 ||
		 std::strncmp(messageId, "c_com", 5) == 0 ||
		 IsCharSelectInfo(messageId)))
		g_charSelectUntil.store(now + CHARSELECT_WINDOW_MS);

	// Char-select RE: when a fighter's signature-jutsu id decodes and logging is
	// on, record the call stack once per distinct id (capped, so the log stays
	// readable). The caller is the character-select code holding the cursor/slot.
	if (DebugLogging && std::strncmp(messageId, "c_jyu", 5) == 0)
	{
		std::lock_guard<std::mutex> lk(g_mtx);
		if (g_stackCaptures < 6 && g_stackCapturedIds.insert(messageId).second)
		{
			++g_stackCaptures;
			LogCallStack(messageId);
		}
	}

	// Capture localized Yes/No labels when a confirm dialog decodes them, so the
	// Adventure dialog reader can name the highlighted option (and we can list the
	// options after the question). Scoped: cleared when the dialog closes.
	if (std::strcmp(messageId, "MSG_Yes") == 0)
	{
		std::lock_guard<std::mutex> lk(g_dlgMtx);
		g_yesText = CleanText(decodedText);
	}
	else if (std::strcmp(messageId, "MSG_No") == 0)
	{
		std::lock_guard<std::mutex> lk(g_dlgMtx);
		g_noText = CleanText(decodedText);
	}

	// Debug log: capture every id decoded right after a navigation (including
	// battle-family ids, which speech skips), once per navigation. This shows
	// exactly what each move re-decodes - revealing decode-once menus and the
	// ids of screens that are otherwise silent.
	if (DebugLogging && recentNav)
	{
		std::lock_guard<std::mutex> lk(g_mtx);
		if (navTick != g_logNavTick) { g_loggedThisNav.clear(); g_logNavTick = navTick; }
		if (g_loggedThisNav.insert(messageId).second)
			DebugLog(messageId, CleanText(decodedText));
	}

	// Popup capture: text decoded OUTSIDE the nav window (login bonus, item
	// acquired, "updated" notices) never reaches the nav-gated log above. Capture
	// each such id once per session so we can find popup ids. Skip risky/battle
	// families and junk so this doesn't flood during battles/idle re-decodes.
	if (DebugLogging && !recentNav && !IsRiskyId(messageId))
	{
		std::string c = CleanText(decodedText);
		if (!c.empty() && c != "???")
		{
			std::lock_guard<std::mutex> lk(g_mtx);
			if (g_loggedPopupEver.insert(messageId).second)
				DebugLog(std::string("(popup?) ") + messageId, c);
		}
	}

	// Popups announce regardless of the nav gate, since they appear with no
	// input. Deduped on consecutive identical text to avoid repeats on refresh.
	if (IsPopup(messageId) && !IsSuppressed(messageId))
	{
		std::string c = CleanText(decodedText);
		if (c.empty() || c == "???") return;
		{
			std::lock_guard<std::mutex> lk(g_mtx);
			if (c == g_lastPopup) return;
			g_lastPopup = c;
			g_lastSpoken = c;
		}
		Speak(c, true);
		return;
	}

	// --- Character-select fighter announcement -------------------------------
	// While the char-select context is fresh, name the hovered fighter from the
	// jutsu ids its info panel decodes (see CHAR_NAMES). Ignores the nav gate -
	// these decodes often land just outside it (they show as "(popup?)" in the
	// log). Buffers the c_jyu/c_ult/c_cha of one hover and speaks it once (via
	// FlushChar) so we say "Naruto, Sage Mode" rather than three fragments, and so
	// the ultimate can override an ambiguous/unmapped signature.
	if (!g_inBattle.load() && now < g_charSelectUntil.load())
	{
		bool isSig  = std::strncmp(messageId, "c_jyu", 5) == 0 ||
			std::strncmp(messageId, "c_com", 5) == 0 ||
			std::strncmp(messageId, "c_union", 7) == 0;
		bool isUlt  = std::strncmp(messageId, "c_ult", 5) == 0;
		bool isForm = std::strncmp(messageId, "c_cha", 5) == 0 ||
			std::strncmp(messageId, "c_costume", 9) == 0;
		if (isSig || isUlt || isForm)
		{
			std::string txt = CleanText(decodedText);
			const char* nm = LookupCharName(messageId);
			if (isSig)
			{
				// A hover can decode SEVERAL signatures at once (e.g. Chakra-Mode
				// Naruto: Rasengan + Tailed Beast Bomb). If another signature is
				// still within the same hover, merge it (keep the mapped name /
				// first jutsu) instead of flushing - otherwise flush the previous
				// hover and start fresh.
				bool merged = false;
				{
					std::lock_guard<std::mutex> lk(g_charMtx);
					if (g_pcActive && now - g_pcTick <= CHAR_SAME_HOVER_MS)
					{
						if (nm && g_pcName.empty()) g_pcName = nm;
						if (g_pcJutsu.empty()) g_pcJutsu = txt;
						g_pcTick = now;
						merged = true;
					}
				}
				if (!merged)
				{
					FlushChar();
					std::lock_guard<std::mutex> lk(g_charMtx);
					g_pcActive = true; g_pcTick = now;
					g_pcJutsu = txt; g_pcName.clear(); g_pcForm.clear();
					g_pcHadUlt = false; g_pcHadForm = false;
					if (nm) g_pcName = nm;
				}
			}
			else if (isUlt)
			{
				std::lock_guard<std::mutex> lk(g_charMtx);
				if (!g_pcActive) g_pcActive = true;   // ult arrived without a signature
				g_pcHadUlt = true; g_pcTick = now;
				if (nm) g_pcName = nm;                // ultimate is the reliable key
			}
			else // isForm
			{
				{
					std::lock_guard<std::mutex> lk(g_charMtx);
					if (g_pcActive)
					{
						std::string t = StripParens(txt);
						if (!t.empty()) g_pcForm = t;
						g_pcHadForm = true;   // form tag => a fighter (even with no ult)
						g_pcTick = now;
					}
				}
				FlushChar();   // form tag is the last id of the triple
			}
			return;
		}
	}

	// Never touch battle/transition families for SPEECH - the hook is fragile -
	// UNLESS the id is explicitly allowlisted. The practice/pause menu
	// (battlestartmenu_*) shares the "battle" prefix but is a deliberate,
	// low-frequency menu we want spoken, so the allowlist overrides risky.
	bool relevant = ShouldAnnounce(messageId) && !IsSuppressed(messageId) && !IsChrome(messageId);

	// Character-select info (signature jutsu / ultimate / version tag) shares ids
	// with in-battle decoding, so only announce it while the character-select
	// context is fresh - this identifies the hovered fighter (whose NAME is a
	// graphic) without leaking jutsu names during real battles.
	if (!relevant && IsCharSelectInfo(messageId) && now < g_charSelectUntil.load())
		relevant = true;

	if (IsRiskyId(messageId) && !relevant) return;
	if (!relevant) return;

	// Only announce text decoded shortly after the player navigated, so
	// background/idle conversions don't interrupt the focused item.
	if (!recentNav) return;

	// Skip blanks and placeholders so the focused item wins the slot below.
	std::string cleaned = CleanText(decodedText);
	if (cleaned.empty() || cleaned == "???") return;

	std::string label, detail;
	ResolveAnnouncement(messageId, cleaned, label, detail);
	if (label.empty()) return;

	{
		std::lock_guard<std::mutex> lk(g_mtx);
		// Each highlight converts a BUNDLE of strings at once and the focused
		// item converts first - so announce only the first useful one per nav.
		// Exception: a question ("...?") may override (dialogs decode the focused
		// option before the question, but the question is what matters).
		bool isQuestion = cleaned.find('?') != std::string::npos;
		bool firstThisNav = (navTick != g_consumedNavTick);

		// Already announced this nav: only a brand-new question may override.
		if (!firstThisNav && (!isQuestion || navTick == g_questionNavTick))
			return;

		// Skip if identical to what we just said - but do NOT consume the nav
		// slot, so a later, DIFFERENT string in the same bundle can still win.
		// (Screen headers/footers decode first and match the last announcement;
		// the old code consumed the slot here and silently swallowed the real
		// focused item - that is why the jutsu list spoke nothing.)
		if (label == g_lastSpoken) return;

		g_consumedNavTick = navTick;
		if (isQuestion) g_questionNavTick = navTick;
		g_lastSpoken = label;
	}
	Speak(label, true);                                  // focused item interrupts
	if (!detail.empty() && detail != label) Speak(detail, false); // detail queued

	// After a confirmation question, list the available options so the player
	// knows the choices (the Adventure reader will then name the highlighted one
	// as they move). Queued, non-interrupting, only when both labels were captured.
	if (cleaned.find('?') != std::string::npos)
	{
		std::string opts;
		{
			std::lock_guard<std::mutex> lk(g_dlgMtx);
			if (!g_yesText.empty() && !g_noText.empty())
			{
				opts = g_yesText + " or " + g_noText;
				// Arm the direction-based fallback: this is a live yes/no dialog.
				g_yesNoDialogUntil.store(now + YESNO_WINDOW_MS);
			}
		}
		if (!opts.empty()) Speak(opts, false);
	}
}

void Accessibility::Update()
{
	if (!p_GetState) return;

	unsigned long long now = GetTickCount64();
	if (now - g_lastPollTick < 16) return; // ~60 Hz is plenty for menus
	g_lastPollTick = now;

	// Flush a pending char-select fighter whose hover decoded no form tag to end
	// its triple (so it still speaks after a short settle instead of waiting for
	// the next hover). Cheap: only touches state when a panel is actually pending.
	{
		bool due;
		{
			std::lock_guard<std::mutex> lk(g_charMtx);
			due = g_pcActive && (now - g_pcTick > 160);
		}
		if (due) FlushChar();
	}

	XINPUT_STATE st;
	ZeroMemory(&st, sizeof(st));
	if (p_GetState(0, &st) != ERROR_SUCCESS) return;

	WORD b = st.Gamepad.wButtons;

	// Any button press or left-stick movement opens the announce window, so the
	// UI response (focused item, opened dialog, new screen) gets spoken. Dialogs
	// open via face/Back buttons, not just the d-pad, so we watch all buttons.
	short lx = st.Gamepad.sThumbLX, ly = st.Gamepad.sThumbLY;
	bool stick = (abs(lx) > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE ||
		abs(ly) > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
	if (b != 0 || stick) g_lastNavTick.store(now);

	// Back + X : repeat last announcement.  Back + Y : toggle debug logging.
	bool back = (b & XINPUT_GAMEPAD_BACK) != 0;
	bool x = (b & XINPUT_GAMEPAD_X) != 0;
	bool y = (b & XINPUT_GAMEPAD_Y) != 0;
	if (back && x && !g_xPrev) RepeatLast();
	if (back && y && !g_yPrev)
	{
		DebugLogging = !DebugLogging;
		if (DebugLogging)
		{
			// Fresh char-select stack captures each time logging is turned on.
			std::lock_guard<std::mutex> lk(g_mtx);
			g_stackCaptures = 0;
			g_stackCapturedIds.clear();
		}
		Speak(DebugLogging ? "debug logging on" : "debug logging off", true);
	}
	g_xPrev = x;
	g_yPrev = y;

	// Adventure decode-once dialog: speak the highlighted option as the cursor
	// moves. ReadAdvDialogCursor self-gates on g_gameReady (won't touch memory
	// until the game is fully up) and is exception-guarded, so this is safe.
	int advIdx = 0;
	bool advOpen = ReadAdvDialogCursor(&advIdx);
	if (advOpen)
	{
		if (!g_advWindowWasOpen)
		{
			// Just opened: the question + "Yes or No" hint is already playing;
			// record the initial highlight without speaking it.
			g_advCursorPrev = advIdx;
			g_advWindowWasOpen = true;
		}
		else if (advIdx != g_advCursorPrev)
		{
			// Cursor moved. Map the index to the captured option label. Non-Yes/No
			// lists have no captured labels, so they stay silent rather than ever
			// announcing a wrong option.
			std::string opt;
			{
				std::lock_guard<std::mutex> lk(g_dlgMtx);
				if (advIdx == 0) opt = g_yesText;       // first option
				else if (advIdx == 1) opt = g_noText;   // second option
			}
			if (!opt.empty()) Speak(opt, true);
			g_advCursorPrev = advIdx;
		}
	}
	else if (g_advWindowWasOpen)
	{
		// Dialog closed: reset and scope the Yes/No labels to one dialog.
		g_advWindowWasOpen = false;
		g_advCursorPrev = -2;
		std::lock_guard<std::mutex> lk(g_dlgMtx);
		g_yesText.clear();
		g_noText.clear();
	}

	// Direction-based yes/no fallback for Flash confirms (when the Adventure C++
	// read isn't available). Positional: press toward an option, hear it. Only
	// while a yes/no dialog is armed, and not while the Adventure read is handling
	// it (avoids double-speak).
	bool dleft = (b & XINPUT_GAMEPAD_DPAD_LEFT) != 0 ||
		lx < -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
	bool dright = (b & XINPUT_GAMEPAD_DPAD_RIGHT) != 0 ||
		lx > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
	if (!advOpen && now < g_yesNoDialogUntil.load())
	{
		std::string yes, no;
		{
			std::lock_guard<std::mutex> lk(g_dlgMtx);
			yes = g_yesText;
			no = g_noText;
		}
		bool spoke = false;
		if (dleft && !g_dpadLeftPrev && !yes.empty()) { Speak(yes, true); spoke = true; }  // left
		if (dright && !g_dpadRightPrev && !no.empty()) { Speak(no, true); spoke = true; }   // right
		// Keep the window alive while the player is actively moving in the dialog.
		if (spoke) g_yesNoDialogUntil.store(now + YESNO_WINDOW_MS);
	}
	g_dpadLeftPrev = dleft;
	g_dpadRightPrev = dright;
}
