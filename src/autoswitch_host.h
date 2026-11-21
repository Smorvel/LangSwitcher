// autoswitch_host.h — связь основной программы с опциональным модулем авто-переключения (autoswitch.dll).
// Включается из main.cpp и использует его внутренние функции, поэтому файл не компилируется отдельно.
// Если autoswitch.dll нет рядом с программой, AsSetup возвращает обычный обработчик клавиатуры, и программа
// работает как без авто-переключения. Саму оценку слов и словарь Words.txt делает DLL.
#pragma once

#include "autoswitch_api.h"

// определены в main.cpp ниже места подключения
static LRESULT CALLBACK KbProc(int code, WPARAM wp, LPARAM lp);
static bool IsModifierVk(DWORD vk);

enum { AL_NONE = 0, AL_EN = 1, AL_RU = 2 };

// Язык раскладки. Латиница: английский язык Windows или QWERTY-раскладка с английскими буквами.
// Кириллица: русская (есть ы и э, нет украинских і ї є). Остальные раскладки (украинская и др.) не поддерживаются.
static int AsLayoutLang(HKL h) {
    static std::map<UINT_PTR, int> cache;
    const std::map<UINT_PTR, int>::const_iterator it = cache.find((UINT_PTR)h);
    if (it != cache.end()) return it->second;
    int lang = AL_NONE;
    const int script = ScriptOfLayout(h);
    if (script == 1) {
        if (PRIMARYLANGID(LOWORD((UINT_PTR)h)) == LANG_ENGLISH) {
            lang = AL_EN;
        } else {
            static const wchar_t* qw = L"qwertyuiop";
            bool qwerty = true;
            for (int i = 0; i < 10; ++i) {
                wchar_t c = 0;
                if (!KeyChar(h, (UINT)(0x10 + i), false, false, c) || c != qw[i]) { qwerty = false; break; }
            }
            if (qwerty) lang = AL_EN;
        }
    } else if (script == 2) {
        const std::set<wchar_t>& ch = LayoutChars(h);
        if (ch.count(L'ы') && ch.count(L'э') && !ch.count(L'і') && !ch.count(L'ї') && !ch.count(L'є')) lang = AL_RU;
    }
    cache[(UINT_PTR)h] = lang;
    return lang;
}

// ---------------------------------------------------------------- вызовы модуля
static AsDecideFn g_asDecideFn = NULL;

static int AsCbScript(UINT_PTR h) { return ScriptOfLayout((HKL)h); }
static int AsCbLang(UINT_PTR h) { return AsLayoutLang((HKL)h); }
static wchar_t AsCbConv(wchar_t c, UINT_PTR from, UINT_PTR to) { return ConvChar(c, (HKL)from, (HKL)to); }

static void AsCbLayouts(UINT_PTR* lat, int* nLat, UINT_PTR* cyr, int* nCyr, int cap) {
    const Cycle cy = BuildCycle();
    *nLat = 0;
    *nCyr = 0;
    for (size_t i = 0; i < cy.lat.size() && *nLat < cap; ++i) lat[(*nLat)++] = (UINT_PTR)cy.lat[i];
    for (size_t i = 0; i < cy.cyr.size() && *nCyr < cap; ++i) cyr[(*nCyr)++] = (UINT_PTR)cy.cyr[i];
}

struct AsFix {
    size_t start;  // индекс первого символа области замены в буфере
    size_t end;    // конец области замены (не включая); (size_t)-1 — до конца буфера
    HKL target;    // раскладка, в которую нужно перевести область
};

static bool AsExcluded() { return ExMode(GetForegroundWindow()) != EX_NONE; }

// Спрашивает у модуля, нужно ли что-то переключить. mode: 0 — после пробела, 1 — по паузе, 2 — перед Enter
static bool AsDecideHost(const std::vector<Rec>& buf, int mode, AsFix& out) {
    if (!g_asDecideFn || buf.empty() || AsExcluded()) return false;
    std::vector<AsChar> v(buf.size());
    for (size_t i = 0; i < buf.size(); ++i) {
        v[i].ch = buf[i].ch;
        v[i].cur = (UINT_PTR)buf[i].cur;
        v[i].home = (UINT_PTR)buf[i].home;
        v[i].neutral = buf[i].neutral ? 1 : 0;
    }
    AsResult r;
    r.start = 0;
    r.end = -1;
    r.target = 0;
    if (!g_asDecideFn(v.data(), (int)v.size(), mode, &r) || !r.target || r.start < 0) return false;
    out.start = (size_t)r.start;
    out.end = r.end < 0 ? (size_t)-1 : (size_t)r.end;
    out.target = (HKL)r.target;
    return true;
}

