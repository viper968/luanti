// Experimental mapblock codec: everything through one binary arithmetic coder,
// spatial context modelling for node data, trained priors.
#pragma once
#include "common.h"
#include <zstd.h>
#include <algorithm>
#include <memory>
#include <numeric>
#include <cmath>

namespace mbc {

// ---------------------------------------------------------------- range coder
struct Encoder {
	u32 x1 = 0, x2 = 0xffffffff;
	std::string &out;
	explicit Encoder(std::string &o) : out(o) {}
	inline void encode(int bit, u32 p1) {
		u32 xmid = x1 + (u32)(((u64)(x2 - x1) * p1) >> 16);
		if (bit) x2 = xmid; else x1 = xmid + 1;
		while (((x1 ^ x2) & 0xff000000) == 0) {
			out.push_back((char)(x2 >> 24));
			x1 <<= 8; x2 = (x2 << 8) | 255;
		}
	}
	// Shortest byte string b such that b followed by *any* bytes lies in [x1, x2].
	static int flushLen(u32 x1, u32 x2, u32 &val) {
		for (int k = 1; k < 4; k++) {
			int s = 32 - 8 * k;
			u64 b = x1 >> s;
			if ((b << s) < x1) b++;
			u64 lo = b << s, hi = lo | ((1ull << s) - 1);
			if (hi <= x2) { val = (u32)lo; return k; }
		}
		val = x1;
		return 4;
	}
	void flush() {
		u32 v; int k = flushLen(x1, x2, v);
		for (int i = 0; i < k; i++) out.push_back((char)(v >> (24 - 8 * i)));
	}
};

struct Decoder {
	u32 x1 = 0, x2 = 0xffffffff, x = 0;
	const u8 *start, *p, *end;
	Decoder(const u8 *b, const u8 *e) : start(b), p(b), end(e) {
		for (int i = 0; i < 4; i++) x = (x << 8) | next();
	}
	inline u8 next() { const u8 *q = p++; return q < end ? *q : 0; }
	inline int decode(u32 p1) {
		u32 xmid = x1 + (u32)(((u64)(x2 - x1) * p1) >> 16);
		int bit = x <= xmid;
		if (bit) x2 = xmid; else x1 = xmid + 1;
		while (((x1 ^ x2) & 0xff000000) == 0) {
			x1 <<= 8; x2 = (x2 << 8) | 255; x = (x << 8) | next();
		}
		return bit;
	}
	// Number of bytes the encoder produced (shifted bytes + flush)
	size_t consumed() const {
		u32 v;
		return (size_t)(p - start) - 4 + Encoder::flushLen(x1, x2, v);
	}
};

// ---------------------------------------------------------------- models
struct Slot { u16 p = 32768; u8 n = 0; u8 pad = 0; };

struct RateTable {
	u32 r[256];
	RateTable() { for (int i = 0; i < 256; i++) r[i] = (u32)(65536.0 / (i + 1.6)); }
};
static const RateTable g_rate;
static int g_limit = 12;

// Static (never adapted) models: header, names, mono blocks.
struct StaticModel {
	Slot flags[256];
	Slot lightFull, tsNone, mono, tailFlag;
	Slot tsBits[64];
	Slot palBits[64];
	Slot builtin[4];
	Slot prefBits[64];
	Slot sufBits[64];
	Slot chars[256][256];
	Slot monoContent[64];
	Slot monoP1[256];
	Slot monoP2[256];
};

// Adaptive models, initialised from priors for every block.
static const int PCLS = 8;
struct NodeModel {
	Slot same[3][8][2][2][PCLS];         // rank, nbmask, prevsame, diagAgree, cand class
	Slot cHit[3][8][2][2];               // rank, nbmask, prevhit, diagAgree
	Slot cEsc[17][256];
	Slot pHit[2][PCLS][4][16][8][2];     // field, class, rank, nbmask(+pred), samecontent, prevhit
	Slot pEsc[2][PCLS][256];
};

struct Priors {
	StaticModel s;
	NodeModel n;
};

// Training statistics: one counter pair per slot.
struct Stats {
	std::vector<u64> s0, s1, n0, n1;
	Stats() : s0(sizeof(StaticModel) / sizeof(Slot)), s1(s0.size()),
		n0(sizeof(NodeModel) / sizeof(Slot)), n1(n0.size()) {}
};
static Stats *g_stats = nullptr;
static double g_cost[8]; static int g_cat;

template<bool ENC>
struct Coder {
	Encoder *e = nullptr; Decoder *d = nullptr;
	const StaticModel *S = nullptr; NodeModel *N = nullptr;

