// supabase/functions/resolve-track/curve.ts
//
// LocalCurve 의 서버 이식본. 장르 + 사용자 설문 성향 -> 31밴드 dB 커브.
//
// [왜 서버로 옮겼나]
//   1) 앱 재배포 없이 튜닝 — 커브는 실측하며 자주 고친다. 클라이언트에 있으면
//      한 번 고칠 때마다 재빌드·재설치·사용자 업데이트가 필요하다.
//   2) 알고리즘 은닉 — 장르 16종 테이블과 설문 20축 매핑이 exe 에서 사라진다.
//   3) 과금 게이트 — 플랜에 따라 다른 커브를 줄 수 있다.
//
// [캐시하지 않는 이유]
//   커브는 (장르 x 사용자 설문) 의 함수다. 장르는 곡 속성이라 공유 가능하지만
//   설문은 사용자마다 다르다. track_meta 에 캐시하면 남의 취향이 적용된다.
//   순수 연산이라 매번 계산해도 비용이 사실상 0 이다.

export const F31 = [
  20, 25, 31, 40, 50, 63, 80, 100, 125, 160,
  200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600,
  2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000,
];

// 2: 에너지 보존 중립화 + 과도 부스트 소프트니 (1 은 가중평균 중립화)
export const CURVE_VERSION = 2;

const CLAMP_DB = 12.0;

type Shape = { k: "peak" | "low" | "high"; fc: number; g: number; w: number };

// 로그 주파수 축의 가우시안 피크.
function evalPeak(f: number, s: Shape): number {
  const x = Math.log2(f / s.fc) / (s.w * 0.5);
  return s.g * Math.exp(-0.5 * x * x);
}

// 로지스틱 셸프. f == fc 에서 정확히 g/2 (표준 셸프 정의와 일치).
function evalShelf(f: number, s: Shape, low: boolean): number {
  const x = Math.log2(f / s.fc) / s.w;
  const k = low ? 3.0 : -3.0;
  return s.g / (1.0 + Math.exp(k * x));
}

function evalShape(f: number, s: Shape): number {
  if (s.k === "peak") return evalPeak(f, s);
  return evalShelf(f, s, s.k === "low");
}

// [v0.1.0 재설계] 실측 기준 스펙트럼 — 31밴드 상대 파워 (총합 1).
//
// [왜 가중평균을 버렸나]
//   이전 판은 200Hz~8kHz 밴드패스 가중평균을 0dB 로 맞췄다. 그 기준에서
//   40Hz 가중치는 1kHz 대비 -13.9dB 라, 40Hz 에 +8dB 를 얹어도 평균이 거의
//   안 움직인다. 실측 결과 "가중평균 0dB" 를 지키면서도 실제 음악에 걸었을 때
//   에너지가 **+3.74dB** 늘었다(설문 bass_heavy). 그 3.74dB 가 피크 -0.3dBFS
//   마스터를 리미터에 처박아 개입률 94% 를 만들었고, 샘플피크 리미터는
//   광대역 감쇠라 킥마다 보컬이 눌리는 먹먹함이 됐다.
//
// [왜 K-weighting 도 아닌가]
//   K-weighting 은 사람이 얼마나 크게 느끼는지의 척도다. 리미터가 무는 것은
//   느낌이 아니라 전기적 에너지가 결정한다. 실측 음악은 40Hz 가 1kHz 보다
//   5.3dB 강한데 K-weighting 은 -6.3dB 로 본다. 여전히 11.6dB 어긋난다.
//
// [출처] mood_log.jsonl 실측. 창 수가 곡마다 12배까지 차이나므로 곡별 평균을
//   먼저 낸 뒤 곡끼리 평균했다.
// [잠정치] 표본 4곡(발라드/재즈/K-pop/첼로). 클래식·메탈·EDM 이 빠져 있다.
//   LocalCurve.cpp 의 kRefSpectrum 과 **반드시 같은 값**이어야 한다 —
//   서버 커브와 폴백 커브가 달라지면 안 된다.
const REF_SPECTRUM = [
  3.833e-3, 8.431e-3, 2.528e-2, 8.391e-2, 5.447e-2, 4.655e-2,
  5.762e-2, 7.529e-2, 7.213e-2, 9.195e-2, 1.089e-1, 6.389e-2,
  5.295e-2, 6.215e-2, 4.103e-2, 3.245e-2, 2.901e-2, 2.182e-2,
  1.622e-2, 1.326e-2, 9.527e-3, 6.629e-3, 5.424e-3, 4.083e-3,
  3.553e-3, 2.969e-3, 2.863e-3, 2.058e-3, 1.203e-3, 4.476e-4,
  5.526e-5,
];

