# Identity v2 — 곡 식별 체계 재설계

대상 버전: **v0.1.1** (본 계획은 0.1.1 에 포함)
작성: 2026-09-21
상태: 코드 일부 작성 완료(미컴파일) / DB 작업 대기

---

## 0. 한 줄 요약

iTunes 검색 결과를 곡 정체성(EQ 캐시 키)에서 완전히 제거하고,
Windows SMTC 메타데이터로 **2단 키**를 만든다.
같은 곡임이 **확정**되면 두 키를 **연결(link)** 한다. 물리 병합은 하지 않는다.

---

## 1. 왜 바꾸는가 — 실제 버그

### 1-1. iTunes 가 틀린 곡의 EQ 를 걸고 있었다

`GenreManager.cpp:192-198`

```cpp
if (src == "itunes") {
  out.genre  = j.value("genre", "");
  out.title  = j.value("canonicalTitle", "");
  out.artist = j.value("canonicalArtist", "");
  out.valid  = !out.title.empty();   // ← 이게 전부다
}
```

`valid` 의 의미가 사실상 **"응답이 왔다"** 이다. 유사도 검증이 없다.
iTunes Search API 는 비어있지 않은 질의면 거의 항상 뭔가를 돌려준다.

그 값이 `MainWindow.cpp:1558-1559` 에서 **캐시 키**가 되면서,
그럴듯하지만 틀린 매치가 **다른 곡의 저장된 EQ 를 자동 적용**했다.

관측된 오식별 예: `Airplane → Widespread Panic`, `unknown / lifehouse`.

### 1-2. 실패 방식의 비대칭 — 이 설계의 핵심 근거

| 경로 | 실패했을 때 |
|---|---|
| iTunes 키 | **틀린 EQ 를 건다** (조용히, 사용자는 모름) |
| 캐시 미스 | 설문 커브 + 적응 (안전) |
| 지문 불일치 | 설문 커브 + 적응 (안전) |

캐시 **미스는 안전하다** (`TriggerAIGeneration` → `LocalCurve::Generate("", userPref)`).
따라서 유일한 위험 경로는 **틀린 키로 캐시 히트**하는 것이다.
"덜 맞히더라도 틀리게 맞히지 않는다" 가 원칙.

### 1-3. 장르는 이미 표시 전용이었다

- `m_currentGenre` 사용처: `MainWindow.cpp:2401-2402` `DrawMetaRow("Genre", ...)` **한 곳**
- `MainWindow.cpp:787` 은 `LocalCurve::Generate("", userPref)` — 장르 인자를 이미 무시

→ iTunes 를 커브 경로에서 빼도 **기능 손실 0**. 장르 표시용으로만 남긴다.

---

## 2. 플랫폼 판별 — 무엇이 가능하고 무엇이 불가능한가

### 2-1. 유일한 신호

`MediaMonitor.cpp:70` — `session.SourceAppUserModelId()`

### 2-2. AUMID 는 *앱*을 가리키지 *사이트*를 가리키지 않는다

| 실제 재생 | AUMID | 현재 `info.source` |
|---|---|---|
| Spotify 데스크톱 | `SpotifyAB.SpotifyMusic_...!Spotify` | `Spotify` ✅ |
| YouTube Music 앱 | `...youtubemusic...` | `YouTube` ✅ |
| **크롬 탭 youtube.com** | `Chrome` | **`Chrome`** ❌ |
| 크롬 탭 사운드클라우드 | `Chrome` | `Chrome` (구분 불가) |

**브라우저로 듣는 유튜브는 "YouTube" 로 잡히지 않는다.**
`MediaMonitor.cpp:78` 의 `youtube` 분기는 전용 앱일 때만 걸린다.
SMTC 는 URL 을 주지 않으며, 줄 방법이 없다.

### 2-3. 그래서 "유튜브냐"는 물어볼 필요 없는 질문이다

필요한 건 사이트 이름이 아니라 **메타데이터를 신뢰할 수 있는가**다.

- **전용 음악 앱** → title/artist 가 태그에서 제대로 분리 → **신뢰(trusted)**
- **브라우저** → 페이지 Media Session 값. title = `곡명 (Official MV) [4K]`, artist = 채널명 → **비신뢰(untrusted)**

