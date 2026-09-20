// ltas_fit - mood_log.jsonl 의 31밴드 실측치를 집계하고 해석적 기준 커브를 적합한다.
//
// [왜 필요한가] LocalCurve.cpp 의 kRefBlob 은 4곡(발라드/재즈/K-pop/첼로) 평균이다.
//   표본 4개로 밴드별 평균 31개를 추정하면 밴드마다 잔차가 크고, 그 잔차가 바로
//   장르 고유 성분이다 (40Hz +3.93dB = 그 곡들의 베이스라인, 20kHz -7.49dB = 코덱
//   컷오프). 기준을 밴드별 타깃으로 쓰는 순간 그 잡음이 전 사용자의 보정 목표가
//   된다. 이 도구는 (1) 실제 청취 로그로 표본을 4곡 -> 수백 곡으로 늘리고,
//   (2) 저차 해석 모델을 적합해 표본 고유 성분을 구조적으로 버린다.
//
// [개인정보] 제목/아티스트는 곡을 묶는 용도로만 쓰고 64비트 해시로만 다룬다.
//   어떤 경로로도 출력하지 않는다.
//
// 진단 전용. 릴리즈 빌드와 게이트에 들어가지 않는다.

// [개인 데이터] 기본 동작은 소스에 박혀 있는 기준 배열만 적합한다 - 어떤 파일도
//   열지 않는다. mood_log 집계는 `--log` 를 명시해야만 돈다 (사용자 승인 필요).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

const int kNB = 31;

const int kF31[kNB] = {20,   25,   31,   40,   50,   63,   80,	 100,  125,  160,  200,
		       250,  315,  400,  500,  630,  800,  1000, 1250, 1600, 2000,
		       2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000};

// 정규화 기준 구간 - 어떤 샘플레이트에서도 항상 유효한 코어.
// (100Hz ~ 5kHz. 20Hz 부근은 재생기기 차이가 크고, 20kHz 는 나이퀴스트/코덱에 걸린다.)
const int kCoreLo = 7;	// 100 Hz
const int kCoreHi = 24; // 5 kHz

const float kInvalidDb = -190.f; // 로그의 -200.0 센티널

// ── 작은 JSON 조각 파서 ────────────────────────────────────────────────────
// 한 줄 = 평평한 객체 하나. 완전한 JSON 파서가 필요 없다.

bool FindKey(const std::string& s, const char* key, size_t& posOut) {
	std::string pat = std::string("\"") + key + "\":";
	size_t p = s.find(pat);
	if (p == std::string::npos)
		return false;
	posOut = p + pat.size();
	return true;
}

bool ParseBands(const std::string& s, float out[kNB]) {
	size_t p;
	if (!FindKey(s, "bands", p))
		return false;
	while (p < s.size() && s[p] != '[')
		++p;
	if (p >= s.size())
		return false;
	++p;
	int n = 0;
	while (p < s.size() && s[p] != ']') {
		while (p < s.size() && (s[p] == ' ' || s[p] == ','))
			++p;
		if (p >= s.size() || s[p] == ']')
			break;
		char* end = nullptr;
		double v = std::strtod(s.c_str() + p, &end);
		if (end == s.c_str() + p)
			return false;
		if (n < kNB)
			out[n] = (float)v;
		++n;
		p = (size_t)(end - s.c_str());
	}
	return n == kNB;
}

// 문자열 값을 읽지 않고 곧바로 해시한다. 원문은 어디에도 남기지 않는다.
uint64_t HashStringValue(const std::string& s, const char* key, uint64_t seed) {
	size_t p;
	if (!FindKey(s, key, p))
		return seed;
	while (p < s.size() && s[p] != '"')
		++p;
	if (p >= s.size())
		return seed;
	++p;
	uint64_t h = seed;
	while (p < s.size() && s[p] != '"') {
		if (s[p] == '\\' && p + 1 < s.size())
			++p;
		h ^= (uint64_t)(unsigned char)s[p];
		h *= 1099511628211ULL;
		++p;
	}
	return h;
}

