// LangSwitch — переключение раскладки (латиница <-> кириллица) двойным нажатием Ctrl
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <mmsystem.h>
#include <process.h>
#include <shellapi.h>
#include <taskschd.h>
#include <uiautomationclient.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "layout_map.h"

#ifdef _MSC_VER
#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#endif

#ifdef _MSC_VER
#define LS_IID(T) __uuidof(T)
#else
#define LS_IID(T) IID_##T
#endif

// ---------------------------------------------------------------- константы
static const ULONG_PTR kMarker = 0x4C535743;   // метка наших собственных SendInput-событий
static const UINT WM_TRIGGER = WM_APP + 2;
static const UINT WM_PLAYSOUND = WM_APP + 3;
static const UINT_PTR kTimerChat = 1;          // проверка "открылся ли чат" после клавиши чата в игре
static const DWORD kChatCheckMs = 250;
static const DWORD kChatIdleMs = 4000;         // пауза в наборе, после которой чат в игре считается закрытым
static const size_t kMaxUiaSelection = 5000;   // длиннее — читаем выделение обычным копированием
static const DWORD kSelChainMs = 120000;       // сколько живёт цепочка повторных конвертаций выделения
static const wchar_t* kMutexName = L"Local\\LangSwitch_SingleInstance";

// ---------------------------------------------------------------- настройки
struct Settings {
    bool sound = true;
    bool wholeText = true;     // false -> только последнее слово
    bool switchLayout = true;
    bool reselect = true;
    bool gameFilter = true;    // особый режим для игр (полноэкранные окна, скрытый курсор)
    bool switchEmpty = false;  // двойной Ctrl переключает раскладку, даже если текст не вводился
    int doubleCtrlMs = 500;
    int maxBuffer = 1000;
};

static Settings g_cfg;
static HWND g_hwnd = NULL;
static HHOOK g_kbHook = NULL, g_msHook = NULL;
static std::wstring g_exePath, g_exeDir, g_iniPath;  // g_exeDir заканчивается на '\'
static UINT g_msgCmd = 0;                            // служебное сообщение между копиями программы
static bool g_elevated = false;                      // эта копия запущена с правами администратора

// форматы буфера обмена, запрещающие попадание в историю / облако / менеджеры
static UINT g_fmtHistory = 0, g_fmtCloud = 0, g_fmtMonitor = 0;

static const wchar_t* kSection = L"LangSwitch";

#include "exclude.h"  // список исключённых программ (exclude.txt)

static void LoadSettings() {
    const wchar_t* ini = g_iniPath.c_str();
    auto geti = [&](const wchar_t* key, int def) {
        return (int)GetPrivateProfileIntW(kSection, key, def, ini);
    };
    g_cfg.sound = geti(L"Sound", 1) != 0;
    g_cfg.switchLayout = geti(L"SwitchLayout", 1) != 0;
    g_cfg.reselect = geti(L"Reselect", 1) != 0;
    g_cfg.gameFilter = geti(L"GameFilter", 1) != 0;
    g_cfg.switchEmpty = geti(L"SwitchEmpty", 0) != 0;
    g_cfg.doubleCtrlMs = geti(L"DoubleCtrlMs", 500);
    g_cfg.maxBuffer = geti(L"MaxBuffer", 1000);
    if (g_cfg.doubleCtrlMs < 100) g_cfg.doubleCtrlMs = 100;
    if (g_cfg.maxBuffer < 10) g_cfg.maxBuffer = 10;

    wchar_t buf[32] = {0};
    GetPrivateProfileStringW(kSection, L"Scope", L"all", buf, 32, ini);
    g_cfg.wholeText = _wcsicmp(buf, L"word") != 0;
}

static void SaveDefaultSettings() {
    const wchar_t* ini = g_iniPath.c_str();
    auto put = [&](const wchar_t* key, const wchar_t* v) {
        WritePrivateProfileStringW(kSection, key, v, ini);
    };
    put(L"Sound", L"1");
    put(L"Scope", L"all");
    put(L"SwitchLayout", L"1");
    put(L"Reselect", L"1");
    put(L"GameFilter", L"1");
    put(L"SwitchEmpty", L"0");
    put(L"DoubleCtrlMs", L"500");
    put(L"MaxBuffer", L"1000");
}

// ---------------------------------------------------------------- общее состояние
struct Rec {
    wchar_t ch;      // текущий символ (после конвертаций)
    wchar_t origCh;  // символ в том виде, в каком он был набран
    HKL home;        // раскладка, в которой символ был набран
    HKL cur;         // раскладка, в которой символ находится сейчас
    bool neutral;    // не зависит от раскладки (цифры numpad)
    bool asStart = false;  // начало слова, переключённого автоматически (см. autoswitch.h)
};

// Установленные раскладки по алфавитам. Порядок: сначала последняя использованная, затем остальные по кругу.
struct Cycle {
    std::vector<HKL> lat;
    std::vector<HKL> cyr;
};

static CRITICAL_SECTION g_cs;
struct Lock {
    Lock() { EnterCriticalSection(&g_cs); }
    ~Lock() { LeaveCriticalSection(&g_cs); }
};

static std::vector<Rec> g_buf;      // что напечатано с момента последнего "сброса"
static bool g_chainActive = false;  // после последней конвертации ничего не набирали: повторное нажатие продолжает цепочку
static Cycle g_chainCycle;          // порядок раскладок, замороженный на время цепочки
static bool g_selHint = false;      // есть вероятность, что в приложении что-то выделено
static bool g_selByMouse = false;   // выделение сделано мышью
static bool g_selForce = false;     // выделение сделано мышью при зажатом Ctrl — точно текст, проверки не нужны
static POINT g_selDownPt = {0, 0};  // где нажата кнопка мыши при выделении
static POINT g_selUpPt = {0, 0};    // где она отпущена
static std::atomic<bool> g_busy(false);
static std::atomic<bool> g_resetAfter(false);
static std::atomic<DWORD> g_cooldownUntil(0);  // до этого момента (мс) двойной Ctrl не принимается

// Последние использованные раскладки (запоминаются глобально, не по приложениям)
static std::atomic<UINT_PTR> g_lastLat(0);
static std::atomic<UINT_PTR> g_lastCyr(0);

// Раскладка, которую мы переключили сами в окне, не сообщающем о смене раскладки (см. EffectiveLayout)
static std::atomic<UINT_PTR> g_assumedHkl(0);    // 0 — не используется
static std::atomic<UINT_PTR> g_assumedHwnd(0);   // окно, для которого это действует
static std::atomic<UINT_PTR> g_assumedStale(0);  // что окно продолжает сообщать (устаревшая раскладка)

// игровой режим
static std::atomic<bool> g_gameArmed(false);    // в игре идёт набор текста (чат открыт)
static std::atomic<bool> g_gamePending(false);  // нажата клавиша чата, ждём подтверждения по курсору
static std::atomic<DWORD> g_gameLastKey(0);

// состояние хуков (используется только потоком хуков)
static bool g_caps = false, g_capsHeld = false;
static bool g_ctrlDown = false, g_ctrlDirty = false;
static bool g_downWithCtrl = false;
static bool g_keyDown[256] = {false};
static int g_chordMask = 0;        // какие из Shift(1)/Alt(2)/Ctrl(4)/Win(8) нажимались с тех пор, как все были отпущены
static bool g_chordDirty = false;  // и нажималась ли между ними обычная клавиша
static HWND g_lastFg = NULL;
static DWORD g_ctrlDownTick = 0, g_lastTap = 0;
static POINT g_downPt = {0, 0}, g_lastDownPt = {0, 0};
static DWORD g_lastDownTime = 0;
static int g_clickCount = 0;

// Набор продолжили после конвертации: считаем уже сконвертированный текст набранным в его новых раскладках
// (вызывать под Lock)
static void NormalizeChain() {
    for (size_t i = 0; i < g_buf.size(); ++i) {
        g_buf[i].home = g_buf[i].cur;
        g_buf[i].origCh = g_buf[i].ch;
    }
    g_chainActive = false;
}

// "Взвести" набор текста в игре (чат открыт) / сбросить его
static void GameArm(DWORD now) {
    g_gamePending = false;
    g_gameArmed = true;
    g_gameLastKey = now;
    KillTimer(g_hwnd, kTimerChat);
    Lock l;
    g_buf.clear();
}

static void GameDisarm() {
    g_gameArmed = false;
    g_gamePending = false;
    KillTimer(g_hwnd, kTimerChat);
    Lock l;
    g_buf.clear();
}

// Забыть всё набранное и выделенное (например, при смене активного окна)
static void ResetTyping() {
    g_gameArmed = false;
    g_gamePending = false;
    KillTimer(g_hwnd, kTimerChat);
    Lock l;
    g_buf.clear();
    g_selHint = false;
    g_selByMouse = false;
    g_selForce = false;
}

// ---------------------------------------------------------------- раскладки клавиатуры
// Таблицы соответствия строятся из установленных раскладок Windows и кэшируются в памяти.

static CRITICAL_SECTION g_layCs;
struct LayLock {
    LayLock() { EnterCriticalSection(&g_layCs); }
    ~LayLock() { LeaveCriticalSection(&g_layCs); }
};

static std::vector<std::pair<UINT_PTR, int> > g_scriptCache;                 // раскладка -> алфавит
static std::map<std::pair<UINT_PTR, UINT_PTR>, lm::CharMap> g_mapCache;      // (откуда, куда) -> таблица символов
static std::map<UINT_PTR, std::set<wchar_t> > g_charsCache;                  // раскладка -> какие символы она даёт

// Скан-коды клавиш основного блока (` 1..0 - =, QWERTY-ряд, ASDF-ряд, \, ZXCV-ряд) и дополнительной клавиши ISO
static const UINT kScans[] = {0x29, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D,
                              0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B,
                              0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x2B,
                              0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x56};
// Клавиши букв — по ним определяем алфавит раскладки
static const UINT kLetterScans[] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
                                    0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
                                    0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32};

static HKL FgLayout() {
    HWND fg = GetForegroundWindow();
    DWORD tid = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
    return GetKeyboardLayout(tid);
}

// Какой символ даёт клавиша (скан-код) в раскладке h. false — клавиша ничего не печатает.
static bool KeyChar(HKL h, UINT scan, bool shift, bool caps, wchar_t& out) {
    const UINT vk = MapVirtualKeyExW(scan, MAPVK_VSC_TO_VK, h);
    if (!vk) return false;
    BYTE ks[256];
    ZeroMemory(ks, sizeof(ks));
    if (shift) ks[VK_SHIFT] = 0x80;
    if (caps) ks[VK_CAPITAL] = 0x01;
    wchar_t buf[8] = {0};
    const int n = ToUnicodeEx(vk, scan, ks, buf, 8, 0x4, h);  // 0x4 = не менять состояние dead keys
    if (n != 1 || buf[0] < 0x20 || buf[0] == 0x7F) return false;
    out = buf[0];
    return true;
}

// Алфавит раскладки: 1 — латиница, 2 — кириллица, 0 — другая (иврит, греческая и т.п.)
static int ScriptOfLayout(HKL h) {
    if (!h) return 0;
    {
        LayLock l;
        for (size_t i = 0; i < g_scriptCache.size(); ++i) {
            if (g_scriptCache[i].first == (UINT_PTR)h) return g_scriptCache[i].second;
        }
    }
    int lat = 0, cyr = 0;
    for (UINT s : kLetterScans) {
        wchar_t c;
        if (KeyChar(h, s, false, false, c)) {
            if (lm::IsLatin(c)) ++lat;
            else if (lm::IsCyrillic(c)) ++cyr;
        }
    }
    const int script = cyr > lat ? 2 : (lat > cyr ? 1 : 0);
    LayLock l;
    g_scriptCache.push_back(std::make_pair((UINT_PTR)h, script));
    return script;
}

// Символы, которые раскладка может напечатать
static const std::set<wchar_t>& LayoutChars(HKL h) {
    LayLock l;
    std::map<UINT_PTR, std::set<wchar_t> >::iterator it = g_charsCache.find((UINT_PTR)h);
    if (it != g_charsCache.end()) return it->second;
    std::set<wchar_t>& s = g_charsCache[(UINT_PTR)h];
    for (UINT scan : kScans) {
        for (int sh = 0; sh < 2; ++sh) {
            wchar_t c;
            if (KeyChar(h, scan, sh != 0, false, c)) s.insert(c);
        }
    }
    return s;
}

// Таблица "символ в раскладке from -> тот же символ на той же клавише в раскладке to"
static const lm::CharMap& MapFor(HKL from, HKL to) {
    LayLock l;
    const std::pair<UINT_PTR, UINT_PTR> key((UINT_PTR)from, (UINT_PTR)to);
    std::map<std::pair<UINT_PTR, UINT_PTR>, lm::CharMap>::iterator it = g_mapCache.find(key);
    if (it != g_mapCache.end()) return it->second;
    lm::CharMap& m = g_mapCache[key];
    for (UINT scan : kScans) {
        for (int sh = 0; sh < 2; ++sh) {
            wchar_t a, b;
            if (KeyChar(from, scan, sh != 0, false, a) && KeyChar(to, scan, sh != 0, false, b)) {
                if (m.find(a) == m.end()) m[a] = b;
            }
        }
    }
    return m;
}

