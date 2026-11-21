// autoswitch.cpp — модуль авто-переключения раскладки (русская <-> английская), собирается в autoswitch.dll.
// Программа загружает его при запуске, если файл autoswitch.dll лежит рядом с LangSwitch.exe.
// Здесь таблицы, оценка слов, решение о переключении и пользовательский словарь Words.txt.
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

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <set>
#include <string>
#include <vector>

#include "autoswitch_api.h"

namespace {

enum { AL_NONE = 0, AL_EN = 1, AL_RU = 2 };

const AsHostApi* g_api = NULL;
std::wstring g_dir;
bool g_ready = false;

// ---------------------------------------------------------------- таблицы (частые буквосочетания)
const wchar_t* kBigEn =
    L"th he in er an re on at en nd ti es or te of ed is it al ar st to nt ng se ha as ou io le ve co me de hi ri ro ic "
    L"ne ea ra ce li ch ll be ma si om ur ca el ta la ns di fo ho pe ec pr no ct us ac ot il tr ly nc et ut ss so rs un "
    L"lo wa ge ie wh ee wi em ad ol rt po we na ul ni ts mo ow pa im mi ai sh ir su id os iv ia am fi ci vi pl ig tu ev "
    L"ld ry mp fe bl ab gh ty op wo sa ay ex ke fr oo av ag if ap gr od bo sp rd do uc bu ei ov by rm ep tt oc fa ef cu "
    L"rn sc gi da yo cr cl du ga qu ue ff ba ey ls va um pp ua up lu go ht ru ug ds lt pi rc rr eg au ck ew bi br tl ms "
    L"ye ud mb ip ub oi oa ok ob ib ak ik ph lk lf lp lm rk rb rg rl rv rp nk nn nl nm gn gs gl hy hr kn kl ks ki ku ko "
    L"ka za ze zi zo xt xp yp yr ys yt yn ym ur us ut ve vi vo wn wr ws";

const wchar_t* kBigRu =
    L"ст но то на ен ов ни ра во ко ол ор ро по ре ан ер ос ти ли ет ва ль ем ат ал ла ка ть ог го ел ны ск ин ие ой ий "
    L"ых ым ом ед ик ит ле ме мо ми ма му не ну ту ты де да ди до др дн дл ве ви вы вн вс вл вр за зн зд зв из иц ир ис ив "
    L"ил им ию ия ке ки кл кр ку лу ло ля мн мы нг нд нс нт нь ню ня об од ое ож оз ок он оп от оф ох оч ош пе пи пл пр пу "
    L"пы рг рд рж рз рк рн рс рт ру рц ря са св се си сл см сн со сп ср су та тв те тк тн тр тя уб ув уд уж уз уй ук ул "
    L"ум ун уп ур ус ут уч фи фо фр ха хо ху ца це ци чи ча че чн чт чу ша ше ши шл шн щи ыв ые ыл ын ыт эт эк юд ющ юч "
    L"ят яв яз як ял ян ях ье ьн ьс ью ьк йт йс йн ай ей ую ою ая яя ее ое бо бы ба бе би бр бл бу гу га ге ги гл гр гн "
    L"жа же жи жн жд зо зи зе зы зм зр иб ег аб ек ап аз ав ад аж ак ам ар ас ач ау ах ац аш бн бс вд вк вм вп вт вч вш "
    L"ех еж ез ел еп ер ес еч еш ещ ёт ёл ён ёр им ин ио ип ич иш кн кс кт км лб лг лж лк лм лн лп лс лт лч лш мб мг мм "
    L"мп мс мт мч мя нб нв нж нз нк нл нм нн нп нр нц нч нш ох оц ощ ух уц уш ущ юб юв юг юж юк юл юм юн юр юс ют юх яд "
    L"яж яй ям яп яр яс яц яч яш ои ио еи ею аю уе уи ёс ёв ёз ёк ёж ьи ьё ьо ьу ья ъе ъё ъя ъю гу гд кё шь шё щё чё жё чь "
    L"ыс ыр ыд ыб ыг ыж ык ыч ыш ыз бя бю бь бё вя вё вю вь гя гю дя дю дё дь зя зю зь кя кю кь лё лю мя мю мь мё пя пю "
    L"пь пё рё рю рь сё сю ся сь тё тю тя ть фя фю фь хё хю хя цю цё ща щу щё аи еа оа уа ри ра ря "
    L"бт вв вг вз вп вх вц гв дв дг дж дз дм дп дс дт дц жж жк жм жр зг зл зс кв кц лд лз лл лх лц мд мк мл мр мц мш нн "
    L"пн пс пт пц пч рб рв рл рм рп рх рч рш сб сг сд сж сз сх сц сч тл тм тс тх тц фл хв хл хм хн хр хт цв чк шк шт ид ще";

// допустимые английские сочетания согласных (для оценки пар не из таблицы)
const wchar_t* kEnCc =
    L"bl br bs bt ck cl cr cs ct dg dl dm dr ds fl fr fs ft gl gm gn gr gs kl kn kr ks lb lc ld lf lg lk lm lp ls lt lv "
    L"mb mn mp ms mt nc nd nf ng nk ns nt nv pl pn pr ps pt rb rc rd rf rg rk rl rm rn rp rs rt rv sc sk sl sm sn sp sq "
    L"st sw tc tr ts tw dw kw gw";

// короткие служебные слова (до 3 букв): по буквосочетаниям такие слова оценить нельзя
const wchar_t* kFuncEn =
    L"a i am an as at be by do go he if in is it me my no of oh ok on or so to up us we the and are but can for her him "
    L"his how its not now one our out she you all any day get has had was who why yes let may new off old own say see two "
    L"way too use did put set try ask end big bad";

const wchar_t* kFuncRu =
    L"я в и к с у о а на не но да ты мы он за из по до от ну же бы ли то во ко со об ни ой ах ох эх уж его все она они "
    L"уже или для мне нет вот тут там при без над под про чем кто где так еще ещё это что как вам вас нам нас мой наш "
    L"ваш был ага ого ура оно ему ней них ним сам сей тот той эта эти";

// сокращения, расширения файлов, технические термины: по-английски допустимы, а набранные кириллицей это бессмыслица
const wchar_t* kTechEn =
    L"vs pc tv id ip os ui ux hr pr qa ai ml db js ts pm cv ps mr dr ms st nd rd th ft kg km cm mm gb mb kb tb hz "
    L"pdf api url sql css cpu gpu ram rom usb ssd hdd png jpg jpeg gif svg bmp ico psd dvd faq fyi lol omg btw wtf vip "
    L"vpn web app dev bot bug git apk txt exe dll zip rar doc docx xls xlsx ppt pptx csv rtf odt iso bat cmd ini tmp sys "
    L"bin msi ipa dmg deb rpm html htm php xml json yaml yml java cpp lua bash mov wav flac ogg webm webp mkv avi tar "
    L"http https www ftp ssh ssl tls dns tcp udp lan wan gui cli sdk ide wifi bios ios mac npm pip";

bool isCyr(wchar_t c) { return c >= 0x0400 && c <= 0x052F; }
bool isLat(wchar_t c) {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
           (c >= 0x00C0 && c <= 0x024F && c != 0x00D7 && c != 0x00F7);
}
bool isLetter(wchar_t c) { return isCyr(c) || isLat(c); }

void fillBigrams(std::set<unsigned>& dst, const wchar_t* list) {
    const wchar_t* p = list;
    while (*p) {
        while (*p == L' ') ++p;
        if (p[0] && p[1] && p[0] != L' ' && p[1] != L' ') dst.insert(((unsigned)p[0] << 16) | (unsigned)p[1]);
        while (*p && *p != L' ') ++p;
    }
}

void fillWords(std::set<std::wstring>& dst, const wchar_t* list) {
    const wchar_t* p = list;
    while (*p) {
        while (*p == L' ') ++p;
        const wchar_t* q = p;
        while (*q && *q != L' ') ++q;
        if (q > p) dst.insert(std::wstring(p, (size_t)(q - p)));
        p = q;
    }
}

bool hasBigram(int lang, wchar_t a, wchar_t b) {
    static std::set<unsigned> en, ru;
    static bool init = false;
    if (!init) {
        fillBigrams(en, kBigEn);
        fillBigrams(ru, kBigRu);
        init = true;
    }
    return (lang == AL_EN ? en : ru).count(((unsigned)a << 16) | (unsigned)b) != 0;
}

bool inFunc(int lang, const std::wstring& w) {
    static std::set<std::wstring> en, ru;
    static bool init = false;
    if (!init) {
        fillWords(en, kFuncEn);
        fillWords(ru, kFuncRu);
        init = true;
    }
    return (lang == AL_EN ? en : ru).count(w) != 0;
}

// английское сокращение / расширение файла / технический термин (не обычное слово)
bool inTech(const std::wstring& w) {
    static std::set<std::wstring> tech;
    static bool init = false;
    if (!init) {
        fillWords(tech, kTechEn);
        init = true;
    }
    return tech.count(w) != 0 && !inFunc(AL_EN, w);
}

bool inShort(int lang, const std::wstring& w) { return inFunc(lang, w) || (lang == AL_EN && inTech(w)); }

bool vowel(int lang, wchar_t c) {
    const wchar_t* v = (lang == AL_EN) ? L"aeiouy" : L"аеёиоуыэюя";
    return c != 0 && wcschr(v, c) != NULL;
}

std::wstring lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}