bool PhaseIs(const std::string& s, const char* want) {
	size_t p;
	if (!FindKey(s, "phase", p))
		return false;
	while (p < s.size() && s[p] != '"')
		++p;
	if (p >= s.size())
		return false;
	++p;
	size_t q = s.find('"', p);
	if (q == std::string::npos)
		return false;
	return s.compare(p, q - p, want) == 0;
}

// ── 선형 최소제곱 (정규방정식 + 가우스 소거) ───────────────────────────────

bool SolveLLS(const std::vector<std::vector<double>>& basis, // [nparam][nb]
	      const std::vector<double>& y, const std::vector<double>& w,
	      std::vector<double>& coefOut) {
	const size_t np = basis.size();
	const size_t nb = y.size();
	std::vector<std::vector<double>> A(np, std::vector<double>(np + 1, 0.0));
	for (size_t i = 0; i < np; ++i) {
		for (size_t j = 0; j < np; ++j) {
			double s = 0.0;
			for (size_t b = 0; b < nb; ++b)
				s += w[b] * basis[i][b] * basis[j][b];
			A[i][j] = s;
		}
		double s = 0.0;
		for (size_t b = 0; b < nb; ++b)
			s += w[b] * basis[i][b] * y[b];
		A[i][np] = s;
	}
	for (size_t c = 0; c < np; ++c) {
		size_t piv = c;
		for (size_t r = c + 1; r < np; ++r)
			if (std::fabs(A[r][c]) > std::fabs(A[piv][c]))
				piv = r;
		if (std::fabs(A[piv][c]) < 1e-12)
			return false;
		std::swap(A[c], A[piv]);
		for (size_t r = 0; r < np; ++r) {
			if (r == c)
				continue;
			const double f = A[r][c] / A[c][c];
			for (size_t k = c; k <= np; ++k)
				A[r][k] -= f * A[c][k];
		}
	}
	coefOut.assign(np, 0.0);
	for (size_t c = 0; c < np; ++c)
		coefOut[c] = A[c][np] / A[c][c];
	return true;
}

struct FitResult {
	std::string name;
	int nparam = 0;
	double rms = 0.0;
	double maxAbs = 0.0;
	int maxAt = 0;
	double knee = 0.0; // 사용한 경우만
	double width = 0.0;
	std::vector<double> pred;
};

void Evaluate(FitResult& fr, const std::vector<double>& y, const std::vector<double>& w) {
	double ss = 0.0, wsum = 0.0;
	fr.maxAbs = 0.0;
	fr.maxAt = 0;
	for (size_t b = 0; b < y.size(); ++b) {
		const double r = y[b] - fr.pred[b];
		if (w[b] > 0.0) {
			ss += w[b] * r * r;
			wsum += w[b];
		}
		if (std::fabs(r) > fr.maxAbs) {
			fr.maxAbs = std::fabs(r);
			fr.maxAt = (int)b;
		}
	}
	fr.rms = (wsum > 0.0) ? std::sqrt(ss / wsum) : 0.0;
}

FitResult FitPoly(const char* name, int order, const std::vector<double>& x,
		  const std::vector<double>& y, const std::vector<double>& w) {
	FitResult fr;
	fr.name = name;
	fr.nparam = order + 1;
	std::vector<std::vector<double>> basis((size_t)order + 1, std::vector<double>(x.size(), 1.0));
	for (int k = 1; k <= order; ++k)
		for (size_t b = 0; b < x.size(); ++b)
			basis[(size_t)k][b] = basis[(size_t)k - 1][b] * x[b];
	std::vector<double> c;
	if (!SolveLLS(basis, y, w, c)) {
		fr.pred.assign(x.size(), 0.0);
		return fr;
	}
	fr.pred.assign(x.size(), 0.0);
	for (size_t b = 0; b < x.size(); ++b)
		for (size_t k = 0; k < c.size(); ++k)
			fr.pred[b] += c[k] * basis[k][b];
	Evaluate(fr, y, w);
	std::printf("  %-22s coef:", name);
	for (size_t k = 0; k < c.size(); ++k)
		std::printf(" %+.4f", c[k]);
	std::printf("\n");
	return fr;
}