static std::vector<HKL> InstalledLayouts() {
    std::vector<HKL> v;
    int n = GetKeyboardLayoutList(0, NULL);
    if (n <= 0) return v;
    v.resize(n);
    n = GetKeyboardLayoutList(n, v.data());
    v.resize(n > 0 ? n : 0);
    return v;
}

static void RotateTo(std::vector<HKL>& v, UINT_PTR first) {
    for (size_t i = 0; i < v.size(); ++i) {
        if ((UINT_PTR)v[i] == first) {
            std::rotate(v.begin(), v.begin() + i, v.end());
            return;
        }
    }
}

// Установленные латинские и кириллические раскладки; первыми — последние использованные
static Cycle BuildCycle() {
    Cycle c;
    const std::vector<HKL> list = InstalledLayouts();
    for (size_t i = 0; i < list.size(); ++i) {
        const int s = ScriptOfLayout(list[i]);
        if (s == 1) c.lat.push_back(list[i]);
        else if (s == 2) c.cyr.push_back(list[i]);
    }
    RotateTo(c.lat, g_lastLat);
    RotateTo(c.cyr, g_lastCyr);
    return c;
}

// Запомнить раскладку как последнюю использованную в своём алфавите
static void NoteLayoutUsed(HKL h) {
    const int s = ScriptOfLayout(h);
    if (s == 1) g_lastLat = (UINT_PTR)h;
    else if (s == 2) g_lastCyr = (UINT_PTR)h;
}

// Последовательность раскладок для повторных нажатий двойного Ctrl, начиная с исходной:
// исходная -> раскладки другого алфавита (последние использованные первыми) -> остальные раскладки того же алфавита.
// При двух раскладках это просто переключение туда и обратно, при трёх — круг из трёх.
static std::vector<HKL> LayoutSeq(HKL home, const Cycle& cy) {
    std::vector<HKL> seq;
    seq.push_back(home);
    const bool homeLat = ScriptOfLayout(home) == 1;
    const std::vector<HKL>& opp = homeLat ? cy.cyr : cy.lat;
    const std::vector<HKL>& same = homeLat ? cy.lat : cy.cyr;
    for (size_t i = 0; i < opp.size(); ++i) seq.push_back(opp[i]);
    for (size_t i = 0; i < same.size(); ++i) {
        if (same[i] != home) seq.push_back(same[i]);
    }
    return seq;
}

// В какую раскладку превратить символ, набранный в home и находящийся сейчас в cur
static HKL NextTarget(HKL home, HKL cur, const Cycle& cy) {
    const std::vector<HKL> seq = LayoutSeq(home, cy);
    if (seq.size() < 2) return cur;
    for (size_t i = 0; i < seq.size(); ++i) {
        if (seq[i] == cur) return seq[(i + 1) % seq.size()];
    }
    return seq[1];
}

// Если в системе ровно две раскладки — вторая из них
static HKL OtherLayoutOfTwo(HKL h) {
    const std::vector<HKL> v = InstalledLayouts();
    if (v.size() != 2) return NULL;
    if (v[0] == h) return v[1];
    if (v[1] == h) return v[0];
    return NULL;
}

// Раскладка, в которой на самом деле идёт набор.
// Обычно это то, что сообщает окно. Но некоторые приложения (например, Epic Games Launcher) не сообщают о смене
// раскладки: после нашего переключения они продолжают называть старую. Тогда используем то, что мы переключили сами,
// пока окно не начнёт сообщать что-то другое.
static HKL EffectiveLayout() {
    const HKL reported = FgLayout();
    const UINT_PTR a = g_assumedHkl;
    if (a != 0) {
        if (g_assumedHwnd == (UINT_PTR)GetForegroundWindow() && g_assumedStale == (UINT_PTR)reported) return (HKL)a;
        g_assumedHkl = 0;  // окно сменилось или снова сообщает актуальную раскладку — доверяем ему
    }
    return reported;
}

// ---------------------------------------------------------------- SendInput
static INPUT KeyInput(WORD vk, bool up) {
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (vk == VK_LEFT ? KEYEVENTF_EXTENDEDKEY : 0);
    in.ki.dwExtraInfo = kMarker;
    return in;
}

static INPUT UniInput(wchar_t ch, bool up) {
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = 0;
    in.ki.wScan = ch;
    in.ki.dwFlags = KEYEVENTF_UNICODE | (up ? KEYEVENTF_KEYUP : 0);
    in.ki.dwExtraInfo = kMarker;
    return in;
}

// Символ вводим как при ручном наборе: пробел — настоящей клавишей (Unicode-события с пробелом
// некоторые приложения теряют), остальное — Unicode-событием
static void AddTypedChar(std::vector<INPUT>& v, wchar_t ch) {
    if (ch == L' ') {
        v.push_back(KeyInput(VK_SPACE, false));
        v.push_back(KeyInput(VK_SPACE, true));
    } else {
        v.push_back(UniInput(ch, false));
        v.push_back(UniInput(ch, true));
    }
}

static void AddTypedText(std::vector<INPUT>& v, const std::wstring& s) {
    for (wchar_t ch : s) AddTypedChar(v, ch);
}

// Делит текст на пробелы в начале, середину и пробелы в конце
static void SplitEdgeSpaces(const std::wstring& s, std::wstring& lead, std::wstring& core, std::wstring& trail) {
    size_t b = 0, e = s.size();
    while (b < e && s[b] == L' ') ++b;
    while (e > b && s[e - 1] == L' ') --e;
    lead = s.substr(0, b);
    core = s.substr(b, e - b);
    trail = s.substr(e);
}

static void SendBatch(const std::vector<INPUT>& v) {
    const size_t chunk = 64;
    for (size_t i = 0; i < v.size(); i += chunk) {
        UINT cnt = (UINT)((v.size() - i) < chunk ? (v.size() - i) : chunk);
        SendInput(cnt, const_cast<INPUT*>(&v[i]), sizeof(INPUT));
        Sleep(1);
    }
}

static void AddChord(std::vector<INPUT>& v, WORD mod, WORD key) {
    v.push_back(KeyInput(mod, false));
    v.push_back(KeyInput(key, false));
    v.push_back(KeyInput(key, true));
    v.push_back(KeyInput(mod, true));
}

static void AddReselect(std::vector<INPUT>& v, size_t n) {
    v.push_back(KeyInput(VK_SHIFT, false));
    for (size_t i = 0; i < n; ++i) {
        v.push_back(KeyInput(VK_LEFT, false));
        v.push_back(KeyInput(VK_LEFT, true));
    }
    v.push_back(KeyInput(VK_SHIFT, true));
}

static void SendChord(WORD mod, WORD key) {
    std::vector<INPUT> v;
    AddChord(v, mod, key);
    SendBatch(v);
}

// ---------------------------------------------------------------- переключение раскладки
// Эмуляция системной горячей клавиши смены раскладки (Alt+Shift / Ctrl+Shift / `)

// Тип горячей клавиши из настроек Windows: 1 = Alt+Shift, 2 = Ctrl+Shift, 3 = отключена, 4 = клавиша ` (Ё)
static int ReadSystemHotkeyType() {
    int type = 1;  // по умолчанию в Windows — Alt+Shift
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Keyboard Layout\\Toggle", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        const wchar_t* names[2] = {L"Language Hotkey", L"Hotkey"};
        for (int i = 0; i < 2; ++i) {
            wchar_t buf[16] = {0};
            DWORD sz = sizeof(buf) - sizeof(wchar_t);
            DWORD t = 0;
            if (RegQueryValueExW(k, names[i], NULL, &t, (LPBYTE)buf, &sz) == ERROR_SUCCESS && t == REG_SZ) {
                type = _wtoi(buf);
                break;
            }
        }
        RegCloseKey(k);
    }
    return type;
}

static bool SendLayoutHotkey() {
    const int t = ReadSystemHotkeyType();
    std::vector<INPUT> v;
    if (t == 1) {          // Alt+Shift
        v.push_back(KeyInput(VK_LMENU, false));
        v.push_back(KeyInput(VK_LSHIFT, false));
        v.push_back(KeyInput(VK_LSHIFT, true));
        v.push_back(KeyInput(VK_LMENU, true));
    } else if (t == 2) {   // Ctrl+Shift
        v.push_back(KeyInput(VK_LCONTROL, false));
        v.push_back(KeyInput(VK_LSHIFT, false));
        v.push_back(KeyInput(VK_LSHIFT, true));
        v.push_back(KeyInput(VK_LCONTROL, true));
    } else if (t == 4) {   // клавиша ` (Ё)
        v.push_back(KeyInput(VK_OEM_3, false));
        v.push_back(KeyInput(VK_OEM_3, true));
    } else {
        return false;      // горячая клавиша отключена в настройках Windows
    }
    SendBatch(v);
    return true;
}

// Ждём, пока раскладка активного окна изменится (на любую). true — изменилась.
static bool WaitLayoutChange(HKL before, DWORD ms) {
    const DWORD t0 = GetTickCount();
    for (;;) {
        if (FgLayout() != before) return true;
        if (GetTickCount() - t0 >= ms) return false;
        Sleep(10);
    }
}

// Переключить системную раскладку на target
static void SwitchLayoutTo(HKL target) {
    if (!g_cfg.switchLayout || !target) return;
    if (EffectiveLayout() == target) return;

    const std::vector<HKL> list = InstalledLayouts();
    if (list.size() < 2 || std::find(list.begin(), list.end(), target) == list.end()) return;
    int n = (int)list.size();
    if (n > 8) n = 8;

    // Нажимаем горячую клавишу и смотрим, что изменилось (чтобы дойти до нужной раскладки, достаточно n-1 нажатий).
    // Если изменений не видно вообще, приложение могло просто не сообщить о смене раскладки — повторное нажатие
    // вернуло бы раскладку обратно, поэтому останавливаемся.
    for (int i = 0; i < n - 1; ++i) {
        const HKL before = FgLayout();
        if (EffectiveLayout() == target) return;
        if (!SendLayoutHotkey()) return;
        if (!WaitLayoutChange(before, 400)) {
            // Окно не сообщает о смене раскладки (так ведут себя некоторые приложения, например Epic Games Launcher).
            // При двух раскладках одно нажатие горячей клавиши гарантированно включило нужную — запоминаем это,
            // чтобы дальше не полагаться на устаревшие данные окна.
            if (list.size() == 2) {
                g_assumedStale = (UINT_PTR)before;
                g_assumedHwnd = (UINT_PTR)GetForegroundWindow();
                g_assumedHkl = (UINT_PTR)target;
            }
            return;
        }
    }
}

// ANSI-кодовая страница раскладки (1251 для русской, 1252 для английской)
static UINT LayoutCodePage(HKL h) {
    wchar_t buf[16] = {0};
    if (!GetLocaleInfoW(MAKELCID(LOWORD((UINT_PTR)h), SORT_DEFAULT), LOCALE_IDEFAULTANSICODEPAGE, buf, 16)) return 0;
    return (UINT)_wtoi(buf);
}

// Можно ли символ закодировать в ANSI-кодовую страницу без потерь
static bool FitsCodePage(UINT cp, wchar_t c) {
    if (c < 0x80 || !cp) return true;
    char b[4] = {0};
    BOOL used = FALSE;
    const int n = WideCharToMultiByte(cp, WC_NO_BEST_FIT_CHARS, &c, 1, b, 4, NULL, &used);
    return n >= 1 && !used;
}

// Нужно ли включить целевую раскладку до ввода текста. Некоторые приложения переводят Unicode-ввод в ANSI
// по кодовой странице раскладки, активной в их потоке в момент приёма, и символы, которых в ней нет
// (кириллица при английской раскладке), превращаются в "?".
static bool NeedLayoutFirst(const std::wstring& text, HKL target) {
    if (!g_cfg.switchLayout || !target) return false;
    const HKL cur = EffectiveLayout();
    if (cur == target) return false;
    // окно не сообщает о смене раскладки: ждать подтверждения было бы лишней задержкой перед вводом
    if (g_assumedHkl != 0 && g_assumedHwnd == (UINT_PTR)GetForegroundWindow()) return false;
    const UINT cp = LayoutCodePage(cur);
    for (size_t i = 0; i < text.size(); ++i) {
        if (!FitsCodePage(cp, text[i])) return true;
    }
    return false;
}

// ---------------------------------------------------------------- буфер обмена
static bool OpenClip() {
    for (int i = 0; i < 20; ++i) {
        if (OpenClipboard(g_hwnd)) return true;
        Sleep(10);
    }
    return false;
}

// Положить в (уже открытый и очищенный) буфер флаг-формат со значением 0
static void AddFlagFormat(UINT fmt) {
    if (!fmt) return;
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (!g) return;
    DWORD* p = (DWORD*)GlobalLock(g);
    if (!p) { GlobalFree(g); return; }
    *p = 0;
    GlobalUnlock(g);
    if (!SetClipboardData(fmt, g)) GlobalFree(g);
}

// Запретить попадание содержимого буфера в историю Windows (Win+V), облачный буфер и менеджеры буфера
static void AddHistoryExclusion(bool skipHistory, bool skipCloud, bool skipMonitor) {
    if (!skipHistory) AddFlagFormat(g_fmtHistory);
    if (!skipCloud) AddFlagFormat(g_fmtCloud);
    if (!skipMonitor) AddFlagFormat(g_fmtMonitor);
}

struct ClipItem {
    UINT fmt;
    std::vector<char> data;
};

struct ClipBackup {
    std::vector<ClipItem> items;