// ---------------------------------------------------------------- замена слов (отдельный поток)
struct AsEv {
    WORD vk;
    WORD scan;
    bool ext;
    bool shift;
};
struct AsJob {
    size_t regionLen;  // сколько символов с конца буфера заменить (область и всё, что набрано после неё)
    HKL target;
    size_t convEnd;    // конец области, которую нужно перевести в target ((size_t)-1 — до конца буфера)
    bool sendEnter;    // после замены отправить Enter (он был задержан)
    size_t snapSize;   // размер буфера в момент решения
};

static std::atomic<bool> g_asBusy(false);
static HWND g_asWnd = NULL;
static CRITICAL_SECTION g_asCs;
static std::vector<AsEv> g_asQueue;  // клавиши, нажатые во время замены
static bool g_asSw[256] = {false};   // клавиши, нажатие которых мы задержали (под g_asCs)
static AsJob g_asJob;
static const UINT_PTR kAsTimer = 1;
static const UINT kAsPauseMs = 1000;

static bool AsIsConsole(HWND fg) {
    wchar_t cls[64] = {0};
    if (fg) GetClassNameW(fg, cls, 64);
    return !wcscmp(cls, L"ConsoleWindowClass") || !wcscmp(cls, L"CASCADIA_HOSTING_WINDOW_CLASS");
}

static bool AsIsPassThrough(DWORD vk) {
    return IsModifierVk(vk) || vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL;
}

static bool AsFocusIsPassword() {
    bool pw = false;
    IUIAutomation* uia = NULL;
    IUIAutomationElement* el = NULL;
    if (CreateUia(&uia) && SUCCEEDED(uia->GetFocusedElement(&el)) && el) {
        BOOL b = FALSE;
        if (SUCCEEDED(el->get_CurrentIsPassword(&b)) && b) pw = true;
    }
    if (el) el->Release();
    if (uia) uia->Release();
    return pw;
}

static INPUT AsPlainKey(WORD vk, WORD scan, bool ext, bool up) {
    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = scan;
    in.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (ext ? KEYEVENTF_EXTENDEDKEY : 0);
    in.ki.dwExtraInfo = 0;  // без метки: хук обработает эти нажатия как обычный ввод
    return in;
}

// Завершение замены: воспроизводим то, что пользователь печатал во время замены (уже в новой раскладке)
static void AsFinish() {
    std::vector<AsEv> q;
    EnterCriticalSection(&g_asCs);
    q.swap(g_asQueue);
    ZeroMemory(g_asSw, sizeof(g_asSw));
    g_asBusy = false;
    LeaveCriticalSection(&g_asCs);
    if (q.empty()) return;

    std::vector<INPUT> in;
    for (size_t i = 0; i < q.size(); ++i) {
        const AsEv& ev = q[i];
        const bool shiftNow = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool wrap = ev.shift && !shiftNow;
        if (wrap) in.push_back(KeyInput(VK_LSHIFT, false));
        in.push_back(AsPlainKey(ev.vk, ev.scan, ev.ext, false));
        in.push_back(AsPlainKey(ev.vk, ev.scan, ev.ext, true));
        if (wrap) in.push_back(KeyInput(VK_LSHIFT, true));
    }
    SendBatch(in);
}

