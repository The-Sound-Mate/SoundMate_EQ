# 곡 연결 검토 화면 — 웹사이트 구현 인수인계

작성일 2026-09-22 · 대상: SoundMate 웹사이트(관리자 영역) 구현자
전제: 읽는 사람은 데스크톱 앱 저장소에 접근 권한이 없다. 이 문서 하나로 끝나야 한다.

---

## 1. 한 문단 요약

SoundMate 는 재생 중인 곡을 식별해 곡별 EQ 를 저장한다.
같은 곡이 Spotify 와 YouTube 에서 다른 메타데이터로 들어오기 때문에
"이 둘은 같은 곡" 이라는 연결을 만들어야 한다.

연결은 **대부분 앱이 자동으로 판정**한다.
자동으로 못 가린 애매한 쌍만 DB 의 검토 큐에 쌓이고,
**그 큐를 사람이 보고 승인/거부하는 화면을 웹사이트에 만드는 것**이 이 작업이다.

**DB 쪽은 이미 다 되어 있다.** 테이블·뷰·RPC·권한 전부 적용 완료다.
이 문서는 그 표면(surface)을 그대로 쓰는 법만 설명한다.
**DDL 을 새로 치지 말 것.** 스키마 변경이 필요하면 먼저 물어볼 것.

---

## 2. 왜 사람이 봐야 하는가 — 이 결정의 무게

`public.track` 은 **`user_id` 컬럼이 없는 전역 카탈로그**다. 곡 1개 = 행 1개이고
모든 사용자가 같은 행을 공유한다.

따라서 `track.canonical_id` 에 쓰는 것은 **전 사용자에게 적용되는 결정**이다.
승인 버튼 한 번을 잘못 누르면 전 사용자가 엉뚱한 곡의 EQ 를 듣는다.
되돌릴 수는 있지만(§6), 그 사이에 잘못된 소리가 나간다.

그래서 이 화면은 일반 사용자에게 절대 노출되지 않는다. 관리자 전용이다.
DB 레벨에서도 막혀 있다 — 일반 로그인 사용자는 검토 큐를 읽지도 못하고,
`canonical_id` 컬럼에 쓰기 권한 자체가 없다.

---

## 3. 읽어야 할 것 — 뷰 `public.track_link_review`

큐는 이 뷰로만 읽는다. 원본 테이블(`track_link_candidate`)을 직접 읽지 말 것 —
뷰가 `observers`(관측자 user_id 배열, PII)를 의도적으로 잘라낸다.

`security_invoker = true` 라서 RLS 가 그대로 걸린다. 관리자가 아니면 0행이 나온다.

| 컬럼 | 타입 | 의미 |
|---|---|---|
| `id` | uuid | 후보 id. RPC 에 넘길 값 |
| `status` | text | `pending` / `approved` / `rejected` / `deferred` |
| `reason` | text | 왜 애매한지 (§3-1) |
| `score` | real | 지문 일치 점수. null 일 수 있음 |
| `observed_users` | int | **서로 다른** 사용자 수. 같은 사람이 30번 들어도 1 |
| `evidence` | jsonb | 판정 근거 원본 (§3-2) |
| `created_at` / `updated_at` / `resolved_at` | timestamptz | |
| `track_a` / `track_b` | uuid | 쌍의 두 곡 |
| `title_a` / `artist_a` / `source_key_a` / `key_tier_a` | text | A 쪽 표시용 |
| `title_b` / `artist_b` / `source_key_b` / `key_tier_b` | text | B 쪽 표시용 |

`source_key` 는 재생 앱의 Windows AUMID 원문이다 (`Spotify.exe`, `chrome.exe` 등).
`key_tier` 는 `t`(신뢰 앱 — Spotify/Apple Music 등 태그가 정확) 또는
`u`(비신뢰 — 브라우저/로컬 플레이어. 제목이 지저분하다).

### 3-1. `reason` 값과 화면에서의 해석