	inline int code(u32 p, int b) {
		if (ENC) {
			e->encode(b, p);
			if (g_stats) g_cost[g_cat] -= std::log2((b ? p : 65536 - p) / 65536.0);
		} else {
			b = d->decode(p);
		}
		return b;
	}
	inline int sbit(const Slot &m, int b) {
		b = code(m.p, b);
		if (ENC && g_stats) {
			size_t i = &m - (const Slot *)S;
			(b ? g_stats->s1 : g_stats->s0)[i]++;
		}
		return b;
	}
	inline int abit(Slot &m, int b) {
		b = code(m.p, b);
		if (ENC && g_stats) {
			size_t i = &m - (Slot *)N;
			(b ? g_stats->n1 : g_stats->n0)[i]++;
		}
		int target = b ? 65535 : 0;
		int np = m.p + (int)(((long long)(target - m.p) * g_rate.r[m.n]) >> 16);
		m.p = (u16)std::clamp(np, 32, 65535 - 32);
		if (m.n < g_limit) m.n++;
		return b;
	}
	inline u32 raw(u32 v, int nbits) {
		u32 r = 0;
		for (int i = nbits - 1; i >= 0; i--)
			r |= (u32)code(32768, (v >> i) & 1) << i;
		return r;
	}
	template<bool ADAPT>
	inline u32 tree(const Slot *m, u32 v, int nbits) {
		u32 node = 1;
		for (int i = nbits - 1; i >= 0; i--) {
			int b = ADAPT ? abit(const_cast<Slot &>(m[node]), (v >> i) & 1)
				: sbit(m[node], (v >> i) & 1);
			node = (node << 1) | b;
		}
		return node - (1u << nbits);
	}
	// Elias-gamma style: bit length via static tree, then raw mantissa. v >= 1.
	inline u64 gamma(const Slot *m, u64 v) {
		int nb = 0;
		if (ENC) { while ((v >> nb) > 1) nb++; }
		nb = tree<false>(m, nb, 6);
		u64 r = 1;
		for (int i = nb - 1; i >= 0; i--)
			r = (r << 1) | (u64)code(32768, (v >> i) & 1);
		return r;
	}
};

static inline int bitsFor(u32 n) { int b = 0; while ((1u << b) < n) b++; return b; }

static inline u8 lightPredict(int nL, int nB, int nU, const u8 *p1, int i)
{
	int day = 0, night = 0;
	auto take = [&](int k) {
		if (k < 0) return;
		day = std::max(day, (p1[k] & 15) - 1);
		night = std::max(night, (p1[k] >> 4) - 1);
	};
	take(nL); take(nB); take(nU);
	if (nU >= 0 && (p1[nU] & 15) == 15) day = 15;
	return (u8)(std::max(day, 0) | (std::max(night, 0) << 4));
}

template<bool ENC>
struct BlockCoder {
	Coder<ENC> c;
	u8 cls[4096 + 1];