// 기준 스펙트럼에 이 커브를 걸었을 때의 총에너지 변화(dB).
function energyChangeDb(gains: number[]): number {
  let p0 = 0, p1 = 0;
  for (let i = 0; i < gains.length && i < REF_SPECTRUM.length; ++i) {
    p0 += REF_SPECTRUM[i];
    p1 += REF_SPECTRUM[i] * Math.pow(10, gains[i] / 10);
  }
  return p0 > 0 ? 10 * Math.log10(p1 / p0) : 0;
}

// [과도 부스트 캡] SOFT_KNEE_DB 를 넘는 부스트를 완만히 포화시킨다.
//
// 주파수 경계를 두지 않는다. "200Hz 미만만 캡" 같은 규칙은 160Hz 와 200Hz
// 사이에 수 dB 단차를 만들고, Q=4.32 바이쿼드가 그것을 그대로 구현하면 그
// 지점에 공진성 굴곡이 생긴다. 전 대역에 같은 규칙을 걸면 경계가 없다.
// 실제로 +3dB 를 넘는 것은 저역뿐이므로 효과는 저역 캡과 같고, 고음 강조
// 설문에서 고역이 과해져도 같은 규칙이 자동 적용된다.
const SOFT_KNEE_DB = 3.0;
const SOFT_KNEE_RANGE_DB = 3.0;
function softKnee(g: number): number {
  if (g <= SOFT_KNEE_DB) return g;
  return SOFT_KNEE_DB +
         SOFT_KNEE_RANGE_DB * Math.tanh((g - SOFT_KNEE_DB) / SOFT_KNEE_RANGE_DB);
}

// ── 장르 커브 ────────────────────────────────────────────────────────────────
// [튜닝 노트] 게인 크기는 "중립화 이후"를 기준으로 잡아야 한다. 저역/고역 셸프만
// 올리면 대부분이 공통 오프셋이라 중립화에서 걷혀나가고 거의 평탄한 커브가
// 남는다. 그래서 모든 장르가 중역 딥(300~500Hz)을 함께 갖는다 — 오프셋이 아니라
// '윤곽'을 만드는 성분이다.
const DEFAULT_GENRE: Shape[] = [
  { k: "low",  fc: 90,    g: 3.0, w: 1.3 },
  { k: "peak", fc: 500,   g: -1.5, w: 2.0 },
  { k: "high", fc: 9000,  g: 2.5, w: 1.4 },
];