// ---------------------------------------------------------------- оценка слова
// Список пар вида "ab cd ef" (ровно по два символа через пробел)
bool listHas(const wchar_t* list, wchar_t a, wchar_t b) {
    for (const wchar_t* p = list; p[0] && p[1]; p += 3) {
        if (p[0] == a && p[1] == b) return true;
        if (!p[2]) break;
    }
    return false;
}

// Класс пары букв по правилам языка: 0 — невозможна, 1 — редкая, 2 — допустимая
int ruPair(wchar_t a, wchar_t b) {
    const bool va = vowel(AL_RU, a), vb = vowel(AL_RU, b);
    if (b == L'ь') return (va || a == L'ь' || a == L'ъ' || a == L'й') ? 0 : 2;
    if (b == L'ъ') return (va || a == L'ь' || a == L'ъ' || a == L'й') ? 0 : 1;
    if (a == L'ъ') return wcschr(L"еёюя", b) ? 2 : 0;
    if (a == L'ь') {
        if (vb) return wcschr(L"еёюяио", b) ? 2 : 0;
        return b == L'й' ? 0 : 2;
    }
    if (a == L'ы') return vb ? (b == L'е' ? 2 : 0) : 2;
    if (b == L'ы') {
        if (va) return 0;
        if (wcschr(L"жшчщ", a)) return 0;
        if (wcschr(L"гкх", a)) return 1;
        return 2;
    }
    if (b == L'э') return 1;
    if (a == L'э') return vb ? 1 : 2;
    if (b == L'й') return va ? 2 : 0;
    if (a == L'й') return b == L'й' ? 0 : 2;
    if (va && vb) return 1;
    if (va) return 2;
    if (vb) {  // согласная + гласная
        if (wcschr(L"чщ", a) && wcschr(L"яю", b)) return 0;
        if (wcschr(L"жш", a) && b == L'я') return 0;
        if (wcschr(L"жш", a) && b == L'ю') return 1;
        return 2;
    }
    // согласная + согласная
    if (a == b) return wcschr(L"лнсмжтрдб", a) ? 1 : 0;
    if (a == L'щ') return 0;
    if (b == L'щ') return wcschr(L"бнтр", a) ? 1 : 0;
    if (wcschr(L"жшчц", a) && wcschr(L"жшчц", b)) return 0;
    if (a == L'ц') return wcschr(L"вкнт", b) ? 2 : 1;
    if (a == L'ч') return wcschr(L"ткнв", b) ? 2 : 1;
    if (a == L'ш') return wcschr(L"кнтлмвпр", b) ? 2 : 1;
    if (a == L'ж') return wcschr(L"дгжкнмвр", b) ? 2 : 1;
    if (a == L'х') return wcschr(L"влмнрткс", b) ? 2 : 1;
    if (a == L'ф') return wcschr(L"лртксн", b) ? 2 : 1;
    return 1;
}