// 꺾은선: y = c0 + c1*(x-xk) + c2*max(0, x-xk)   (xk 는 격자 탐색)
FitResult FitHinge(const std::vector<double>& x, const std::vector<double>& y,
		   const std::vector<double>& w) {
	FitResult best;
	best.name = "broken-line(hinge)";
	best.nparam = 4; // c0,c1,c2 + knee
	best.rms = 1e9;
	std::vector<double> bestC;
	for (double xk = -5.0; xk <= 3.0; xk += 0.05) {
		std::vector<std::vector<double>> basis(3, std::vector<double>(x.size(), 0.0));
		for (size_t b = 0; b < x.size(); ++b) {
			basis[0][b] = 1.0;
			basis[1][b] = x[b] - xk;
			basis[2][b] = std::max(0.0, x[b] - xk);
		}
		std::vector<double> c;
		if (!SolveLLS(basis, y, w, c))
			continue;
		FitResult fr;
		fr.name = best.name;
		fr.nparam = 4;
		fr.knee = xk;
		fr.pred.assign(x.size(), 0.0);
		for (size_t b = 0; b < x.size(); ++b)
			for (size_t k = 0; k < 3; ++k)
				fr.pred[b] += c[k] * basis[k][b];
		Evaluate(fr, y, w);
		if (fr.rms < best.rms) {
			best = fr;
			bestC = c;
		}
	}
	std::printf("  %-22s knee=%.3f (%.0f Hz) coef: %+.4f %+.4f %+.4f\n", best.name.c_str(),
		    best.knee, 1000.0 * std::pow(2.0, best.knee), bestC[0], bestC[1], bestC[2]);
	return best;
}

// 부드러운 꺾은선: y = c0 + c1*(x-xk) + c2*W*log(2*cosh((x-xk)/W))
//   W -> 0 이면 위의 hinge 와 같아진다. 밴드 경계에서 기울기가 튀지 않는다.
FitResult FitSoftKnee(const std::vector<double>& x, const std::vector<double>& y,
		      const std::vector<double>& w) {
	FitResult best;
	best.name = "soft-knee";
	best.nparam = 5; // c0,c1,c2 + knee + width
	best.rms = 1e9;
	std::vector<double> bestC;
	for (double xk = -5.0; xk <= 3.0; xk += 0.1) {
		for (double W = 0.2; W <= 3.0; W += 0.1) {
			std::vector<std::vector<double>> basis(3, std::vector<double>(x.size(), 0.0));
			for (size_t b = 0; b < x.size(); ++b) {
				const double t = (x[b] - xk) / W;
				basis[0][b] = 1.0;
				basis[1][b] = x[b] - xk;
				// 수치 안정: log(2*cosh t) = |t| + log(1+exp(-2|t|))
				const double at = std::fabs(t);
				basis[2][b] = W * (at + std::log1p(std::exp(-2.0 * at)));
			}
			std::vector<double> c;
			if (!SolveLLS(basis, y, w, c))
				continue;
			FitResult fr;
			fr.name = best.name;
			fr.nparam = 5;
			fr.knee = xk;
			fr.width = W;
			fr.pred.assign(x.size(), 0.0);
			for (size_t b = 0; b < x.size(); ++b)
				for (size_t k = 0; k < 3; ++k)
					fr.pred[b] += c[k] * basis[k][b];
			Evaluate(fr, y, w);
			if (fr.rms < best.rms) {
				best = fr;
				bestC = c;
			}
		}
	}
	std::printf("  %-22s knee=%.3f (%.0f Hz) W=%.2f coef: %+.4f %+.4f %+.4f\n",
		    best.name.c_str(), best.knee, 1000.0 * std::pow(2.0, best.knee), best.width,
		    bestC[0], bestC[1], bestC[2]);
	return best;
}

