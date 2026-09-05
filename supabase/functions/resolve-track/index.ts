import "jsr:@supabase/functions-js/edge-runtime.d.ts";
import { generateCurve, CURVE_VERSION } from "./curve.ts";

// Supabase Edge Function: resolve-track
//
//   1) 우리 DB 에서 곡을 찾는다 (장르 + 언어별 제목/아티스트)
//   2) 없을 때만 iTunes 를 호출하고, 결과를 우리 DB 에 적립한다
//   3) 장르 + 사용자 설문으로 31밴드 커브를 만들어 함께 돌려준다
//
// [2단 구조인 이유]
//   track_meta  = 곡 1개 = 1행 (canonical + 언어별 표기)
//   track_alias = 관측된 제목 1개 = 1행 -> track_meta 를 가리킴
//   이렇게 해야 "청춘만화" 와 "Coming Of Age Story" 가 **같은 곡**으로 묶인다.
//   1판처럼 norm_key 가 곧 행이면 둘은 영원히 별개다.
//
// [클라이언트가 직접 iTunes 를 안 부르는 이유]
//   신뢰성(위조 불가) + 캐시 효율(100명이 같은 곡을 들어도 iTunes 호출 1번).

const NORM_VERSION = 1;
const UNRESOLVED_RETRY_DAYS = 7;
const MAX_RAW_SAMPLES = 8;

const cors = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Headers": "authorization, x-client-info, apikey, content-type",
};

// ── 정규화 ───────────────────────────────────────────────────────────────────
// 용도가 다르면 안전 요건도 다르다.
//   normKey()     : 키용. 정밀도가 생명 — 과하게 지우면 서로 다른 곡이 한 키로
//                   붕괴한다. 그래서 구조적 규칙(괄호 태그 제거)만 쓴다.
//   searchQuery() : iTunes 질의용. 재현율이 중요 — 노이즈 단어를 적극 제거한다.
// 괄호 안 내용에 이런 단어가 있으면 **곡의 정체성**이다. 지우면 안 된다.
//   "Levitating (Remix)" 와 "Levitating (Acoustic)" 을 둘 다 "levitating" 으로
//   만들면 어쿠스틱 곡에 EDM 커브가 걸린다 — 우리가 가장 피하려던 over-merge.
const VERSION_WORDS = [
  "remix", "acoustic", "live", "instrumental", "inst", "cover", "version",
  "ver.", "edit", "extended", "remaster", "unplugged", "demo", "session",
  "orchestral", "piano", "karaoke", "reprise", "radio",
  "리믹스", "어쿠스틱", "라이브", "인스트", "커버", "버전", "리마스터", "피아노",
];

// [] 와 () 를 다르게 다룬다.
//   [] : 유튜브에서 거의 항상 업로더 태그다 ([MV], [Official], [4K], [가사]).
//        통째로 제거해도 곡 식별에 지장이 없다.
//   () : 편곡/버전 정보나 정식 제목의 일부일 수 있다 ("(Don't) Let Me Go").
//        버전 단어가 들어 있으면 **보존**하고, 아니면 제거한다.
function stripBrackets(s: string): string {
  let out = s.replace(/\[[^\]]*\]/g, " ").replace(/\{[^}]*\}/g, " ");
  out = out.replace(/\(([^)]*)\)/g, (m, inner) => {
    const low = String(inner).toLowerCase();
    return VERSION_WORDS.some((w) => low.includes(w)) ? m : " ";
  });
  return out;
}
function collapse(s: string): string {
  return s.replace(/\s+/g, " ").trim();
}
function normKey(title: string): string {
  return collapse(stripBrackets(title))
    .toLowerCase()
    .replace(/[^\p{L}\p{N}\s]/gu, " ")
    .replace(/\s+/g, " ")
    .trim();
}
// 사람이 읽을 표기 (키가 아님). 언어별 titles 에 저장한다.
function displayTitle(s: string): string {
  return collapse(stripBrackets(s));
}