int enPair(wchar_t a, wchar_t b) {
    const bool va = vowel(AL_EN, a), vb = vowel(AL_EN, b);
    if (a == L'q') return b == L'u' ? 2 : 0;
    if (b == L'q') return (va || wcschr(L"cs", a)) ? 1 : 0;
    if (a == L'j') return vb ? 2 : 0;
    if (b == L'j') return (va || wcschr(L"bdn", a)) ? 1 : 0;
    if (a == L'v') return vb ? 2 : (b == L'v' ? 1 : 0);
    if (b == L'v') return va ? 2 : (wcschr(L"dlrn", a) ? 2 : (a == L's' ? 1 : 0));
    if (a == L'x') return vb ? 2 : (wcschr(L"cpth", b) ? 2 : 0);
    if (b == L'x') return va ? 2 : (wcschr(L"nlr", a) ? 1 : 0);
    if (a == L'z') return vb ? 2 : (b == L'z' ? 1 : 0);
    if (b == L'z') return va ? 2 : (wcschr(L"lntrdz", a) ? 1 : 0);
    if (a == L'h' && !vb) return b == L't' ? 2 : (wcschr(L"mnrs", b) ? 1 : 0);
    if (b == L'h' && !va) return wcschr(L"ctspgwr", a) ? 2 : (wcschr(L"kdzb", a) ? 1 : 0);
    if (b == L'w' && !va) return wcschr(L"stdkglnr", a) ? 2 : 0;
    if (a == L'w' && !vb) return wcschr(L"hrnls", b) ? 2 : 0;
    if (va && vb) return 1;
    if (va || vb) return 2;
    if (a == b) return wcschr(L"lsf", a) ? 2 : 1;
    return listHas(kEnCc, a, b) ? 2 : 1;
}