// 현재 봉인돼 있는 4곡 기준 (상대 파워). 비교용.
const double kCurrentRef[kNB] = {
    3.833e-3, 8.431e-3, 2.528e-2, 8.391e-2, 5.447e-2, 4.655e-2, 5.762e-2, 7.529e-2,
    7.213e-2, 9.195e-2, 1.089e-1, 6.389e-2, 5.295e-2, 6.215e-2, 4.103e-2, 3.245e-2,
    2.901e-2, 2.182e-2, 1.622e-2, 1.326e-2, 9.527e-3, 6.629e-3, 5.424e-3, 4.083e-3,
    3.553e-3, 2.969e-3, 2.863e-3, 2.058e-3, 1.203e-3, 4.476e-4, 5.526e-5};

struct Song {
	double sum[kNB] = {0};
	int cnt[kNB] = {0};
	int windows = 0;
};

// 저차 해석 모델들을 같은 타깃에 적합하고 잔차를 나란히 낸다.
// 파라미터를 늘릴수록 rms 는 반드시 줄지만, 줄어든 만큼이 표본 잡음이면
// 우리가 버리려던 장르 성분을 도로 주워 담는 것이다 - 잔차표로 판단한다.
void RunFits(const std::vector<double>& y) {
	std::vector<double> x(kNB), wAll(kNB, 1.0), wTrim(kNB, 1.0);
	for (int b = 0; b < kNB; ++b)
		x[b] = std::log(kF31[b] / 1000.0) / std::log(2.0);
	// 양 끝 2밴드는 가중 0 - 20Hz 는 기기 차이, 20kHz 는 코덱/나이퀴스트.
	wTrim[0] = wTrim[1] = wTrim[kNB - 1] = wTrim[kNB - 2] = 0.0;

	for (int pass = 0; pass < 2; ++pass) {
		const std::vector<double>& w = (pass == 0) ? wAll : wTrim;
		std::printf("\n=== fits (%s) ===\n",
			    (pass == 0) ? "all 31 bands" : "edges 20/25Hz,16k/20kHz excluded");
		std::vector<FitResult> fits;
		fits.push_back(FitPoly("linear(log2f)", 1, x, y, w));
		fits.push_back(FitPoly("quadratic", 2, x, y, w));
		fits.push_back(FitPoly("cubic", 3, x, y, w));
		fits.push_back(FitHinge(x, y, w));
		fits.push_back(FitSoftKnee(x, y, w));

		std::printf("\n  %-22s %6s %8s %8s %10s\n", "model", "npar", "rms(dB)", "max(dB)",
			    "max@Hz");
		for (size_t i = 0; i < fits.size(); ++i)
			std::printf("  %-22s %6d %8.3f %8.3f %10d\n", fits[i].name.c_str(),
				    fits[i].nparam, fits[i].rms, fits[i].maxAbs,
				    kF31[fits[i].maxAt]);

		std::printf("\n  residual (target - model), dB\n");
		std::printf("  %6s", "Hz");
		for (size_t i = 0; i < fits.size(); ++i)
			std::printf(" %10s", fits[i].name.substr(0, 10).c_str());
		std::printf("\n");
		for (int b = 0; b < kNB; ++b) {
			std::printf("  %6d", kF31[b]);
			for (size_t i = 0; i < fits.size(); ++i)
				std::printf(" %10.2f", y[b] - fits[i].pred[b]);
			std::printf("\n");
		}
	}
}