브라우저는 유튜브든 사운드클라우드든 똑같이 비신뢰다. 사이트 구분이 불필요해진다.

### 2-4. 현재 코드의 약점 (수정 대상)

1. **raw AUMID 를 버린다.** `SongInfo` 에 friendly name 만 남아 유일한 안정 식별자가 소실.
   → `SongInfo.sourceKey` (raw AUMID) 추가 필요.
2. `MediaMonitor.cpp:88` 의 `find("edge")` — 이름에 `edge` 가 들어간 아무 앱이나 "Edge".
   표시용으론 허용, **키 재료로는 불가**.

---

## 3. 2단 키 규격 (확정)

### 3-1. 정규화 `norm()`

`RecordManager.cpp` 익명 네임스페이스의 `IdentityPart` 과 동일:

- 앞뒤 공백 제거
- **ASCII 만** 소문자화
- 연속 공백 1칸으로 축약

**공격적 정제 금지.** 괄호/특수문자/한글 제거는 하지 않는다 — 과거 오식별의 원인.
비ASCII(한글)는 그대로 통과.

### 3-2. 앱 키 `appKey()`

raw AUMID 에서:
1. 소문자화
2. 마지막 `\` `/` `!` 뒤 부분만
3. 끝의 `.exe` 제거

예) `SpotifyAB.SpotifyMusic_zpd...!Spotify` → `spotify`, `chrome.exe` → `chrome`

### 3-3. 신뢰 판정 (raw AUMID 소문자 부분문자열)

```
TRUSTED :  spotify | applemusic | itunes | youtubemusic | tidal | deezer | amazonmusic
UNTRUSTED: 그 외 전부 (브라우저, 로컬 플레이어, 불명)
```

> `youtube` **단독은 신뢰 아님** — 브라우저일 수 있다. `youtubemusic` 만 신뢰.
> 로컬 플레이어(foobar/VLC)는 태그가 좋을 수 있으나 알 수 없으므로 비신뢰가 기본.
> 비신뢰는 "저장하지 않음"이 아니라 "네임스페이스를 분리함"이다.

### 3-4. 키 생성

```
trusted   : "v2t:" + sha256hex( "v2t" \x1f norm(artist) \x1f norm(title) )
untrusted : "v2u:" + sha256hex( "v2u" \x1f appKey \x1f norm(artist) \x1f norm(title) )
```

길이 = 4 + 64 = **68자**. `sync_track_history` 의 `track_hash ≤ 128` 제한 통과.

`norm(title)` 이 비면 키를 만들지 않는다(`"unknown"` 반환 → 저장 생략).
`artist` 는 비어도 된다 — 유튜브/브라우저 메타데이터에서 흔하다.

### 3-5. 이 설계가 주는 것

| | |
|---|---|
| Spotify 와 Apple Music | **같은 키** (앱 이름이 키에 없음 → 자연히 일치) |
| 유튜브 | `v2u:` 로 격리 → 지저분한 제목이 깨끗한 쪽을 오염 못 시킴 |
| Spotify 가 AUMID 변경 | `v2t:` 키는 안 깨짐 (앱 이름이 키에 없음) |
| 유튜브 ↔ 스포티파이 통합 | **소스 정보로는 불가능.** 지문(§5)만이 유일한 수단 |

---

## 4. 연결(link) 모델 — "같은 곡 확정 시"

### 4-0. 전제: `track` 은 전역 카탈로그다 (2026-09-22 실측)

```
public.track
  track_hash   UNIQUE      ← 전역 유일
  canonical_id uuid        ← self-FK
  (user_id 컬럼이 없다)