// Класс пары: 3 — частая (есть в таблице), 2 — допустимая, 1 — редкая, 0 — невозможная
int pairClass(int lang, wchar_t a, wchar_t b) {
    const int cl = (lang == AL_EN) ? enPair(a, b) : ruPair(a, b);
    if (cl == 0) return 0;
    return hasBigram(lang, a, b) ? 3 : cl;
}

struct E {
    int rate;   // правдоподобие слова в языке, 0..100
    bool bad;   // слово не может быть словом языка
    bool hard;  // есть невозможное сочетание (а не просто нет гласных)
    int letters;
    E() : rate(0), bad(false), hard(false), letters(0) {}
};

// Оценка непрерывной группы букв
void runEval(const std::wstring& run, int lang, int& pairs, int& sum, bool& bad, bool& hard) {
    const size_t n = run.size();
    int vowels = 0, cons = 0, same = 1;
    for (size_t i = 0; i < n; ++i) {
        const wchar_t c = run[i];
        if (vowel(lang, c)) {
            ++vowels;
            cons = 0;
        } else if (++cons >= 5) {
            bad = true;
            hard = true;
        }
        if (i == 0) continue;
        const wchar_t p = run[i - 1];
        const int cl = pairClass(lang, p, c);
        ++pairs;
        sum += (cl == 3) ? 100 : (cl == 2) ? 70 : (cl == 1) ? 40 : 0;
        if (cl == 0) {
            bad = true;
            hard = true;
        }
        if (c == p) {
            if (++same >= 3) { bad = true; hard = true; }
        } else {
            same = 1;
        }
    }
    if (n >= 2 && vowels == 0) bad = true;
    if (n >= 1) {
        if (lang == AL_EN) {
            if (run[n - 1] == L'q') { bad = true; hard = true; }
        } else if (run[0] == L'ъ' || run[0] == L'ы' || run[0] == L'ь' || run[n - 1] == L'ъ') {
            bad = true;
            hard = true;
        }
    }
}

// Оценка слова (в нижнем регистре) как слова языка lang. В английском слове не бывает , ; [ ] < > { } : " между буквами
// и апострофа в начале: если они есть, текст, скорее всего, набран в чужой раскладке (там это буквы б ж х ъ э).
E evalWord(const std::wstring& s, int lang) {
    E r;
    int pairs = 0, sum = 0;
    std::wstring run;
    const size_t n = s.size();
    for (size_t i = 0; i <= n; ++i) {
        const wchar_t c = i < n ? s[i] : L'\0';
        if (c != L'\0' && isLetter(c)) {
            run += c;
            ++r.letters;
            continue;
        }
        if (!run.empty()) {
            runEval(run, lang, pairs, sum, r.bad, r.hard);
            run.clear();
        }
        if (lang == AL_EN && c != L'\0') {
            const bool inner = i > 0 && i + 1 < n && isLetter(s[i - 1]) && isLetter(s[i + 1]);
            if (inner && wcschr(L",;[]<>{}:\"", c)) { r.bad = true; r.hard = true; }
            if (c == L'\'' && i == 0 && s[n - 1] != L'\'') { r.bad = true; r.hard = true; }
        }
    }
    r.rate = pairs > 0 ? sum / pairs : (r.letters > 0 ? 50 : 0);
    return r;
}

// Повторяющийся шаблон длиной 1-3 символа (ахах, ыыыы, hehe)
bool isPattern(const std::wstring& s) {
    const size_t n = s.size();
    if (n < 4) return false;
    for (size_t p = 1; p <= 3 && 2 * p <= n; ++p) {
        bool ok = true;
        for (size_t i = p; i < n && ok; ++i) {
            if (s[i] != s[i - p]) ok = false;
        }
        if (ok) return true;
    }
    return false;
}

