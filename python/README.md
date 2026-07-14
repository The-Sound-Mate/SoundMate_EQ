# SoundMate EQ

오디오 파일(MP3/WAV)을 분석해 태그를 추론하고, 태그별 EQ 프리셋을 가중 합산해 **10-band 최종 EQ 곡선**을 만드는 시스템.

```
python main.py music.mp3
```
→ 콘솔에 features / tags / 10-band EQ를 시각화하고, `output/`에 `feature.json`, `tag.json`, `eq.json`을 저장합니다.

---

## 실행 흐름

```
 1. MP3 / WAV 입력
        │
 2.  Audio Loader           (audio/loader.py)
        │           librosa 기반 디코딩 → AudioData(samples, sr, channels)
        ▼
 3.  Feature Extractor      (features/extractor.py)
        │           Tempo · Pitch · Spectrum · Loudness · Harmony ·
        │           Timbre · Stereo · Rhythm · HPSS → FeatureVector
        ▼
 4.  Tag Generator          (tagging/generator.py)
        │           66개 rule × config/tags.yaml → TagSet
        │           {"bass_heavy": 0.82, "warm": 0.64, ...}
        ▼
 5.  EQ Preset Loader       (eq/presets.py)
        │           config/eq_presets.yaml → PresetStore
        │           (auto-reload 지원)
        ▼
 6.  EQ Engine (Mixer)      (eq/mixer.py)
        │           Final EQ = Σ (tag.intensity × preset.gain)
        │           per-band 합산 → ±12dB 클립 → preamp 자동 계산
        ▼
 7.  Final EQ 생성           FinalEQ (hz_31, hz_62, ..., hz_16000, preamp_db)
        ▼
 8.  JSON 저장               output/{feature,tag,eq}.json
```

각 단계는 독립적으로 테스트 가능하고, 다른 구현으로 교체 가능합니다.

## 설치

Python 3.10 이상 필요.

```powershell
# uv (권장)
uv sync                                # 기본 의존성
uv sync --extra essentia               # Linux/macOS/WSL에서 essentia 추가 (선택)

# 또는 pip
pip install -r requirements.txt        # 실행용
pip install -r requirements-dev.txt    # 개발/테스트용
```

`essentia`는 Linux/macOS만 공식 wheel이 있어서 Windows에서는 자동으로 librosa로 폴백됩니다.

## 사용법

```powershell
# 기본 실행
python main.py music.mp3

# 옵션들
python main.py music.mp3 -v                    # INFO 로그
python main.py music.mp3 -vv                   # DEBUG 로그
python main.py music.mp3 -q                    # 에러만 표시
python main.py music.mp3 --sr 44100            # 분석 sample rate
python main.py music.mp3 --max-gain 6          # ±6dB로 클립 한도 변경
python main.py music.mp3 --min-gain -9         # 음의 클립 한도만 변경
python main.py music.mp3 --min-intensity 0.1   # 낮은 강도 태그 제외
python main.py music.mp3 --no-arrays           # feature.json에 배열 제외 (스칼라만)
python main.py music.mp3 --auto-reload         # YAML 편집 자동 반영
python main.py music.mp3 -o my_output/         # 출력 디렉터리 지정
```

Exit code:
- `0` — 성공
- `1` — 파일 없음
- `2` — 오디오 디코드 실패
- `3` — 파이프라인 내부 오류
- `4` — 잘못된 인자

### 콘솔 출력 예시

```
─── SoundMate EQ — music.mp3 ────────────────────────────────
                    Features
  samplerate      44100 Hz
  duration        213.47 s
  channels        2
  tempo           128.00 BPM  (essentia)
  centroid        2843 Hz
  ...

                Tags (top 10)
  tag              category   intensity
 ──────────────────────────────────────────────────
  wide             space         0.91   ██████████████████░░
  bass_heavy       bass          0.82   ████████████████░░░░
  warm             tone          0.64   ████████████░░░░░░░░
  ...

           Final EQ  (preamp: -0.50 dB)
      freq        gain
 ─────────────────────────────────────
     31 Hz    -2.46 dB   ████│
     62 Hz    -1.64 dB   ███ │
    125 Hz    -0.82 dB   ██  │
    250 Hz    +0.00 dB       │
     ...

                    Saved
  feature → output/feature.json
  tag     → output/tag.json
  eq      → output/eq.json
```