```

곡 1개 = 행 1개이고, 유저별 데이터는 `user_track_history` 가 `track_id` 로 참조한다.
따라서 **`track.canonical_id` 에 쓰는 순간 그 결정은 전 사용자에게 적용된다.**

이 사실이 설계를 가른다. 초기안은 "앱에서 사용자에게 물어보고 그 답을
`canonical_id` 에 쓴다" 였는데, 그러면:

- 누군가 음악 듣다가 귀찮아서 아무거나 누른 게 **전 사용자의 EQ 를 틀리게 만든다.**
  E10 보다 나쁘다 — E10 은 조회 한 건이 틀린 거라 그 행만 고치면 됐다.
- 같은 질문을 N 명에게 N 번 물어보는 중복이다.
- 음악 재생 중에 모달을 띄우는 최악의 UX 다.

**결정 (2026-09-22): 앱은 사용자에게 묻지 않는다.**
매핑은 자동으로 하고, 자동으로 못 가리는 것만 **웹사이트 관리 화면**으로 보낸다.

### 4-1. 원칙: 연결하되 병합하지 않는다

`track.canonical_id` (self-FK, nullable) 를 둔다.

- `NULL` = 이 행이 스스로 canonical
- 확정 시 **비신뢰 행**의 `canonical_id` 를 **신뢰 행**의 `id` 로 설정
- 조회: `canonical_id` 를 먼저 해석하고, 없으면 자기 자신

**물리 병합(행 삭제/통합)은 하지 않는다.** 이유:
틀린 연결이야말로 지금 고치고 있는 버그다. 되돌릴 수 있어야 한다.
`canonical_id = NULL` 한 줄로 연결 해제가 끝나야 한다.

### 4-2. 저장 계층 2개 — 매핑은 전체곡 테이블에만 쓴다

> **2026-09-22 정정.** 초안은 유저별 링크 테이블(`user_track_link`) 을 둔
> 3계층이었다. 사용자 지시로 **매핑은 전체곡 테이블 한 곳**으로 좁혔고
> 해당 테이블은 삭제했다(0행, 데이터 손실 없음). 아래가 확정안이다.

| 계층 | 테이블 | 범위 | 누가 쓰는가 |
|---|---|---|---|
| ① 확정 링크 | `track.canonical_id` | **전역** | 승격 잡 / 웹 관리자 |
| ② 관측·검토 큐 | `track_link_candidate` | 전역 | 클라이언트가 제안(`propose_track_link`), 웹 관리 화면이 소비 |

유저별 링크 테이블을 없앤 이유:

- **유저별 데이터는 이미 `user_track_history` 가 담당한다.** 곡↔곡 매핑을
  유저별로 또 저장하면 같은 사실이 두 곳에 생기고 어느 쪽이 진짜인지 갈린다.
- **근거는 ②가 이미 남긴다.** `track_link_candidate.observers`(uuid 배열) 에
  누가 제안했는지가 쌓이므로, "유저별 집계" 요구는 이것으로 충족된다.
  결론(`canonical_id`)만 한 곳에 쓴다.

**대가(의도된 것):** 개인 예외 — "나는 유튜브 버전은 따로 듣겠다" — 를
DB 로 표현하는 경로가 사라졌다. 필요해지면 클라이언트 로컬 설정으로 푼다.
전역 카탈로그에 유저별 예외를 섞지 않는다.

### 4-3. 신뢰도 등급 — 무엇이 자동이고 무엇이 웹으로 가는가

| 등급 | 근거 | 동작 |
|---|---|---|
| **高** | 지문: 비중첩 윈도 **3개 이상**이 같은 오프셋 빈에 동의 + 표 집중도 임계 초과 | ② 에 `reason='strong_fingerprint'` 로 제안. 관측자 N명이면 자동 승격 |
| **中** | 윈도 2개 동의 / 지문 약함 + 텍스트 강일치 | ② 에 `reason='weak_fingerprint'`. 자동 승격 대상 |
| **低·애매** | 윈도 1개 / 표 분산 / 텍스트 충돌(아티스트가 다름) / 한쪽만 `v2t` | ② 에 `vote_spread` / `text_conflict` / `tier_mismatch`. **자동 승격 제외 → 웹 관리 화면** |

**클라이언트는 전역 링크를 직접 만들지 못한다.** 고신뢰라도 제안까지다.
"高면 즉시 전역 설정" 을 RPC 로 열면, 신뢰도를 클라이언트가 주장하게 되어
E11(로그인한 아무나 전역 링크를 덮어쓰던 구멍)이 RPC 로 되살아난다.
전역 링크는 **㉠ 독립 관측자 N명 또는 ㉡ 관리자 승인** 두 경로로만 생긴다.

"서로 다른 유저 N명" 이 핵심이다. 한 사람이 같은 곡을 30번 들어도 1표다
(`observers` 배열에 중복 추가하지 않는다). 독립 관측이 아니면 증거가 아니다.

### 4-4. 웹 관리 화면이 필요로 하는 것 (별도 저장소)

검토 큐 한 행당 화면에 있어야 하는 것:

- 쌍 양쪽의 `title` / `artist` / `source_key` / `key_tier`
- **왜 애매한지** — 동의 윈도 수, 표 집중도, 텍스트 유사도
- 몇 명에게서 관측됐는지 (`observed_users`)
- **승인** → `track.canonical_id` 설정 + `linked_by='admin'` + 큐 행 `resolved`
- **거부** → 큐 행 `rejected`. 같은 쌍을 다시 제안하지 않는다
- **보류** → 표본이 더 쌓일 때까지 대기

> `track.linked_by` CHECK 는 `'fingerprint'|'promoted'|'admin'|'user'` 로
> 확장 적용 완료 (§8-1).
> 등급별로 나눠 놓는 이유는 **나중에 한 등급만 골라 되돌리기 위해서**다.
> 자동 링크가 잘못 걸린 게 드러나면 `linked_by='fingerprint'` 만 한 번에 끌 수 있다.

### 4-5. 연결 ≠ EQ 자동 적용

| 축 | 하는 일 | 실패하면 | 복구 |
|---|---|---|---|
| ① 매핑 | "이 둘은 같은 곡" 판정 | 조용히 **다른 곡 EQ** | 어렵다 — 언제 틀렸는지 모른다 |
| ② 동기화 | 로컬 → DB 업로드 | 업로드 누락 | 쉽다 — 멱등 키로 재시도 |
| ③ 자동 적용 | 연결된 곡 EQ 를 가져다 씀 | 원치 않는 EQ | 쉽다 — 끄면 된다 |

**셋은 별개 축이다.** 묶으면 하나가 고장날 때 셋 다 고장난다.
특히 ①과 ③은 반드시 분리한다 — "같은 곡이다" 와 "그러니 EQ 를 가져와라" 는 다른 말이고,
유튜브 MV 로 들을 때는 일부러 다르게 듣고 싶을 수 있다.

**v0.1.2 범위 결정 (2026-09-22): ① + ② 까지. ③은 v0.1.3.**
연결이 실제로 맞는지 데이터가 쌓인 뒤에 켜는 순서다.

---

## 5. 지문(fingerprint) — v0.1.2

DB 없음. **전부 로컬.** 곡 전환 경로 DB 호출 **0회**.

### 5-1. 알고리즘: 랜드마크(peak-pair) 해시

```
모노 다운믹스 → 11~16 kHz 리샘플 → STFT → 국소 스펙트럼 피크
→ 앵커/타깃 쌍 → 해시 (f1_bin, f2_bin, Δt_bin)
로컬 인덱스: hash64 → postings(track_local_id, anchor_time_bin)
```

### 5-2. 길이·인트로가 달라도 되는 이유

"처음부터 비교"가 아니다.
질의 해시가 맞을 때마다 `(track_id, db_anchor_time − query_anchor_time)` 에 투표하고,
**하나의 오프셋 빈에 표가 몰리는가**를 본다.

→ 인트로가 잘리거나 광고가 붙으면 오프셋만 이동할 뿐 피크 군집은 그대로.
→ **길이는 절대 키가 아니다.** 신뢰도 가산점일 뿐
   (유튜브 MV 는 인트로/아웃트로/광고로 길이가 항상 다르다).

### 5-3. 파라미터

- 윈도 8~15초, 홉 4~8초
- **서로 겹치지 않는 윈도 2개 이상**이 같은 오프셋에 동의해야 인정
- 사전 게이트: RMS / 스펙트럼 피크 밀도 / 스펙트럼 평탄도 → 무음·대사 구간 차단

### 5-4. post-mix / post-EQ 강건성

우리 탭은 EQ 가 걸린 뒤의 신호다. 대응:

- 로그 크기 사용
- 대역별 화이트닝 / 국소 대비 정규화
- 절대 임계 대신 **국소 최대값**
- 주파수 거친 양자화
- 극저역/극고역 피크 수 제한
- 참조 지문도 **질의와 동일한 모노 다운믹스 규칙**으로 생성

### 5-5. 실패하는 경우 (정상 동작)

라이브 / 리믹스 / 커버 / 피치·템포 변조(nightcore, slowed+reverb) / 과도한 리마스터.
→ **실패가 옳은 결과다.** 다른 음원이므로 다른 EQ 가 맞다.

### 5-6. 저장량

곡당 3~8 KB. 2,000곡 ≈ 10~15 MB.

### 5-7. 라이선스 — 반드시 지킬 것

| 대상 | 판정 |
|---|---|
| **Chromaprint** | ❌ 코드는 MIT 계열이나 LICENSE.md 가 번들 FFmpeg 때문에 LGPL-2.1. 폐쇄소스 정적 링크 불가 |
| **FFTW** | ❌ GPL 또는 상용 |
| Dejavu / audfprint | 참고만 (MIT 이나 Python/Matlab) |
| **채택** | ✅ 클린룸 C++ 랜드마크 구현 + 퍼미시브 FFT, notices 파일 유지 |

- 제품/문서에 **"Shazam" 단어 사용 금지**. "랜드마크 해시" / "peak-pair fingerprint" 로 표기
- 핵심 특허(US6990453B2, US7627477B2, US8688248B2)는 Google Patents 상 만료로 표시되나
  계속출원·타 관할은 법률 검토 필요
- **실제 최대 리스크는 특허가 아니라 플랫폼 ToS**:
  스포티파이/유튜브/애플 콘텐츠나 API 데이터로 인식 DB 를 구축하지 말 것.
  raw 오디오 업로드 금지. 지문 해시는 **청취 이력 = PII** 로 취급.
  현 설계는 사용자 본인 이력 내부 매칭이므로 안전하다.

---

## 6. DB 접근 최소화

### 6-1. 곡 전환 경로 = 로컬 전용, DB 호출 0회

로컬 계층:
1. 인메모리 핫 캐시
2. SQLite 영속 캐시
3. 지문 역색인
4. pending outbox
5. 후보 테이블

DB 는 **배치 동기화/백업 저장소**일 뿐이다.

### 6-2. 동기화 정책

- 30~120초 디바운스
- idle / 종료 / 로그인 시 flush
- 배치당 50~200건, 중복 합침
- 지수 백오프, 멱등 키

### 6-3. 현존 버그

`MainWindow.cpp:1635-1636` 이 **곡이 바뀔 때마다** 새 스레드로 `ProcessBatchSync(false)` 를 띄운다.

```cpp
std::thread([]() { g_recordManager.ProcessBatchSync(false); }).detach();
```

→ v0.1.2 에서 디바운스 필요.

---

## 7. 이미 작성된 코드 (컴파일·게이트 3종 통과·커밋 완료 — `119ae5d`)

> 이 절은 작성 당시 "미컴파일" 상태를 적은 것이다.
> 2026-09-22 에 구현·빌드·게이트 통과·커밋이 끝났다.
> 상세는 `docs/ALGORITHM_CHANGES.md` 의 **A12** 항목.


| 파일 | 변경 |
|---|---|
| `RecordManager.h` | `EQEntry` 에 `mappingKey` / `identitySource` / `sourceApp` 추가, `GenerateTrackMappingKey` 선언 |
| `RecordManager.cpp` | `#include <cctype>`, `Sha256Hex` / `IdentityPart` 헬퍼, `GenerateTrackMappingKey`, `PendingFilenameForKey`, pending `schema_version: 2`, `GetCachedEQ` 복원 필드, `ClearSongEQCache` v1+v2 삭제, `SyncToDB` mapping_key 우선 |
| `MainWindow.cpp` | iTunes canonical → 캐시 키 사용 중단(장르만 유지), `aiSourceApp` 캡처, AI/prompt·manual 저장 경로에 `sourceApp`/`identitySource`, 프롬프트 게이트를 `trackId` → `!m_currentTitle.empty()` 로 변경 |

