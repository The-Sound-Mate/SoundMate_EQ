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

export const CURVE_VERSION = 1;

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

// 라우드니스 가중치. 단순 산술평균으로 중립화하면 로그 간격 31밴드에서 저역
// 부스트가 과도하게 상쇄된다 — 20~60Hz 는 실제 프로그램 에너지가 적은데도
// 밴드 개수는 중역과 같아 평균을 크게 끌어올리기 때문이다. 그 결과 "저음 강조"
// 선택이 중역을 몇 dB 씩 깎아내는 부작용이 생긴다.
function loudnessWeight(f: number): number {
  const lo = f / 200.0;
  const hi = f / 8000.0;
  return (lo * lo / (1.0 + lo * lo)) * (1.0 / (1.0 + hi * hi));
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

  // 라우드니스 중립화 — 가중평균을 0dB 로. 부스트 총량이 곧 음량 증가로
  // 이어지는 것을 막아 헤드룸과 리미터 여유를 보존한다.
  let acc = 0, wsum = 0;
  for (let i = 0; i < F31.length; ++i) {
    const w = loudnessWeight(F31[i]);
    acc += gains[i] * w;
    wsum += w;
  }
  if (wsum > 1e-6) {
    const mean = acc / wsum;
    for (let i = 0; i < gains.length; ++i) {
      gains[i] = Math.max(-CLAMP_DB, Math.min(CLAMP_DB, gains[i] - mean));
    }
  }

  return gains.map((v) => Math.round(v * 100) / 100);
}
