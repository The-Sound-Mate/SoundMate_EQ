/*
 * SoundMate Controller — monitors config.txt and pushes EQ settings
 * into shared memory for the APO to pick up in real-time.
 *
 * Uses FindFirstChangeNotificationW + WaitForMultipleObjects instead of
 * polling, so changes are applied within one audio callback (~10 ms) rather
 * than up to 500 ms later.
 */

#include "include/EQController.h"
#include "include/DeviceManager.h"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <string>
#include <cstdio>
#include <cmath>

static const wchar_t* CONFIG_PATH =
    L"C:\\Program Files\\SoundMate Equalizer\\config.txt";
static const char* CONFIG_PATH_A =
    "C:\\Program Files\\SoundMate Equalizer\\config.txt";
static const wchar_t* CONFIG_DIR =
    L"C:\\Program Files\\SoundMate Equalizer";

// ============================================================================
// Parse config.txt and push settings into shared memory via EQController.
// Format (one EQ band per line):
//   Preamp: -3.0 dB
//   Filter: 1 100.0 +3.5 1.41
// Returns true if the file was read successfully.
// ============================================================================
static bool ParseAndApply(EQController& eq) {
    std::ifstream file(CONFIG_PATH_A);
    if (!file.is_open()) {
        std::cerr << " [!] config.txt not found — using flat response\n";
        eq.ResetBands();
        eq.SetMasterGain(0.f);
        eq.Apply();
        return false;
    }

    // Guard ALL writes with writeInProgress before touching shared memory
    eq.BeginWrite();

    eq.ResetBands();  // clear stale bands from previous config
    float preamp = 0.f;
    int   bandIdx = 0;
    std::string line;

    // Validation bounds — anything outside these ranges is either physically
    // pointless (gain > +24 dB causes hard clipping no matter what the limiter
    // does) or mathematically dangerous (Q < 0.1 produces extremely wide bands
    // with huge cumulative phase shift, Q > 10 yields ringing that the biquad
    // implementation can't keep numerically stable). NaN/inf in any field
    // would propagate through the filter state and silence the channel.
    auto sanitizeFloat = [](float v, float lo, float hi, float fallback) -> float {
        if (!std::isfinite(v)) return fallback;
        if (v < lo) return lo;
        if (v > hi) return hi;
        return v;
    };

    int skippedBands = 0;

    while (std::getline(file, line) && bandIdx < SOUNDMATE_MAX_BANDS) {
        if (line.empty() || line[0] == '#') continue;

        if (line.rfind("Preamp:", 0) == 0) {
            float v = 0.f;
            if (sscanf_s(line.c_str(), "Preamp: %f dB", &v) == 1) {
                preamp = sanitizeFloat(v, -30.f, 12.f, 0.f);
            }
        }
        else if (line.rfind("Filter:", 0) == 0) {
            int   id   = 0;
            float freq = 1000.f, gain = 0.f, q = 1.f;
            // Format: Filter: <id> <freq> <gain> <q>
            if (sscanf_s(line.c_str(), "Filter: %d %f %f %f",
                         &id, &freq, &gain, &q) == 4) {
                if (!std::isfinite(freq) || !std::isfinite(gain) || !std::isfinite(q)) {
                    ++skippedBands;
                    continue;
                }
                freq = sanitizeFloat(freq,    20.f, 20000.f, 1000.f);
                gain = sanitizeFloat(gain,   -24.f,    24.f,    0.f);
                q    = sanitizeFloat(q,        0.1f,   10.f,    1.f);
                eq.SetBand(bandIdx, freq, gain, q, true);
                ++bandIdx;
            } else {
                ++skippedBands;
            }
        }
    }

    eq.SetMasterGain(preamp);
    eq.Apply();

    if (skippedBands > 0) {
        std::cerr << " [!] " << skippedBands
                  << " filter line(s) skipped (out-of-range or malformed)\n";
    }

    std::cout << " [V] Config applied — preamp=" << preamp
              << " dB, " << bandIdx << " band(s)\n";
    return true;
}

// ============================================================================
// Write a default config.txt if none exists.
// ============================================================================
static void EnsureDefaultConfig() {
    DWORD attr = GetFileAttributesW(CONFIG_PATH);
    if (attr != INVALID_FILE_ATTRIBUTES) return;  // already exists

    // Create the directory if needed
    CreateDirectoryW(CONFIG_DIR, NULL);

    std::ofstream f(CONFIG_PATH_A);
    if (!f.is_open()) return;
    f << "# SoundMate EQ Configuration\n"
         "# Format: Filter: <id> <freq Hz> <gain dB> <Q>\n"
         "Preamp: 0.0 dB\n"
         "# Filter: 1 1000.0 0.0 1.0\n";
}