	void codeNodes(u16 *content, u8 *p1, u8 *p2, int npal)
	{
		NodeModel &N = *c.N;
		int cbits = bitsFor(npal);
		int phC = 0, phP[2] = {0, 0}, prevSame = 0;
		u32 pk[NODES];
		for (int z = 0; z < 16; z++)
		for (int y = 15; y >= 0; y--)
		for (int x = 0; x < 16; x++) {
			const int i = z * 256 + y * 16 + x;
			const int nL = x > 0 ? i - 1 : -1;
			const int nB = z > 0 ? i - 256 : -1;
			const int nU = y < 15 ? i + 16 : -1;
			const int nb[3] = { nL, nB, nU };
			g_cat = 6;
			// ---- whole node identical to one of the neighbours?
			{
				u32 cand[3]; int cm[3]; int nc = 0;
				for (int k = 0; k < 3; k++) {
					if (nb[k] < 0) continue;
					u32 v = pk[nb[k]];
					int j = 0;
					while (j < nc && cand[j] != v) j++;
					if (j == nc) { cand[nc] = v; cm[nc] = 0; nc++; }
					cm[j] |= 1 << k;
				}
				const int nD = (x > 0 && y < 15) ? i - 1 + 16 : -1;
				u32 me = ENC ? ((u32)content[i] << 16 | p1[i] << 8 | p2[i]) : 0;
				int j = 0;
				for (; j < nc; j++) {
					int agree = nD >= 0 && pk[nD] == cand[j];
					if (c.abit(N.same[j][cm[j]][prevSame][agree][cls[std::min<u32>(cand[j] >> 16, 4096)]],
							ENC ? me == cand[j] : 0))
						break;
				}
				if (j < nc) {
					pk[i] = cand[j];
					content[i] = cand[j] >> 16; p1[i] = (cand[j] >> 8) & 255; p2[i] = cand[j] & 255;
					prevSame = 1; phC = phP[0] = phP[1] = 1;
					continue;
				}
				prevSame = 0;
			}
			g_cat = 0;
			// ---- content
			if (npal <= 1) {
				content[i] = 0;
			} else {
				u16 cand[3]; int cm[3]; int nc = 0;
				for (int k = 0; k < 3; k++) {
					if (nb[k] < 0) continue;
					u16 v = content[nb[k]];
					int j = 0;
					while (j < nc && cand[j] != v) j++;
					if (j == nc) { cand[nc] = v; cm[nc] = 0; nc++; }
					cm[j] |= 1 << k;
				}
				const int nD = (x > 0 && y < 15) ? i - 1 + 16 : -1;
				u16 v = content[i];
				int j = 0;
				for (; j < nc; j++) {
					int agree = nD >= 0 && content[nD] == cand[j];
					if (c.abit(N.cHit[j][cm[j]][phC][agree], ENC ? v == cand[j] : 0))
						break;
				}
				if (j < nc) {
					v = cand[j]; phC = 1;
				} else {
					phC = 0;
					if (cbits <= 8) {
						int ctx = nc > 0 ? std::min<int>(cand[0], 15) : 16;
						v = (u16)c.template tree<true>(N.cEsc[ctx], v, cbits);
					} else {
						v = (u16)c.raw(v, cbits);
					}
				}
				content[i] = v;
			}
			// ---- params
			const u16 ci = content[i];
			const int k_cls = cls[std::min<int>(ci, 4096)];
			for (int f = 0; f < 2; f++) {
				g_cat = 1 + f;
				u8 *vals = f ? p2 : p1;
				u8 cand[4]; int cm[4], sm[4]; int nc = 0;
				for (int k = 0; k < 3; k++) {
					if (nb[k] < 0) continue;
					u8 v = vals[nb[k]];
					int j = 0;
					while (j < nc && cand[j] != v) j++;
					if (j == nc) { cand[nc] = v; cm[nc] = 0; sm[nc] = 0; nc++; }
					cm[j] |= 1 << k;
					if (content[nb[k]] == ci) sm[j] |= 1 << k;
				}
				if (f == 0) {
					u8 v = lightPredict(nL, nB, nU, p1, i);
					int j = 0;
					while (j < nc && cand[j] != v) j++;
					if (j == nc) { cand[nc] = v; cm[nc] = 0; sm[nc] = 0; nc++; }
					cm[j] |= 8;
				}
				u8 v = vals[i];
				int j = 0;
				for (; j < nc; j++) {
					if (c.abit(N.pHit[f][k_cls][j][cm[j]][sm[j]][phP[f]], ENC ? v == cand[j] : 0))
						break;
				}
				if (j < nc) {
					v = cand[j]; phP[f] = 1;
				} else {
					phP[f] = 0;
					v = (u8)c.template tree<true>(N.pEsc[f][k_cls], v, 8);
				}
				vals[i] = v;
			}
			pk[i] = (u32)content[i] << 16 | p1[i] << 8 | p2[i];
		}
	}
};

// Put palette in sorted-name order (lets us front-code names). Returns remapped block.
inline void canonicalize(Block &b)
{
	std::vector<u16> order(b.names.size());
	std::iota(order.begin(), order.end(), 0);
	std::sort(order.begin(), order.end(), [&](u16 a, u16 c) { return b.names[a] < b.names[c]; });
	std::vector<u16> remap(b.names.size());
	std::vector<std::string> nn(b.names.size());
	for (size_t i = 0; i < order.size(); i++) { remap[order[i]] = i; nn[i] = b.names[order[i]]; }
	b.names.swap(nn);
	for (int i = 0; i < NODES; i++)
		if (b.content[i] < remap.size()) b.content[i] = remap[b.content[i]];
}

static const char *kBuiltin[4] = { nullptr, "air", "ignore", "unknown" };
static const std::string kEmptyTail("\x00\x00\x00\x00\x0a\x00\x00", 7);

template<bool ENC>
static void codeHeaderAndNames(Coder<ENC> &c, Block &b, bool &mono, bool &hasTail)
{
	const StaticModel &S = *c.S;
	g_cat = 3;
	b.flags = (u8)c.template tree<false>(S.flags, b.flags, 8);
	if (c.sbit(S.lightFull, b.lighting == 0xffff)) b.lighting = 0xffff;
	else b.lighting = (u16)c.raw(b.lighting, 16);
	b.timestamp = (u32)(c.gamma(S.tsBits, (u64)b.timestamp + 1) - 1);
	mono = c.sbit(S.mono, mono);
	hasTail = c.sbit(S.tailFlag, hasTail);

	g_cat = 4;
	size_t np = (size_t)c.gamma(S.palBits, b.names.size());
	if (!ENC) b.names.resize(np);
	const std::string *prev = nullptr;
	static const std::string empty;
	for (size_t k = 0; k < np; k++) {
		std::string &nm = b.names[k];
		int bi = 0;
		if (ENC) for (int t = 1; t < 4; t++) if (nm == kBuiltin[t]) bi = t;
		bi = c.template tree<false>(S.builtin, bi, 2);
		if (bi) { nm = kBuiltin[bi]; continue; }
		const std::string &pv = prev ? *prev : empty;
		size_t pre = 0;
		if (ENC) while (pre < pv.size() && pre < nm.size() && pv[pre] == nm[pre]) pre++;
		pre = c.gamma(S.prefBits, pre + 1) - 1;
		size_t suf = c.gamma(S.sufBits, (ENC ? nm.size() - pre : 0) + 1) - 1;
		if (!ENC) nm.assign(pv, 0, pre);
		u8 ctx = pre ? (u8)nm[pre - 1] : 0;
		for (size_t q = 0; q < suf; q++) {
			u8 ch = ENC ? (u8)nm[pre + q] : 0;
			ch = (u8)c.template tree<false>(S.chars[ctx], ch, 8);
			if (!ENC) nm.push_back((char)ch);
			ctx = ch;
		}
		prev = &nm;
	}
}

inline std::string encode(const Block &in, const Priors &P)
{
	Block b = in;
	canonicalize(b);
	bool mono = true;
	for (int i = 1; i < NODES && mono; i++)
		mono = b.content[i] == b.content[0] && b.p1[i] == b.p1[0] && b.p2[i] == b.p2[0];
	bool hasTail = b.tail != kEmptyTail;

	std::string out;
	Encoder enc(out);
	Coder<true> c; c.e = &enc; c.S = &P.s;
	codeHeaderAndNames(c, b, mono, hasTail);
	int cb = bitsFor(b.names.size());
	if (mono) {
		g_cat = 5;
		if (cb <= 6) c.template tree<false>(P.s.monoContent, b.content[0], cb);
		else c.raw(b.content[0], cb);
		c.template tree<false>(P.s.monoP1, b.p1[0], 8);
		c.template tree<false>(P.s.monoP2, b.p2[0], 8);
	} else {
		std::unique_ptr<NodeModel> N(new NodeModel(P.n));
		BlockCoder<true> bc; bc.c = c; bc.c.N = N.get();
		for (size_t k = 0; k <= 4096; k++)
			bc.cls[k] = k >= b.names.size() ? PCLS - 1 :
				b.names[k] == "air" ? 0 : b.names[k] == "ignore" ? 1 : std::min<int>(2 + k, PCLS - 1);
		bc.codeNodes(b.content, b.p1, b.p2, b.names.size());
	}
	enc.flush();
	if (hasTail) {
		std::string z(ZSTD_compressBound(b.tail.size()), 0);
		z.resize(ZSTD_compress(z.data(), z.size(), b.tail.data(), b.tail.size(), 19));
		if (z.size() < b.tail.size()) { out.push_back(1); out += z; }
		else { out.push_back(0); out += b.tail; }
	}
	return out;
}

inline Block decode(const std::string &in, const Priors &P)
{
	Block b;
	const u8 *p = (const u8 *)in.data(), *end = p + in.size();
	Decoder dec(p, end);
	Coder<false> c; c.d = &dec; c.S = &P.s;
	bool mono = false, hasTail = false;
	codeHeaderAndNames(c, b, mono, hasTail);
	int cb = bitsFor(b.names.size());
	if (mono) {
		u16 ct = cb <= 6 ? c.template tree<false>(P.s.monoContent, 0, cb) : c.raw(0, cb);
		u8 a = c.template tree<false>(P.s.monoP1, 0, 8);
		u8 d = c.template tree<false>(P.s.monoP2, 0, 8);
		for (int i = 0; i < NODES; i++) { b.content[i] = ct; b.p1[i] = a; b.p2[i] = d; }
	} else {
		std::unique_ptr<NodeModel> N(new NodeModel(P.n));
		BlockCoder<false> bc; bc.c = c; bc.c.N = N.get();
		for (size_t k = 0; k <= 4096; k++)
			bc.cls[k] = k >= b.names.size() ? PCLS - 1 :
				b.names[k] == "air" ? 0 : b.names[k] == "ignore" ? 1 : std::min<int>(2 + k, PCLS - 1);
		bc.codeNodes(b.content, b.p1, b.p2, b.names.size());
	}
	if (hasTail) {
		const u8 *t = p + dec.consumed();
		if (t >= end) throw std::runtime_error("truncated tail");
		u8 method = *t++;
		if (method == 0) {
			b.tail.assign((const char *)t, end - t);
		} else {
			unsigned long long sz = ZSTD_getFrameContentSize(t, end - t);
			if (sz == ZSTD_CONTENTSIZE_ERROR || sz == ZSTD_CONTENTSIZE_UNKNOWN || sz > (1u << 26))
				throw std::runtime_error("bad tail");
			b.tail.resize(sz);
			ZSTD_decompress(b.tail.data(), sz, t, end - t);
		}
	} else {
		b.tail = kEmptyTail;
	}
	return b;
}

// Turn training statistics into priors. conf = initial confidence for adaptive slots.
inline void buildPriors(const Stats &st, Priors &P, int conf)
{
	Slot *s = (Slot *)&P.s;
	for (size_t i = 0; i < st.s0.size(); i++) {
		double n0 = st.s0[i], n1 = st.s1[i];
		double pr = (n1 + 0.4) / (n0 + n1 + 0.8);
		s[i].p = (u16)std::clamp<int>((int)(pr * 65536), 32, 65535 - 32);
		s[i].n = 0;
	}
	Slot *n = (Slot *)&P.n;
	for (size_t i = 0; i < st.n0.size(); i++) {
		double n0 = st.n0[i], n1 = st.n1[i];
		double pr = (n1 + 0.4) / (n0 + n1 + 0.8);
		n[i].p = (u16)std::clamp<int>((int)(pr * 65536), 32, 65535 - 32);
		n[i].n = (u8)std::min<double>(conf, (n0 + n1) / 4);
	}
}

} // namespace mbc