    bool Save() {
        if (!OpenClip()) return false;
        UINT f = 0;
        while ((f = EnumClipboardFormats(f)) != 0) {
            if (f == CF_BITMAP || f == CF_PALETTE || f == CF_METAFILEPICT || f == CF_ENHMETAFILE) continue;
            if (f >= 0x80 && f <= 0x8F) continue;
            if (f >= 0x300 && f <= 0x3FF) continue;
            HANDLE h = GetClipboardData(f);
            if (!h) continue;
            SIZE_T sz = GlobalSize(h);
            if (sz == 0) continue;
            const char* p = (const char*)GlobalLock(h);
            if (!p) continue;
            ClipItem it;
            it.fmt = f;
            it.data.assign(p, p + sz);
            GlobalUnlock(h);
            items.push_back(std::move(it));
        }
        CloseClipboard();
        return true;
    }

    void Restore() {
        if (!OpenClip()) return;
        EmptyClipboard();
        bool hasHistory = false, hasCloud = false, hasMonitor = false;
        for (const ClipItem& it : items) {
            if (it.fmt == g_fmtHistory) hasHistory = true;
            if (it.fmt == g_fmtCloud) hasCloud = true;
            if (it.fmt == g_fmtMonitor) hasMonitor = true;
            HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, it.data.size());
            if (!g) continue;
            void* p = GlobalLock(g);
            if (!p) { GlobalFree(g); continue; }
            memcpy(p, it.data.data(), it.data.size());
            GlobalUnlock(g);
            if (!SetClipboardData(it.fmt, g)) GlobalFree(g);
        }
        // Возврат прежнего содержимого не должен второй раз попадать в историю
        if (!items.empty()) AddHistoryExclusion(hasHistory, hasCloud, hasMonitor);
        CloseClipboard();
    }
};

// 1 — текст прочитан, 0 — в буфере нет текста, -1 — буфер сейчас занят другим приложением
static int ReadClipText(std::wstring& out) {
    if (!OpenClip()) return -1;
    int result = 0;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* p = (const wchar_t*)GlobalLock(h);
        if (p) {
            out = p;
            GlobalUnlock(h);
            result = 1;
        }
    }
    CloseClipboard();
    return result;
}

static bool WriteClipText(const std::wstring& s) {
    if (!OpenClip()) return false;
    EmptyClipboard();
    size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!g) { CloseClipboard(); return false; }
    void* p = GlobalLock(g);
    if (!p) { GlobalFree(g); CloseClipboard(); return false; }
    memcpy(p, s.c_str(), bytes);
    GlobalUnlock(g);
    bool ok = SetClipboardData(CF_UNICODETEXT, g) != NULL;
    if (!ok) GlobalFree(g);
    else AddHistoryExclusion(false, false, false);  // временный текст не должен попасть в историю
    CloseClipboard();
    return ok;
}

// Копирует выделение через Ctrl+C. Перед копированием кладём в буфер уникальную метку:
// выделением считается только текст, пришедший на её место. Так прежнее содержимое буфера
// никогда не будет принято за выделение (например, если буфер менял менеджер буфера).
static bool CopySelectionViaClipboard(std::wstring& out) {
    wchar_t tag[64];
    _snwprintf(tag, 63, L"__LS_%lu_%lu__", (unsigned long)GetTickCount(), (unsigned long)GetCurrentThreadId());
    tag[63] = 0;
    if (!WriteClipText(tag)) return false;

    DWORD seq = GetClipboardSequenceNumber();
    SendChord(VK_CONTROL, 'C');

    const DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < 600) {
        const DWORD now = GetClipboardSequenceNumber();
        if (now != seq) {
            std::wstring t;
            const int r = ReadClipText(t);
            if (r < 0) { Sleep(10); continue; }  // буфер занят — повторим
            if (r == 1 && !t.empty() && t != tag) { out = t; return true; }
            seq = now;  // буфер тронул кто-то другой, а выделения в нём нет — продолжаем ждать
        }
        Sleep(10);
    }
    return false;
}

// ---------------------------------------------------------------- UI Automation: что сейчас в фокусе
static bool CreateUia(IUIAutomation** out) {
#ifdef _MSC_VER
    HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation), NULL, CLSCTX_INPROC_SERVER,
                                  __uuidof(IUIAutomation), (void**)out);
#else
    HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER,
                                  IID_IUIAutomation, (void**)out);
#endif
    return SUCCEEDED(hr) && *out;
}

static bool IsBrowserWindow(HWND fg) {
    wchar_t cls[64] = {0};
    if (fg) GetClassNameW(fg, cls, 64);
    return wcsncmp(cls, L"Chrome_WidgetWin", 16) == 0 || wcscmp(cls, L"MozillaWindowClass") == 0;
}

// Элементы, в которые заведомо нельзя вводить текст
static bool IsNonTextControl(CONTROLTYPEID type) {
    switch (type) {
        case UIA_ButtonControlTypeId:
        case UIA_CheckBoxControlTypeId:
        case UIA_RadioButtonControlTypeId:
        case UIA_HyperlinkControlTypeId:
        case UIA_ImageControlTypeId:
        case UIA_ListControlTypeId:
        case UIA_ListItemControlTypeId:
        case UIA_MenuControlTypeId:
        case UIA_MenuBarControlTypeId:
        case UIA_MenuItemControlTypeId:
        case UIA_ProgressBarControlTypeId:
        case UIA_ScrollBarControlTypeId:
        case UIA_SliderControlTypeId:
        case UIA_StatusBarControlTypeId:
        case UIA_TabControlTypeId:
        case UIA_TabItemControlTypeId:
        case UIA_TextControlTypeId:
        case UIA_ToolBarControlTypeId:
        case UIA_ToolTipControlTypeId:
        case UIA_TreeControlTypeId:
        case UIA_TreeItemControlTypeId:
        case UIA_TitleBarControlTypeId:
        case UIA_SeparatorControlTypeId:
        case UIA_ThumbControlTypeId:
        case UIA_HeaderControlTypeId:
        case UIA_HeaderItemControlTypeId:
        case UIA_SplitButtonControlTypeId:
        case UIA_DataItemControlTypeId:
            return true;
        default:
            return false;
    }
}

// 1 — значение можно менять, 0 — только чтение, -1 — у элемента нет значения
static int ValuePatternState(IUIAutomationElement* el) {
    int state = -1;
    IUnknown* unk = NULL;
    if (SUCCEEDED(el->GetCurrentPattern(UIA_ValuePatternId, &unk)) && unk) {
        IUIAutomationValuePattern* vp = NULL;
        if (SUCCEEDED(unk->QueryInterface(LS_IID(IUIAutomationValuePattern), (void**)&vp)) && vp) {
            BOOL ro = FALSE;
            if (SUCCEEDED(vp->get_CurrentIsReadOnly(&ro))) state = ro ? 0 : 1;
            vp->Release();
        }
        unk->Release();
    }
    return state;
}

// ARIA-роль textbox / searchbox / combobox (поля ввода, сделанные из div)
static bool HasTextAriaRole(IUIAutomationElement* el) {
    bool result = false;
    BSTR role = NULL;
    if (SUCCEEDED(el->get_CurrentAriaRole(&role)) && role) {
        result = wcsstr(role, L"textbox") || wcsstr(role, L"searchbox") || wcsstr(role, L"combobox");
        SysFreeString(role);
    }
    return result;
}

// Выделение/курсор внутри элемента помечены как редактируемые (contenteditable и подобное)
static bool TextRangeIsEditable(IUIAutomationElement* el) {
    bool editable = false;
    IUnknown* unk = NULL;
    if (SUCCEEDED(el->GetCurrentPattern(UIA_TextPatternId, &unk)) && unk) {
        IUIAutomationTextPattern* tp = NULL;
        if (SUCCEEDED(unk->QueryInterface(LS_IID(IUIAutomationTextPattern), (void**)&tp)) && tp) {
            IUIAutomationTextRangeArray* arr = NULL;
            if (SUCCEEDED(tp->GetSelection(&arr)) && arr) {
                int len = 0;
                arr->get_Length(&len);
                if (len > 0) {
                    IUIAutomationTextRange* range = NULL;
                    if (SUCCEEDED(arr->GetElement(0, &range)) && range) {
                        VARIANT v;
                        VariantInit(&v);
                        if (SUCCEEDED(range->GetAttributeValue(UIA_IsReadOnlyAttributeId, &v)) && v.vt == VT_BOOL)
                            editable = (v.boolVal == VARIANT_FALSE);
                        VariantClear(&v);
                        range->Release();
                    }
                }
                arr->Release();
            }
            tp->Release();
        }
        unk->Release();
    }
    return editable;
}

// Читает выделенный текст через UI Automation (TextPattern) — без Ctrl+C и без буфера обмена.
// true — найдено непустое выделение разумного размера.
static bool ReadSelectionViaUia(std::wstring& out) {
    bool ok = false;
    IUIAutomation* uia = NULL;
    IUIAutomationElement* el = NULL;
    if (CreateUia(&uia) && SUCCEEDED(uia->GetFocusedElement(&el)) && el) {
        IUnknown* unk = NULL;
        if (SUCCEEDED(el->GetCurrentPattern(UIA_TextPatternId, &unk)) && unk) {
            IUIAutomationTextPattern* tp = NULL;
            if (SUCCEEDED(unk->QueryInterface(LS_IID(IUIAutomationTextPattern), (void**)&tp)) && tp) {
                IUIAutomationTextRangeArray* arr = NULL;
                if (SUCCEEDED(tp->GetSelection(&arr)) && arr) {
                    int len = 0;
                    arr->get_Length(&len);
                    if (len == 1) {
                        IUIAutomationTextRange* range = NULL;
                        if (SUCCEEDED(arr->GetElement(0, &range)) && range) {
                            BSTR s = NULL;
                            if (SUCCEEDED(range->GetText((int)kMaxUiaSelection + 1, &s)) && s) {
                                const size_t n = SysStringLen(s);
                                if (n > 0 && n <= kMaxUiaSelection) {
                                    out.assign(s, n);
                                    ok = true;
                                }
                                SysFreeString(s);
                            }
                            range->Release();
                        }
                    }
                    arr->Release();
                }
                tp->Release();
            }
            unk->Release();
        }
    }
    if (el) el->Release();
    if (uia) uia->Release();
    return ok;
}

// true, если фокус стоит в текстовом поле интерфейса браузера (адресная строка, поиск по странице),
// а не внутри веб-страницы. Веб-страница в UI Automation всегда имеет предка типа "Document".
static bool FocusIsBrowserUiEdit() {
    bool result = false;
    IUIAutomation* uia = NULL;
    IUIAutomationElement* focused = NULL;
    IUIAutomationTreeWalker* walker = NULL;

    if (CreateUia(&uia) && SUCCEEDED(uia->GetFocusedElement(&focused)) && focused) {
        CONTROLTYPEID type = 0;
        focused->get_CurrentControlType(&type);
        if (type == UIA_EditControlTypeId || type == UIA_ComboBoxControlTypeId) {
            if (SUCCEEDED(uia->get_ControlViewWalker(&walker)) && walker) {
                bool inDocument = false;
                IUIAutomationElement* cur = focused;
                cur->AddRef();
                for (int i = 0; i < 40 && cur; ++i) {
                    IUIAutomationElement* parent = NULL;
                    if (FAILED(walker->GetParentElement(cur, &parent)) || !parent) {
                        cur->Release();
                        cur = NULL;
                        break;
                    }
                    cur->Release();
                    cur = parent;
                    CONTROLTYPEID pt = 0;
                    cur->get_CurrentControlType(&pt);
                    if (pt == UIA_DocumentControlTypeId) { inDocument = true; break; }
                }
                if (cur) cur->Release();
                result = !inDocument;
            }
        }
    }

    if (walker) walker->Release();
    if (focused) focused->Release();
    if (uia) uia->Release();
    return result;
}

enum FocusKind { FK_UNKNOWN = 0, FK_TEXT = 1, FK_NOTEXT = 2 };

// Определяем, находится ли фокус в поле ввода текста.
// FK_NOTEXT — уверены, что это НЕ поле ввода (кнопка, список, значок, страница браузера и т.п.).
// FK_UNKNOWN — приложение ничего определённого не сообщает (работаем как обычно).
static FocusKind GetFocusKind() {
    FocusKind result = FK_UNKNOWN;
    IUIAutomation* uia = NULL;
    IUIAutomationElement* el = NULL;

    if (CreateUia(&uia) && SUCCEEDED(uia->GetFocusedElement(&el)) && el) {
        const bool browser = IsBrowserWindow(GetForegroundWindow());
        CONTROLTYPEID type = 0;
        el->get_CurrentControlType(&type);

        if (IsNonTextControl(type)) {
            result = FK_NOTEXT;
        } else {
            const int value = ValuePatternState(el);
            if (value == 1) result = FK_TEXT;
            else if (value == 0) result = FK_NOTEXT;
            else if (type == UIA_EditControlTypeId) result = FK_TEXT;
            else if (browser) {
                // В браузере поле ввода — это Edit/ComboBox, элемент с ролью textbox
                // или редактируемый блок (div с contenteditable). Всё остальное — страница.
                const bool text = type == UIA_ComboBoxControlTypeId || HasTextAriaRole(el) || TextRangeIsEditable(el);
                result = text ? FK_TEXT : FK_NOTEXT;
            }

            // В Word и подобных программах документ может некорректно сообщать себя read-only
            if (!browser && type == UIA_DocumentControlTypeId && result == FK_NOTEXT) result = FK_UNKNOWN;
        }
    }

    if (el) el->Release();
    if (uia) uia->Release();
    return result;
}