> 프롬프트 게이트를 안 고쳤으면 `m_canonicalTrackId = 0` 때문에
> **프롬프트 입력이 영구 비활성화**되는 회귀가 있었다.

### 7-1. 현재 코드의 미해결 결함

작성된 `GenerateTrackMappingKey` 는 `sourceApp` 을 **무조건** 키에 넣는다.

```
v2:smtc:sha256("v2" | sourceApp | artist | title)
```

→ 같은 곡을 스포티파이/유튜브에서 들으면 키가 영구히 2개로 쪼개진다.
→ **§3-4 의 2단 키로 교체해야 한다.**

---

## 8. 내일 할 일 (v0.1.1 잔여)

1. `MediaMonitor.h/.cpp` — `SongInfo.sourceKey` (raw AUMID) 추가, 기존 friendly name 은 표시용으로 유지
2. `MainWindow` — `sourceKey` 를 `EQEntry` 까지 전달
3. `RecordManager.cpp` — `GenerateTrackMappingKey` 를 §3-4 2단 키로 교체
   - `IsTrustedSource(rawAumid)` 추가
   - `AppKeyFrom(rawAumid)` 추가
4. 빌드 + 두 게이트 (`run_curve_equiv.bat`, `run_gain_probe.bat`)
5. `docs/ALGORITHM_CHANGES.md` 기록 — identity-v2 / iTunes 강등 / **iTunes 오식별이 잘못된 EQ 적용 경로였다는 사실**
6. 청취 확인