static unsigned __stdcall AsFixThread(void*) {
    const HRESULT coHr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    const AsJob job = g_asJob;
    Sleep(15);  // даём пробелу дойти до приложения

    std::vector<Rec> snap;
    {
        Lock l;
        snap = g_buf;
    }
    HWND fg = GetForegroundWindow();
    if (fg && snap.size() == job.snapSize && snap.size() >= job.regionLen && job.regionLen > 0) {
        bool allow = true;
        const GameInfo gi = QueryGame(fg);
        if (!gi.game && GetFocusKind() == FK_NOTEXT) allow = false;
        if (allow && AsFocusIsPassword()) allow = false;  // пароли не трогаем
        if (allow) {
            const size_t st = snap.size() - job.regionLen;
            // Круг для повторного двойного Ctrl сразу после автозамены: только исходная и целевая раскладки,
            // чтобы первое нажатие вернуло исходное слово
            const Cycle cy = BuildCycle();
            Cycle ca;
            const HKL home0 = snap[st].home;
            for (size_t i = 0; i < cy.lat.size(); ++i) {
                if (cy.lat[i] == home0 || cy.lat[i] == job.target) ca.lat.push_back(cy.lat[i]);
            }
            for (size_t i = 0; i < cy.cyr.size(); ++i) {
                if (cy.cyr[i] == home0 || cy.cyr[i] == job.target) ca.cyr.push_back(cy.cyr[i]);
            }
            ConvertBuffer(snap, ca, st, job.target, job.convEnd);
            // слово переключено автоматически: двойной Ctrl дальше работает с текстом от него
            Lock l;
            if (!job.sendEnter && !g_resetAfter && g_buf.size() > st) g_buf[st].asStart = true;
        }
    }

    if (job.sendEnter) {
        {
            Lock l;
            g_buf.clear();
            g_chainActive = false;
            g_selHint = false;
            g_selForce = false;
        }
        if (fg && QueryGame(fg).game) GameDisarm();
        std::vector<INPUT> in;
        in.push_back(KeyInput(VK_RETURN, false));
        in.push_back(KeyInput(VK_RETURN, true));
        SendBatch(in);
    }

    AsFinish();
    if (SUCCEEDED(coHr)) CoUninitialize();
    return 0;
}

static bool AsStartJob(const AsJob& j) {
    if (g_asBusy.exchange(true)) return false;
    g_asJob = j;
    const uintptr_t th = _beginthreadex(NULL, 0, AsFixThread, NULL, 0, NULL);
    if (th) {
        CloseHandle((HANDLE)th);
        return true;
    }
    g_asBusy = false;
    return false;
}

static AsJob AsMakeJob(const AsFix& fx, size_t bufSize, bool sendEnter) {
    AsJob j;
    j.regionLen = bufSize - fx.start;
    j.target = fx.target;
    j.convEnd = fx.end;
    j.sendEnter = sendEnter;
    j.snapSize = bufSize;
    return j;
}

// ---------------------------------------------------------------- точки входа из хука и таймера
// Откуда начинать ручную конвертацию (двойной Ctrl): с последнего слова, переключённого автоматически.
// Если такого нет — (size_t)-1, и работает обычная настройка Scope (весь текст или последнее слово).
static size_t AsManualStart(const std::vector<Rec>& snap) {
    if (!g_cfg.wholeText) return (size_t)-1;
    for (size_t i = snap.size(); i-- > 0;) {
        if (snap[i].asStart) return i;
    }
    return (size_t)-1;
}

static void AsOnTimer() {
    KillTimer(g_asWnd, kAsTimer);
    if (g_asBusy || g_busy) return;
    HWND fg = GetForegroundWindow();
    if (!fg || fg != g_lastFg || AsIsConsole(fg)) return;
    if (QueryGame(fg).game && !g_gameArmed) return;
    std::vector<Rec> snap;
    {
        Lock l;
        snap = g_buf;
    }
    if (snap.empty() || snap.back().ch == L' ') return;
    AsFix fx;
    if (!AsDecideHost(snap, 1, fx)) return;
    AsStartJob(AsMakeJob(fx, snap.size(), false));
}