| 값 | 뜻 | 관리자가 봐야 할 것 |
|---|---|---|
| `strong_fingerprint` | 오디오 지문이 여러 구간에서 일치 | 대개 맞다. 관측자가 모이면 §7 잡이 자동 승격하므로 큐에 오래 남지 않는다 |
| `weak_fingerprint` | 오디오 지문이 맞긴 하는데 근거 구간이 부족 | 제목/아티스트가 상식적으로 같은 곡인가 |
| `vote_spread` | 지문 표가 여러 시점으로 흩어짐 (리믹스/라이브 의심) | **기본은 거부.** 다른 음원이면 다른 EQ 가 맞다 |
| `text_conflict` | 지문은 맞는데 아티스트 이름이 다름 | 피처링/리마스터 표기 차이인지, 진짜 다른 곡인지 |
| `tier_mismatch` | 한쪽만 신뢰 앱 출처 | 비신뢰 쪽 제목의 잡음(`[Official MV]` 등) 제거 후 같은가 |

### 3-2. `evidence` jsonb

앱이 채우는 자유 형식이다. 스키마를 강제하지 않는다.
**키가 없을 수 있다고 가정하고 방어적으로 렌더링할 것.** 예상 키:

```jsonc
{
  "windows_agreed": 2,      // 같은 오프셋에 동의한 비중첩 분석 윈도 수
  "vote_ratio": 0.41,       // 최다 득표 오프셋 / 전체 표
  "text_sim": 0.88,         // 제목 문자열 유사도 0~1
  "duration_a": 214,        // 초. 참고값일 뿐 — 판정 근거 아님
  "duration_b": 247
}
```

> **길이 차이를 근거로 거부하지 말 것.** YouTube MV 는 인트로/아웃트로/광고로
> 길이가 항상 다르다. 길이는 가산점이지 키가 아니다.

### 3-3. 쿼리 예

```sql
-- 대기 큐. 여러 사람에게서 관측된 것부터.
select * from public.track_link_review
 where status = 'pending'
 order by observed_users desc, created_at asc
 limit 50;
```

인덱스 `track_link_candidate_queue_idx (status, observed_users desc, created_at)` 가
이 정렬에 맞춰져 있다. **다른 정렬을 기본값으로 쓰지 말 것.**

---

## 4. 써야 할 것 — RPC `public.resolve_track_link`

테이블에 직접 UPDATE 하지 말 것. `track.canonical_id` 는 컬럼 GRANT 로 막혀 있어서
어차피 실패한다. 통로는 이 함수뿐이다.

```
resolve_track_link(
  p_candidate_id uuid,      -- track_link_review.id
  p_action       text,      -- 'approve' | 'reject' | 'defer'
  p_canonical_id uuid       -- approve 일 때 필수. track_a 또는 track_b 중 하나
) returns void
```

Supabase JS:

```ts
const { error } = await supabase.rpc('resolve_track_link', {
  p_candidate_id: candidate.id,
  p_action: 'approve',
  p_canonical_id: candidate.track_a,   // 대표로 삼을 쪽
})
```

### 동작

- **approve** — `p_canonical_id` 가 아닌 **반대쪽** 곡의 `canonical_id` 를
  `p_canonical_id` 로 설정하고 `linked_by='admin'`, `linked_at=now()`.
  후보 행은 `approved` 로 닫힌다.
- **reject** — 연결하지 않고 후보를 `rejected` 로 닫는다.
  **앱이 같은 쌍을 다시 제안해도 큐가 다시 열리지 않는다.**
- **defer** — `deferred`. 표본이 더 쌓이길 기다린다는 뜻.

### 함수가 거부하는 경우 (전부 `raise exception` → JS 에서 `error`)

| 메시지 | 원인 | UI 처리 |
|---|---|---|
| `admin required` | 호출자가 관리자가 아님 | 화면 진입 자체를 막았어야 한다 |
| `candidate not found` | 잘못된 id / 이미 삭제 | 목록 새로고침 |
| `canonical must be one of the pair` | `p_canonical_id` 가 A/B 둘 다 아님 | 버그. 전송 전 검증할 것 |
| `target is not canonical` | 대표로 고른 곡이 **이미 다른 곡에 연결돼 있음** | §4-1 |