// true, если в точке pt лежит обычный нередактируемый текст страницы, не относящийся к элементу focused
static bool IsStaticTextAt(IUIAutomation* uia, IUIAutomationTreeWalker* walker, IUIAutomationElement* focused,
                           POINT pt) {
    bool isStatic = false;
    IUIAutomationElement* el = NULL;
    if (SUCCEEDED(uia->ElementFromPoint(pt, &el)) && el) {
        CONTROLTYPEID type = 0;
        el->get_CurrentControlType(&type);
        if (IsNonTextControl(type)) {
            isStatic = true;
            IUIAutomationElement* cur = el;
            cur->AddRef();
            for (int i = 0; i < 12 && cur; ++i) {
                // элемент внутри поля с фокусом или внутри редактируемого элемента — это не просто текст страницы
                BOOL same = FALSE;
                if (SUCCEEDED(uia->CompareElements(cur, focused, &same)) && same) { isStatic = false; break; }
                if (ValuePatternState(cur) == 1 || HasTextAriaRole(cur)) { isStatic = false; break; }
                IUIAutomationElement* parent = NULL;
                if (FAILED(walker->GetParentElement(cur, &parent)) || !parent) {
                    cur->Release();
                    cur = NULL;
                    break;
                }
                cur->Release();
                cur = parent;
            }
            if (cur) cur->Release();
        }
        el->Release();
    }
    return isStatic;
}

// false — только если есть явное доказательство, что выделен обычный текст страницы вне поля, где стоит фокус
// (например, выделен текст ответа в чате, а фокус остался в поле ввода).
// Если определить нельзя (редакторы вроде Google Docs, которые рисуют текст сами и ничего не отдают
// в UI Automation), выделение считается выделением в редакторе.
static bool SelectionBelongsToFocus(bool byMouse, POINT down, POINT up) {
    if (!byMouse) return true;
    bool belongs = true;
    IUIAutomation* uia = NULL;
    IUIAutomationElement* focused = NULL;
    IUIAutomationTreeWalker* walker = NULL;

    if (CreateUia(&uia) && SUCCEEDED(uia->GetFocusedElement(&focused)) && focused) {
        // нажатие или отпускание кнопки внутри элемента с фокусом — выделение в нём
        bool inside = false;
        RECT r;
        ZeroMemory(&r, sizeof(r));
        if (SUCCEEDED(focused->get_CurrentBoundingRectangle(&r))) {
            const LONG w = r.right - r.left, h = r.bottom - r.top;
            if (w >= 50 && h >= 10) {
                InflateRect(&r, 8, 8);
                inside = PtInRect(&r, down) || PtInRect(&r, up);
            }
        }
        // иначе отклоняем выделение, только если оба конца лежат на обычном тексте страницы
        if (!inside && SUCCEEDED(uia->get_ControlViewWalker(&walker)) && walker) {
            if (IsStaticTextAt(uia, walker, focused, down) && IsStaticTextAt(uia, walker, focused, up))
                belongs = false;
        }
    }

    if (walker) walker->Release();
    if (focused) focused->Release();
    if (uia) uia->Release();
    return belongs;
}

// ---------------------------------------------------------------- определение игры
struct GameInfo {
    bool game;         // активное окно — вероятно игра
    bool cursorShown;  // системный курсор мыши виден
};

// Курсор виден? (CURSOR_SHOWING; игры, скрывающие курсор через SetCursor(NULL), тоже считаются со скрытым курсором)
static bool CursorShown() {
    CURSORINFO ci;
    ZeroMemory(&ci, sizeof(ci));
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci)) return true;
    return (ci.flags & CURSOR_SHOWING) != 0 && ci.hCursor != NULL;
}

// Окно на весь монитор и без заголовка (эксклюзивный или "безрамочный" полноэкранный режим)
static bool IsFullscreenWindow(HWND fg) {
    if (!fg) return false;
    wchar_t cls[64] = {0};
    GetClassNameW(fg, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") ||
        !wcscmp(cls, L"Shell_SecondaryTrayWnd") || !wcscmp(cls, L"ConsoleWindowClass") ||
        !wcscmp(cls, L"CASCADIA_HOSTING_WINDOW_CLASS") || IsBrowserWindow(fg))
        return false;

    const LONG_PTR style = GetWindowLongPtrW(fg, GWL_STYLE);
    if ((style & WS_CAPTION) == WS_CAPTION) return false;  // обычное окно с заголовком (в т.ч. развёрнутое)

    RECT r;
    if (!GetWindowRect(fg, &r)) return false;
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right &&
           r.bottom >= mi.rcMonitor.bottom;
}

// Курсор заперт в рамке меньше экрана (ClipCursor) — так делают игры в окне
static bool CursorClipped() {
    RECT c;
    if (!GetClipCursor(&c)) return false;
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return c.left > vx || c.top > vy || c.right < vx + vw || c.bottom < vy + vh;
}

static GameInfo QueryGame(HWND fg) {
    GameInfo gi;
    gi.game = false;
    gi.cursorShown = true;
    if (!g_cfg.gameFilter) return gi;
    gi.cursorShown = CursorShown();
    gi.game = IsFullscreenWindow(fg) || (!gi.cursorShown && CursorClipped());
    return gi;
}

// Клавиши, которыми в играх обычно открывают чат или консоль
static bool IsChatTriggerVk(DWORD vk) {
    return vk == VK_RETURN || vk == 'T' || vk == 'Y' || vk == 'U' || vk == VK_OEM_2 || vk == VK_OEM_3;
}

// В игре без открытого чата и без выделенного текста двойной Ctrl ни к чему не ведёт
static bool GameIgnoresTrigger() {
    if (g_cfg.switchEmpty) return false;  // двойной Ctrl переключает раскладку и в игре
    if (!g_cfg.gameFilter) return false;
    if (g_gameArmed || g_gamePending) return false;
    if (!QueryGame(GetForegroundWindow()).game) return false;
    Lock l;
    return !g_selHint;
}

// ---------------------------------------------------------------- конвертация выделенного текста
struct SelResult {
    std::wstring text;  // результат
    HKL endLayout;      // раскладка, в которую нужно переключиться (по последней букве), или NULL
};

// Раскладка из списка, которая лучше всего покрывает символы текста (при равенстве — первая в списке)
static HKL PickHome(const std::vector<HKL>& cand, const std::wstring& text, bool cyr) {
    if (cand.empty()) return NULL;
    if (cand.size() == 1) return cand[0];
    HKL best = cand[0];
    size_t bestCount = 0;
    bool first = true;
    for (size_t k = 0; k < cand.size(); ++k) {
        const std::set<wchar_t>& chars = LayoutChars(cand[k]);
        size_t cnt = 0;
        for (size_t i = 0; i < text.size(); ++i) {
            const wchar_t c = text[i];
            if ((cyr ? lm::IsCyrillic(c) : lm::IsLatin(c)) && chars.count(c)) ++cnt;
        }
        if (first || cnt > bestCount) { best = cand[k]; bestCount = cnt; first = false; }
    }
    return best;
}

static wchar_t ConvChar(wchar_t c, HKL home, HKL target) {
    if (target == home) return c;
    const lm::CharMap& m = MapFor(home, target);
    const lm::CharMap::const_iterator it = m.find(c);
    return it == m.end() ? c : it->second;
}

// Конвертирует выделенный текст. src — текст в исходном виде, step — номер нажатия в цепочке (1 — первое).
// Набранное латиницей превращается в кириллицу, набранное кириллицей — в латиницу; при нескольких раскладках
// повторные нажатия перебирают их по кругу, возвращаясь к исходному тексту.
static SelResult ConvertSelectionText(const std::wstring& src, int step, const Cycle& cy, bool defaultCyr) {
    SelResult r;
    r.text = src;
    r.endLayout = NULL;
    if (cy.lat.empty() || cy.cyr.empty()) return r;

    const HKL L0 = PickHome(cy.lat, src, false);
    const HKL C0 = PickHome(cy.cyr, src, true);
    const std::vector<unsigned char> cls = lm::ClassifyText(src, MapFor(L0, C0), MapFor(C0, L0), defaultCyr);

    // круг раскладок для каждого алфавита (исходная -> другого алфавита -> остальные того же алфавита)
    const std::vector<HKL> seqL = LayoutSeq(L0, cy), seqC = LayoutSeq(C0, cy);
    const HKL tLat = seqL[(size_t)step % seqL.size()];  // куда превращаем символы, набранные латиницей
    const HKL tCyr = seqC[(size_t)step % seqC.size()];  // куда — набранные кириллицей

    for (size_t i = 0; i < src.size(); ++i) {
        if (cls[i] == lm::CC_LATIN) r.text[i] = ConvChar(src[i], L0, tLat);
        else if (cls[i] == lm::CC_CYRILLIC) r.text[i] = ConvChar(src[i], C0, tCyr);
    }
    for (size_t i = src.size(); i-- > 0;) {
        if (cls[i] != lm::CC_KEEP && lm::IsLetter(r.text[i])) {
            r.endLayout = (cls[i] == lm::CC_LATIN) ? tLat : tCyr;
            break;
        }
    }
    return r;
}

// Цепочка повторных конвертаций выделенного текста
struct SelChain {
    bool valid = false;
    std::wstring orig;    // исходный текст (до первой конвертации цепочки)
    std::wstring result;  // что мы вставили последним
    int step = 0;
    UINT_PTR hwnd = 0;
    DWORD time = 0;
    Cycle cycle;
};
static SelChain g_selChain;

// ---------------------------------------------------------------- конвертация
// Заменять текст одной вставкой нужно только в адресной строке браузера: её подсказки и автодополнение
// реагируют на каждый Backspace и символ, из-за чего текст "печатается" по одному символу.
static bool UsePasteForBuffer() {
    if (!IsBrowserWindow(GetForegroundWindow())) return false;
    return FocusIsBrowserUiEdit();
}

// trusted — выделение гарантированно текст (игра или выделение с Ctrl), проверки поля ввода не нужны
static bool ConvertSelection(bool byMouse, POINT down, POINT up, bool trusted) {
    HWND fg = GetForegroundWindow();
    wchar_t cls[64] = {0};
    if (fg) GetClassNameW(fg, cls, 64);

    // В консолях Ctrl+C прерывает программу — там выделение не трогаем
    if (!wcscmp(cls, L"ConsoleWindowClass") || !wcscmp(cls, L"CASCADIA_HOSTING_WINDOW_CLASS")) {
        Lock l;
        g_selHint = false;
        return false;
    }

    // Выделение сделано не в том элементе, где фокус (например, текст на странице) — ничего не делаем
    if (!trusted && !SelectionBelongsToFocus(byMouse, down, up)) {
        Lock l;
        g_selHint = false;
        return true;
    }

    const bool defaultCyr = ScriptOfLayout(EffectiveLayout()) == 2;

    // Сначала пробуем прочитать выделение через UI Automation: без Ctrl+C и без буфера обмена.
    // Если приложение его не отдаёт — копируем через Ctrl+C.
    std::wstring text;
    ClipBackup bak;
    bool saved = false;
    // Текст из UIA используем, только если в нём есть что конвертировать: некоторые приложения
    // (Google Docs) отдают через UIA пустышку вместо настоящего выделения
    bool viaUia = ReadSelectionViaUia(text);
    if (viaUia && ConvertSelectionText(text, 1, BuildCycle(), defaultCyr).text == text) viaUia = false;
    if (!viaUia) {
        saved = bak.Save();
        if (!CopySelectionViaClipboard(text)) {
            if (saved) bak.Restore();
            Lock l;
            g_selHint = false;
            g_selForce = false;
            return false;
        }
    }

    { Lock l; g_selHint = false; g_selForce = false; }

    // Если выделен тот же текст, который мы только что вставили, продолжаем цепочку (следующая раскладка по кругу).
    // Иначе начинаем новую: запоминаем текущую раскладку и берём последние использованные.
    std::wstring src = text;
    int step = 1;
    Cycle cycle;
    if (g_selChain.valid && g_selChain.hwnd == (UINT_PTR)fg && text == g_selChain.result &&
        (DWORD)(GetTickCount() - g_selChain.time) < kSelChainMs) {
        src = g_selChain.orig;
        step = g_selChain.step + 1;
        cycle = g_selChain.cycle;
    } else {
        NoteLayoutUsed(EffectiveLayout());
        cycle = BuildCycle();
    }

    const SelResult res = ConvertSelectionText(src, step, cycle, defaultCyr);
    const std::wstring& conv = res.text;
    if (conv == text) {
        if (saved) bak.Restore();
        return true;
    }

    // Вставка через Ctrl+V нужна для многострочного текста (Enter мог бы отправить сообщение)
    // и для адресной строки браузера. Всё остальное заменяем обычным вводом символов поверх выделения.
    bool hasCtl = false;
    for (wchar_t c : conv) {
        if (c < 0x20) { hasCtl = true; break; }
    }

    if (hasCtl || UsePasteForBuffer()) {
        // Пробелы по краям вводим клавишей: многие поля обрезают их у вставляемого текста
        std::wstring lead, core, trail;
        SplitEdgeSpaces(conv, lead, core, trail);
        if (!saved) saved = bak.Save();  // при чтении через UIA буфер ещё не сохранён
        if (!core.empty()) WriteClipText(core);
        std::vector<INPUT> in;
        AddTypedText(in, lead);
        if (!core.empty()) AddChord(in, VK_CONTROL, 'V');
        AddTypedText(in, trail);
        SendBatch(in);
        PostMessageW(g_hwnd, WM_PLAYSOUND, 0, 0);
        Sleep(450);  // даём приложению время забрать текст из буфера
        if (saved) bak.Restore();
    } else {
        if (saved) bak.Restore();  // буфер обмена больше не нужен — возвращаем сразу
        if (NeedLayoutFirst(conv, res.endLayout)) SwitchLayoutTo(res.endLayout);
        std::vector<INPUT> in;
        AddTypedText(in, conv);
        SendBatch(in);
        PostMessageW(g_hwnd, WM_PLAYSOUND, 0, 0);
        Sleep(40);
    }

    g_selChain.valid = true;
    g_selChain.orig = src;
    g_selChain.result = conv;
    g_selChain.step = step;
    g_selChain.hwnd = (UINT_PTR)fg;
    g_selChain.time = GetTickCount();
    g_selChain.cycle = cycle;

    if (g_cfg.reselect && conv.size() <= 5000 && !hasCtl) {
        std::vector<INPUT> in;
        AddReselect(in, conv.size());
        SendBatch(in);
        Lock l;
        g_selHint = true;
        g_selByMouse = false;
    }

    SwitchLayoutTo(res.endLayout);
    return true;
}