// ============================================================================
// SoundMate 앱이 살아있는지.
//
//   앱은 WinMain 첫 줄에서 이 뮤텍스를 만든다. 프로세스가 어떻게 끝나든 —
//   정상 종료든, 작업관리자 강제 종료든, 크래시든 — 커널이 핸들을 닫아
//   이름이 사라진다. 그래서 이보다 정확한 생사 표식이 없다.
//
//   [트레이는 살아있는 것이다] 창을 트레이로 숨겨도 프로세스는 남으므로
//   뮤텍스도 남는다. 트레이로 내렸을 때 EQ 를 유지하는 것은 의도된 동작이다
//   (main.cpp 의 "종료 시 EQ 해제" 주석 참고).
//
//   [만들지 못한 경우] Global\\ 이름은 권한이 필요해서, 앱이 권한 없이 떴다면
//   뮤텍스가 아예 없다. 그때는 아래 appSeen 가드에 걸려 이 기능이 그냥 꺼진다
//   — 예전과 똑같이 동작할 뿐 잘못 내려가지 않는다.
// ============================================================================
static const wchar_t* APP_MUTEX_NAME = L"Global\\SoundMate_EQ_AppMutex";

static bool AppAlive() {
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, APP_MUTEX_NAME);
    if (!h) return false;
    CloseHandle(h);
    return true;
}

// ============================================================================
// 커브를 거두고 진짜 패스스루로 만든다.
//
//   APO 의 eqActive 판정이 (활성 밴드 != 0 || masterGain != 0dB) 이므로,
//   밴드를 0 개로 만들고 masterGain 을 0 dB 로 내리면 EQ 체인 자체를 우회한다.
//
//   [스트림별 배정은 왜 안 건드리나] 앱이 칸에 써넣는 값은 -1(전역 커브)
//   아니면 0번 프로파일(평탄) 둘 뿐이다. 전역 커브가 평탄해졌으니 어느 쪽이든
//   결과는 원음이다.
//
//   [config.txt 도 같이 비운다] 앱이 정상 종료할 때 하는 일과 정확히 맞춘다.
//   앱은 종료 직전 ApplyBypass() 로 이 파일을 "Preamp: 0.0 dB" 한 줄로
//   덮어쓴다 (main.cpp 의 "종료 시 EQ 해제" — POWER OFF 와 같은 동작이라고
//   적혀 있다). 그리고 앱은 시작할 때 이 파일을 읽어 슬라이더를 채운다
//   (MainWindow 의 LoadEQFromFile).
//
//   그래서 파일을 남겨 두면 종료 방식에 따라 앱이 다르게 뜬다 — 정상 종료
//   뒤엔 0 에서 시작하는데 강제 종료 뒤엔 옛 커브가 그대로 살아난다.
//   실제로 그렇게 갈렸다. 소리는 우리가 껐는데 화면만 커브가 남아 있으니
//   더 헷갈린다.
//
//   [지워도 되는 파일인가] 된다. 곡별 EQ 와 프리셋은 RecordManager 의 DB 와
//   user_presets.json 에 따로 있다. 이 파일은 앱에서 엔진으로 가는 전달
//   통로일 뿐이고, 정상 종료가 이미 매번 비우고 있다.
// ============================================================================
static void FlattenToPassthrough(EQController& eq) {
    eq.BeginWrite();
    eq.ResetBands();
    eq.SetMasterGain(0.f);
    eq.Apply();

    // 앱의 ApplyBypass() 와 같은 내용. 우리가 쓰면 변경 알림이 한 번 뜨지만,
    //   호출한 쪽이 곧바로 루프를 빠져나가므로 다시 읽히지 않는다.
    std::ofstream f(CONFIG_PATH_A, std::ios::trunc);
    if (f.is_open())
        f << "Preamp: 0.0 dB\n";
}