### 4-1. `target is not canonical` — 중요

연결 체인(A→B→C)을 금지한다. 조회는 **1홉만** 해석하기 때문에
체인이 생기면 A 가 영영 C 에 도달하지 못한다.

대표로 고른 쪽이 이미 다른 곡의 별칭이면 함수가 거부한다.
UI 는 이 에러를 잡아서 **"반대쪽을 대표로 선택해 보세요"** 를 안내하거나,
애초에 `track.canonical_id is null` 인 쪽만 대표 후보로 보여주면 된다.

두 쪽 다 이미 연결돼 있으면 사람이 손대야 하는 상황이다. `defer` 로 미루고
운영자에게 알린다.

---

## 5. 화면 요구사항

### 5-1. 접근 제어

- 라우트는 기존 관리자 영역 안에 둔다 (예: `/admin/track-links`).
- 프런트 가드만 믿지 말 것 — 어차피 RLS 가 막지만, 비관리자에게
  빈 화면 대신 명시적인 403 을 보여준다.
- 관리자 판정은 기존에 쓰던 방식을 그대로 쓴다 (DB 함수 `public.is_admin(uuid)`).

### 5-2. 목록

- 기본 필터 `status = 'pending'`, 정렬 `observed_users desc, created_at asc`
- `approved` / `rejected` / `deferred` 도 볼 수 있어야 한다 (감사 목적)
- 페이지네이션. 큐는 커질 수 있다

### 5-3. 한 건 비교 화면

좌우 2열로 A / B 를 나란히 놓는다. 각 열에 `title` · `artist` · `source_key` · `key_tier`.

그리고 **왜 애매한지를 반드시 같이 보여준다** — `reason` 을 §3-1 의 사람 말로 풀고,
`observed_users`, 그리고 `evidence` 에 실제로 들어 있는 키만 골라서.

> 근거 없이 버튼만 있는 화면을 만들면 안 된다.
> 관리자가 제목만 보고 찍게 되고, 그게 이 시스템이 막으려는 실패다.

액션 3개: **A를 대표로 승인** / **B를 대표로 승인** / **거부** / **보류**.
승인은 되돌리기 번거로우므로 확인 단계를 한 번 둔다.

### 5-4. 기본값에 대한 조언

- `reason = 'vote_spread'` 는 **거부를 기본 강조**로 둔다. 리믹스/라이브일 가능성이 높고,
  다른 음원이면 다른 EQ 가 맞다. 연결 안 하는 게 옳은 결과다.
- `observed_users = 1` 은 신중하게. 한 사람의 재생 환경 특성일 수 있다.

---

## 6. 되돌리기 (운영 도구로 같이 만들면 좋다)

연결은 **물리 병합이 아니다.** 행을 지우거나 합치지 않으므로 해제가 한 줄이다.

```sql
-- 특정 연결 해제
update public.track set canonical_id = null, linked_by = null, linked_at = null
 where id = '<track_id>';

-- 등급 단위 일괄 해제 — 자동 링크가 잘못 걸린 게 드러났을 때
update public.track set canonical_id = null, linked_by = null, linked_at = null
 where linked_by = 'fingerprint';
```

`linked_by` 를 등급별로 나눠 놓은 이유가 이것이다. 값은 4가지:

| 값 | 누가 연결했나 |
|---|---|
| `fingerprint` | 고신뢰 지문 자동 링크 |
| `promoted` | 서로 다른 사용자 N명의 관측이 쌓여 자동 승격 |
| `admin` | **이 화면에서 사람이 승인** |
| `user` | (레거시) 신규 생성 경로 없음 |

이 SQL 은 관리자 대시보드에서 서비스 롤로 실행하거나 Supabase 콘솔에서 돌린다.
일반 로그인 사용자는 이 컬럼들에 쓰기 권한이 없다.

---

## 7. 부가 — 승격 잡 `promote_track_links`

```
promote_track_links(p_min_users integer default 3) returns integer
```