static void ConvertBuffer(const std::vector<Rec>& snap, const Cycle& cycle, size_t forceStart = (size_t)-1,
                          HKL forceTarget = NULL, size_t forceEnd = (size_t)-1) {
    if (snap.empty()) return;
    if (!forceTarget && (cycle.lat.empty() || cycle.cyr.empty())) return;  // нужны и латинская, и кириллическая раскладки

    size_t start = 0;
    if (forceStart != (size_t)-1) {
        start = forceStart;
    } else if (!g_cfg.wholeText) {
        size_t i = snap.size();
        while (i > 0 && snap[i - 1].ch == L' ') --i;
        while (i > 0 && snap[i - 1].ch != L' ') --i;
        start = i;
    }

    // Каждый символ переходит в следующую раскладку своей цепочки: набранное латиницей — в кириллицу,
    // набранное кириллицей — в латиницу; при нескольких раскладках повторные нажатия перебирают их по кругу
    std::vector<Rec> neu;
    std::wstring newText;
    for (size_t i = start; i < snap.size(); ++i) {
        Rec r = snap[i];
        if (!r.neutral && i < forceEnd) {
            const HKL target = forceTarget ? forceTarget : NextTarget(r.home, r.cur, cycle);
            if (target != r.cur) {
                r.ch = (target == r.home) ? r.origCh : ConvChar(r.origCh, r.home, target);
                r.cur = target;
            }
        }
        neu.push_back(r);
        newText += r.ch;
    }
    const size_t n = snap.size() - start;

    bool done = false;

    // Адресная строка браузера: выделить набранный текст и заменить его одной вставкой
    if (UsePasteForBuffer()) {
        std::wstring lead, core, trail;
        SplitEdgeSpaces(newText, lead, core, trail);  // пробелы по краям вводим клавишей, их вставка может обрезаться
        ClipBackup bak;
        bool saved = bak.Save();
        if (core.empty() || WriteClipText(core)) {
            std::vector<INPUT> in;
            AddReselect(in, n);
            AddTypedText(in, lead);
            if (!core.empty()) AddChord(in, VK_CONTROL, 'V');
            AddTypedText(in, trail);
            SendBatch(in);
            PostMessageW(g_hwnd, WM_PLAYSOUND, 0, 0);
            Sleep(450);
            done = true;
        }
        if (saved) bak.Restore();
    }

    // Везде остальное: Backspace x n и ввод символов
    if (!done) {
        // сначала включаем раскладку, в которой окажется текст (если приложению это нужно, см. NeedLayoutFirst)
        HKL firstLayout = NULL;
        for (size_t i = neu.size(); i-- > 0;) {
            if (!neu[i].neutral && lm::IsLetter(neu[i].ch)) { firstLayout = neu[i].cur; break; }
        }
        if (NeedLayoutFirst(newText, firstLayout)) SwitchLayoutTo(firstLayout);

        std::vector<INPUT> in;
        for (size_t i = 0; i < n; ++i) {
            in.push_back(KeyInput(VK_BACK, false));
            in.push_back(KeyInput(VK_BACK, true));
        }
        AddTypedText(in, newText);
        SendBatch(in);
        PostMessageW(g_hwnd, WM_PLAYSOUND, 0, 0);
    }

    {
        Lock l;
        if (g_resetAfter || g_buf.size() != snap.size()) {
            g_buf.clear();
            g_chainActive = false;
        } else {
            g_buf.resize(start);
            g_buf.insert(g_buf.end(), neu.begin(), neu.end());
            g_chainActive = true;   // пока ничего не набрано, повторное нажатие продолжит цепочку
            g_chainCycle = cycle;
        }
    }

    // Системная раскладка — та, в которой оказалась последняя буква
    HKL endLayout = NULL;
    for (size_t i = neu.size(); i-- > 0;) {
        if (!neu[i].neutral && lm::IsLetter(neu[i].ch)) { endLayout = neu[i].cur; break; }
    }
    if (!endLayout) endLayout = neu.back().cur;
    SwitchLayoutTo(endLayout);
}

#include "autoswitch_host.h"  // связь с опциональным модулем авто-переключения (autoswitch.dll)

// Двойной Ctrl без введённого текста: переключаем раскладку на последнюю использованную другого алфавита
static void SwitchLayoutEmpty() {
    const HKL cur = EffectiveLayout();
    NoteLayoutUsed(cur);
    const Cycle cy = BuildCycle();
    HKL target = NULL;
    if (ScriptOfLayout(cur) == 1) { if (!cy.cyr.empty()) target = cy.cyr[0]; }
    else if (!cy.lat.empty()) target = cy.lat[0];
    if (!target || target == cur) return;
    SwitchLayoutTo(target);
    PostMessageW(g_hwnd, WM_PLAYSOUND, 0, 0);
}

static unsigned __stdcall ConvertThread(void*) {
    HRESULT coHr = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    Sleep(30);  // чтобы физическое отпускание Ctrl успело обработаться

    bool hint, byMouse, force, chain;
    POINT down, up;
    Cycle cycle;
    std::vector<Rec> snap;
    {
        Lock l;
        hint = g_selHint;
        byMouse = g_selByMouse;
        force = g_selForce;
        down = g_selDownPt;
        up = g_selUpPt;
        snap = g_buf;
        chain = g_chainActive;
        if (chain) cycle = g_chainCycle;
        g_resetAfter = false;
    }
    if (!chain) {
        // Новая цепочка: запоминаем текущую раскладку и берём последние использованные
        NoteLayoutUsed(EffectiveLayout());
        cycle = BuildCycle();
    }

    // В игре набранное учитываем, только если чат открыт и набор не "протух"
    const GameInfo gi = QueryGame(GetForegroundWindow());
    if (gi.game) {
        const bool active = (g_gameArmed || g_gamePending) &&
                            (DWORD)(GetTickCount() - g_gameLastKey.load()) <= kChatIdleMs + 1000;
        if (!active) snap.clear();
    }

    // В игре и при выделении с Ctrl проверки "это поле ввода?" не нужны
    const bool trusted = gi.game || force;

    if (hint || !snap.empty()) {
        if (!trusted && GetFocusKind() == FK_NOTEXT) {
            // Фокус не в поле ввода — это был "ввод в никуда". Ничего не трогаем и забываем набранное.
            { Lock l; g_buf.clear(); g_selHint = false; }
            if (g_cfg.switchEmpty) SwitchLayoutEmpty();
        } else {
            bool done = false;
            if (hint) done = ConvertSelection(byMouse, down, up, trusted);
            if (!done) ConvertBuffer(snap, cycle, AsManualStart(snap));
        }
    } else if (g_cfg.switchEmpty) {
        SwitchLayoutEmpty();  // текст не вводился: просто переключаем раскладку
    }

    if (gi.game && (g_gameArmed || g_gamePending)) g_gameLastKey = GetTickCount();

    if (SUCCEEDED(coHr)) CoUninitialize();
    g_cooldownUntil = GetTickCount() + 400;  // защита от случайных повторных срабатываний
    g_busy = false;
    return 0;
}

// ---------------------------------------------------------------- хуки
static bool IsModifierVk(DWORD vk) {
    switch (vk) {
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
        case VK_MENU: case VK_LMENU: case VK_RMENU:
        case VK_LWIN: case VK_RWIN:
        case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL:
            return true;
    }
    return false;
}

// Пользователь сам переключил раскладку горячей клавишей. Если мы ведём учёт раскладки вместо окна,
// которое о ней не сообщает, при двух раскладках она просто сменилась на другую.
static void OnManualLayoutSwitch() {
    const UINT_PTR a = g_assumedHkl;
    if (a == 0) return;
    const HKL other = OtherLayoutOfTwo((HKL)a);
    g_assumedHkl = (UINT_PTR)other;  // NULL, если раскладок не две: учёт сбрасывается
}

// Отслеживаем жест смены раскладки (Alt+Shift / Ctrl+Shift по настройкам Windows, Win+Space)
static void TrackLayoutChord(DWORD vk, bool down) {
    int bit = 0;
    if (vk == VK_LSHIFT || vk == VK_RSHIFT) bit = 1;
    else if (vk == VK_LMENU || vk == VK_RMENU) bit = 2;
    else if (vk == VK_LCONTROL || vk == VK_RCONTROL) bit = 4;
    else if (vk == VK_LWIN || vk == VK_RWIN) bit = 8;

    if (bit == 0) {  // обычная клавиша
        if (down) {
            if (vk == VK_SPACE && (g_keyDown[VK_LWIN] || g_keyDown[VK_RWIN])) OnManualLayoutSwitch();
            if (g_chordMask) g_chordDirty = true;
        }
        return;
    }
    if (down) {
        g_chordMask |= bit;
        return;
    }

    // модификатор отпущен: жест завершён, когда отпущены все модификаторы
    const bool anyHeld = g_keyDown[VK_LSHIFT] || g_keyDown[VK_RSHIFT] || g_keyDown[VK_LMENU] || g_keyDown[VK_RMENU] ||
                         g_keyDown[VK_LCONTROL] || g_keyDown[VK_RCONTROL] || g_keyDown[VK_LWIN] || g_keyDown[VK_RWIN];
    if (anyHeld) return;
    if (!g_chordDirty && g_chordMask != 0) {
        const int t = ReadSystemHotkeyType();
        if ((t == 1 && g_chordMask == 3) || (t == 2 && g_chordMask == 5)) OnManualLayoutSwitch();
    }
    g_chordMask = 0;
    g_chordDirty = false;
}