// Смех и подобные междометия (хахаха, пхпхпх, ыыыы, hahaha)
bool laughter(const std::wstring& c, int lang) {
    if (c.size() < 4 || lang == AL_NONE) return false;
    const wchar_t* set = (lang == AL_EN) ? L"haeiou" : L"хапыеиэоукфгм";
    const wchar_t key = (lang == AL_EN) ? L'h' : L'х';
    int keys = 0;
    for (size_t i = 0; i < c.size(); ++i) {
        if (!wcschr(set, c[i])) return false;
        if (c[i] == key) ++keys;
    }
    return keys >= 2 || isPattern(c);
}

bool badChar(wchar_t c) {
    if (c >= L'0' && c <= L'9') return true;
    switch (c) {
        case L'@': case L'\\': case L'_': case L'#': case L'$': case L'%':
        case L'^': case L'&': case L'=': case L'+': case L'|':
            return true;
    }
    return false;
}

// ---------------------------------------------------------------- словарь пользователя (Words.txt)
std::set<std::wstring> g_dict;
FILETIME g_dictTime = {0, 0};
DWORD g_dictCheck = 0;

std::wstring dictPath() { return g_dir + L"Words.txt"; }

bool readUtf8(const std::wstring& path, std::wstring& out, FILETIME* ft) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    if (ft) GetFileTime(h, NULL, NULL, ft);
    const DWORD size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > 4 * 1024 * 1024) { CloseHandle(h); return false; }
    std::string data(size, '\0');
    DWORD rd = 0;
    if (size > 0) ReadFile(h, &data[0], size, &rd, NULL);
    CloseHandle(h);
    data.resize(rd);
    size_t off = 0;
    if (data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB &&
        (unsigned char)data[2] == 0xBF)
        off = 3;
    out.clear();
    if (data.size() <= off) return true;
    const int wn = MultiByteToWideChar(CP_UTF8, 0, data.c_str() + off, (int)(data.size() - off), NULL, 0);
    if (wn > 0) {
        out.assign((size_t)wn, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, data.c_str() + off, (int)(data.size() - off), &out[0], wn);
    }
    return true;
}

void loadDict() {
    g_dict.clear();
    std::wstring ws;
    if (!readUtf8(dictPath(), ws, &g_dictTime)) return;
    size_t pos = 0;
    while (pos <= ws.size()) {
        size_t nl = ws.find(L'\n', pos);
        if (nl == std::wstring::npos) nl = ws.size();
        const std::wstring line = ws.substr(pos, nl - pos);
        pos = nl + 1;
        size_t b = 0, e = line.size();
        while (b < e && (line[b] == L' ' || line[b] == L'\t' || line[b] == L'\r')) ++b;
        while (e > b && (line[e - 1] == L' ' || line[e - 1] == L'\t' || line[e - 1] == L'\r')) --e;
        if (b >= e || line[b] == L'#') continue;
        g_dict.insert(lower(line.substr(b, e - b)));
    }
}

void createDictTemplate() {
    if (GetFileAttributesW(dictPath().c_str()) != INVALID_FILE_ATTRIBUTES) return;
    HANDLE h = CreateFileW(dictPath().c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    const char* t =
        "# LangSwitch: словарь авто-переключения раскладки. Одно слово в строке, строки с # игнорируются, регистр не важен.\r\n"
        "# Слово из словаря всегда переключается в раскладку, где оно набирается как записано (высший приоритет).\r\n"
        "# Пример: если записать \"бг\", то набранное в английской раскладке \",u\" всегда станет \"бг\".\r\n"
        "# Если записать оба варианта (\"бг\" и \",u\"), переключения для этого слова не будет.\r\n";
    DWORD wr = 0;
    WriteFile(h, t, (DWORD)strlen(t), &wr, NULL);
    CloseHandle(h);
}

void reloadDictIfChanged(DWORD now) {
    if (now - g_dictCheck < 3000) return;
    g_dictCheck = now;
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(dictPath().c_str(), GetFileExInfoStandard, &d)) {
        g_dict.clear();
        g_dictTime.dwLowDateTime = 0;
        g_dictTime.dwHighDateTime = 0;
        return;
    }
    if (CompareFileTime(&d.ftLastWriteTime, &g_dictTime) != 0) loadDict();
}