const SEARCH_NOISE = [
  "lyrics", "official", "audio", "video", "music video", "mv", "live",
  "playlist", "1 hour", "1시간",
  "가사", "해석", "자막", "번역", "발음", "한글", "영어",
  "뮤비", "공식", "교차편집", "무대", "플레이리스트", "플리", "추천",
  "모음", "명곡", "띵곡", "팝송모음", "빌보드차트",
];
// 대형 음악 채널이 제목 앞에 붙는 경우가 많다. iTunes 질의에서는 걷어내야
// 매칭률이 오른다. **normKey 에는 적용하지 않는다** — 키는 결정성이 생명이라
// 규칙을 늘릴수록 흔들릴 여지가 커진다. 여기(재현율 영역)에만 쓴다.
const CHANNEL_PREFIX = [
  "1thek", "원더케이", "dingo", "딩고", "kbs kpop", "sbs kpop", "mbc kpop",
  "mnet", "smtown", "hybe labels", "jyp entertainment", "yg entertainment",
  "stone music", "스톤뮤직", "genie music", "지니뮤직", "studio choom",
  "it's live", "잇츠라이브", "방구석", "leenalchi", "officia",
];

// 플레이리스트/모음집은 단일 곡이 아니라서 iTunes 에 물어봐야 100% 실패한다.
// 조회를 아예 건너뛰어 rate limit 을 아낀다.
// 주의: "mix" 는 "Extended Mix" 같은 버전 표기와 충돌하므로 넣지 않는다.
const COMPILATION_WORDS = [
  "playlist", "플레이리스트", "플리", "모음", "연속재생", "노동요",
  "compilation", "메들리", "medley", "1시간", "2시간", "3시간",
  "1 hour", "2 hour", "3 hour", "10 hour",
];

export function isCompilation(title: string): boolean {
  const t = title.toLowerCase();
  return COMPILATION_WORDS.some((w) => t.includes(w));
}

function searchQuery(title: string, artist: string): string {
  let t = collapse(stripBrackets(title)).toLowerCase();
  for (const w of SEARCH_NOISE) t = t.split(w).join(" ");
  for (const p of CHANNEL_PREFIX) t = t.split(p).join(" ");
  t = collapse(t);

  // "듣자마자 귀 녹음 주의 ㄷㄷ 성시경 - 희재" 처럼 어그로 문구가 앞에 붙는
  // 경우, 업로더들도 " - " 는 대체로 지킨다. 마지막 " - " 기준 우측이 곡명일
  // 가능성이 높지만 좌측(아티스트)도 필요하므로 **양쪽을 다 남기고** 그 앞의
  // 군더더기만 자른다. 하이픈이 여러 개면 마지막 두 조각만 취한다.
  const parts = t.split(" - ").map((s) => s.trim()).filter(Boolean);
  if (parts.length > 2) t = parts.slice(-2).join(" ");
  else if (parts.length === 2) t = parts.join(" ");

  const a = collapse(artist || "").toLowerCase();
  return a ? `${t} ${a}` : t;
}

// 문자 체계로 언어를 추정한다. 정밀한 언어 판별이 아니라 "어느 표기인가"를
// 구분하는 용도라 이 정도면 충분하다.
function detectLang(s: string): string {
  if (/[가-힣]/.test(s)) return "ko";
  if (/[぀-ヿ]/.test(s)) return "ja";
  if (/[一-鿿]/.test(s)) return "zh";
  if (/[Ѐ-ӿ]/.test(s)) return "ru";
  if (/[฀-๿]/.test(s)) return "th";
  return "en";
}

// 곡을 식별하는 2차 키. iTunes canonical 은 표기가 안정적이라 이걸로 같은 곡을
// 합칠 수 있다.
function canonicalKeyOf(title: string, artist: string): string {
  return `${normKey(title)}|${normKey(artist)}`;
}

function dbHeaders(k: string, extra: Record<string, string> = {}) {
  return {
    apikey: k,
    Authorization: `Bearer ${k}`,
    "Content-Type": "application/json",
    ...extra,
  };
}