static LRESULT CALLBACK KbProc(int code, WPARAM wp, LPARAM lp) {
    if (code != HC_ACTION) return CallNextHookEx(NULL, code, wp, lp);
    const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)lp;
    if (k->dwExtraInfo == kMarker) return CallNextHookEx(NULL, code, wp, lp);  // наши собственные события

    const bool down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
    const DWORD vk = k->vkCode;

    // повтор клавиши (зажата): нажатие, пока она уже считается нажатой
    bool repeat = false;
    if (vk < 256) {
        if (down) { repeat = g_keyDown[vk]; g_keyDown[vk] = true; }
        else g_keyDown[vk] = false;
    }

    TrackLayoutChord(vk, down);

    if (vk == VK_CAPITAL) {
        if (down) { if (!g_capsHeld) { g_capsHeld = true; g_caps = !g_caps; } }
        else g_capsHeld = false;
        return CallNextHookEx(NULL, code, wp, lp);
    }

    // ---- Ctrl: ловим двойное "одиночное" нажатие
    if (vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_CONTROL) {
        if (down) {
            if (!g_ctrlDown) {
                g_ctrlDown = true;
                g_ctrlDirty = false;
                g_ctrlDownTick = k->time;
            }
        } else {
            // во время конвертации и сразу после неё нажатия Ctrl не учитываем
            const bool cooling = g_busy || (LONG)(g_cooldownUntil.load() - k->time) > 0;
            if (g_ctrlDown && !g_ctrlDirty && !cooling && (k->time - g_ctrlDownTick) <= 400) {
                if (g_lastTap != 0 && (k->time - g_lastTap) <= (DWORD)g_cfg.doubleCtrlMs) {
                    g_lastTap = 0;
                    if (!GameIgnoresTrigger()) PostMessageW(g_hwnd, WM_TRIGGER, 0, 0);
                } else {
                    g_lastTap = k->time;
                }
            } else {
                g_lastTap = 0;
            }
            g_ctrlDown = false;
        }
        return CallNextHookEx(NULL, code, wp, lp);
    }

    if (down) {
        if (g_ctrlDown) g_ctrlDirty = true;
        g_lastTap = 0;
    }

    if (!down) return CallNextHookEx(NULL, code, wp, lp);

    if (g_busy) {
        if (!IsModifierVk(vk)) g_resetAfter = true;
        return CallNextHookEx(NULL, code, wp, lp);
    }

    if (IsModifierVk(vk)) return CallNextHookEx(NULL, code, wp, lp);

    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const bool win = ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0;
    const bool ctrl = g_ctrlDown;
    const bool nav = (vk >= VK_PRIOR && vk <= VK_DOWN);

    // Сменилось активное окно — набранное в другом окне забываем
    HWND fg = GetForegroundWindow();
    if (fg != g_lastFg) {
        g_lastFg = fg;
        ResetTyping();
    }

    // ---- игровой режим
    const GameInfo gi = QueryGame(fg);
    if (gi.game) {
        const DWORD now = k->time;
        if ((g_gameArmed || g_gamePending) && (DWORD)(now - g_gameLastKey.load()) > kChatIdleMs) GameDisarm();

        if (vk == VK_ESCAPE) {  // чат закрыт
            GameDisarm();
            Lock l;
            g_selHint = false;
            g_selForce = false;
            return CallNextHookEx(NULL, code, wp, lp);
        }

        if (!g_gameArmed && !g_gamePending) {
            // Идёт игра. Единственное, что нас интересует, — попытка открыть чат.
            if (IsChatTriggerVk(vk) && !repeat && !ctrl && !alt && !win) {
                GameDisarm();
                if (gi.cursorShown) {
                    GameArm(now);  // курсор уже виден (меню/RTS): подтвердить по нему нельзя, считаем чат открытым
                } else {
                    // Курсор скрыт: если клавиша открыла чат, курсор появится. Пока ждём, ввод запоминаем условно.
                    g_gamePending = true;
                    g_gameLastKey = now;
                    SetTimer(g_hwnd, kTimerChat, kChatCheckMs, NULL);
                }
            }
            return CallNextHookEx(NULL, code, wp, lp);
        }

        g_gameLastKey = now;  // чат открыт (или ждём подтверждения): продолжаем как обычный набор
    } else if (g_gameArmed || g_gamePending) {
        GameDisarm();
    }

    // Горячие клавиши: сбрасываем буфер. Ctrl+A и Ctrl/Shift+стрелки — вероятное выделение.
    if (ctrl || alt || win) {
        Lock l;
        g_buf.clear();
        g_selHint = ctrl && !alt && !win && (vk == 'A' || (shift && nav));
        g_selByMouse = false;
        g_selForce = false;
        return CallNextHookEx(NULL, code, wp, lp);
    }

    if (vk == VK_BACK) {
        Lock l;
        if (g_selHint) { g_buf.clear(); g_selHint = false; g_selForce = false; }
        else if (!g_buf.empty()) {
            if (g_chainActive) NormalizeChain();
            g_buf.pop_back();
        }
        return CallNextHookEx(NULL, code, wp, lp);
    }

    if (vk == VK_RETURN) {
        if (gi.game) GameDisarm();  // сообщение отправлено — чат закрыт
        Lock l;
        g_buf.clear();
        g_selHint = false;
        g_selForce = false;
        return CallNextHookEx(NULL, code, wp, lp);
    }

    if (vk == VK_TAB || vk == VK_ESCAPE || vk == VK_DELETE || vk == VK_INSERT || (vk >= VK_F1 && vk <= VK_F24)) {
        Lock l;
        g_buf.clear();
        g_selHint = false;
        g_selForce = false;
        return CallNextHookEx(NULL, code, wp, lp);
    }

    if (nav) {
        Lock l;
        g_buf.clear();
        g_selHint = shift;
        g_selByMouse = false;
        g_selForce = false;
        return CallNextHookEx(NULL, code, wp, lp);
    }

    // ---- обычная клавиша: узнаём, какой символ она даёт в текущей раскладке
    HKL hkl = EffectiveLayout();
    BYTE ks[256];
    ZeroMemory(ks, sizeof(ks));
    if (shift) ks[VK_SHIFT] = 0x80;
    if (g_caps) ks[VK_CAPITAL] = 0x01;
    wchar_t out[8] = {0};
    int n = ToUnicodeEx((UINT)vk, k->scanCode, ks, out, 8, 0x4, hkl);  // 0x4 = не менять состояние dead keys
    if (n != 1 || out[0] < 0x20 || out[0] == 0x7F) return CallNextHookEx(NULL, code, wp, lp);

    const int script = ScriptOfLayout(hkl);  // 1 — латинская раскладка, 2 — кириллическая, 0 — другая

    Lock l;
    if (g_selHint) { g_buf.clear(); g_selHint = false; g_selForce = false; }  // набор заменил выделенный текст
    if (script == 0) { g_buf.clear(); return CallNextHookEx(NULL, code, wp, lp); }

    // запоминаем последнюю использованную латиницу / кириллицу (глобально, не по приложениям)
    if (script == 1) g_lastLat = (UINT_PTR)hkl;
    else g_lastCyr = (UINT_PTR)hkl;

    if (gi.game) {
        // В игре зажатая клавиша или серия одинаковых символов (wwww) — это управление, а не текст
        const size_t sz = g_buf.size();
        const bool series = sz >= 3 && g_buf[sz - 1].ch == out[0] && g_buf[sz - 2].ch == out[0] &&
                            g_buf[sz - 3].ch == out[0];
        if (repeat || series) {
            g_buf.clear();
            g_gameArmed = false;
            g_gamePending = false;
            KillTimer(g_hwnd, kTimerChat);
            return CallNextHookEx(NULL, code, wp, lp);
        }
    }

    if (g_chainActive) NormalizeChain();  // набор продолжился: сконвертированное считаем набранным как есть

    Rec r;
    r.ch = out[0];
    r.origCh = out[0];
    r.home = hkl;
    r.cur = hkl;
    r.neutral = (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE);
    g_buf.push_back(r);
    if ((int)g_buf.size() > g_cfg.maxBuffer) {
        g_buf.erase(g_buf.begin(), g_buf.begin() + (g_buf.size() - (size_t)g_cfg.maxBuffer));
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static LRESULT CALLBACK MsProc(int code, WPARAM wp, LPARAM lp) {
    if (code != HC_ACTION || wp == WM_MOUSEMOVE) return CallNextHookEx(NULL, code, wp, lp);
    const MSLLHOOKSTRUCT* m = (const MSLLHOOKSTRUCT*)lp;
    if (m->dwExtraInfo == kMarker || g_busy) return CallNextHookEx(NULL, code, wp, lp);

    switch (wp) {
        case WM_LBUTTONDOWN: {
            if (g_ctrlDown) g_ctrlDirty = true;
            g_lastTap = 0;
            g_downPt = m->pt;
            g_downWithCtrl = g_ctrlDown;
            const int dx = abs(m->pt.x - g_lastDownPt.x), dy = abs(m->pt.y - g_lastDownPt.y);
            if (g_lastDownTime != 0 && (m->time - g_lastDownTime) <= GetDoubleClickTime() &&
                dx <= GetSystemMetrics(SM_CXDOUBLECLK) && dy <= GetSystemMetrics(SM_CYDOUBLECLK))
                g_clickCount++;
            else
                g_clickCount = 1;
            g_lastDownTime = m->time;
            g_lastDownPt = m->pt;
            Lock l;
            g_buf.clear();
            g_selHint = false;
            g_selForce = false;
            break;
        }
        case WM_LBUTTONUP: {
            const bool drag = abs(m->pt.x - g_downPt.x) > 3 || abs(m->pt.y - g_downPt.y) > 3;
            const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            // выделение при зажатом Ctrl — гарантированно текст
            const bool force = g_downWithCtrl && (drag || g_clickCount >= 2);

            HWND fg = GetForegroundWindow();
            g_lastFg = fg;

            const GameInfo gi = QueryGame(fg);
            if (gi.game) {
                if (!gi.cursorShown) break;  // курсор скрыт — это управление камерой, а не выделение текста
                GameArm(m->time);            // курсор виден: клик мог открыть чат (или выбрать поле ввода)
            }

            if (drag || g_clickCount >= 2 || shift || force) {
                Lock l;
                g_selHint = true;
                g_selByMouse = true;
                g_selForce = force;
                g_selDownPt = g_downPt;
                g_selUpPt = m->pt;
            }
            break;
        }
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN: {
            if (g_ctrlDown) g_ctrlDirty = true;
            g_lastTap = 0;
            Lock l;
            g_buf.clear();
            g_selHint = false;
            g_selForce = false;
            break;
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

// ---------------------------------------------------------------- звук
static void PlayMp3(const std::wstring& path) {
    mciSendStringW(L"close lssnd", NULL, 0, NULL);
    std::wstring cmd = L"open \"" + path + L"\" type mpegvideo alias lssnd";
    if (mciSendStringW(cmd.c_str(), NULL, 0, NULL) == 0) mciSendStringW(L"play lssnd", NULL, 0, NULL);
}

static void PlaySwitchSound() {
    if (!g_cfg.sound) return;
    std::wstring wav = g_exeDir + L"switch.wav";
    if (GetFileAttributesW(wav.c_str()) != INVALID_FILE_ATTRIBUTES) {
        PlaySoundW(wav.c_str(), NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
        return;
    }
    std::wstring mp3 = g_exeDir + L"switch.mp3";
    if (GetFileAttributesW(mp3.c_str()) != INVALID_FILE_ATTRIBUTES) PlayMp3(mp3);
}

// ---------------------------------------------------------------- права администратора и связь между копиями
static const WPARAM CMD_QUIT = 1;    // работающая копия должна закрыться
static const WPARAM CMD_QUERY = 2;   // работающая копия отвечает: 0x100 | (с правами администратора ? 1 : 0)

static bool TokenIsElevated(HANDLE token) {
    TOKEN_ELEVATION e;
    DWORD sz = 0;
    return GetTokenInformation(token, TokenElevation, &e, sizeof(e), &sz) && e.TokenIsElevated != 0;
}

static bool IsElevated() {
    HANDLE t = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return false;
    const bool r = TokenIsElevated(t);
    CloseHandle(t);
    return r;
}

static bool ProcessIsElevated(DWORD pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    HANDLE t = NULL;
    bool r = false;
    if (OpenProcessToken(p, TOKEN_QUERY, &t)) {
        r = TokenIsElevated(t);
        CloseHandle(t);
    }
    CloseHandle(p);
    return r;
}

static HWND FindRunning() { return FindWindowW(L"LangSwitchWnd", NULL); }

// Просим работающую копию закрыться (для копии с правами администратора обычный WM_CLOSE блокируется Windows,
// поэтому используется собственное сообщение, которое она разрешила принимать)
static void StopRunning() {
    HWND w = FindRunning();
    if (!w) return;
    DWORD_PTR r = 0;
    SendMessageTimeoutW(w, g_msgCmd, CMD_QUIT, 0, SMTO_ABORTIFHUNG, 1500, &r);
    PostMessageW(w, WM_CLOSE, 0, 0);  // для старых версий программы
}

static bool RunningIsElevated() {
    HWND w = FindRunning();
    if (!w) return false;
    DWORD_PTR r = 0;
    if (SendMessageTimeoutW(w, g_msgCmd, CMD_QUERY, 0, SMTO_ABORTIFHUNG, 800, &r) && (r & 0x100)) return (r & 1) != 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    return pid != 0 && ProcessIsElevated(pid);
}

// Запустить эту программу с правами администратора (покажет запрос UAC). wait — дождаться завершения.
// true — программа запущена (и при wait — завершилась с кодом 0).
static bool RunSelfElevated(const wchar_t* args, HWND owner, bool wait) {
    SHELLEXECUTEINFOW sei;
    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.hwnd = owner;
    sei.lpVerb = L"runas";
    sei.lpFile = g_exePath.c_str();
    sei.lpParameters = args;
    sei.lpDirectory = g_exeDir.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return false;  // в том числе, если пользователь отказался в UAC
    if (!sei.hProcess) return true;
    if (!wait) {
        CloseHandle(sei.hProcess);
        return true;
    }

    // ждём завершения, обрабатывая сообщения окна
    bool finished = false;
    const DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < 120000) {
        const DWORD r = MsgWaitForMultipleObjects(1, &sei.hProcess, FALSE, 200, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) { finished = true; break; }
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DWORD code = 1;
    if (finished) GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return finished && code == 0;
}

static bool HasArg(const wchar_t* arg) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool found = false;
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            if (_wcsicmp(argv[i], arg) == 0) found = true;
        }
        LocalFree(argv);
    }
    return found;
}

// ---------------------------------------------------------------- автозагрузка
// Предпочтительный способ — задача планировщика "при входе в систему" с наивысшими правами: программа запускается
// с правами администратора без запроса UAC. Для её создания нужны права администратора. Если их нет — обычный
// запуск через раздел Run реестра текущего пользователя.
static const wchar_t* kTaskName = L"LangSwitch";
static const wchar_t* kRunValue = L"LangSwitch";
static const wchar_t* kRunKeyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

template <class T>
struct Com {
    T* p;
    Com() : p(NULL) {}
    ~Com() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
private:
    Com(const Com&);
    Com& operator=(const Com&);
};

struct Bstr {
    BSTR s;
    explicit Bstr(const wchar_t* w) : s(SysAllocString(w)) {}
    ~Bstr() { SysFreeString(s); }
    operator BSTR() const { return s; }
private:
    Bstr(const Bstr&);
    Bstr& operator=(const Bstr&);
};

static std::wstring StripQuotes(std::wstring s) {
    while (!s.empty() && s.front() == L'"') s.erase(0, 1);
    while (!s.empty() && s.back() == L'"') s.pop_back();
    return s;
}

static bool RunKeyEnabled() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    wchar_t buf[1024] = {0};
    DWORD sz = sizeof(buf) - sizeof(wchar_t), type = 0;
    bool ok = false;
    if (RegQueryValueExW(k, kRunValue, NULL, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS && type == REG_SZ)
        ok = _wcsicmp(StripQuotes(buf).c_str(), g_exePath.c_str()) == 0;
    RegCloseKey(k);
    return ok;
}

static void SetRunKey(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        const std::wstring v = L"\"" + g_exePath + L"\"";
        RegSetValueExW(k, kRunValue, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kRunValue);
    }
    RegCloseKey(k);
}

static std::wstring CurrentUserName() {
    wchar_t user[256] = {0}, dom[256] = {0};
    DWORD n = 256;
    GetUserNameW(user, &n);
    GetEnvironmentVariableW(L"USERDOMAIN", dom, 256);
    return dom[0] ? std::wstring(dom) + L"\\" + user : std::wstring(user);
}

// Подключиться к планировщику и открыть корневую папку задач
static bool OpenTaskRoot(Com<ITaskService>& svc, Com<ITaskFolder>& root) {
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER, IID_ITaskService, (void**)&svc)))
        return false;
    VARIANT e;
    VariantInit(&e);
    if (FAILED(svc->Connect(e, e, e, e))) return false;
    Bstr folder(L"\\");
    return SUCCEEDED(svc->GetFolder(folder, &root));
}