검토 큐에서 **서로 다른 사용자 `p_min_users` 명이 독립적으로 제안한**
(`observed_users >= p_min_users`) 대기 후보를 전역으로 승격하고
(`linked_by='promoted'`) 그 후보를 `approved` 로 닫는다. 반환값은 승격된 행 수.

- 실행 권한: **service_role 전용.** `anon`/`authenticated` 모두 EXECUTE 없음
- **지문이 근거인 후보만** 자동 승격한다 —
  `reason in ('strong_fingerprint','weak_fingerprint')`.
  `text_conflict` / `tier_mismatch` / `vote_spread` 는 **사람이 봐야 하므로 건드리지 않는다.**
  즉 이 잡을 돌려도 검토 화면에서 처리해야 할 항목은 줄지 않는다
- 대표 선정은 결정적이다: 제목·아티스트가 채워진 쪽 우선 → 동률이면 `id` 순
- 1홉 보장 — 대표가 이미 남을 가리키거나, 피링크 곡이 이미 연결됐거나,
  피링크 곡을 누군가 대표로 삼고 있으면 건너뛴다 (체인 방지)
- `p_min_users < 2` 는 거부한다 (1명짜리 "합의" 는 합의가 아니다)
- cron 또는 관리자 버튼으로 하루 1회 정도. **호출은 멱등하다** — 여러 번 돌려도 안전

웹에서 붙인다면 서버 사이드(서비스 롤 키)에서만 호출할 것.
브라우저에서 직접 호출하면 권한 없음으로 실패한다.

---

## 8. 하지 말 것

- **DDL 금지.** 테이블/컬럼/제약 추가·변경 전에 반드시 확인받을 것
- `track_link_candidate` 직접 읽기 금지 — `observers`(user_id 배열)가 노출된다. 뷰를 쓸 것
- `track` 에 직접 UPDATE 로 `canonical_id` 쓰기 금지 — 권한이 없어 실패하고,
  성공했다면 그게 더 큰 문제다
- 일반 사용자에게 "이거 같은 곡인가요?" 를 묻는 UI 금지.
  전역 결정을 아무나 내리게 하는 순간 이 설계 전체가 무의미해진다
- 서비스 롤 키를 브라우저 번들에 넣지 말 것

---

## 9. 테스트 체크리스트

DB 쪽은 정적 검증과 `promote_track_links` 실행(0행 반환)까지 확인됐지만,
**RPC 를 실제 관리자 세션으로 호출한 통합 테스트는 아직 안 됐다.**
(SQL 콘솔에는 `auth.uid()` 가 없어 여기서 불가능했다.)
구현하면서 아래를 직접 확인할 것.

- [ ] 비관리자 로그인 → `track_link_review` SELECT 가 0행
- [ ] 비관리자 로그인 → `resolve_track_link` 호출이 `admin required` 로 실패
- [ ] 관리자 → approve 후 반대쪽 `track.canonical_id` 가 설정되고 `linked_by='admin'`
- [ ] 관리자 → 이미 연결된 곡을 대표로 지정 시 `target is not canonical`
- [ ] 관리자 → `p_canonical_id` 에 쌍과 무관한 uuid → `canonical must be one of the pair`
- [ ] reject 후 같은 쌍이 다시 `pending` 으로 안 열리는지
- [ ] 일반 사용자 토큰으로 `track` 의 `canonical_id` 직접 UPDATE 시도 → 실패
- [ ] `evidence` 가 `{}` 인 후보도 화면이 안 깨지는지

---

## 10. 용어

| 용어 | 뜻 |
|---|---|
| canonical | 같은 곡 묶음의 대표 행. `canonical_id is null` 인 행 |
| 별칭(alias) | `canonical_id` 가 설정된 행. 대표를 가리킨다 |
| 지문(fingerprint) | 오디오 신호에서 뽑은 랜드마크 해시. 앱 안에서만 계산하며 서버로 올리지 않는다 |
| 신뢰 앱 (`key_tier='t'`) | Spotify / Apple Music / iTunes / YouTube Music / Tidal / Deezer / Amazon Music |
| 비신뢰 앱 (`key_tier='u'`) | 브라우저, 로컬 플레이어, 미상 |