// 형상(무릎/기울기)을 고정하고 DC 항만 최적화해서 평가한다.
// [왜 DC 만] 제어식의 offset 이 (measured-ref) 의 가중평균을 빼므로 ref 의 상수항은
//   dev 에서 정확히 상쇄된다. 즉 DC 는 자유 파라미터가 아니라 무의미한 항이다.
//   따라서 모델끼리 공정하게 비교하려면 DC 는 각각 최적으로 맞춰 놓고 형상만 본다.
void EvalFixedShape(const char* label, double kneeHz, double slopeLo, double slopeHi,
		    const std::vector<double>& y, const std::vector<double>& w) {
	const double xk = std::log(kneeHz / 1000.0) / std::log(2.0);
	std::vector<double> shape(kNB, 0.0);
	for (int b = 0; b < kNB; ++b) {
		const double x = std::log(kF31[b] / 1000.0) / std::log(2.0);
		shape[b] = (x < xk) ? slopeLo * (x - xk) : slopeHi * (x - xk);
	}
	double num = 0.0, den = 0.0;
	for (int b = 0; b < kNB; ++b) {
		num += w[b] * (y[b] - shape[b]);
		den += w[b];
	}
	const double dc = (den > 0.0) ? num / den : 0.0;
	double ss = 0.0, wsum = 0.0, mx = 0.0;
	int mxAt = 0;
	for (int b = 0; b < kNB; ++b) {
		const double r = y[b] - (shape[b] + dc);
		if (w[b] > 0.0) {
			ss += w[b] * r * r;
			wsum += w[b];
			if (std::fabs(r) > mx) {
				mx = std::fabs(r);
				mxAt = b;
			}
		}
	}
	std::printf("  %-28s knee=%6.1fHz lo=%+.2f hi=%+.2f  rms=%.3f  max=%.3f @%dHz\n", label,
		    kneeHz, slopeLo, slopeHi, std::sqrt(ss / wsum), mx, kF31[mxAt]);
}

// 적합값을 그대로 박으면 소수점 뒤에 표본 고유 정보가 남는다. 반올림해도
// 형상이 버티는지 본다 - 버티면 그 값은 표본이 아니라 모델이다.
void RunRoundingCheck(const std::vector<double>& y) {
	std::vector<double> w(kNB, 1.0);
	w[0] = w[1] = w[kNB - 1] = w[kNB - 2] = 0.0;
	std::printf("\n=== rounded parameter candidates (edges excluded) ===\n");
	EvalFixedShape("fitted", 203.1, 1.5106, -3.0995, y, w);
	EvalFixedShape("round 200 / +1.5 / -3.0", 200.0, 1.5, -3.0, y, w);
	EvalFixedShape("round 200 / +1.5 / -3.1", 200.0, 1.5, -3.1, y, w);
	EvalFixedShape("round 200 / +1.6 / -3.1", 200.0, 1.6, -3.1, y, w);
	EvalFixedShape("round 250 / +1.5 / -3.0", 250.0, 1.5, -3.0, y, w);
	EvalFixedShape("round 160 / +1.5 / -3.0", 160.0, 1.5, -3.0, y, w);
	EvalFixedShape("round 200 / +2.0 / -3.0", 200.0, 2.0, -3.0, y, w);
	// 비교용: 평탄(핑크) 기준이 왜 안 되는지 숫자로 보이기.
	EvalFixedShape("pink (flat, no tilt)", 200.0, 0.0, 0.0, y, w);
}

// 3구간 고정 형상: 초저역 롤오프 / 바디 / 고역 롤오프.
// [왜 3구간인가] 2구간이 20~31Hz 에서 크게 빗나가는 게 "표본 잡음"이면 모델을
//   늘리면 안 되고, "물리"면 늘려야 한다. 마스터링 하이패스(20~30Hz)는 장르가
//   아니라 제작 관행이므로 물리 쪽이다. 숫자로 가른다.
// 상단 무릎은 일부러 넣지 않는다 - 16k/20k 의 절벽은 코덱 아티팩트이고,
//   그건 기준이 아니라 프레즌스 게이트가 처리할 문제다.
void EvalFixedShape3(const char* label, double kneeLoHz, double slopeSub, double kneeHz,
		     double slopeLo, double slopeHi, const std::vector<double>& y,
		     const std::vector<double>& w) {
	const double xkl = std::log(kneeLoHz / 1000.0) / std::log(2.0);
	const double xk = std::log(kneeHz / 1000.0) / std::log(2.0);
	std::vector<double> shape(kNB, 0.0);
	for (int b = 0; b < kNB; ++b) {
		const double x = std::log(kF31[b] / 1000.0) / std::log(2.0);
		if (x >= xk)
			shape[b] = slopeHi * (x - xk);
		else if (x >= xkl)
			shape[b] = slopeLo * (x - xk);
		else
			shape[b] = slopeLo * (xkl - xk) + slopeSub * (x - xkl);
	}
	double num = 0.0, den = 0.0;
	for (int b = 0; b < kNB; ++b) {
		num += w[b] * (y[b] - shape[b]);
		den += w[b];
	}
	const double dc = (den > 0.0) ? num / den : 0.0;
	double ss = 0.0, wsum = 0.0, mx = 0.0;
	int mxAt = 0;
	for (int b = 0; b < kNB; ++b) {
		const double r = y[b] - (shape[b] + dc);
		if (w[b] > 0.0) {
			ss += w[b] * r * r;
			wsum += w[b];
			if (std::fabs(r) > mx) {
				mx = std::fabs(r);
				mxAt = b;
			}
		}
	}
	std::printf("  %-30s sub<%3.0fHz %+.1f/oct | knee %3.0fHz %+.1f/%+.1f  rms=%.3f  "
		    "max=%.2f @%dHz\n",
		    label, kneeLoHz, slopeSub, kneeHz, slopeLo, slopeHi, std::sqrt(ss / wsum), mx,
		    kF31[mxAt]);
}