Deno.serve(async (req: Request) => {
  if (req.method === "OPTIONS") return new Response("ok", { headers: cors });

  const json = (b: unknown, s = 200) =>
    new Response(JSON.stringify(b), {
      status: s,
      headers: { "Content-Type": "application/json", ...cors },
    });

  try {
    const URL_ = Deno.env.get("SUPABASE_URL");
    const KEY = Deno.env.get("SUPABASE_SERVICE_ROLE_KEY");
    if (!URL_ || !KEY) return json({ error: "server_misconfigured" }, 500);

    const body = await req.json().catch(() => ({}));
    const rawTitle = String(body.rawTitle ?? "").slice(0, 500);
    const rawArtist = String(body.rawArtist ?? "").slice(0, 300);
    // 설문 성향. 커브 산출에만 쓰이고 저장하지 않는다 (사용자 데이터).
    const tendency = String(body.tendency ?? "").slice(0, 300);

    const key = normKey(rawTitle);
    if (!key) return json({ error: "empty_title" }, 400);

    const META = `${URL_}/rest/v1/track_meta`;
    const ALIAS = `${URL_}/rest/v1/track_alias`;
    const enc = encodeURIComponent(key);

    const reply = (m: any, cached: boolean, extra: any = {}) =>
      json({
        normKey: key,
        canonicalTitle: m?.canonical_title ?? null,
        canonicalArtist: m?.canonical_artist ?? null,
        genre: m?.genre ?? null,
        itunesTrackId: m?.itunes_track_id ?? 0,
        titles: m?.titles ?? {},
        source: m?.source ?? "unresolved",
        cached,
        curve31: generateCurve(m?.genre ?? "", tendency),
        curveVersion: CURVE_VERSION,
        ...extra,
      });

    // ── 1) 우리 DB 먼저 ──────────────────────────────────────────────────
    const aResp = await fetch(
      `${ALIAS}?norm_key=eq.${enc}&select=*,track_meta(*)&limit=1`,
      { headers: dbHeaders(KEY) },
    );
    const aRows = aResp.ok ? await aResp.json() : [];
    const alias = Array.isArray(aRows) && aRows.length ? aRows[0] : null;
    const meta = alias?.track_meta ?? null;

    const stale =
      meta?.source === "unresolved" &&
      Date.now() - new Date(meta.updated_at).getTime() >
        UNRESOLVED_RETRY_DAYS * 86400000;

    if (alias && meta && !stale) {
      await fetch(`${ALIAS}?norm_key=eq.${enc}`, {
        method: "PATCH",
        headers: dbHeaders(KEY, { Prefer: "return=minimal" }),
        body: JSON.stringify({
          lookup_count: (alias.lookup_count ?? 0) + 1,
          updated_at: new Date().toISOString(),
        }),
      });
      return reply(meta, true);
    }

    // ── 2) 미스 — iTunes 조회 ───────────────────────────────────────────
    const tryItunes = async (term: string) => {
      if (!term) return { hit: null as any, transient: false };
      try {
        const r = await fetch(
          `https://itunes.apple.com/search?term=${encodeURIComponent(term)}` +
            `&media=music&entity=song&limit=1`,
          { headers: { "User-Agent": "SoundMate/0.1" } },
        );
        if (!r.ok) return { hit: null, transient: true };  // 429 / 5xx
        const d = await r.json();
        return { hit: d?.results?.[0] ?? null, transient: false };
      } catch {
        return { hit: null, transient: true };
      }
    };

    // [아티스트 폴백] 유튜브의 artist 는 채널명일 때가 많고, 검색어에 붙으면
    //   매칭이 통째로 실패한다. 실측: "Maroon 5 - Memories" 를 "감자탕Gamjatang"
    //   과 함께 검색하면 0건, 제목만 검색하면 정상 매칭.
    // [조기 차단] 플레이리스트/모음집은 단일 곡이 아니라 iTunes 조회가 100%
    //   실패한다. 부르지 않고 바로 미해석 처리해 rate limit 을 아낀다.
    //   곡을 식별하지 못해도 실시간 스펙트럼 보정은 그대로 동작하므로,
    //   사용자에게는 여전히 맞춤 EQ 가 걸린다.
    const compilation = isCompilation(rawTitle);

    let r = compilation
      ? { hit: null as any, transient: false }
      : await tryItunes(searchQuery(rawTitle, rawArtist));
    if (!compilation && !r.hit && !r.transient && rawArtist) {
      r = await tryItunes(searchQuery(rawTitle, ""));
    }

    // [중요] 일시적 실패(429/5xx)는 기록하지 않는다. 기록하면 rate limit 한 번에
    //   해석 가능한 곡이 7일간 미해석으로 굳는다.
    if (r.transient) {
      return json({
        normKey: key,
        canonicalTitle: null, canonicalArtist: null, genre: null, titles: {},
        itunesTrackId: 0,
        source: "transient_error", cached: false, retryable: true,
        // 장르를 못 얻어도 설문 기반 커브는 준다. EQ 가 아예 안 걸리는 상황을
        // 만들지 않는 것이 로컬 전환의 목표였다.
        curve31: generateCurve("", tendency),
        curveVersion: CURVE_VERSION,
      });
    }

    const nowIso = new Date().toISOString();
    const rawLang = detectLang(rawTitle);
    const rawDisp = displayTitle(rawTitle);

    let metaRow: any = null;

    if (r.hit) {
      const cTitle = r.hit.trackName ?? "";
      const cArtist = r.hit.artistName ?? "";
      const cKey = canonicalKeyOf(cTitle, cArtist);
      const cLang = detectLang(cTitle);

      // 같은 canonical 을 가진 곡이 이미 있는가 — 여기서 언어가 다른 제목들이
      // 하나로 합쳐진다.
      const mResp = await fetch(
        `${META}?canonical_key=eq.${encodeURIComponent(cKey)}&select=*&limit=1`,
        { headers: dbHeaders(KEY) },
      );
      const mRows = mResp.ok ? await mResp.json() : [];
      metaRow = Array.isArray(mRows) && mRows.length ? mRows[0] : null;

      // 언어별 표기 병합. 관측된 표기와 iTunes 표기를 둘 다 담는다.
      const titles = { ...(metaRow?.titles ?? {}) };
      titles[cLang] = { title: cTitle, artist: cArtist };
      if (!titles[rawLang]) {
        titles[rawLang] = { title: rawDisp, artist: rawArtist || null };
      }

      const payload = {
        canonical_key: cKey,
        canonical_title: cTitle,
        canonical_artist: cArtist,
        genre: r.hit.primaryGenreName ?? null,
        itunes_track_id: r.hit.trackId ?? null,
        titles,
        source: "itunes",
        norm_version: NORM_VERSION,
        lookup_count: (metaRow?.lookup_count ?? 0) + 1,
        updated_at: nowIso,
      };

      const upResp = await fetch(`${META}?on_conflict=canonical_key`, {
        method: "POST",
        headers: dbHeaders(KEY, {
          Prefer: "resolution=merge-duplicates,return=representation",
        }),
        body: JSON.stringify([payload]),
      });
      const upRows = upResp.ok ? await upResp.json() : [];
      metaRow = Array.isArray(upRows) && upRows.length ? upRows[0] : metaRow;
    } else {
      // iTunes 가 200 을 주고 결과가 없었다 — 진짜 미해석. 관측된 표기만 담아
      // 곡 행을 만들고 7일 쿨다운을 건다.
      const cKey = `unresolved:${key}`;
      const mResp = await fetch(
        `${META}?canonical_key=eq.${encodeURIComponent(cKey)}&select=*&limit=1`,
        { headers: dbHeaders(KEY) },
      );
      const mRows = mResp.ok ? await mResp.json() : [];
      const prev = Array.isArray(mRows) && mRows.length ? mRows[0] : null;

      const titles = { ...(prev?.titles ?? {}) };
      titles[rawLang] = { title: rawDisp, artist: rawArtist || null };

      const upResp = await fetch(`${META}?on_conflict=canonical_key`, {
        method: "POST",
        headers: dbHeaders(KEY, {
          Prefer: "resolution=merge-duplicates,return=representation",
        }),
        body: JSON.stringify([{
          canonical_key: cKey,
          canonical_title: null,
          canonical_artist: null,
          genre: null,
          titles,
          source: "unresolved",
          norm_version: NORM_VERSION,
          lookup_count: (prev?.lookup_count ?? 0) + 1,
          updated_at: nowIso,
        }]),
      });
      const upRows = upResp.ok ? await upResp.json() : [];
      metaRow = Array.isArray(upRows) && upRows.length ? upRows[0] : prev;
    }

    // ── 3) 별칭 등록 — 이 제목이 그 곡을 가리키게 한다 ────────────────────
    if (metaRow?.id) {
      const sample = { t: rawTitle, a: rawArtist, at: nowIso };
      const prevSamples = Array.isArray(alias?.raw_samples) ? alias.raw_samples : [];
      await fetch(`${ALIAS}?on_conflict=norm_key`, {
        method: "POST",
        headers: dbHeaders(KEY, {
          Prefer: "resolution=merge-duplicates,return=minimal",
        }),
        body: JSON.stringify([{
          norm_key: key,
          track_id: metaRow.id,
          lang: rawLang,
          raw_samples: [...prevSamples, sample].slice(-MAX_RAW_SAMPLES),
          norm_version: NORM_VERSION,
          lookup_count: (alias?.lookup_count ?? 0) + 1,
          updated_at: nowIso,
        }]),
      });
    }

    return reply(metaRow, false);
  } catch (e) {
    return json({ error: "internal", detail: String(e) }, 500);
  }
});