// 위에서부터 먼저 일치하는 항목을 쓴다. "hip"/"k-pop" 이 "pop" 보다 먼저 걸리도록
// "pop" 은 맨 뒤에 둔다.
const GENRE_TABLE: Array<[string, Shape[]]> = [
  ["hip", [
    { k: "low", fc: 90, g: 4.5, w: 1.2 }, { k: "peak", fc: 500, g: -2.0, w: 1.8 },
    { k: "peak", fc: 3000, g: 2.0, w: 1.5 }, { k: "high", fc: 8000, g: 2.0, w: 1.5 }]],
  ["rap", [
    { k: "low", fc: 90, g: 4.5, w: 1.2 }, { k: "peak", fc: 500, g: -2.0, w: 1.8 },
    { k: "peak", fc: 3000, g: 2.0, w: 1.5 }, { k: "high", fc: 8000, g: 2.0, w: 1.5 }]],
  ["r&b", [
    { k: "low", fc: 85, g: 4.0, w: 1.2 }, { k: "peak", fc: 500, g: -1.5, w: 1.8 },
    { k: "peak", fc: 2500, g: 2.0, w: 1.5 }, { k: "high", fc: 9000, g: 1.5, w: 1.4 }]],
  ["soul", [
    { k: "low", fc: 85, g: 4.0, w: 1.2 }, { k: "peak", fc: 500, g: -1.5, w: 1.8 },
    { k: "peak", fc: 2500, g: 2.0, w: 1.5 }, { k: "high", fc: 9000, g: 1.5, w: 1.4 }]],
  ["dance", [
    { k: "low", fc: 80, g: 5.0, w: 1.2 }, { k: "peak", fc: 400, g: -2.5, w: 1.6 },
    { k: "high", fc: 10000, g: 3.5, w: 1.2 }]],
  ["electronic", [
    { k: "low", fc: 80, g: 5.0, w: 1.2 }, { k: "peak", fc: 400, g: -2.5, w: 1.6 },
    { k: "high", fc: 10000, g: 3.5, w: 1.2 }]],
  ["house", [
    { k: "low", fc: 80, g: 5.0, w: 1.2 }, { k: "peak", fc: 400, g: -2.0, w: 1.6 },
    { k: "high", fc: 10000, g: 3.5, w: 1.2 }]],
  ["techno", [
    { k: "low", fc: 80, g: 5.0, w: 1.2 }, { k: "peak", fc: 400, g: -2.0, w: 1.6 },
    { k: "high", fc: 10000, g: 3.5, w: 1.2 }]],
  ["metal", [
    { k: "peak", fc: 100, g: 3.0, w: 1.2 }, { k: "peak", fc: 400, g: -3.0, w: 1.4 },
    { k: "peak", fc: 4000, g: 3.0, w: 1.4 }, { k: "high", fc: 9000, g: 1.5, w: 1.4 }]],
  ["rock", [
    { k: "peak", fc: 100, g: 2.5, w: 1.2 }, { k: "peak", fc: 350, g: -2.5, w: 1.4 },
    { k: "peak", fc: 3500, g: 3.0, w: 1.4 }, { k: "high", fc: 8000, g: 2.0, w: 1.5 }]],
  ["alternative", [
    { k: "peak", fc: 100, g: 2.5, w: 1.2 }, { k: "peak", fc: 350, g: -2.0, w: 1.4 },
    { k: "peak", fc: 3500, g: 2.5, w: 1.4 }, { k: "high", fc: 9000, g: 1.5, w: 1.4 }]],
  ["punk", [
    { k: "peak", fc: 100, g: 2.5, w: 1.2 }, { k: "peak", fc: 400, g: -2.0, w: 1.4 },
    { k: "peak", fc: 3500, g: 3.0, w: 1.4 }]],
  ["indie", [
    { k: "peak", fc: 150, g: 2.0, w: 1.2 }, { k: "peak", fc: 400, g: -1.5, w: 1.6 },
    { k: "peak", fc: 3500, g: 2.5, w: 1.4 }, { k: "high", fc: 9000, g: 1.5, w: 1.4 }]],
  // 클래식/오페라는 원본 밸런스 존중 — 과한 스마일 금지.
  ["classical", [
    { k: "low", fc: 60, g: 1.0, w: 1.3 }, { k: "peak", fc: 250, g: -1.5, w: 1.8 },
    { k: "high", fc: 12000, g: 2.0, w: 1.5 }]],
  ["opera", [
    { k: "low", fc: 60, g: 1.0, w: 1.3 }, { k: "peak", fc: 250, g: -1.5, w: 1.8 },
    { k: "high", fc: 12000, g: 2.0, w: 1.5 }]],
  ["jazz", [
    { k: "low", fc: 120, g: 2.0, w: 1.3 }, { k: "peak", fc: 300, g: -2.0, w: 1.6 },
    { k: "peak", fc: 5000, g: 2.5, w: 1.4 }, { k: "high", fc: 11000, g: 1.5, w: 1.4 }]],
  ["blues", [
    { k: "low", fc: 120, g: 2.0, w: 1.3 }, { k: "peak", fc: 350, g: -1.5, w: 1.6 },
    { k: "peak", fc: 4000, g: 2.0, w: 1.4 }]],
  ["country", [
    { k: "low", fc: 150, g: 1.5, w: 1.3 }, { k: "peak", fc: 400, g: -1.5, w: 1.6 },
    { k: "peak", fc: 4000, g: 2.5, w: 1.4 }, { k: "high", fc: 10000, g: 1.5, w: 1.4 }]],
  ["folk", [
    { k: "low", fc: 150, g: 1.5, w: 1.3 }, { k: "peak", fc: 400, g: -1.5, w: 1.6 },
    { k: "peak", fc: 4000, g: 2.5, w: 1.4 }, { k: "high", fc: 10000, g: 1.5, w: 1.4 }]],
  ["acoustic", [
    { k: "low", fc: 150, g: 1.5, w: 1.3 }, { k: "peak", fc: 400, g: -1.5, w: 1.6 },
    { k: "peak", fc: 4000, g: 2.5, w: 1.4 }, { k: "high", fc: 10000, g: 1.5, w: 1.4 }]],
  ["soundtrack", [
    { k: "low", fc: 70, g: 4.0, w: 1.2 }, { k: "peak", fc: 500, g: -2.0, w: 1.8 },
    { k: "peak", fc: 1500, g: 1.5, w: 1.5 }, { k: "high", fc: 10000, g: 2.0, w: 1.4 }]],
  ["anime", [
    { k: "low", fc: 90, g: 3.5, w: 1.2 }, { k: "peak", fc: 400, g: -1.5, w: 1.8 },
    { k: "peak", fc: 3000, g: 2.5, w: 1.4 }, { k: "high", fc: 10000, g: 2.5, w: 1.3 }]],
  ["pop", [
    { k: "low", fc: 90, g: 3.5, w: 1.2 }, { k: "peak", fc: 400, g: -1.5, w: 1.8 },
    { k: "peak", fc: 3000, g: 2.5, w: 1.4 }, { k: "high", fc: 10000, g: 2.5, w: 1.3 }]],
];