**reset.exe 동반 갱신 불필요** — 엔진 DSP/레지스트리/GUID 변경이 아니라 앱 측 식별 로직만 바뀐다.

---

## 9. 단계별 범위

| 버전 | 내용 |
|---|---|
| **v0.1.1** | iTunes 를 정체성에서 제거. 2단 SMTC 키. DB 백업 후 초기화 + 컬럼 추가 |
| **v0.1.2** | 로컬 랜드마크 지문. **후보 생성기로만.** 자동 적용 없음. `ProcessBatchSync` 디바운스 |
| **v0.1.3** | 사용자가 확정한 별칭만 자동 EQ. 불확실하면 적용하지 않음 |

---

## 10. 불변 원칙

1. **불확실하면 저장된 EQ 를 적용하지 않는다.** 설문 커브 + 적응으로 간다
2. **애매하면 사람이 본다.** 고신뢰만 자동 연결하고, 나머지는 웹 검토 큐로 보낸다.
   일반 사용자에게는 묻지 않는다 — 전역 결정이기 때문이다 (§4-0)
3. **길이는 키가 아니다.** 신뢰도 가산점일 뿐
4. **연결은 되돌릴 수 있어야 한다.** 물리 병합 금지
5. **곡 전환 경로에서 DB 를 부르지 않는다**
6. 프리앰프는 건드리지 않는다 (`EQController.cpp:132`, `const float preamp = 0.0f;`)

