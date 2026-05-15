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

    while (std::getline(file, line) && bandIdx < SOUNDMATE_MAX_BANDS) {
        if (line.empty() || line[0] == '#') continue;

        if (line.rfind("Preamp:", 0) == 0) {
            sscanf_s(line.c_str(), "Preamp: %f dB", &preamp);
        }
        else if (line.rfind("Filter:", 0) == 0) {
            int   id   = 0;
            float freq = 1000.f, gain = 0.f, q = 1.f;
            // Format: Filter: <id> <freq> <gain> <q>
            if (sscanf_s(line.c_str(), "Filter: %d %f %f %f",
                         &id, &freq, &gain, &q) == 4) {
                eq.SetBand(bandIdx, freq, gain, q, true);
                ++bandIdx;
            }
        }
    }

    eq.SetMasterGain(preamp);
    eq.Apply();

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

    HANDLE handles[2] = { hChange ? hChange : INVALID_HANDLE_VALUE, hStop };
    DWORD  handleCount = hChange ? 2 : 1;

    while (true) {
        DWORD result = WaitForMultipleObjects(
            handleCount, handles,
            FALSE,               // wake on ANY handle
            hChange ? INFINITE : 1000);  // poll fallback if no notification

        if (result == WAIT_OBJECT_0 && hChange) {
            // File-change notification fired — re-parse config.txt
            // Small sleep to let the writer finish flushing
            Sleep(50);
            ParseAndApply(eq);

            // Re-arm the notification for the next change
            FindNextChangeNotification(hChange);
        }
        else if (result == WAIT_OBJECT_0 + 1) {
            // Stop event — clean exit
            break;
        }
        else if (result == WAIT_TIMEOUT) {
            // Fallback poll (only active when FindFirstChangeNotification failed)
            ParseAndApply(eq);
        }
        // WAIT_FAILED: log and continue
    }

    if (hChange) FindCloseChangeNotification(hChange);
    if (hStop)   CloseHandle(hStop);
    return 0;
}