### Python API 사용

```python
from audio import AudioLoader
from features.extractor import FeatureExtractor
from tagging import TagGenerator
from eq import PresetStore, TagEQMixer, MixerConfig

# 각 단계를 명시적으로 조합
audio    = AudioLoader(target_sr=22050).load("music.mp3")
features = FeatureExtractor().extract(audio)
tags     = TagGenerator().generate(features)
presets  = PresetStore.load_default(auto_reload=True)
final_eq = TagEQMixer(presets, MixerConfig(max_gain_db=12, min_gain_db=-12)).mix(tags)

# 결과 조회
final_eq.hz_62               # 62Hz gain (float)
final_eq.preamp_db           # 자동 계산된 preamp
final_eq.as_dict()           # {31: ..., 62: ..., ..., 16000: ...}
final_eq.to_json("out.json")

# 또는 원샷
from eq import EQEngine
result = EQEngine().compute(features)
result.tags.as_dict()
result.final_eq.as_dict()
```

## 프로젝트 구조

```
python/
├── main.py                      # CLI 엔트리 (8단계 파이프라인)
├── pyproject.toml               # uv용
├── requirements.txt             # pip용 (실행)
├── requirements-dev.txt         # pip용 (개발)
│
├── audio/
│   └── loader.py                # AudioLoader, AudioData, AudioLoadError
│
├── features/
│   ├── extractor.py             # FeatureExtractor, FeatureVector
│   ├── tempo.py                 # BPM, beat positions
│   ├── pitch.py                 # f0, pitch histogram
│   ├── spectral.py              # FFT, STFT, centroid/bandwidth/…
│   ├── loudness.py              # RMS, Peak, LUFS, DR
│   ├── harmony.py               # Chroma, Tonnetz
│   ├── timbre.py                # MFCC + delta
│   ├── stereo.py                # width, L/R balance, correlation
│   ├── rhythm.py                # onset strength envelope
│   └── hpss.py                  # Harmonic/Percussive split
│
├── tagging/
│   ├── tags.py                  # Tag, TagCategory, TagSet
│   ├── config.py                # YAML → TagRuleConfig
│   ├── helpers.py               # ramp_up / bell / sigmoid / combine_*
│   ├── derived.py               # DerivedFeatures (밴드 에너지 등)
│   ├── generator.py             # TagGenerator, register_tag registry
│   └── rules_{bass,mid,treble,vocal,drum,space,dynamics,tone,mix,instrument}.py
│                                # 66개 태그 규칙
│
├── eq/
│   ├── presets.py               # EQBand, EQPreset, PresetStore + YAML + auto-reload
│   ├── mixer.py                 # TagEQMixer, FinalEQ, EQContribution
│   └── engine.py                # EQEngine, EQResult (편의 orchestrator)
│
├── config/
│   ├── tags.yaml                # 66개 태그 threshold
│   └── eq_presets.yaml          # 태그별 EQ preset
│
├── output/                      # feature.json, tag.json, eq.json (gitignored)
│
└── tests/                       # 107개 unit test
    ├── conftest.py              # requires_audio_libs marker
    ├── test_helpers.py
    ├── test_tags.py
    ├── test_presets.py
    ├── test_mixer.py
    ├── test_final_eq.py
    └── test_audio_loader.py
```

## 설계 원칙

- **타입 힌트** — 모든 public API에 명시적 type annotation. `mypy --strict` 준수 목표.
- **dataclass** — `AudioData`, `FeatureVector`, `TagSet`, `EQPreset`, `FinalEQ` 등 모든 값 타입은 `@dataclass(frozen=True, slots=True)`. 불변성 + 메모리 효율.
- **예외 처리** — 커스텀 예외 (`AudioLoadError`)로 실패 유형 구분. `main.py`에서 종류별로 exit code 매핑.
- **로깅** — `logging` 모듈 사용. 각 파일에 `logger = logging.getLogger(__name__)`. `main.py`가 verbosity 제어.
- **테스트 가능성** — 각 단계가 순수 함수 또는 상태 없는 클래스. AudioData/FeatureVector 같은 값 타입을 인자로 받아 다음 값 타입 반환. Mock 없이 dataclass만 있으면 테스트 가능.

## 설정 파일

### `config/tags.yaml`

66개 태그 각각의 threshold를 조정하는 rule engine 설정:

```yaml
bass_heavy:
    thresholds: { ratio_min: 0.25, ratio_max: 0.40 }
    # optional: enabled: true, weight: 1.0
```

카테고리: `bass`, `mid`, `treble`, `vocal`, `drum`, `space`, `dynamics`, `tone`, `mix`, `instrument_focus`.

### `config/eq_presets.yaml`

태그별 10-band EQ preset. **사용자 예시 형식 그대로**:

```yaml
bass_heavy:
    31: -3
    62: -2
    125: -1

bright:
    4000: -2
    8000: -2

vocal_forward:
    1000: -1
    2000: -2
    4000: -1
```

지정하지 않은 밴드는 자동으로 0dB. 10-band 외 주파수(예: `60`) 사용 시 로드 시점에 즉시 `ValueError`.

**Auto-reload**: `--auto-reload` 플래그로 실행하면 파일 mtime 감지 시 다음 요청부터 자동 반영됩니다.

## Final EQ 공식

```
Final EQ[band]  =  Σ over tags  ( tag.intensity  ×  preset[tag].gain[band] )
                     ↓
                per-band clip to [min_gain_db, max_gain_db]  (default ±12dB)
                     ↓
                preamp_db = -(max_boost + headroom) if any band > 0 else 0
```

FinalEQ dataclass:

```python
@dataclass(frozen=True, slots=True)
class FinalEQ:
    hz_31: float = 0.0
    hz_62: float = 0.0
    hz_125: float = 0.0
    hz_250: float = 0.0
    hz_500: float = 0.0
    hz_1000: float = 0.0
    hz_2000: float = 0.0
    hz_4000: float = 0.0
    hz_8000: float = 0.0
    hz_16000: float = 0.0
    preamp_db: float = 0.0
```

## 테스트

```powershell
uv run pytest                    # 107 tests
uv run pytest -v                 # verbose
uv run pytest -k mixer           # 특정 파일만
uv run pytest --cov              # coverage (pytest-cov 필요)
```

| 파일 | 커버 내용 |
| --- | --- |
| `test_helpers.py` | ramp/bell/sigmoid/combine_* 정확성 (26 tests) |
| `test_tags.py` | Tag/TagSet 조작 및 JSON (11 tests) |
| `test_presets.py` | YAML 로딩, from_gains, auto-reload mtime, invalid freq 감지 (21 tests) |
| `test_mixer.py` | Formula 검증, band 합산, ±12dB clip, preamp 자동 계산, contribution trace (20 tests) |
| `test_final_eq.py` | 필드 스키마, JSON 라운드트립, immutability (15 tests) |
| `test_audio_loader.py` | AudioData 프로퍼티, 파일 없음/손상 처리, WAV 라운드트립, 리샘플, stereo/mono (14 tests) |

`test_audio_loader.py`의 실제 WAV 테스트는 `librosa`/`soundfile` 이 필요합니다. 순수 로직 테스트는 이들 없이도 실행됩니다 (`requires_audio_libs` 마커로 자동 스킵).

## 확장 포인트

- **새 오디오 포맷**: `AudioLoader`가 이미 librosa/soundfile/audioread가 지원하는 모든 포맷을 처리합니다. 커스텀이 필요하면 `AudioLoaderProtocol`을 구현.
- **새 피처**: `features/`에 새 파일 추가, `FeatureExtractor.__init__`에 sub-extractor 추가, `FeatureVector`에 필드 추가.
- **새 태그**: `tagging/rules_<category>.py`에 `@register_tag` 함수 추가 + `config/tags.yaml`에 threshold 추가.
- **새 EQ 프리셋**: `config/eq_presets.yaml`에 태그명 아래 밴드 값 추가 — 코드 수정 불필요.
- **다른 mixer 전략**: `TagEQMixer`를 상속하거나 직접 `FinalEQ`를 반환하는 클래스 작성.

## 다음 단계 (제안)

- `main.py`의 파이프라인 단계별 unit test (mock AudioData 사용)
- FeatureExtractor / TagGenerator 통합 테스트 (합성 오디오 사용)
- 배치 모드 (`python main.py *.mp3` — 여러 파일을 output/<stem>/에 나눠 저장)
- HTTP API 래퍼 (FastAPI 등)
- 웹 UI (feature/tag/eq JSON을 시각화)

## License

Proprietary. See `pyproject.toml`.