// ============================================================================
int main() {
    std::cout << "==============================================\n"
                 "   SoundMate Controller\n"
                 "==============================================\n";

    EQController eq;
    if (!eq.Initialize()) {
        std::cerr << " [Error] Failed to connect to shared memory.\n"
                     "         Ensure the APO DLL is registered and audio is active.\n";
        return 1;
    }

    EnsureDefaultConfig();
    ParseAndApply(eq);  // load current config on startup

    // Create file-change notification on the config directory
    HANDLE hChange = FindFirstChangeNotificationW(
        CONFIG_DIR,
        FALSE,                        // don't watch subtrees
        FILE_NOTIFY_CHANGE_LAST_WRITE // only care about writes
    );

    if (hChange == INVALID_HANDLE_VALUE) {
        std::cerr << " [!] Cannot watch config directory — falling back to 1-second poll\n";
        hChange = NULL;
    }

    // Console-control event so Ctrl+C triggers a clean shutdown
    HANDLE hStop = CreateEventW(NULL, TRUE, FALSE, NULL);

    SetConsoleCtrlHandler([](DWORD) -> BOOL {
        // Signal the stop event from within the handler
        // (we find it via a global since lambdas can't capture here easily)
        return FALSE;  // let the default handler run (terminates the process)
    }, TRUE);

    std::cout << " [*] Monitoring: " << CONFIG_PATH_A << "\n"
                 " [*] Press Ctrl+C to stop.\n\n";

    // 앱 생사를 확인하는 주기. 강제 종료 후 소리가 돌아오기까지 최대 이만큼
    //   걸린다. 1초면 사람은 "바로" 로 느끼고, 뮤텍스 open 한 번은 공짜다.
    const DWORD kLivenessTickMs = 1000;

    // [핸들 배열] 예전에는 hChange 가 없을 때 handles[0] 에
    //   INVALID_HANDLE_VALUE 를 넣고 기다렸다. WaitForMultipleObjects 가 즉시
    //   WAIT_FAILED 로 떨어지고 루프가 그대로 도니 CPU 를 한 코어 태우는
    //   자리였다. 있는 핸들만 담는다.
    HANDLE handles[2];
    DWORD  handleCount = 0;
    if (hChange) handles[handleCount++] = hChange;  // 있으면 항상 0번
    const DWORD idxStop = handleCount;              // 그 다음이 정지 이벤트
    handles[handleCount++] = hStop;

    // 뮤텍스를 한 번이라도 본 적이 있어야 "앱이 죽었다" 고 판정한다.
    //   손으로 Controller 만 띄운 경우(개발/점검)에 곧바로 내려가 버리는 것과,
    //   앱이 뮤텍스를 만들지 못한 경우를 함께 막는다.
    bool appSeen = false;

    while (true) {
        DWORD result = WaitForMultipleObjects(handleCount, handles,
                                              FALSE,  // wake on ANY handle
                                              kLivenessTickMs);

        if (hChange && result == WAIT_OBJECT_0) {
            // File-change notification fired — re-parse config.txt
            // Small sleep to let the writer finish flushing
            Sleep(50);
            ParseAndApply(eq);

            // Re-arm the notification for the next change
            FindNextChangeNotification(hChange);
        }
        else if (result == WAIT_OBJECT_0 + idxStop) {
            // Stop event — clean exit
            break;
        }
        else if (result == WAIT_TIMEOUT && !hChange) {
            // Fallback poll (only active when FindFirstChangeNotification failed)
            ParseAndApply(eq);
        }
        // WAIT_FAILED: log and continue

        // ── [강제 종료 복구] ────────────────────────────────────────────────
        //   정상 종료라면 앱이 config.txt 를 평탄하게 덮어쓰고(ApplyBypass)
        //   우리를 taskkill 한다. 그런데 작업관리자로 앱을 죽이면 그 정리
        //   경로가 통째로 건너뛰어진다. 그러면 config.txt 에 마지막 커브가
        //   남고 우리는 멀쩡히 살아서 그걸 계속 공유 메모리에 밀어넣는다 —
        //   앱이 없는데도 EQ 가 영원히 걸린 채로 남는 것이다. 공유 메모리
        //   섹션은 audiodg 재시작도 견디므로 재부팅 전까지 안 풀린다.
        //
        //   여기서 커브를 거두고 우리도 함께 내려간다 — 앱이 정상 종료했을
        //   때와 똑같은 상태로 만들어 두는 것이다. 앱을 다시 켜면 앱이 우리를
        //   다시 띄우고(5초 감시), 슬라이더는 0 에서 시작한다.
        if (AppAlive()) {
            appSeen = true;
        }
        else if (appSeen) {
            std::cout << " [*] 앱이 사라졌다 — EQ 를 거두고 종료한다\n";
            FlattenToPassthrough(eq);
            break;
        }
    }

    if (hChange) FindCloseChangeNotification(hChange);
    if (hStop)   CloseHandle(hStop);
    return 0;
}