// ── 설문 성향 ────────────────────────────────────────────────────────────────
// [정직한 한계] soundstage(공간감)는 본래 리버브/크로스피드/스테레오 폭의
// 영역이라 31밴드 게인만으로는 재현할 수 없다. "거리감"에 기여하는 중고역
// 프레즌스(2~4kHz)와 에어(8kHz+) 밸런스만 약하게 근사한다.
const BASS: Shape[][] = [
  [{ k: "low", fc: 100, g: 4.0, w: 1.2 }],                                    // bass_heavy
  [{ k: "low", fc: 100, g: 1.0, w: 1.2 }],                                    // bass_balanced
  [{ k: "low", fc: 80, g: -1.5, w: 1.2 }, { k: "peak", fc: 2000, g: 1.5, w: 1.5 }], // bass_vocal_focused
  [],                                                                          // bass_flat
];
const VOCAL: Shape[][] = [
  [{ k: "peak", fc: 2500, g: 3.0, w: 1.2 }],
  [{ k: "peak", fc: 2500, g: 0.5, w: 1.2 }],
  [{ k: "peak", fc: 2500, g: -1.0, w: 1.2 }, { k: "high", fc: 8000, g: 1.0, w: 1.4 }],
  [{ k: "high", fc: 10000, g: 2.0, w: 1.3 }, { k: "peak", fc: 5000, g: 0.5, w: 1.4 }],
];
const SOUNDSTAGE: Shape[][] = [
  [{ k: "peak", fc: 3000, g: -1.0, w: 1.4 }, { k: "high", fc: 9000, g: 1.5, w: 1.4 }],
  [{ k: "peak", fc: 2000, g: 1.5, w: 1.4 }, { k: "high", fc: 9000, g: -0.5, w: 1.4 }],
  [],
  [{ k: "peak", fc: 3000, g: -0.5, w: 1.4 }, { k: "high", fc: 8000, g: 1.0, w: 1.4 }],
];
const TREBLE: Shape[][] = [
  [{ k: "high", fc: 7000, g: 3.0, w: 1.3 }],
  [{ k: "high", fc: 8000, g: -1.5, w: 1.3 }],
  [{ k: "high", fc: 6000, g: -2.5, w: 1.3 }, { k: "peak", fc: 200, g: 1.0, w: 1.3 }],
  [],
];
const VOLUME: Shape[][] = [
  [{ k: "low", fc: 80, g: 2.0, w: 1.2 }, { k: "peak", fc: 4000, g: 1.5, w: 1.4 }],
  [{ k: "peak", fc: 150, g: 1.0, w: 1.2 }, { k: "peak", fc: 3500, g: -1.0, w: 1.4 },
   { k: "high", fc: 9000, g: -1.0, w: 1.4 }],
  [{ k: "low", fc: 60, g: 2.5, w: 1.2 }, { k: "peak", fc: 1500, g: 1.0, w: 1.5 },
   { k: "high", fc: 10000, g: 1.0, w: 1.4 }],
  [],
];