// exists — задача с таким именем есть; path — путь к программе в её действии (пусто, если прочитать не удалось)
static bool QueryTask(bool& exists, std::wstring& path) {
    exists = false;
    path.clear();
    Com<ITaskService> svc;
    Com<ITaskFolder> root;
    if (!OpenTaskRoot(svc, root)) return false;

    Com<IRegisteredTask> task;
    HRESULT hr;
    {
        Bstr n(kTaskName);
        hr = root->GetTask(n, &task);
    }
    if (FAILED(hr)) {
        if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
            return true;  // задачи нет
        exists = true;    // задача есть, но прочитать её без прав администратора нельзя
        return true;
    }
    exists = true;

    Com<ITaskDefinition> def;
    if (SUCCEEDED(task->get_Definition(&def))) {
        Com<IActionCollection> acts;
        if (SUCCEEDED(def->get_Actions(&acts))) {
            Com<IAction> act;
            if (SUCCEEDED(acts->get_Item(1, &act))) {
                Com<IExecAction> exec;
                if (SUCCEEDED(act->QueryInterface(IID_IExecAction, (void**)&exec))) {
                    BSTR p = NULL;
                    if (SUCCEEDED(exec->get_Path(&p)) && p) {
                        path = StripQuotes(p);
                        SysFreeString(p);
                    }
                }
            }
        }
    }
    return true;
}

// Создать (или обновить) задачу запуска при входе в систему с наивысшими правами. Нужны права администратора.
static bool RegisterLogonTask() {
    const std::wstring dir = g_exeDir.empty() ? g_exeDir : g_exeDir.substr(0, g_exeDir.size() - 1);
    const std::wstring user = CurrentUserName();

    Com<ITaskService> svc;
    Com<ITaskFolder> root;
    if (!OpenTaskRoot(svc, root)) return false;

    Com<ITaskDefinition> def;
    if (FAILED(svc->NewTask(0, &def))) return false;

    Com<IRegistrationInfo> info;
    if (SUCCEEDED(def->get_RegistrationInfo(&info))) {
        Bstr d(L"LangSwitch: запуск при входе в систему");
        info->put_Description(d);
    }

    Com<IPrincipal> pr;
    if (FAILED(def->get_Principal(&pr))) return false;
    pr->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
    pr->put_RunLevel(TASK_RUNLEVEL_HIGHEST);

    Com<ITaskSettings> st;
    if (SUCCEEDED(def->get_Settings(&st))) {
        st->put_StartWhenAvailable(VARIANT_TRUE);
        st->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
        st->put_StopIfGoingOnBatteries(VARIANT_FALSE);
        st->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
        Bstr lim(L"PT0S");  // без ограничения времени работы
        st->put_ExecutionTimeLimit(lim);
    }

    Com<ITriggerCollection> trigs;
    if (FAILED(def->get_Triggers(&trigs))) return false;
    Com<ITrigger> trig;
    if (FAILED(trigs->Create(TASK_TRIGGER_LOGON, &trig))) return false;
    Com<ILogonTrigger> logon;
    if (FAILED(trig->QueryInterface(IID_ILogonTrigger, (void**)&logon))) return false;
    {
        Bstr id(L"LangSwitchLogon");
        logon->put_Id(id);
        Bstr u(user.c_str());
        logon->put_UserId(u);
    }

    Com<IActionCollection> acts;
    if (FAILED(def->get_Actions(&acts))) return false;
    Com<IAction> act;
    if (FAILED(acts->Create(TASK_ACTION_EXEC, &act))) return false;
    Com<IExecAction> exec;
    if (FAILED(act->QueryInterface(IID_IExecAction, (void**)&exec))) return false;
    {
        Bstr p(g_exePath.c_str());
        exec->put_Path(p);
        Bstr w(dir.c_str());
        exec->put_WorkingDirectory(w);
    }

    VARIANT e;
    VariantInit(&e);
    VARIANT sddl;
    VariantInit(&sddl);
    sddl.vt = VT_BSTR;
    sddl.bstrVal = SysAllocString(L"");
    Com<IRegisteredTask> reg;
    Bstr name(kTaskName);
    const HRESULT hr = root->RegisterTaskDefinition(name, def.p, TASK_CREATE_OR_UPDATE, e, e,
                                                    TASK_LOGON_INTERACTIVE_TOKEN, sddl, &reg);
    VariantClear(&sddl);
    return SUCCEEDED(hr);
}