// ---------------------------------------------------------------- слова строки и их классификация
// Классы слова
enum {
    AC_R = 0,    // слово верное, не трогаем
    AC_W = 1,    // слово заведомо набрано не в той раскладке
    AC_SK = 2,   // подозрительное слово: нужны подтверждения (вес weight)
    AC_AMB = 3,  // допустимо в обоих языках ("vs" и "мы"): само не переключается
    AC_U0 = 4,   // неясно (в обеих раскладках выглядит нормально)
    AC_BAR = 5   // не оценивается (смешанная раскладка, цифры, чужая раскладка)
};

struct Tok {
    size_t ps, pe;
    int cls;
    UINT_PTR H, T;
    int langH, langT;
    int weight;
    bool listed;  // слово из списка служебных слов
    Tok() : ps(0), pe(0), cls(AC_BAR), H(0), T(0), langH(AL_NONE), langT(AL_NONE), weight(0), listed(false) {}
};

struct Cyc {
    std::vector<UINT_PTR> lat, cyr;
};

Cyc buildCyc() {
    Cyc c;
    UINT_PTR lat[16], cyr[16];
    int nl = 0, nc = 0;
    g_api->layouts(lat, &nl, cyr, &nc, 16);
    for (int i = 0; i < nl && i < 16; ++i) c.lat.push_back(lat[i]);
    for (int i = 0; i < nc && i < 16; ++i) c.cyr.push_back(cyr[i]);
    return c;
}

// Язык предыдущих слов строки (до 3 слов, по раскладке, в которой они сейчас стоят); 0 — неопределённо
int context(const AsChar* buf, size_t s) {
    int cnt[3] = {0, 0, 0};
    int tokens = 0;
    size_t i = s;
    for (int t = 0; t < 3; ++t) {
        while (i > 0 && buf[i - 1].ch == L' ') --i;
        if (i == 0) break;
        const size_t e = i;
        while (i > 0 && buf[i - 1].ch != L' ') --i;
        int letters = 0;
        for (size_t k = i; k < e; ++k) {
            if (isLetter(buf[k].ch)) ++letters;
        }
        if (letters < 2) continue;
        const int lg = g_api->lang(buf[e - 1].cur);
        if (lg == AL_EN || lg == AL_RU) {
            ++cnt[lg];
            ++tokens;
        }
    }
    for (int lg = 1; lg <= 2; ++lg) {
        if (cnt[lg] >= 2 || (cnt[lg] == 1 && tokens == 1)) return lg;
    }
    return 0;
}

bool atStart(const AsChar* buf, size_t pos) {
    for (size_t i = 0; i < pos; ++i) {
        if (buf[i].ch != L' ') return false;
    }
    return true;
}

// Последние слова буфера (до maxTokens), от первого к последнему
std::vector<Tok> tokens(const AsChar* buf, size_t n, size_t maxTokens) {
    std::vector<Tok> v;
    size_t e = n;
    while (v.size() < maxTokens) {
        while (e > 0 && buf[e - 1].ch == L' ') --e;
        if (e == 0) break;
        size_t s = e;
        while (s > 0 && buf[s - 1].ch != L' ') --s;
        Tok t;
        t.ps = s;
        t.pe = e;
        v.push_back(t);
        e = s;
    }
    std::reverse(v.begin(), v.end());
    return v;
}

// Слово можно переключить вместе с соседним (набрано в той же раскладке и подозрительное)
bool pullable(const Tok& p, UINT_PTR H) {
    return p.H == H && (p.cls == AC_W || p.cls == AC_SK || p.cls == AC_AMB);
}

