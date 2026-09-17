// SoundMate_ImGui/src/ui/Lang.cpp
// 문자열 표 실체. 설명은 Lang.h 참고.
#include "Lang.h"

#include <atomic>
#include <cstring>

namespace Lang {
namespace {

// ---------------------------------------------------------------------------
// 영어 — 기준 표. 여기는 항상 COUNT 칸이 꽉 차 있다.
// ---------------------------------------------------------------------------
const char* const kEn[COUNT] = {
#define SM_STR(id, en) en,
#include "Lang_Strings.inc"
#undef SM_STR
};

// ---------------------------------------------------------------------------
// 한국어 — 번역된 칸만 채운다. 비어 있는 칸은 nullptr 로 남고 T() 가
// 영어를 내보낸다. 표는 Set() 이 처음 불릴 때 한 번만 채워진다.
//
// [언어 추가] 아래 세 덩어리를 그대로 복사해서 이름만 바꾸면 끝이다:
//   표(kKo) / 채우는 함수(InitKo) / kLanguages[] 한 줄.
//   호출부(.cpp 248곳)는 건드리지 않는다.
// ---------------------------------------------------------------------------
const char* kKo[COUNT] = {};  // 전부 nullptr 로 시작

void InitKo() {
#define SM_TR(id, tr) kKo[id] = tr;
#include "Lang_ko.inc"
#undef SM_TR
}

struct Entry {
    const char*        code;   // 설정 파일에 저장되는 값 ("en")
    const char*        name;   // 콤보에 보이는 이름
    const char* const* table;  // COUNT 칸. nullptr 칸은 영어로 대체
    void             (*init)(); // 표를 채우는 함수 (영어는 필요 없음)
};

// [순서 고정] 0번은 반드시 init 이 필요 없는 언어여야 한다. g_cur 가 0 으로
//   시작하는데 번역표는 Set() 이 처음 불릴 때야 채워지므로, 지연 초기화가
//   필요한 언어를 0번에 두면 Set() 이전의 T() 가 전부 영어로 떨어진다.
//   기본 표시 언어를 바꾸려면 여기가 아니라 AppSettings::language 를 고친다.
const Entry kLanguages[] = {
    { "en", u8"English",  kEn, nullptr },
    { "ko", u8"한국어", kKo, InitKo },
};

constexpr int kLangCount = (int)(sizeof(kLanguages) / sizeof(kLanguages[0]));

// init() 을 언어당 한 번만 부르기 위한 표시. 언어 수가 늘면 같이 늘린다.
constexpr int kMaxLanguages = 16;
static_assert(kLangCount <= kMaxLanguages, "kMaxLanguages 를 늘릴 것");
bool g_inited[kMaxLanguages] = {};

// MainWindow 가 작업 스레드에서도 SetStatus() 로 문구를 읽는다.
// 읽기 경합만 막으면 되므로 relaxed 로 충분하다.
std::atomic<int> g_cur{0};

} // namespace

const char* T(Id id) {
    if (id < 0 || id >= COUNT) return "";
    const int i = g_cur.load(std::memory_order_relaxed);
    const char* const* tbl = kLanguages[i].table;
    if (tbl) {
        const char* s = tbl[id];
        if (s) return s;
    }
    return kEn[id];  // 번역 누락 — 영어로 떨어진다
}

int Count() { return kLangCount; }

const char* CodeAt(int index) {
    if (index < 0 || index >= kLangCount) return "";
    return kLanguages[index].code;
}

const char* NameAt(int index) {
    if (index < 0 || index >= kLangCount) return "";
    return kLanguages[index].name;
}

int IndexOfCode(const char* code) {
    if (!code || !*code) return -1;
    for (int i = 0; i < kLangCount; ++i) {
        if (_stricmp(kLanguages[i].code, code) == 0) return i;
    }
    return -1;
}

bool Set(const char* code) {
    const int i = IndexOfCode(code);
    if (i < 0) return false;
    // 번역 표는 처음 고를 때 한 번만 채운다.
    if (kLanguages[i].init && !g_inited[i]) {
        kLanguages[i].init();
        g_inited[i] = true;
    }
    g_cur.store(i, std::memory_order_relaxed);
    return true;
}

int CurrentIndex() { return g_cur.load(std::memory_order_relaxed); }

const char* CurrentCode() { return kLanguages[CurrentIndex()].code; }

const char* CurrentName() { return kLanguages[CurrentIndex()].name; }

} // namespace Lang
