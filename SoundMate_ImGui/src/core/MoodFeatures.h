// src/core/MoodFeatures.h
//
// [측정 단계] 순수 오디오 신호에서 뽑는 지각적 음향 지표 — 1차 4종.
//
// 목적은 아직 EQ 를 바꾸는 것이 **아니다**. "발라드와 EDM 이 실제로 다른 숫자를
// 내는가"를 로그로 확인하는 것이 전부다. 여기서 안 갈리면 나머지 지표나
// 스테레오 탭(엔진 수정)은 고민할 가치가 없다.
//
// [왜 4종만인가]
//   현재 오디오 탭은 audiodg 안에서 **모노로 다운믹스**된다(AudioTapWriter).
//   Vocal Presence(Mid/Side)와 Spatiality(Side 확산도)는 스테레오가 있어야만
//   계산되므로 탭 레이아웃 변경 = 엔진 수정 = reset.exe 동반 갱신이 필요하다.
//   Groove/Rhythmic Density 는 온셋 검출에 ~10ms hop 이 필요한데 현재 빠른
//   레벨은 120ms 시상수라 별도 경로가 필요하다.
//   반면 아래 4종은 SpectrumAnalyzer 가 이미 들고 있는 값의 산술이다.
#pragma once

#include <string>
#include <vector>

struct MoodFeatures {
  bool valid = false;

  // ── 원시값 (판단은 이 숫자로 한다) ──────────────────────────────────────
  // [1차 실측 결과] centroidHz 는 밝기 지표로 **실패**했다. 8곡에서 57~307Hz
  //   범위에 뭉쳤고 순서까지 역전됐다(EDM 69Hz < 첼로 248Hz). 원인은 파워
  //   가중 x 로그축 조합이다 — 음악 스펙트럼은 에너지 대부분이 최저역에
  //   있어서 결국 "베이스가 얼마나 센가"를 재게 된다.
  //   그래서 밝기 대표값을 tiltDb 로 옮기고 centroidHz 는 진단용으로만 남긴다.
  float centroidHz = 0.0f;  // 스펙트럼 중심 (로그축 가중평균) — 참고용
  float tiltDb = 0.0f;      // 고역(2k~16k) / 저역(100~800) 에너지 비 (dB) = 밝기
  float warmthDb = 0.0f;    // 중저역(160~630Hz) 대비 전대역 에너지 비 (dB)
  float rmsDb = 0.0f;       // 시간영역 RMS (dBFS)
  float peakDb = 0.0f;      // 시간영역 샘플 피크 (dBFS)
  float crestDb = 0.0f;     // peakDb - rmsDb. 압축 정도.

  // ── 0~1 정규화 (브리프의 무드 벡터 형식) ────────────────────────────────
  // [주의] 정규화 범위는 아직 근거 없는 임시값이다. 실측 로그를 보고
  //   조정해야 한다. 판단은 위 원시값으로 하고 이 값은 참고만.
  float brightness = 0.0f;  // dark 0 <-> 1 bright (tiltDb 기반)
  float warmth = 0.0f;      // cold 0 <-> 1 warm
  float energy = 0.0f;      // calm 0 <-> 1 intense
  float density = 0.0f;     // dynamic 0 <-> 1 compressed

  // 진단 로그 한 줄(JSON). AdaptiveEngine 이 파일에 append 한다.
  //
  // levelsDb 를 같이 받아 **31밴드 원시 레벨을 그대로 적는다.** 파생값만
  // 남기면 지표 정의를 고칠 때마다 곡을 다시 틀어야 한다 — 청취 세션이 가장
  // 비싼 자원이므로 원시값을 남겨 오프라인 재계산이 가능하게 한다.
  // (centroid 정의 실패를 이 로그 없이 재현하려면 8곡을 다시 틀어야 했다.)
  // limiterPct: 이 창에서 리미터가 활성이던 폴링 비율(%). 측정 불가면 음수.
  std::string ToJson(const std::string& title, const std::string& artist,
                     const char* phase, const std::vector<float>& levelsDb,
                     double limiterPct) const;
};

// levelsDb  : SpectrumAnalyzer 의 밴드 레벨 (BandLevelsDb / SlowLevelsDb)
// usable    : 나이퀴스트 제외 마스크
// freqs     : AIClient::F31
// peakLin/rmsLin : SpectrumAnalyzer::TakeTimeStats 결과 (선형 진폭)
//
// 크기가 안 맞거나 무음이면 valid=false 로 돌려준다.
MoodFeatures ComputeMoodFeatures(const std::vector<float>& levelsDb,
                                 const std::vector<bool>& usable,
                                 const std::vector<int>& freqs,
                                 double peakLin, double rmsLin);

// 로그 파일 경로: %LOCALAPPDATA%\SoundMateEqualizer\record\mood_log.jsonl
// (Program Files 가 아니라 LOCALAPPDATA — 관리자 권한 없이 쓸 수 있어야 한다.)
std::string MoodLogPath();

// JSON Lines 한 줄 append. 실패는 조용히 무시한다 (진단용이라 본류를
// 막으면 안 된다). 파일이 2MB 를 넘으면 앞 절반을 잘라낸다.
void AppendMoodLog(const std::string& line);