// Определяет класс слова. Слово переключается, только когда оно неправдоподобно в раскладке набора
// и правдоподобно в другой; сомнительные слова остаются без изменений и ждут соседей.
void classify(const AsChar* buf, Tok& t, const Cyc& cy, int mode) {
    const bool pause = (mode == 1);
    t.cls = AC_BAR;
    t.weight = 0;
    t.listed = false;
    t.H = t.T = 0;
    t.langH = t.langT = AL_NONE;

    const size_t len = t.pe - t.ps;
    if (len == 0 || len > 40) return;
    const UINT_PTR H = buf[t.ps].cur;
    std::wstring w;
    w.reserve(len);
    for (size_t i = t.ps; i < t.pe; ++i) {
        const AsChar& r = buf[i];
        if (r.neutral || r.cur != H || r.home != H || badChar(r.ch)) return;  // только свежий текст одной раскладки
        w += r.ch;
    }
    const int scrH = g_api->script(H);
    const int langH = g_api->lang(H);
    if (langH == AL_NONE) return;

    UINT_PTR T = 0;
    const std::vector<UINT_PTR>& cand = (scrH == 1) ? cy.cyr : cy.lat;
    for (size_t i = 0; i < cand.size(); ++i) {
        const int lg = g_api->lang(cand[i]);
        if (lg != AL_NONE && lg != langH) { T = cand[i]; break; }
    }
    if (!T) return;
    const int langT = g_api->lang(T);

    t.H = H;
    t.T = T;
    t.langH = langH;
    t.langT = langT;
    t.cls = AC_R;  // по умолчанию слово не трогаем

    std::wstring cv(w.size(), L' ');
    for (size_t i = 0; i < w.size(); ++i) cv[i] = g_api->conv(w[i], H, T);

    // ядро слова: без знаков по краям. Край обрезается, если в другой раскладке там знак препинания
    // (например, русская "ю" в начале слова — точка английской раскладки: ".apk")
    size_t a = 0, b = w.size();
    while (a < b && !isLetter(cv[a])) ++a;
    while (b > a && !isLetter(cv[b - 1])) --b;
    if (a >= b || a > 2 || w.size() - b > 3) return;

    const std::wstring lw = lower(w.substr(a, b - a));
    const std::wstring lc = lower(cv.substr(a, b - a));
    const size_t n = lw.size();
    const bool dotEdge = (a > 0 && cv[a - 1] == L'.');

    // словарь пользователя сильнее оценок
    if (!g_dict.empty()) {
        if (g_dict.count(lw)) return;  // набранная форма объявлена верной
        if (g_dict.count(lc)) { t.cls = AC_W; return; }
    }

    bool lcLetters = true;
    for (size_t i = 0; i < lc.size(); ++i) {
        if (!isLetter(lc[i])) { lcLetters = false; break; }
    }
    const E eH = evalWord(lw, langH);
    E eT = evalWord(lc, langT);
    if (!lcLetters) { eT.bad = true; eT.hard = true; }

    const bool typedKnown = inShort(langH, lw);
    const bool tFunc = lcLetters && inFunc(langT, lc);
    const bool tTech = lcLetters && langT == AL_EN && inTech(lc);
    const bool typedBad = eH.bad || eH.rate < 55;
    const bool targetGood = !eT.bad && eT.rate >= ((n >= 5) ? 70 : 80) && eT.letters >= 3;

    if (typedKnown) {  // допустимо в своей раскладке: само по себе не переключается
        t.cls = (tFunc || tTech) ? AC_AMB : AC_R;
        return;
    }
    if (laughter(lw, langH)) return;
    if (lcLetters && n >= 4 && laughter(lc, langT)) { t.cls = AC_W; return; }
    // английская аббревиатура / расширение файла, набранные кириллицей: по-русски это бессмыслица
    if (tTech && (n >= 3 || dotEdge)) { t.cls = AC_W; return; }

    if (tFunc && n <= 3) {  // служебное слово другой раскладки
        t.listed = true;
        if (n == 1) {
            t.cls = AC_SK;
            t.weight = 1;
        } else if (typedBad) {
            t.cls = AC_SK;
            t.weight = 6;
        } else {
            t.cls = pause ? AC_U0 : AC_SK;
            t.weight = 4;
        }
        return;
    }
    if (n <= 2) { t.cls = AC_U0; return; }

    if (n >= 4) {
        if (eH.bad && targetGood) t.cls = pause ? (eH.hard ? AC_W : AC_U0) : AC_W;
        else if (!eH.bad && eH.rate < 55 && targetGood) { t.cls = pause ? AC_U0 : AC_SK; t.weight = 3; }
        else t.cls = AC_U0;
        return;
    }
    // слово из трёх букв без списка: нужны подтверждения соседних слов
    if (eH.bad && targetGood && !pause) {
        t.cls = AC_SK;
        t.weight = eH.hard ? 3 : 2;
        return;
    }
    // слово, которое в набранной раскладке выглядит заметно хуже, чем в другой ("сге" и "cut"):
    // само не переключается, но подтягивается к соседним словам
    if (!pause && targetGood && !eH.bad && eH.rate <= 75 && eT.rate >= 90 && eT.rate - eH.rate >= 25) {
        t.cls = AC_SK;
        t.weight = 1;
        return;
    }
    t.cls = AC_U0;
}