// 저역 끝을 포함해서 본다. 상단 2밴드(16k/20k)만 코덱 절벽이라 제외.
void RunEdgeCheck(const std::vector<double>& y) {
	std::vector<double> w(kNB, 1.0);
	w[kNB - 1] = w[kNB - 2] = 0.0;
	std::printf("\n=== low-end: 2-segment vs 3-segment (20Hz..12.5kHz weighted) ===\n");
	EvalFixedShape("2-seg 200 / +1.5 / -3.0", 200.0, 1.5, -3.0, y, w);
	const double subs[] = {4.0, 6.0, 8.0, 10.0, 12.0};
	const double knees[] = {40.0, 50.0, 63.0, 80.0};
	for (size_t k = 0; k < sizeof(knees) / sizeof(knees[0]); ++k)
		for (size_t s = 0; s < sizeof(subs) / sizeof(subs[0]); ++s) {
			char buf[64];
			std::snprintf(buf, sizeof(buf), "3-seg");
			EvalFixedShape3(buf, knees[k], subs[s], 200.0, 1.5, -3.0, y, w);
		}
}

// 코어 구간(100Hz~5kHz) 평균을 0 으로 맞춘 dB 형상.
std::vector<double> CurrentReferenceDb() {
	std::vector<double> v(kNB, 0.0);
	double core = 0.0;
	for (int b = 0; b < kNB; ++b)
		v[b] = 10.0 * std::log10(kCurrentRef[b]);
	for (int b = kCoreLo; b <= kCoreHi; ++b)
		core += v[b];
	core /= (kCoreHi - kCoreLo + 1);
	for (int b = 0; b < kNB; ++b)
		v[b] -= core;
	return v;
}

} // namespace

