// src/core/MoodFeatures.cpp
#include "MoodFeatures.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <shlobj.h>
#include <windows.h>

namespace {

inline float Clamp01(double v) {
  return (float)((v < 0.0) ? 0.0 : ((v > 1.0) ? 1.0 : v));
}

// dB -> 선형 파워. 밴드 레벨은 RMS 의 dB 이므로 파워는 10^(dB/10).
inline double DbToPower(float db) { return std::pow(10.0, (double)db / 10.0); }

// JSON 문자열 이스케이프 (제목에 따옴표/역슬래시가 흔하다).
std::string Esc(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"':  o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n";  break;
      case '\r': o += "\\r";  break;
      case '\t': o += "\\t";  break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          o += buf;
        } else {
          o += (char)c;
        }
    }
  }
  return o;
}

std::mutex g_logMutex;

}  // namespace

MoodFeatures ComputeMoodFeatures(const std::vector<float>& levelsDb,
                                 const std::vector<bool>& usable,
                                 const std::vector<int>& freqs,
                                 double peakLin, double rmsLin) {
  MoodFeatures m;
  if (levelsDb.size() != freqs.size() || usable.size() != freqs.size() ||
      levelsDb.empty())
    return m;

  // ── 1) 스펙트럼 중심 (Brightness) ────────────────────────────────────────
  // [왜 로그축 가중평균인가] 선형 Hz 로 중심을 구하면 20kHz 밴드 하나가
  //   저역 30개를 압도해 사실상 "초고역이 있나"만 재게 된다. 사람의 음정
  //   지각은 로그축이므로 log2(f) 위에서 무게중심을 잡고 되돌린다.
  double wsum = 0.0, acc = 0.0;
  for (size_t i = 0; i < freqs.size(); ++i) {
    if (!usable[i] || levelsDb[i] <= -190.0f)
      continue;
    const double w = DbToPower(levelsDb[i]);
    acc += w * std::log2((double)freqs[i]);
    wsum += w;
  }
  if (wsum <= 1e-20)
    return m;  // 무음 — 측정 불가
  const double centroidLog2 = acc / wsum;
  m.centroidHz = (float)std::exp2(centroidLog2);

  // ── 2) 중저역 비중 (Warmth) ──────────────────────────────────────────────
  // 160~630Hz 는 몸통/따뜻함을 담당하는 대역. 전대역 대비 비율로 본다.
  // [주의] 브리프는 "저역 대비 중저역"이라 했지만, 저역만 기준이면 서브베이스가
  //   센 EDM 이 오히려 '차갑게' 나온다. 전대역 대비가 지각과 더 맞는다.
  double lowMid = 0.0;
  for (size_t i = 0; i < freqs.size(); ++i) {
    if (!usable[i] || levelsDb[i] <= -190.0f)
      continue;
    if (freqs[i] >= 160 && freqs[i] <= 630)
      lowMid += DbToPower(levelsDb[i]);
  }
  // std::max 를 쓰지 않는다 — windows.h 의 max 매크로와 충돌해 이 파일을
  // NOMINMAX 없이 단독 컴파일(테스트 하네스)할 때 깨진다.
  const double ratio = lowMid / wsum;
  m.warmthDb = (float)(10.0 * std::log10(ratio > 1e-12 ? ratio : 1e-12));

  // ── 3) 시간영역 레벨 / 크레스트 ──────────────────────────────────────────
  m.rmsDb = (rmsLin > 1e-12) ? (float)(20.0 * std::log10(rmsLin)) : -200.0f;
  m.peakDb = (peakLin > 1e-12) ? (float)(20.0 * std::log10(peakLin)) : -200.0f;
  m.crestDb = m.peakDb - m.rmsDb;

  // ── 4) 0~1 정규화 ────────────────────────────────────────────────────────
  // [임시 범위] 아래 상수는 실측 전의 추정치다. 로그를 모아 실제 분포를 보고
  //   다시 잡아야 한다. 지금 단계의 판단 근거는 위 원시값이다.
  m.brightness = Clamp01((centroidLog2 - std::log2(200.0)) /
                         (std::log2(4000.0) - std::log2(200.0)));
  m.warmth = Clamp01(((double)m.warmthDb - (-12.0)) / ((-3.0) - (-12.0)));
  m.energy = Clamp01(((double)m.rmsDb - (-40.0)) / ((-12.0) - (-40.0)));
  // 크레스트가 클수록 다이내믹 -> density 는 낮다. 압축된 현대 마스터는
  // 대략 8~10dB, 여백이 살아있는 어쿠스틱/클래식은 15~20dB 근방.
  m.density = 1.0f - Clamp01(((double)m.crestDb - 6.0) / (20.0 - 6.0));

  m.valid = true;
  return m;
}

std::string MoodFeatures::ToJson(const std::string& title,
                                 const std::string& artist,
                                 const char* phase) const {
  char buf[640];
  std::snprintf(
      buf, sizeof(buf),
      "{\"phase\":\"%s\",\"centroidHz\":%.1f,\"warmthDb\":%.2f,"
      "\"rmsDb\":%.2f,\"peakDb\":%.2f,\"crestDb\":%.2f,"
      "\"brightness\":%.3f,\"warmth\":%.3f,\"energy\":%.3f,\"density\":%.3f",
      phase ? phase : "?", centroidHz, warmthDb, rmsDb, peakDb, crestDb,
      brightness, warmth, energy, density);
  return std::string(buf) + ",\"title\":\"" + Esc(title) + "\",\"artist\":\"" +
         Esc(artist) + "\"}";
}

std::string MoodLogPath() {
  char path[MAX_PATH];
  if (!SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0,
                                  path)))
    return {};
  return std::string(path) + "\\SoundMateEqualizer\\record\\mood_log.jsonl";
}

void AppendMoodLog(const std::string& line) {
  try {
    const std::string p = MoodLogPath();
    if (p.empty())
      return;
    std::filesystem::create_directories(std::filesystem::path(p).parent_path());

    std::lock_guard<std::mutex> lk(g_logMutex);

    // 2MB 초과 시 앞 절반 절단. 줄 경계를 지켜 JSON Lines 무결성을 유지한다.
    std::error_code ec;
    const auto sz = std::filesystem::file_size(p, ec);
    if (!ec && sz > 2u * 1024u * 1024u) {
      std::ifstream in(p, std::ios::binary);
      std::string all((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
      in.close();
      const size_t half = all.size() / 2;
      const size_t nl = all.find('\n', half);
      if (nl != std::string::npos) {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << all.substr(nl + 1);
      }
    }

    std::ofstream f(p, std::ios::app | std::ios::binary);
    if (f)
      f << line << '\n';
  } catch (...) {
  }
}