// SurveyMapping 의 ID/라벨 양쪽을 받는다. 클라이언트가 어느 형태로 저장했는지에
// 따라 다르게 오기 때문이다 (UI 는 라벨, DB 동기화 경로는 ID).
const DIMS: Array<[string[], string[], Shape[][]]> = [
  [["bass_heavy","bass_balanced","bass_vocal_focused","bass_flat"],
   ["Bass Heavy","Balanced Bass","Vocal Focused","Flat Bass"], BASS],
  [["vocal_forward","vocal_blended","vocal_spacious","vocal_airy"],
   ["Forward Vocal","Blended Vocal","Spacious Vocal","Airy Vocal"], VOCAL],
  [["soundstage_huge","soundstage_intimate","soundstage_dry","soundstage_virtual"],
   ["Huge Soundstage","Intimate Room","Dry Studio","Virtual Surround"], SOUNDSTAGE],
  [["treble_high_resolution","treble_smooth","treble_warm","treble_reference"],
   ["High Resolution","Smooth Treble","Warm Treble","Reference Treble"], TREBLE],
  [["volume_energetic","volume_relaxing","volume_cinematic","volume_versatile"],
   ["Energetic","Relaxing","Cinematic","Versatile"], VOLUME],
];

function pickGenre(genre: string): Shape[] {
  const g = (genre || "").toLowerCase();
  if (!g) return DEFAULT_GENRE;
  for (const [kw, shapes] of GENRE_TABLE) {
    if (g.includes(kw)) return shapes;
  }
  return DEFAULT_GENRE;
}

// tendency 는 ", " 로 연결된 5개 항목. 조각 수가 5가 아니면(설문 미완료,
// 기본값 "Balanced and clear sound") 성향 보정 없이 장르 커브만 적용한다.
function surveyShapes(tendency: string): Shape[] {
  const parts = (tendency || "").split(", ");
  if (parts.length !== 5) return [];
  const out: Shape[] = [];
  for (let d = 0; d < 5; ++d) {
    const [ids, labels, table] = DIMS[d];
    const v = parts[d].trim();
    let idx = ids.indexOf(v);
    if (idx < 0) idx = labels.indexOf(v);
    if (idx >= 0 && idx < table.length) out.push(...table[idx]);
  }
  return out;
}

export function generateCurve(genre: string, tendency: string): number[] {
  const shapes = [...pickGenre(genre), ...surveyShapes(tendency)];

  const gains = F31.map((f) => {
    let sum = 0;
    for (const s of shapes) sum += evalShape(f, s);
    return Math.max(-CLAMP_DB, Math.min(CLAMP_DB, sum));
  });

  // 과도 부스트 캡 — 세 축(설문 저역 + 볼륨 성향 + 장르)이 선형 합산되어
  // 20Hz 에서 +8.6dB 까지 치솟는 것을 막는다.
  for (let i = 0; i < gains.length; ++i) gains[i] = softKnee(gains[i]);

  // 에너지 보존 중립화 — 기준 스펙트럼에 걸었을 때 총에너지가 변하지 않도록
  // 전역 오프셋을 뺀다. 전역 오프셋이라 커브 **모양은 바뀌지 않는다**.
  // 저역과 중역의 상대 관계는 그대로고 전체가 내려갈 뿐이다. "저음을 올리면
  // 그만큼 나머지가 내려간다"는 것은 0dBFS 천장 아래에서 저음을 얻는 대가다.
  // 곡마다 스펙트럼이 다른 데서 오는 잔차(실측 +-4dB)는 클라이언트의 헤드룸
  // 서보가 메운다. 여기에 적응형 중립화를 또 붙이면 제어 루프가 둘이 된다.
  const d = energyChangeDb(gains);
  for (let i = 0; i < gains.length; ++i) {
    gains[i] = Math.max(-CLAMP_DB, Math.min(CLAMP_DB, gains[i] - d));
  }

  return gains.map((v) => Math.round(v * 100) / 100);
}