> 원칙 2 는 2026-09-22 에 바뀌었다. 이전 문구는 "첫 연결은 반드시 사람이 확인한다"
> 였는데, `track` 이 전역 카탈로그라 그 '사람'이 일반 사용자면 한 번의 오클릭이
> 전 사용자를 오염시킨다. 확인 주체를 관리자로 옮기고 고신뢰는 자동화했다.

---

## 8. DB 연결 계층 — 적용 완료 (2026-09-22)

Supabase 에 마이그레이션 4건 적용됨. 웹 구현자용 문서는
`docs/WEB_LINK_REVIEW_HANDOFF.md` (이 저장소 밖 사람이 읽는 전제로 따로 작성).

### 8-1. 적용된 것

| 대상 | 내용 |
|---|---|
| `track.linked_by` CHECK | `'user'\|'fingerprint'` → `'fingerprint'\|'promoted'\|'admin'\|'user'` |
| **컬럼 GRANT** | `authenticated` 에서 `canonical_id` / `linked_by` / `linked_at` **쓰기 제거**. `track_hash` 는 UPDATE 제거 (INSERT 만 허용) |
| ~~`user_track_link`~~ | 신설했다가 **같은 날 삭제**(0행). 매핑은 전체곡 테이블만 — §4-2 정정 |
| `track_link_candidate` | 신설. 검토 큐. RLS = **관리자만**. `observers uuid[]` 로 중복 관측 제거. `reason` 에 `strong_fingerprint` 추가 |
| `track_link_review` | 신설 뷰. `observers` 를 가린 관리 화면용. `security_invoker` |
| `propose_track_link()` | 클라이언트용. SECURITY DEFINER. 쌍 방향 정규화 + 관측자 누적. `authenticated` 만 |
| `resolve_track_link()` | 관리자용 승인/거부/보류. 체인 방지 검증 포함. `authenticated` + `is_admin` |
| `promote_track_links()` | 승격 잡. **service_role 전용**. 근거를 `track_link_candidate.observed_users` 로 교체(구 `user_track_link` 집계). 지문 근거 후보만 자동 승격, 1홉 3중 가드 |