int main(int argc, char** argv) {
	// 기본 모드는 아무 파일도 열지 않는다 - 소스에 박힌 기준 배열만 적합한다.
	// mood_log 집계는 --log 를 명시해야 돌고, 그건 사용자 승인 사항이다.
	bool useLog = false;
	std::string path;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--log") == 0) {
			useLog = true;
			if (i + 1 < argc && argv[i + 1][0] != '-')
				path = argv[++i];
		}
	}

	if (!useLog) {
		std::printf("=== builtin reference fit (no files read) ===\n");
		std::printf("target = kRefBlob (4-song reference), core-normalized to 0 dB\n");
		const std::vector<double> ref = CurrentReferenceDb();
		RunFits(ref);
		RunRoundingCheck(ref);
		RunEdgeCheck(ref);
		return 0;
	}

	if (path.empty()) {
		const char* lad = std::getenv("LOCALAPPDATA");
		if (!lad) {
			std::printf("[ERROR] LOCALAPPDATA not set\n");
			return 2;
		}
		path = std::string(lad) + "\\SoundMateEqualizer\\record\\mood_log.jsonl";
	}

	std::ifstream f(path.c_str());
	if (!f) {
		std::printf("[ERROR] cannot open log\n");
		return 2;
	}

	std::map<uint64_t, Song> songs;
	long lines = 0, used = 0, skippedNoBands = 0, skippedCore = 0;
	int phaseFirst = 0, phaseTrack = 0;

	std::string line;
	while (std::getline(f, line)) {
		++lines;
		if (line.size() < 20)
			continue;
		float raw[kNB];
		if (!ParseBands(line, raw)) {
			++skippedNoBands;
			continue;
		}

		// 코어 구간 평균으로 정규화 - 재생 음량과 기기 차이를 제거한다.
		double coreSum = 0.0;
		int coreN = 0;
		for (int b = kCoreLo; b <= kCoreHi; ++b) {
			if (raw[b] > kInvalidDb) {
				coreSum += raw[b];
				++coreN;
			}
		}
		if (coreN < (kCoreHi - kCoreLo + 1)) {
			++skippedCore;
			continue;
		}
		const double coreMean = coreSum / coreN;

		if (PhaseIs(line, "first"))
			++phaseFirst;
		else
			++phaseTrack;

		uint64_t key = 1469598103934665603ULL;
		key = HashStringValue(line, "title", key);
		key = HashStringValue(line, "artist", key);

		Song& s = songs[key];
		++s.windows;
		for (int b = 0; b < kNB; ++b) {
			if (raw[b] > kInvalidDb) {
				s.sum[b] += raw[b] - coreMean;
				s.cnt[b] += 1;
			}
		}
		++used;
	}

	std::printf("=== mood_log LTAS aggregate ===\n");
	std::printf("lines=%ld  used=%ld  noBands=%ld  coreIncomplete=%ld\n", lines, used,
		    skippedNoBands, skippedCore);
	std::printf("phase first=%d track=%d   distinct songs=%d\n", phaseFirst, phaseTrack,
		    (int)songs.size());

	if (songs.empty()) {
		std::printf("[ERROR] no usable records\n");
		return 2;
	}

	// 곡별 평균을 먼저 낸 뒤 곡끼리 집계한다 - 오래 튼 곡이 지배하지 않도록.
	std::vector<std::vector<double>> perSong(kNB);
	for (std::map<uint64_t, Song>::const_iterator it = songs.begin(); it != songs.end(); ++it) {
		const Song& s = it->second;
		for (int b = 0; b < kNB; ++b)
			if (s.cnt[b] > 0)
				perSong[b].push_back(s.sum[b] / s.cnt[b]);
	}

	std::vector<double> mean(kNB, 0.0), median(kNB, 0.0);
	std::vector<int> nsong(kNB, 0);
	for (int b = 0; b < kNB; ++b) {
		std::vector<double>& v = perSong[b];
		nsong[b] = (int)v.size();
		if (v.empty())
			continue;
		double s = 0.0;
		for (size_t i = 0; i < v.size(); ++i)
			s += v[i];
		mean[b] = s / v.size();
		std::sort(v.begin(), v.end());
		median[b] = (v.size() % 2) ? v[v.size() / 2]
					   : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
	}

	// 현재 4곡 기준도 같은 방식으로 정규화해서 나란히 본다.
	double curDb[kNB], curCore = 0.0;
	for (int b = 0; b < kNB; ++b)
		curDb[b] = 10.0 * std::log10(kCurrentRef[b]);
	for (int b = kCoreLo; b <= kCoreHi; ++b)
		curCore += curDb[b];
	curCore /= (kCoreHi - kCoreLo + 1);

	std::printf("\n  idx     Hz   songs   median     mean   cur(4곡)   med-cur\n");
	for (int b = 0; b < kNB; ++b) {
		const double c = curDb[b] - curCore;
		std::printf("  %3d  %6d   %5d  %+7.2f  %+7.2f   %+7.2f   %+7.2f\n", b, kF31[b],
			    nsong[b], median[b], mean[b], c, median[b] - c);
	}

	RunFits(median);
	RunRoundingCheck(median);
	return 0;
}