// Решает, нужно ли что-то переключить. mode: 0 — после пробела, 1 — по паузе в наборе, 2 — перед Enter.
// Слово решается по соседям: подозрительные слова ждут подтверждения; уверенное слово "тянет" за собой подозрительные
// слова, набранные до него в той же раскладке; верное слово другой раскладки подтверждает их.
bool decide(const AsChar* buf, size_t n, int mode, AsResult* out) {
    out->start = 0;
    out->end = -1;
    out->target = 0;
    reloadDictIfChanged(GetTickCount());

    const bool pause = (mode == 1);
    const Cyc cy = buildCyc();
    std::vector<Tok> tk = tokens(buf, n, 9);
    if (tk.empty()) return false;
    for (size_t i = 0; i < tk.size(); ++i) classify(buf, tk[i], cy, mode);

    const size_t k = tk.size() - 1;
    const Tok& last = tk[k];
    if (last.cls == AC_BAR || last.cls == AC_U0) return false;

    // --- последнее слово подозрительное или неправильное: оцениваем его вместе с предыдущими такими же
    if (last.cls != AC_R) {
        size_t lo = k;
        while (lo > 0 && pullable(tk[lo - 1], last.H)) --lo;
        // неоднозначные слова в начале области подтягиваем только в начале текста или после слов целевой раскладки
        while (lo < k && tk[lo].cls == AC_AMB && !(atStart(buf, tk[lo].ps) || context(buf, tk[lo].ps) == last.langT))
            ++lo;

        bool strong = false;
        int total = 0, listed = 0;
        for (size_t i = lo; i <= k; ++i) {
            const Tok& t = tk[i];
            if (t.cls == AC_W) strong = true;
            else if (t.cls == AC_SK) { total += t.weight; if (t.listed) listed += t.weight; }
        }
        if (!strong && total == 0) return false;
        const int ctx = context(buf, tk[lo].ps);
        const bool start = atStart(buf, tk[lo].ps);
        const int lt = last.langT;

        bool go = strong || total >= 6;
        if (!go && total >= 4 && (start || ctx == lt || mode == 2)) go = true;
        if (!go && total >= 3 && mode == 2) go = true;
        if (!go && total >= 2 && ctx == lt && mode != 1) go = true;
        if (!go && listed >= 1 && mode == 2 && (start || ctx == lt)) go = true;  // "z" + Enter -> "я"
        if (!go) return false;

        out->start = (int)tk[lo].ps;
        out->end = -1;
        out->target = last.T;
        return true;
    }

    // --- последнее слово верное: оно подтверждает подозрительные слова другой раскладки перед ним
    if (pause || k == 0) return false;
    const size_t hi = k - 1;
    const Tok& prev = tk[hi];
    if (!(prev.cls == AC_SK || prev.cls == AC_W || prev.cls == AC_AMB)) return false;
    if (prev.H == last.H || prev.langT != last.langH) return false;

    size_t lo = hi;
    while (lo > 0 && pullable(tk[lo - 1], prev.H)) --lo;
    while (lo < hi && tk[lo].cls == AC_AMB && !(atStart(buf, tk[lo].ps) || context(buf, tk[lo].ps) == prev.langT))
        ++lo;

    bool strong = false;
    int total = 0, amb = 0;
    for (size_t i = lo; i <= hi; ++i) {
        const Tok& t = tk[i];
        if (t.cls == AC_W) strong = true;
        else if (t.cls == AC_SK) total += t.weight;
        else if (t.cls == AC_AMB) ++amb;
    }
    const int ctx = context(buf, tk[lo].ps);
    const bool start = atStart(buf, tk[lo].ps);
    const int sum = total + 3;  // верное слово следом — подтверждение
    bool go = strong || (total > 0 && (sum >= 6 || (sum >= 4 && (start || ctx == prev.langT))));
    if (!go && total == 0 && amb > 0 && start && tk[lo].cls == AC_AMB) go = true;  // "vs при" -> "мы при"
    if (!go) return false;

    out->start = (int)tk[lo].ps;
    out->end = (int)tk[hi].pe;
    out->target = last.H;
    return true;
}

}  // namespace

extern "C" {

__declspec(dllexport) int AsVersion(void) { return AS_API_VERSION; }

__declspec(dllexport) int AsInit(const AsHostApi* api) {
    try {
        if (!api || !api->script || !api->lang || !api->conv || !api->layouts || !api->exeDir) return 0;
        g_api = api;
        g_dir = api->exeDir;
        createDictTemplate();
        loadDict();
        g_ready = true;
        return 1;
    } catch (...) {
        return 0;
    }
}

__declspec(dllexport) int AsDecide(const AsChar* buf, int n, int mode, AsResult* out) {
    if (!g_ready || !buf || n <= 0 || !out) return 0;
    try {
        return decide(buf, (size_t)n, mode, out) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

}  // extern "C"