// Удалить задачу. Если её нет — тоже успех.
static bool DeleteLogonTask() {
    Com<ITaskService> svc;
    Com<ITaskFolder> root;
    if (!OpenTaskRoot(svc, root)) return false;
    Bstr name(kTaskName);
    const HRESULT hr = root->DeleteTask(name, 0);
    return SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

struct AutoInfo {
    bool task;  // автозагрузка через планировщик (с правами администратора)
    bool run;   // обычная автозагрузка текущего пользователя
};

static AutoInfo GetAutostartInfo() {
    AutoInfo a;
    a.task = false;
    a.run = RunKeyEnabled();
    bool exists = false;
    std::wstring p;
    if (QueryTask(exists, p) && exists && (p.empty() || _wcsicmp(p.c_str(), g_exePath.c_str()) == 0)) a.task = true;
    return a;
}

// Служебный режим: программа запущена с правами администратора только чтобы изменить автозагрузку
static int RunAutostartHelper(bool on) {
    const HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    bool ok;
    if (on) {
        ok = RegisterLogonTask();
        if (ok) SetRunKey(false);  // задача уже запускает программу; второй запуск не нужен
    } else {
        SetRunKey(false);
        ok = DeleteLogonTask();
    }
    if (SUCCEEDED(hr)) CoUninitialize();
    return ok ? 0 : 1;
}

// Работает ли пользователь под учётной записью администратора с урезанным токеном (повышение через UAC возможно)
static bool CanSelfElevate() {
    HANDLE t = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return false;
    TOKEN_ELEVATION_TYPE type = TokenElevationTypeDefault;
    DWORD sz = 0;
    const bool ok = GetTokenInformation(t, TokenElevationType, &type, sizeof(type), &sz) != 0;
    CloseHandle(t);
    return ok && type == TokenElevationTypeLimited;
}

// Запустить созданную ранее задачу автозагрузки: она стартует программу с правами администратора без запроса UAC.
// true — задача запущена.
static bool RunLogonTaskNow() {
    const HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    bool ok = false;
    bool exists = false;
    std::wstring p;
    // запускаем только задачу, которая указывает именно на эту программу
    if (QueryTask(exists, p) && exists && !p.empty() && _wcsicmp(p.c_str(), g_exePath.c_str()) == 0) {
        Com<ITaskService> svc;
        Com<ITaskFolder> root;
        if (OpenTaskRoot(svc, root)) {
            Com<IRegisteredTask> task;
            Bstr n(kTaskName);
            if (SUCCEEDED(root->GetTask(n, &task))) {
                Com<IRunningTask> running;
                VARIANT e;
                VariantInit(&e);
                ok = SUCCEEDED(task->Run(e, &running));
            }
        }
    }
    if (SUCCEEDED(hr)) CoUninitialize();
    return ok;
}

// ---------------------------------------------------------------- окно управления (при повторном запуске)
enum { ID_BTN_STOP = 101, ID_BTN_ADMIN = 102, ID_BTN_AUTO = 103 };

static const COLORREF kBg = RGB(30, 30, 30);
static const COLORREF kBtn = RGB(48, 48, 48);
static const COLORREF kBtnHover = RGB(64, 64, 64);
static const COLORREF kBtnDown = RGB(38, 38, 38);
static const COLORREF kBorder = RGB(74, 74, 74);
static const COLORREF kText = RGB(236, 236, 236);
static const COLORREF kMuted = RGB(150, 150, 150);
static const COLORREF kGreen = RGB(84, 206, 126);
static const COLORREF kRed = RGB(244, 92, 92);
static const COLORREF kAccent = RGB(96, 165, 250);

static HWND g_dlg = NULL, g_btnStop = NULL, g_btnAdmin = NULL, g_btnAuto = NULL, g_hover = NULL;
static HFONT g_fontTitle = NULL, g_fontText = NULL, g_fontBtn = NULL;
static WNDPROC g_btnProc = NULL;
static int g_dpi = 96;
static bool g_runElevated = false;  // работающая копия запущена с правами администратора
static bool g_autoOn = false;
static bool g_dlgBusy = false;

static int Sc(int v) { return MulDiv(v, g_dpi, 96); }

static HFONT MakeFont(int pt, int weight) {
    return CreateFontW(-MulDiv(pt, g_dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                       L"Segoe UI");
}

// Тёмный заголовок окна (Windows 10 1809+ / Windows 11)
static void ApplyDarkTitleBar(HWND h) {
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    typedef HRESULT(WINAPI * SetAttrFn)(HWND, DWORD, LPCVOID, DWORD);
    SetAttrFn fn = (SetAttrFn)GetProcAddress(dwm, "DwmSetWindowAttribute");
    if (fn) {
        const BOOL dark = TRUE;
        if (FAILED(fn(h, 20, &dark, sizeof(dark)))) fn(h, 19, &dark, sizeof(dark));
        const COLORREF cap = kBg, txt = kText;
        fn(h, 35, &cap, sizeof(cap));  // цвет заголовка (Windows 11)
        fn(h, 36, &txt, sizeof(txt));  // цвет текста заголовка (Windows 11)
    }
    FreeLibrary(dwm);
}

static LRESULT CALLBACK BtnProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_MOUSEMOVE:
            if (g_hover != h) {
                g_hover = h;
                TRACKMOUSEEVENT t;
                ZeroMemory(&t, sizeof(t));
                t.cbSize = sizeof(t);
                t.dwFlags = TME_LEAVE;
                t.hwndTrack = h;
                TrackMouseEvent(&t);
                InvalidateRect(h, NULL, FALSE);
            }
            break;
        case WM_MOUSELEAVE:
            if (g_hover == h) {
                g_hover = NULL;
                InvalidateRect(h, NULL, FALSE);
            }
            break;
        case WM_SETCURSOR:
            if (IsWindowEnabled(h)) {
                SetCursor(LoadCursorW(NULL, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_ERASEBKGND:
            return 1;
    }
    return CallWindowProcW(g_btnProc, h, m, w, l);
}

static HWND MakeButton(int id, const wchar_t* text, int y, int w) {
    HWND b = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, Sc(24), y, w,
                             Sc(44), g_dlg, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    if (b) {
        if (!g_btnProc) g_btnProc = (WNDPROC)GetWindowLongPtrW(b, GWLP_WNDPROC);
        SetWindowLongPtrW(b, GWLP_WNDPROC, (LONG_PTR)BtnProc);
    }
    return b;
}

static void DrawButton(const DRAWITEMSTRUCT* d) {
    HDC dc = d->hDC;
    const RECT rc = d->rcItem;
    const bool disabled = (d->itemState & ODS_DISABLED) != 0 || g_dlgBusy;
    const bool pressed = (d->itemState & ODS_SELECTED) != 0;
    const bool hover = (g_hover == d->hwndItem);
    const bool focus = (d->itemState & ODS_FOCUS) != 0 && (d->itemState & ODS_NOFOCUSRECT) == 0;

    HBRUSH bg = CreateSolidBrush(kBg);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    const COLORREF fill = pressed ? kBtnDown : ((hover && !disabled) ? kBtnHover : kBtn);
    HBRUSH fb = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, focus ? Sc(2) : 1, focus ? kAccent : kBorder);
    HGDIOBJ ob = SelectObject(dc, fb);
    HGDIOBJ op = SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, Sc(10), Sc(10));
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(fb);
    DeleteObject(pen);

    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ of = SelectObject(dc, g_fontBtn);
    const COLORREF txt = disabled ? kMuted : kText;

    if (d->CtlID == ID_BTN_AUTO) {
        const wchar_t* prefix = L"Автозагрузка: ";
        const wchar_t* status = g_autoOn ? L"включена" : L"выключена";
        const int lp = (int)wcslen(prefix), ls = (int)wcslen(status);
        SIZE a, b;
        GetTextExtentPoint32W(dc, prefix, lp, &a);
        GetTextExtentPoint32W(dc, status, ls, &b);
        const int x = rc.left + ((rc.right - rc.left) - (a.cx + b.cx)) / 2;
        const int y = rc.top + ((rc.bottom - rc.top) - a.cy) / 2;
        SetTextColor(dc, txt);
        TextOutW(dc, x, y, prefix, lp);
        SetTextColor(dc, disabled ? kMuted : (g_autoOn ? kGreen : kRed));
        TextOutW(dc, x + a.cx, y, status, ls);
    } else {
        wchar_t text[64] = {0};
        GetWindowTextW(d->hwndItem, text, 64);
        RECT tr = rc;
        SetTextColor(dc, txt);
        DrawTextW(dc, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(dc, of);
}

static void PaintDialog(HDC dc) {
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ old = SelectObject(dc, g_fontTitle);
    SetTextColor(dc, kText);
    RECT r = {Sc(24), Sc(20), Sc(376), Sc(50)};
    DrawTextW(dc, L"LangSwitch", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(dc, g_fontText);
    SetTextColor(dc, kMuted);
    r.top = Sc(54);
    r.bottom = Sc(76);
    DrawTextW(dc, L"Программа запущена. Это окно можно закрыть.", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    if (g_runElevated) {
        SetTextColor(dc, kGreen);
        r.top = Sc(80);
        r.bottom = Sc(102);
        DrawTextW(dc, L"Работает с правами администратора", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(dc, old);
}

// Включить или выключить автозагрузку
static void DoToggleAutostart() {
    const AutoInfo a = GetAutostartInfo();
    if (a.task || a.run) {
        SetRunKey(false);
        if (a.task) {
            if (g_elevated) DeleteLogonTask();
            else RunSelfElevated(L"/autostart-off", g_dlg, true);
        }
        return;
    }

    bool done = false;
    if (g_elevated) {
        done = RegisterLogonTask();
        if (done) SetRunKey(false);
    } else {
        done = RunSelfElevated(L"/autostart-on", g_dlg, true);  // запрос UAC
    }
    if (!done) SetRunKey(true);  // права администратора недоступны — обычная автозагрузка текущего пользователя
}

static void SetButtonsEnabled(bool enabled) {
    if (g_btnStop) EnableWindow(g_btnStop, enabled);
    if (g_btnAdmin) EnableWindow(g_btnAdmin, enabled);
    if (g_btnAuto) EnableWindow(g_btnAuto, enabled);
}

static void ToggleAutostartUi() {
    g_dlgBusy = true;
    SetButtonsEnabled(false);
    UpdateWindow(g_dlg);

    DoToggleAutostart();

    const AutoInfo a = GetAutostartInfo();
    g_autoOn = a.task || a.run;
    g_dlgBusy = false;
    SetButtonsEnabled(true);
    InvalidateRect(g_btnAuto, NULL, FALSE);
    SetFocus(g_btnAuto);
}

static LRESULT CALLBACK DlgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(h, &rc);
            HBRUSH b = CreateSolidBrush(kBg);
            FillRect((HDC)w, &rc, b);
            DeleteObject(b);
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            PaintDialog(dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_DRAWITEM:
            DrawButton((const DRAWITEMSTRUCT*)l);
            return TRUE;
        case WM_COMMAND:
            if (HIWORD(w) == BN_CLICKED) {
                switch (LOWORD(w)) {
                    case ID_BTN_STOP:
                        StopRunning();
                        DestroyWindow(h);
                        break;
                    case ID_BTN_ADMIN:
                        // перезапуск с правами администратора: новая копия сама закроет старую и займёт её место
                        if (RunSelfElevated(L"/takeover", h, false)) DestroyWindow(h);
                        break;
                    case ID_BTN_AUTO:
                        ToggleAutostartUi();
                        break;
                }
            } else if (LOWORD(w) == IDCANCEL) {
                DestroyWindow(h);
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void ShowControlDialog(HINSTANCE hInst) {
    const HRESULT coHr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    HDC sdc = GetDC(NULL);
    g_dpi = GetDeviceCaps(sdc, LOGPIXELSX);
    ReleaseDC(NULL, sdc);
    if (g_dpi < 96) g_dpi = 96;

    g_runElevated = RunningIsElevated();
    {
        const AutoInfo a = GetAutostartInfo();
        g_autoOn = a.task || a.run;
    }

    g_fontTitle = MakeFont(16, FW_SEMIBOLD);
    g_fontText = MakeFont(10, FW_NORMAL);
    g_fontBtn = MakeFont(11, FW_NORMAL);

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = DlgProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"LangSwitchDlg";
    RegisterClassW(&wc);

    const bool showAdmin = !g_runElevated;
    const int nBtn = showAdmin ? 3 : 2;
    const int headerH = g_runElevated ? 106 : 80;  // в единицах при 96 DPI
    const int clientW = Sc(400);
    const int clientH = Sc(headerH + 14) + nBtn * Sc(44) + (nBtn - 1) * Sc(10) + Sc(24);

    const DWORD style = WS_CAPTION | WS_SYSMENU;
    RECT r = {0, 0, clientW, clientH};
    AdjustWindowRectEx(&r, style, FALSE, 0);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const int x = wa.left + ((wa.right - wa.left) - ww) / 2;
    const int y = wa.top + ((wa.bottom - wa.top) - wh) / 2;

    g_dlg = CreateWindowExW(0, wc.lpszClassName, L"LangSwitch", style, x, y, ww, wh, NULL, NULL, hInst, NULL);
    if (g_dlg) {
        ApplyDarkTitleBar(g_dlg);

        const int bw = Sc(352);
        int by = Sc(headerH + 14);
        g_btnStop = MakeButton(ID_BTN_STOP, L"Остановить", by, bw);
        by += Sc(54);
        if (showAdmin) {
            g_btnAdmin = MakeButton(ID_BTN_ADMIN, L"Права администратора", by, bw);
            by += Sc(54);
        }
        g_btnAuto = MakeButton(ID_BTN_AUTO, L"", by, bw);

        ShowWindow(g_dlg, SW_SHOW);
        UpdateWindow(g_dlg);
        SetForegroundWindow(g_dlg);
        SetFocus(g_btnStop);

        MSG msg;
        while (GetMessageW(&msg, NULL, 0, 0) > 0) {
            if (msg.message == WM_KEYDOWN) {
                if (msg.wParam == VK_ESCAPE) {
                    DestroyWindow(g_dlg);
                    continue;
                }
                if (msg.wParam == VK_RETURN) {
                    HWND f = GetFocus();
                    if (f && (f == g_btnStop || f == g_btnAdmin || f == g_btnAuto)) {
                        SendMessageW(f, BM_CLICK, 0, 0);
                        continue;
                    }
                }
            }
            if (IsDialogMessageW(g_dlg, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    DeleteObject(g_fontTitle);
    DeleteObject(g_fontText);
    DeleteObject(g_fontBtn);
    if (SUCCEEDED(coHr)) CoUninitialize();
}

// ---------------------------------------------------------------- окно (скрытое, нужно для сообщений, таймера и буфера обмена)
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_msgCmd != 0 && m == g_msgCmd) {
        if (w == CMD_QUIT) {
            PostMessageW(h, WM_CLOSE, 0, 0);
            return 0;
        }
        if (w == CMD_QUERY) return 0x100 | (g_elevated ? 1 : 0);
        return 0;
    }
    switch (m) {
        case WM_TRIGGER:
            if (ExMode(GetForegroundWindow()) == EX_FULL) return 0;  // программа исключена целиком (exclude.txt)
            if (!g_busy.exchange(true)) {
                uintptr_t th = _beginthreadex(NULL, 0, ConvertThread, NULL, 0, NULL);
                if (th) CloseHandle((HANDLE)th);
                else g_busy = false;
            }
            return 0;
        case WM_PLAYSOUND:
            PlaySwitchSound();
            return 0;
        case WM_TIMER:
            if (w == kTimerChat) {
                // После клавиши чата в игре: курсор появился — чат открылся, иначе это был промах
                KillTimer(h, kTimerChat);
                if (g_gamePending) {
                    g_gamePending = false;
                    if (CursorShown()) {
                        g_gameArmed = true;
                        g_gameLastKey = GetTickCount();
                    } else {
                        Lock lk;
                        g_buf.clear();
                    }
                }
                return 0;
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------- main
// Чтобы координаты мыши и элементов интерфейса (UI Automation) совпадали при масштабировании экрана
static void EnableDpiAwareness() {
    typedef BOOL(WINAPI * SetCtxFn)(HANDLE);
    HMODULE u = GetModuleHandleW(L"user32.dll");
    SetCtxFn fn = u ? (SetCtxFn)GetProcAddress(u, "SetProcessDpiAwarenessContext") : NULL;
    if (!(fn && fn((HANDLE)(INT_PTR)-4))) SetProcessDPIAware();  // -4 = PER_MONITOR_AWARE_V2
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    EnableDpiAwareness();

    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, path, MAX_PATH);
    g_exePath = path;
    g_exeDir = g_exePath.substr(0, g_exePath.find_last_of(L"\\/") + 1);
    g_iniPath = g_exeDir + L"LangSwitch.ini";
    g_msgCmd = RegisterWindowMessageW(L"LangSwitch.Command.1");
    g_elevated = IsElevated();

    // Служебные режимы: программа запущена с правами администратора только чтобы изменить автозагрузку
    if (HasArg(L"/autostart-on")) return RunAutostartHelper(true);
    if (HasArg(L"/autostart-off")) return RunAutostartHelper(false);
    const bool takeover = HasArg(L"/takeover");

    HANDLE mtx = CreateMutexW(NULL, TRUE, kMutexName);
    const DWORD mtxErr = GetLastError();
    // Если работающая копия запущена с правами администратора, мьютекс для обычной копии недоступен (ACCESS_DENIED)
    if (mtxErr == ERROR_ALREADY_EXISTS || (!mtx && mtxErr == ERROR_ACCESS_DENIED)) {
        if (!takeover) {
            if (mtx) CloseHandle(mtx);
            ShowControlDialog(hInst);  // программа уже запущена: предлагаем действия
            return 0;
        }

        // Перезапуск с правами администратора: закрываем старую копию и занимаем её место
        StopRunning();
        if (mtx) { CloseHandle(mtx); mtx = NULL; }
        for (int i = 0; i < 50 && !mtx; ++i) {
            Sleep(100);
            HANDLE m2 = CreateMutexW(NULL, TRUE, kMutexName);
            if (m2 && GetLastError() != ERROR_ALREADY_EXISTS) mtx = m2;
            else if (m2) CloseHandle(m2);
        }
        if (!mtx) return 1;  // старая копия не закрылась
    }

    // По умолчанию работаем с правами администратора: иначе Windows не пускает ввод в окна, запущенные от администратора.
    // Если пользователь отказался в запросе UAC (или повышение не удалось), продолжаем без прав администратора.
    if (!g_elevated && !HasArg(L"/noelevate") && CanSelfElevate()) {
        if (mtx) { CloseHandle(mtx); mtx = NULL; }  // освобождаем, чтобы копия с правами администратора заняла место
        if (RunLogonTaskNow() || RunSelfElevated(L"/noelevate", NULL, false)) return 0;
        mtx = CreateMutexW(NULL, TRUE, kMutexName);  // отказ или ошибка: продолжаем без прав администратора
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            if (mtx) CloseHandle(mtx);
            return 0;
        }
    }

    InitializeCriticalSection(&g_cs);
    InitializeCriticalSection(&g_layCs);

    g_fmtHistory = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    g_fmtCloud = RegisterClipboardFormatW(L"CanUploadToCloudClipboard");
    g_fmtMonitor = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");

    if (GetFileAttributesW(g_iniPath.c_str()) == INVALID_FILE_ATTRIBUTES) SaveDefaultSettings();
    LoadSettings();

    g_caps = (GetKeyState(VK_CAPITAL) & 1) != 0;
    NoteLayoutUsed(FgLayout());  // начальные "последние использованные" раскладки

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"LangSwitchWnd";
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"LangSwitch", WS_OVERLAPPED, 0, 0, 0, 0, NULL, NULL, hInst, NULL);
    if (!g_hwnd) return 1;

    // Разрешаем копиям без прав администратора присылать служебные команды (закрыться / сообщить права)
    if (g_msgCmd) ChangeWindowMessageFilterEx(g_hwnd, g_msgCmd, MSGFLT_ALLOW, NULL);

    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, AsSetup(), GetModuleHandleW(NULL), 0);
    g_msHook = SetWindowsHookExW(WH_MOUSE_LL, MsProc, GetModuleHandleW(NULL), 0);
    if (!g_kbHook) {
        MessageBoxW(NULL, L"Не удалось установить перехват клавиатуры.", L"LangSwitch", MB_ICONERROR);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnhookWindowsHookEx(g_kbHook);
    if (g_msHook) UnhookWindowsHookEx(g_msHook);
    DeleteCriticalSection(&g_layCs);
    DeleteCriticalSection(&g_cs);
    if (mtx) CloseHandle(mtx);
    return 0;
}