static LRESULT CALLBACK AsWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_TIMER && w == kAsTimer) {
        AsOnTimer();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// Во время замены нажатия обычных клавиш откладываем (true — нажатие задержано)
static bool AsSwallow(const KBDLLHOOKSTRUCT* k, bool down) {
    const DWORD vk = k->vkCode;
    if (vk >= 256) return false;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool hot = ((GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_MENU) | GetAsyncKeyState(VK_LWIN) |
                       GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0;
    bool swallow = false;
    EnterCriticalSection(&g_asCs);
    if (g_asBusy) {
        if (down) {
            if (!hot) {
                if (g_asQueue.size() < 32) {
                    AsEv ev;
                    ev.vk = (WORD)vk;
                    ev.scan = (WORD)k->scanCode;
                    ev.ext = (k->flags & LLKHF_EXTENDED) != 0;
                    ev.shift = shift;
                    g_asQueue.push_back(ev);
                }
                g_asSw[vk] = true;
                swallow = true;
            }
        } else if (g_asSw[vk]) {
            g_asSw[vk] = false;
            swallow = true;
        }
    }
    LeaveCriticalSection(&g_asCs);
    return swallow;
}

static bool AsEnterPlain() {
    return ((GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_MENU) |
             GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) == 0;
}

// Enter перед отправкой: если последнее слово набрано не в той раскладке — задерживаем Enter, чтобы заменить слово
static bool AsWantsEnter() {
    if (g_busy) return false;
    HWND fg = GetForegroundWindow();
    // буфер набирался в другом окне (например, до запуска игры) — Enter не трогаем
    if (!fg || fg != g_lastFg || AsIsConsole(fg) || AsExcluded()) return false;
    // в игре слова проверяются, только пока открыт чат
    if (QueryGame(fg).game && !g_gameArmed) return false;
    std::vector<Rec> snap;
    {
        Lock l;
        snap = g_buf;
    }
    if (snap.empty()) return false;
    AsFix fx;
    if (!AsDecideHost(snap, 2, fx)) return false;
    return AsStartJob(AsMakeJob(fx, snap.size(), true));
}

// Вызывается после обработки нажатия основным обработчиком (буфер уже обновлён)
static void AsAfterKey(DWORD vk) {
    if (AsIsPassThrough(vk) || g_busy) return;
    if (AsExcluded()) {
        KillTimer(g_asWnd, kAsTimer);
        return;
    }
    bool open = false, spaced = false;
    {
        Lock l;
        const size_t n = g_buf.size();
        open = n > 0 && g_buf[n - 1].ch != L' ';
        spaced = vk == VK_SPACE && n >= 2 && g_buf[n - 1].ch == L' ' && g_buf[n - 2].ch != L' ';
    }
    if (spaced && !g_asBusy) {
        HWND fg = GetForegroundWindow();
        if (fg && !AsIsConsole(fg)) {
            std::vector<Rec> snap;
            {
                Lock l;
                snap = g_buf;
            }
            AsFix fx;
            if (AsDecideHost(snap, 0, fx)) AsStartJob(AsMakeJob(fx, snap.size(), false));
        }
    }
    if (open) SetTimer(g_asWnd, kAsTimer, kAsPauseMs, NULL);
    else KillTimer(g_asWnd, kAsTimer);
}

// Обработчик клавиатуры при подключённом модуле: вызывает основной обработчик и добавляет проверку слов
static LRESULT CALLBACK AsKbProc(int code, WPARAM wp, LPARAM lp) {
    if (code != HC_ACTION) return KbProc(code, wp, lp);
    const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)lp;
    if (k->dwExtraInfo == kMarker) return KbProc(code, wp, lp);
    const bool down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
    const DWORD vk = k->vkCode;

    if (g_asBusy && !AsIsPassThrough(vk) && AsSwallow(k, down)) return 1;
    if (down && vk == VK_RETURN && !g_asBusy && AsEnterPlain() && AsWantsEnter()) return 1;

    const LRESULT r = KbProc(code, wp, lp);
    if (down) AsAfterKey(vk);
    return r;
}

// Выбор обработчика клавиатуры при запуске. Если рядом с программой нет autoswitch.dll (или он не подходит),
// возвращается обычный обработчик, и авто-переключение недоступно. Включается только наличием файла.
static HOOKPROC AsSetup() {
    const std::wstring path = g_exeDir + L"autoswitch.dll";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return KbProc;
    HMODULE mod = LoadLibraryExW(path.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!mod) return KbProc;

    AsVersionFn ver = (AsVersionFn)GetProcAddress(mod, "AsVersion");
    AsInitFn init = (AsInitFn)GetProcAddress(mod, "AsInit");
    AsDecideFn dec = (AsDecideFn)GetProcAddress(mod, "AsDecide");

    static AsHostApi api;
    api.script = AsCbScript;
    api.lang = AsCbLang;
    api.conv = AsCbConv;
    api.layouts = AsCbLayouts;
    api.exeDir = g_exeDir.c_str();

    if (!ver || !init || !dec || ver() != AS_API_VERSION || !init(&api)) {
        FreeLibrary(mod);
        return KbProc;
    }

    InitializeCriticalSection(&g_asCs);
    HINSTANCE hInst = GetModuleHandleW(NULL);
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = AsWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"LangSwitchAsWnd";
    RegisterClassW(&wc);
    g_asWnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInst, NULL);
    if (!g_asWnd) {
        FreeLibrary(mod);
        return KbProc;
    }
    g_asDecideFn = dec;
    return AsKbProc;
}