### 8-2. 같이 고친 보안 구멍

| # | 문제 | 조치 |
|---|---|---|
| S1 | `track` 의 RLS 가 `UPDATE USING (true)` 라 **로그인한 아무나 아무 곡의 `canonical_id` 를 바꿀 수 있었다** | RLS 는 컬럼 단위가 안 되므로 테이블 GRANT 를 내리고 컬럼 화이트리스트로 재부여 |
| S2 | 신규 RPC 3개가 `anon` 에게도 EXECUTE 로 열림 (Supabase 기본 권한이 `revoke ... from public` 으로 안 지워짐) | `revoke execute ... from anon` 명시 |
| S3 | `promote_track_links` 가드가 `auth.uid() is not null and not is_admin(...)` 이라 **미인증이면 통째로 통과** | JWT 클레임 기반으로 뒤집음. 클레임 없음(=DB 직결)만 허용 |
| S4 | `revoke update (col)` 이 무시됨 — 테이블 레벨 GRANT 가 전 컬럼을 포함 | S1 의 재부여 방식으로 해결. **적용 후 `information_schema.column_privileges` 로 실측 확인함** |
| S5 | `min(uuid)` 집계가 Postgres 에 없어 `promote_track_links` 가 런타임 실패 | `(array_agg(...))[1]` 로 교체. 스모크 실행으로 발견 |
| S6 | `password_resets` 에 `anon`/`authenticated` **전체 CRUD GRANT**. RLS 무정책이 막고 있을 뿐이라 정책 1개만 추가돼도 재설정 토큰이 열린다 | `revoke all from anon, authenticated`. RLS 가 이미 전부 막고 있었으므로 쓰는 경로가 존재할 수 없음 |
| S7 | `jsonb_to_eq_array` 의 `search_path` 미고정 (`sync_track_history` DEFINER 안에서 호출됨) | 본문 변경 없이 `alter function ... set search_path = public, pg_temp` |

전수 점검에서 **오탐으로 판정한 것**(근거는 `ALGORITHM_CHANGES.md` 2026-09-22 절):
`track_alias`/`track_meta` 무정책 RLS(권한 자체가 0건), `pg_net` in public
(소유 객체 전부 `net` 스키마, `public` 에 0개 — 옮기면 지원티켓 웹훅만 깨진다),
`authenticated` SECURITY DEFINER 16건(관리자 함수 4종은 본문에 `is_admin` 가드 확인).

### 8-3. 아직 안 된 것

- `sync_track_history` RPC 가 `track` 의 `key_tier` / `source_key` / `identity_source` 를
  아직 안 채운다 → 검토 화면에 표시할 재료가 비어 있다. **v0.1.2 선행 과제**
- 클라이언트의 `propose_track_link` 호출부 — 지문(§5) 구현에 의존
- RPC 통합 테스트: SQL 콘솔에는 `auth.uid()` 가 없어 관리자 세션 호출을 검증 못 했다.
  정적 검증 + `promote_track_links` 0행 반환까지만 확인됨
- **`check_user_exists` / `get_user_id_by_email` 이 `anon` 에 열려 있다** —
  가입 여부 열거 + 이메일→UUID 직접 조회. 가드 없음. 끊는 SQL 은 준비됐지만
  웹사이트 가입·재설정 흐름이 쓸 가능성이 높고 사이트 코드는 수정 금지라 **보류.**
  사용자 확인 필요
- `auth_leaked_password_protection` 비활성 — SQL 로 못 켠다. 대시보드 Auth 설정
