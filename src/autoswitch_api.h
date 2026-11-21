// autoswitch_api.h — интерфейс между программой (LangSwitch.exe) и модулем авто-переключения (autoswitch.dll).
// Только простые структуры и функции, без типов стандартной библиотеки.
#pragma once

#include <windows.h>

#define AS_API_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

// символ буфера набранного текста
typedef struct AsChar {
    wchar_t ch;
    UINT_PTR cur;   // раскладка (HKL), в которой символ находится сейчас
    UINT_PTR home;  // раскладка, в которой он был набран
    int neutral;    // 1 — не зависит от раскладки
} AsChar;

// что программа предоставляет модулю
typedef struct AsHostApi {
    int (*script)(UINT_PTR hkl);  // 1 — латиница, 2 — кириллица, 0 — другая
    int (*lang)(UINT_PTR hkl);    // 1 — английская, 2 — русская, 0 — не поддерживается
    wchar_t (*conv)(wchar_t c, UINT_PTR from, UINT_PTR to);  // тот же символ на той же клавише в другой раскладке
    // установленные латинские и кириллические раскладки (последние использованные первыми), до cap штук каждого вида
    void (*layouts)(UINT_PTR* lat, int* nLat, UINT_PTR* cyr, int* nCyr, int cap);
    const wchar_t* exeDir;  // папка программы, заканчивается на '\'
} AsHostApi;

// решение модуля
typedef struct AsResult {
    int start;       // индекс первого символа области замены
    int end;         // конец области (не включая); -1 — до конца буфера
    UINT_PTR target; // раскладка, в которую нужно перевести область
} AsResult;

typedef int (*AsVersionFn)(void);
typedef int (*AsInitFn)(const AsHostApi* api);
// mode: 0 — после пробела, 1 — по паузе в наборе, 2 — перед Enter. Возвращает 1, если нужно переключить область
typedef int (*AsDecideFn)(const AsChar* buf, int n, int mode, AsResult* out);

#ifdef __cplusplus
}
#endif
