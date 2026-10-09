// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "mapblock_codec.h"
#include "exceptions.h"

#include <zstd.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <numeric>
#include <vector>
#ifdef MAPBLOCK_CODEC_TRAINING
#include <cmath>
#include <sstream>
#endif

/*
	Overview (see also doc/world_format.md):

	Everything except the node metadata/static object/node timer "tail" is
	coded with one binary arithmetic coder. Header fields and names use static
	probabilities. Node data uses adaptive models that start from trained
	priors for every block.

	Nodes are visited in z (ascending), y (descending), x (ascending) order so
	that the neighbours left (x-1), behind (z-1) and above (y+1) are known.
	For every row of 16 nodes along x we first ask whether it is identical to
	the row behind or above it. Otherwise, for every node we ask whether it is identical to one of the distinct
	neighbour values; only on a miss content, param1 and param2 are coded
	individually, again preferring neighbour values (and a light propagation
	estimate for param1) before falling back to a binary tree.

	IMPORTANT: Everything here is part of the on-disk format. Any change to the
	models, their context layout, the update rule or the priors changes the
	bitstream and needs a new serialization version.
*/

namespace mapblock_codec
{

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

namespace
{

constexpr int NODECOUNT = 4096;
// Upper bound on the palette size of a block (one name per node)
constexpr u32 MAX_NAMES = NODECOUNT;
constexpr u32 MAX_NAME_LEN = 65535;
// How many bytes the decoder may read past the end of its input before the
// data is considered corrupt (the coder looks ahead up to 4 bytes).
constexpr int MAX_OVERREAD = 16;

/*
	Uncompressed block (version >= 29, disk)
*/

struct RawBlock {
	u8 flags;
	u16 lighting;
	u32 timestamp;
	std::vector<std::string> names; // index == block-local content id
	u16 content[NODECOUNT];
	u8 p1[NODECOUNT];
	u8 p2[NODECOUNT];
	std::string tail; // node metadata, static objects, node timers
};

inline u16 rd16(const u8 *p) { return (p[0] << 8) | p[1]; }
inline u32 rd32(const u8 *p) { return ((u32)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
inline void wr8(std::string &s, u8 v) { s.push_back((char)v); }
inline void wr16(std::string &s, u16 v) { wr8(s, v >> 8); wr8(s, v & 0xff); }
inline void wr32(std::string &s, u32 v) { wr16(s, v >> 16); wr16(s, v & 0xffff); }

void parseRaw(std::string_view raw, RawBlock &b)
{
	const u8 *p = (const u8 *)raw.data(), *end = p + raw.size();
	auto need = [&](size_t n) {
		if ((size_t)(end - p) < n)
			throw SerializationError("mapblock_codec: truncated block");
	};
	need(7 + 3);
	b.flags = p[0];
	b.lighting = rd16(p + 1);
	b.timestamp = rd32(p + 3);
	p += 7;
	if (*p++ != 0)
		throw SerializationError("mapblock_codec: unsupported NameIdMapping version");
	u16 count = rd16(p);
	p += 2;
	b.names.assign(count, std::string());
	std::vector<bool> seen(count);
	for (u16 i = 0; i < count; i++) {
		need(4);
		u16 id = rd16(p), len = rd16(p + 2);
		p += 4;
		need(len);
		if (id >= count || seen[id])
			throw SerializationError("mapblock_codec: non-contiguous NameIdMapping");
		seen[id] = true;
		b.names[id].assign((const char *)p, len);
		p += len;
	}
	need(2 + NODECOUNT * 4);
	if (p[0] != 2 || p[1] != 2)
		throw SerializationError("mapblock_codec: unsupported content/params width");
	p += 2;
	for (int i = 0; i < NODECOUNT; i++) {
		b.content[i] = rd16(p + 2 * i);
		if (b.content[i] >= count)
			throw SerializationError("mapblock_codec: content id without name");
	}
	p += 2 * NODECOUNT;
	memcpy(b.p1, p, NODECOUNT);
	p += NODECOUNT;
	memcpy(b.p2, p, NODECOUNT);
	p += NODECOUNT;
	b.tail.assign((const char *)p, end - p);
}

void writeRaw(const RawBlock &b, std::ostream &os)
{
	// Written in pieces: building one string first would cost an extra copy
	u8 buf[2 * NODECOUNT];
	u8 *p = buf;
	auto put16 = [&](u16 v) { p[0] = v >> 8; p[1] = v & 0xff; p += 2; };
	*p++ = b.flags;
	put16(b.lighting);
	put16(b.timestamp >> 16);
	put16(b.timestamp & 0xffff);
	*p++ = 0;
	put16(b.names.size());
	os.write(reinterpret_cast<char *>(buf), p - buf);
	for (size_t i = 0; i < b.names.size(); i++) {
		p = buf;
		put16(i);
		put16(b.names[i].size());
		os.write(reinterpret_cast<char *>(buf), p - buf);
		os.write(b.names[i].data(), b.names[i].size());
	}
	p = buf;
	*p++ = 2;
	*p++ = 2;
	os.write(reinterpret_cast<char *>(buf), p - buf);
	p = buf;
	for (int i = 0; i < NODECOUNT; i++)
		put16(b.content[i]);
	os.write(reinterpret_cast<char *>(buf), p - buf);
	os.write(reinterpret_cast<const char *>(b.p1), NODECOUNT);
	os.write(reinterpret_cast<const char *>(b.p2), NODECOUNT);
	os.write(b.tail.data(), b.tail.size());
}

// Reorder the palette by name so that names can be front-coded.
void sortPalette(RawBlock &b)
{
	const size_t n = b.names.size();
	std::vector<u16> order(n);
	std::iota(order.begin(), order.end(), 0);
	std::sort(order.begin(), order.end(),
		[&](u16 a, u16 c) { return b.names[a] < b.names[c]; });
	std::vector<u16> remap(n);
	std::vector<std::string> names(n);
	for (size_t i = 0; i < n; i++) {
		remap[order[i]] = i;
		names[i] = std::move(b.names[order[i]]);
	}
	b.names.swap(names);
	for (int i = 0; i < NODECOUNT; i++)
		b.content[i] = remap[b.content[i]];
}

/*
	Binary arithmetic coder (carry-less, 32-bit range, 16-bit probabilities)
*/

struct Encoder {
	u32 x1 = 0, x2 = 0xffffffff;
	std::string &out;

	explicit Encoder(std::string &o) : out(o) {}

	// p1 = probability of a 1 bit, scaled to 16 bits
	inline void encode(int bit, u32 p1)
	{
		u32 xmid = x1 + (u32)(((u64)(x2 - x1) * p1) >> 16);
		if (bit)
			x2 = xmid;
		else
			x1 = xmid + 1;
		while (((x1 ^ x2) & 0xff000000) == 0) {
			out.push_back((char)(x2 >> 24));
			x1 <<= 8;
			x2 = (x2 << 8) | 255;
		}
	}

	// Shortest byte string such that it followed by *any* bytes decodes
	// into [x1, x2]. This lets other data follow the coded stream directly.
	static int flushLength(u32 x1, u32 x2, u32 &value)
	{
		for (int k = 1; k < 4; k++) {
			const int s = 32 - 8 * k;
			u64 b = x1 >> s;
			if ((b << s) < x1)
				b++;
			u64 lo = b << s, hi = lo | ((1ull << s) - 1);
			if (hi <= x2) {
				value = (u32)lo;
				return k;
			}
		}
		value = x1;
		return 4;
	}

	void flush()
	{
		u32 v;
		int k = flushLength(x1, x2, v);
		for (int i = 0; i < k; i++)
			out.push_back((char)(v >> (24 - 8 * i)));
	}
};

struct Decoder {
	u32 x1 = 0, x2 = 0xffffffff, x = 0;
	const u8 *start, *p, *end;

	Decoder(const u8 *b, const u8 *e) : start(b), p(b), end(e)
	{
		for (int i = 0; i < 4; i++)
			x = (x << 8) | next();
	}

	inline u8 next()
	{
		if (p < end)
			return *p++;
		if (p - end >= MAX_OVERREAD)
			throw SerializationError("mapblock_codec: truncated data");
		p++;
		return 0;
	}

	inline int decode(u32 p1)
	{
		u32 xmid = x1 + (u32)(((u64)(x2 - x1) * p1) >> 16);
		int bit = x <= xmid;
		if (bit)
			x2 = xmid;
		else
			x1 = xmid + 1;
		while (((x1 ^ x2) & 0xff000000) == 0) {
			x1 <<= 8;
			x2 = (x2 << 8) | 255;
			x = (x << 8) | next();
		}
		return bit;
	}

	// Number of bytes the encoder produced for everything decoded so far
	size_t consumed() const
	{
		u32 v;
		return (size_t)(p - start) - 4 + Encoder::flushLength(x1, x2, v);
	}
};

/*
	Models
*/

struct Slot {
	u16 p = 32768;  // probability of a 1 bit
	u8 n = 0;       // number of updates seen (caps the learning rate)
	u8 dirty = 0;   // modified since the model was last reset (not part of the format)
};

constexpr int RATE_LIMIT = 12;
// RATE[n] = 65536 / (n + 1.6)
constexpr u32 RATE[RATE_LIMIT + 1] = {
	40960, 25206, 18204, 14246, 11702, 9929, 8623, 7620, 6826, 6182, 5649, 5201, 4818
};

// Never adapted: header, names, mono blocks.
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

// Adaptive, reset to the priors for every block.
constexpr int PCLS = 8; // node classes: 0 air, 1 ignore, 2.. by palette index
struct NodeModel {
	Slot row[2][2][2][2][2];         // candidate (behind/above), prev row hit, behind == above, candidate uniform, same relation held for the neighbouring row
	Slot same[3][8][2][2][PCLS];     // rank, neighbour mask, prev hit, diagonal agrees, class
	Slot cHit[3][8][2][2];           // rank, neighbour mask, prev hit, diagonal agrees
	Slot cEsc[17][256];              // context: first candidate (capped) or none
	Slot pHit[2][PCLS][4][16][8][2]; // field, class, rank, neighbour/predictor mask, same-content mask, prev hit
	Slot pEsc[2][PCLS][256];
};

constexpr size_t STATIC_SLOTS = sizeof(StaticModel) / sizeof(Slot);
constexpr size_t NODE_SLOTS = sizeof(NodeModel) / sizeof(Slot);
static_assert(sizeof(Slot) == 4);

struct Priors {
	StaticModel s;
	NodeModel n;
};

struct PriorEntry {
	u32 index;
	u16 p;
};

#include "mapblock_codec_priors.h"

const Priors *buildDefaultPriors()
{
	auto *P = new Priors();
	Slot *s = reinterpret_cast<Slot *>(&P->s);
	for (const auto &e : STATIC_PRIORS)
		s[e.index].p = e.p;
	Slot *n = reinterpret_cast<Slot *>(&P->n);
	for (const auto &e : NODE_PRIORS)
		n[e.index].p = e.p;
	return P;
}

const Priors &defaultPriors()
{
	static const std::unique_ptr<const Priors> P(buildDefaultPriors());
	return *P;
}

#ifdef MAPBLOCK_CODEC_TRAINING
struct Stats {
	std::vector<u64> s0, s1, n0, n1;
	double cost[8] = {};
	Stats() : s0(STATIC_SLOTS), s1(STATIC_SLOTS), n0(NODE_SLOTS), n1(NODE_SLOTS) {}
};
Stats *g_stats = nullptr;
std::unique_ptr<Priors> g_priors_override;
u64 g_priors_generation = 0;
int g_cat = 0;
#define TRAINING_CATEGORY(c) (g_cat = (c))

const Priors &activePriors()
{
	return g_priors_override ? *g_priors_override : defaultPriors();
}
#else
#define TRAINING_CATEGORY(c) ((void)0)
constexpr u64 g_priors_generation = 0;

inline const Priors &activePriors()
{
	return defaultPriors();
}
#endif

template <bool ENC>
struct Coder {
	Encoder *e = nullptr;
	Decoder *d = nullptr;
	const StaticModel *S = nullptr;
	NodeModel *N = nullptr;
	std::vector<Slot *> *dirty = nullptr; // adaptive slots modified in this block

	inline int code(u32 p, int b)
	{
		if constexpr (ENC) {
			e->encode(b, p);
#ifdef MAPBLOCK_CODEC_TRAINING
			if (g_stats)
				g_stats->cost[g_cat] -= std::log2((b ? p : 65536 - p) / 65536.0) / 8;
#endif
			return b;
		} else {
			return d->decode(p);
		}
	}

	inline int sbit(const Slot &m, int b)
	{
		b = code(m.p, b);
#ifdef MAPBLOCK_CODEC_TRAINING
		if (ENC && g_stats)
			(b ? g_stats->s1 : g_stats->s0)[&m - reinterpret_cast<const Slot *>(S)]++;
#endif
		return b;
	}

	inline int abit(Slot &m, int b)
	{
		b = code(m.p, b);
#ifdef MAPBLOCK_CODEC_TRAINING
		if (ENC && g_stats)
			(b ? g_stats->n1 : g_stats->n0)[&m - reinterpret_cast<Slot *>(N)]++;
#endif
		if (!m.dirty) {
			m.dirty = 1;
			dirty->push_back(&m);
		}
		const int target = b ? 65535 : 0;
		const int np = m.p + (int)(((int64_t)(target - m.p) * (int64_t)RATE[m.n]) >> 16);
		m.p = (u16)std::clamp(np, 32, 65535 - 32);
		if (m.n < RATE_LIMIT)
			m.n++;
		return b;
	}

	inline u32 raw(u32 v, int nbits)
	{
		u32 r = 0;
		for (int i = nbits - 1; i >= 0; i--)
			r |= (u32)code(32768, (v >> i) & 1) << i;
		return r;
	}

	// Binary tree over nbits; m must have at least 1 << nbits entries
	template <bool ADAPT>
	inline u32 tree(const Slot *m, u32 v, int nbits)
	{
		u32 node = 1;
		for (int i = nbits - 1; i >= 0; i--) {
			int b = (v >> i) & 1;
			if constexpr (ADAPT)
				b = abit(const_cast<Slot &>(m[node]), b);
			else
				b = sbit(m[node], b);
			node = (node << 1) | b;
		}
		return node - (1u << nbits);
	}

	// Elias-gamma style integer >= 1: bit length via static tree, then raw
	// mantissa. Values with more than max_bits bits are rejected.
	inline u64 gamma(const Slot *m, u64 v, int max_bits)
	{
		int nb = 0;
		if constexpr (ENC) {
			while ((v >> nb) > 1)
				nb++;
		}
		nb = tree<false>(m, nb, 6);
		if (nb >= max_bits)
			throw SerializationError("mapblock_codec: integer out of range");
		u64 r = 1;
		for (int i = nb - 1; i >= 0; i--)
			r = (r << 1) | (u64)code(32768, (v >> i) & 1);
		return r;
	}
};

inline int bitsFor(u32 n)
{
	int b = 0;
	while ((1u << b) < n)
		b++;
	return b;
}

// Estimate of param1 (light) from causal neighbours
inline u8 lightPredict(int nL, int nB, int nU, const u8 *p1)
{
	int day = 0, night = 0;
	auto take = [&](int k) {
		if (k < 0)
			return;
		day = std::max(day, (p1[k] & 15) - 1);
		night = std::max(night, (p1[k] >> 4) - 1);
	};
	take(nL);
	take(nB);
	take(nU);
	if (nU >= 0 && (p1[nU] & 15) == 15)
		day = 15; // sunlight propagates downwards without loss
	return (u8)(std::max(day, 0) | (std::max(night, 0) << 4));
}

inline bool rowsEqual(const u32 *a, const u32 *b)
{
	return memcmp(a, b, 16 * sizeof(u32)) == 0;
}

inline int rowUniform(const u32 *r)
{
	for (int x = 1; x < 16; x++)
		if (r[x] != r[0])
			return 0;
	return 1;
}

template <bool ENC>
struct NodeCoder {
	Coder<ENC> c;
	u8 cls[MAX_NAMES];

	void setClasses(const std::vector<std::string> &names)
	{
		for (size_t k = 0; k < names.size(); k++) {
			cls[k] = names[k] == "air" ? 0 : names[k] == "ignore" ? 1 :
				(u8)std::min<size_t>(2 + k, PCLS - 1);
		}
	}

	void codeNodes(u16 *content, u8 *p1, u8 *p2, u32 npal)
	{
		NodeModel &N = *c.N;
		const int cbits = bitsFor(npal);
		int phC = 0, phP[2] = {0, 0}, prevSame = 0, prevRow = 0;
		u32 pk[NODECOUNT]; // packed content << 16 | param1 << 8 | param2

		for (int z = 0; z < 16; z++)
		for (int y = 15; y >= 0; y--) {
			// Whole row (16 nodes along x) identical to the row behind or above?
			TRAINING_CATEGORY(7);
			{
				const int r0 = z * 256 + y * 16;
				const int rB = z > 0 ? r0 - 256 : -1;
				const int rU = y < 15 ? r0 + 16 : -1;
				const bool same_rows = rB >= 0 && rU >= 0 && rowsEqual(pk + rB, pk + rU);
				int cand[2], nc = 0;
				if (rB >= 0)
					cand[nc++] = rB;
				if (rU >= 0 && !same_rows)
					cand[nc++] = rU;
				u32 cur[16];
				if constexpr (ENC) {
					for (int x = 0; x < 16; x++)
						cur[x] = (u32)content[r0 + x] << 16 | p1[r0 + x] << 8 | p2[r0 + x];
				}
				// Did the same relation hold one row over? (above vs. above-behind,
				// or behind vs. behind-above)
				const bool diag = rB >= 0 && rU >= 0;
				const int rel[2] = {
					diag && rowsEqual(pk + rU, pk + rU - 256),
					diag && rowsEqual(pk + rB, pk + rB + 16),
				};
				int j = 0;
				for (; j < nc; j++) {
					const u32 *cr = pk + cand[j];
					const int which = cand[j] == rB ? 0 : 1;
					Slot &s = N.row[which][prevRow][same_rows][rowUniform(cr)][rel[which]];
					if (c.abit(s, ENC ? rowsEqual(cur, cr) : 0))
						break;
				}
				if (j < nc) {
					const int src = cand[j];
					memcpy(pk + r0, pk + src, 16 * sizeof(u32));
					memcpy(content + r0, content + src, 16 * sizeof(u16));
					memcpy(p1 + r0, p1 + src, 16);
					memcpy(p2 + r0, p2 + src, 16);
					prevRow = prevSame = phC = phP[0] = phP[1] = 1;
					continue;
				}
				prevRow = 0;
			}

			for (int x = 0; x < 16; x++) {
				const int i = z * 256 + y * 16 + x;
				const int nL = x > 0 ? i - 1 : -1;
				const int nB = z > 0 ? i - 256 : -1;
				const int nU = y < 15 ? i + 16 : -1;
				const int nD = (x > 0 && y < 15) ? i - 1 + 16 : -1; // diagonal
				const int nb[3] = { nL, nB, nU };

				// Whole node identical to one of the neighbours?
				TRAINING_CATEGORY(0);
				{
					u32 cand[3];
					int cm[3], nc = 0;
					for (int k = 0; k < 3; k++) {
						if (nb[k] < 0)
							continue;
						const u32 v = pk[nb[k]];
						int j = 0;
						while (j < nc && cand[j] != v)
							j++;
						if (j == nc) {
							cand[nc] = v;
							cm[nc] = 0;
							nc++;
						}
						cm[j] |= 1 << k;
					}
					const u32 me = ENC ? ((u32)content[i] << 16 | p1[i] << 8 | p2[i]) : 0;
					int j = 0;
					for (; j < nc; j++) {
						const int agree = nD >= 0 && pk[nD] == cand[j];
						Slot &s = N.same[j][cm[j]][prevSame][agree][cls[cand[j] >> 16]];
						if (c.abit(s, ENC ? me == cand[j] : 0))
							break;
					}
					if (j < nc) {
						pk[i] = cand[j];
						content[i] = cand[j] >> 16;
						p1[i] = (cand[j] >> 8) & 255;
						p2[i] = cand[j] & 255;
						prevSame = phC = phP[0] = phP[1] = 1;
						continue;
					}
					prevSame = 0;
				}

				// Content
				TRAINING_CATEGORY(1);
				if (npal <= 1) {
					content[i] = 0;
				} else {
					u16 cand[3];
					int cm[3], nc = 0;
					for (int k = 0; k < 3; k++) {
						if (nb[k] < 0)
							continue;
						const u16 v = content[nb[k]];
						int j = 0;
						while (j < nc && cand[j] != v)
							j++;
						if (j == nc) {
							cand[nc] = v;
							cm[nc] = 0;
							nc++;
						}
						cm[j] |= 1 << k;
					}
					u16 v = content[i];
					int j = 0;
					for (; j < nc; j++) {
						const int agree = nD >= 0 && content[nD] == cand[j];
						if (c.abit(N.cHit[j][cm[j]][phC][agree], ENC ? v == cand[j] : 0))
							break;
					}
					if (j < nc) {
						v = cand[j];
						phC = 1;
					} else {
						phC = 0;
						if (cbits <= 8) {
							const int ctx = nc > 0 ? std::min<int>(cand[0], 15) : 16;
							v = (u16)c.template tree<true>(N.cEsc[ctx], v, cbits);
						} else {
							v = (u16)c.raw(v, cbits);
						}
						if (v >= npal)
							throw SerializationError("mapblock_codec: content id out of range");
					}
					content[i] = v;
				}

				// param1, param2
				const u16 ci = content[i];
				const int k_cls = cls[ci];
				for (int f = 0; f < 2; f++) {
					TRAINING_CATEGORY(2 + f);
					u8 *vals = f ? p2 : p1;
					u8 cand[4];
					int cm[4], sm[4], nc = 0;
					for (int k = 0; k < 3; k++) {
						if (nb[k] < 0)
							continue;
						const u8 v = vals[nb[k]];
						int j = 0;
						while (j < nc && cand[j] != v)
							j++;
						if (j == nc) {
							cand[nc] = v;
							cm[nc] = sm[nc] = 0;
							nc++;
						}
						cm[j] |= 1 << k;
						if (content[nb[k]] == ci)
							sm[j] |= 1 << k;
					}
					if (f == 0) {
						const u8 v = lightPredict(nL, nB, nU, p1);
						int j = 0;
						while (j < nc && cand[j] != v)
							j++;
						if (j == nc) {
							cand[nc] = v;
							cm[nc] = sm[nc] = 0;
							nc++;
						}
						cm[j] |= 8;
					}
					u8 v = vals[i];
					int j = 0;
					for (; j < nc; j++) {
						Slot &s = N.pHit[f][k_cls][j][cm[j]][sm[j]][phP[f]];
						if (c.abit(s, ENC ? v == cand[j] : 0))
							break;
					}
					if (j < nc) {
						v = cand[j];
						phP[f] = 1;
					} else {
						phP[f] = 0;
						v = (u8)c.template tree<true>(N.pEsc[f][k_cls], v, 8);
					}
					vals[i] = v;
				}
				pk[i] = (u32)content[i] << 16 | p1[i] << 8 | p2[i];
			}
		}
	}
};

const char *const BUILTIN_NAMES[4] = { nullptr, "air", "ignore", "unknown" };

// What MapBlock::serialize() writes when there is no metadata, no static
// objects and no node timers.
const std::string_view EMPTY_TAIL("\x00\x00\x00\x00\x0a\x00\x00", 7);

template <bool ENC>
void codeHeaderAndNames(Coder<ENC> &c, RawBlock &b, bool &mono, bool &has_tail)
{
	const StaticModel &S = *c.S;
	TRAINING_CATEGORY(4);
	b.flags = (u8)c.template tree<false>(S.flags, b.flags, 8);
	if (c.sbit(S.lightFull, b.lighting == 0xffff))
		b.lighting = 0xffff;
	else
		b.lighting = (u16)c.raw(b.lighting, 16);
	b.timestamp = (u32)(c.gamma(S.tsBits, (u64)b.timestamp + 1, 34) - 1);
	mono = c.sbit(S.mono, mono);
	has_tail = c.sbit(S.tailFlag, has_tail);

	TRAINING_CATEGORY(5);
	const u64 npal = c.gamma(S.palBits, b.names.size(), 14);
	if (npal > MAX_NAMES)
		throw SerializationError("mapblock_codec: too many names");
	if constexpr (!ENC)
		b.names.resize(npal);
	const std::string *prev = nullptr;
	static const std::string empty;
	for (size_t k = 0; k < npal; k++) {
		std::string &name = b.names[k];
		int bi = 0;
		if constexpr (ENC) {
			for (int t = 1; t < 4; t++)
				if (name == BUILTIN_NAMES[t])
					bi = t;
		}
		bi = c.template tree<false>(S.builtin, bi, 2);
		if (bi) {
			name = BUILTIN_NAMES[bi];
			continue;
		}
		// Shared prefix with the previous (non-builtin) name, then the suffix
		const std::string &pv = prev ? *prev : empty;
		size_t pre = 0;
		if constexpr (ENC) {
			while (pre < pv.size() && pre < name.size() && pv[pre] == name[pre])
				pre++;
		}
		pre = c.gamma(S.prefBits, pre + 1, 17) - 1;
		size_t suf = c.gamma(S.sufBits, (ENC ? name.size() - pre : 0) + 1, 17) - 1;
		if constexpr (!ENC) {
			if (pre > pv.size() || pre + suf > MAX_NAME_LEN)
				throw SerializationError("mapblock_codec: invalid name");
			name.assign(pv, 0, pre);
		}
		u8 ctx = pre ? (u8)name[pre - 1] : 0;
		for (size_t q = 0; q < suf; q++) {
			u8 ch = ENC ? (u8)name[pre + q] : 0;
			ch = (u8)c.template tree<false>(S.chars[ctx], ch, 8);
			if constexpr (!ENC)
				name.push_back((char)ch);
			ctx = ch;
		}
		prev = &name;
	}
}

void putVarint(std::string &s, u64 v)
{
	while (v >= 0x80) {
		s.push_back((char)(v | 0x80));
		v >>= 7;
	}
	s.push_back((char)v);
}

u64 getVarint(const u8 *&p, const u8 *end)
{
	u64 v = 0;
	for (int sh = 0; sh < 64; sh += 7) {
		if (p >= end)
			throw SerializationError("mapblock_codec: truncated varint");
		const u8 c = *p++;
		v |= (u64)(c & 0x7f) << sh;
		if (!(c & 0x80))
			return v;
	}
	throw SerializationError("mapblock_codec: invalid varint");
}

// Per-thread scratch model, reset to the priors for every block. Only the
// slots the previous block modified are restored, which is much cheaper than
// copying the whole ~100KB model.
struct ScratchModel {
	NodeModel n;
	std::vector<Slot *> dirty;
	const Priors *priors = nullptr;
	u64 generation = 0;
};

ScratchModel &scratchModel(const Priors &P)
{
	thread_local std::unique_ptr<ScratchModel> M(new ScratchModel());
	if (M->priors != &P || M->generation != g_priors_generation) {
		memcpy(static_cast<void *>(&M->n), &P.n, sizeof(NodeModel));
		M->priors = &P;
		M->generation = g_priors_generation;
	} else {
		Slot *base = reinterpret_cast<Slot *>(&M->n);
		const Slot *prior = reinterpret_cast<const Slot *>(&P.n);
		for (Slot *s : M->dirty)
			*s = prior[s - base];
	}
	M->dirty.clear();
	return *M;
}

constexpr u8 TAIL_RAW = 0;
constexpr u8 TAIL_ZSTD = 1;
constexpr size_t MAX_TAIL_SIZE = 64 << 20;

} // anonymous namespace

void compress(std::string_view raw, std::ostream &os)
{
	std::unique_ptr<RawBlock> b(new RawBlock);
	parseRaw(raw, *b);
	sortPalette(*b);

	bool mono = true;
	for (int i = 1; i < NODECOUNT && mono; i++) {
		mono = b->content[i] == b->content[0] && b->p1[i] == b->p1[0] &&
			b->p2[i] == b->p2[0];
	}
	bool has_tail = b->tail != EMPTY_TAIL;

	const Priors &P = activePriors();
	std::string out;
	out.reserve(256);
	Encoder enc(out);
	Coder<true> c;
	c.e = &enc;
	c.S = &P.s;
	codeHeaderAndNames(c, *b, mono, has_tail);

	const u32 npal = b->names.size();
	const int cbits = bitsFor(npal);
	if (mono) {
		TRAINING_CATEGORY(6);
		if (cbits <= 6)
			c.template tree<false>(P.s.monoContent, b->content[0], cbits);
		else
			c.raw(b->content[0], cbits);
		c.template tree<false>(P.s.monoP1, b->p1[0], 8);
		c.template tree<false>(P.s.monoP2, b->p2[0], 8);
	} else {
		ScratchModel &M = scratchModel(P);
		NodeCoder<true> nc;
		nc.c = c;
		nc.c.N = &M.n;
		nc.c.dirty = &M.dirty;
		nc.setClasses(b->names);
		nc.codeNodes(b->content, b->p1, b->p2, npal);
	}
	enc.flush();

	if (has_tail) {
		std::string z(ZSTD_compressBound(b->tail.size()), '\0');
		size_t zs = ZSTD_compress(z.data(), z.size(), b->tail.data(), b->tail.size(), 19);
		if (!ZSTD_isError(zs) && zs < b->tail.size()) {
			out.push_back(TAIL_ZSTD);
			putVarint(out, zs);
			out.append(z.data(), zs);
		} else {
			out.push_back(TAIL_RAW);
			putVarint(out, b->tail.size());
			out += b->tail;
		}
	}
	os.write(out.data(), out.size());
}

void decompress(std::istream &is, std::ostream &os)
{
	// Our input is self-delimiting, but we don't know its length up front.
	// Read everything and put back what we didn't use.
	const std::streampos start = is.tellg();
	std::string in;
	{
		char buf[4096];
		while (is.read(buf, sizeof(buf)), is.gcount() > 0)
			in.append(buf, is.gcount());
	}
	const u8 *begin = (const u8 *)in.data(), *end = begin + in.size();

	std::unique_ptr<RawBlock> b(new RawBlock);
	const Priors &P = activePriors();
	Decoder dec(begin, end);
	Coder<false> c;
	c.d = &dec;
	c.S = &P.s;
	bool mono = false, has_tail = false;
	codeHeaderAndNames(c, *b, mono, has_tail);

	const u32 npal = b->names.size();
	if (npal == 0)
		throw SerializationError("mapblock_codec: empty palette");
	const int cbits = bitsFor(npal);
	if (mono) {
		u16 ct = cbits <= 6 ? c.template tree<false>(P.s.monoContent, 0, cbits) :
			c.raw(0, cbits);
		if (ct >= npal)
			throw SerializationError("mapblock_codec: content id out of range");
		u8 a = c.template tree<false>(P.s.monoP1, 0, 8);
		u8 d = c.template tree<false>(P.s.monoP2, 0, 8);
		std::fill_n(b->content, NODECOUNT, ct);
		std::fill_n(b->p1, NODECOUNT, a);
		std::fill_n(b->p2, NODECOUNT, d);
	} else {
		ScratchModel &M = scratchModel(P);
		NodeCoder<false> nc;
		nc.c = c;
		nc.c.N = &M.n;
		nc.c.dirty = &M.dirty;
		nc.setClasses(b->names);
		nc.codeNodes(b->content, b->p1, b->p2, npal);
	}

	size_t used = dec.consumed();
	if (used > in.size())
		throw SerializationError("mapblock_codec: truncated data");

	if (has_tail) {
		const u8 *t = begin + used;
		if (t >= end)
			throw SerializationError("mapblock_codec: truncated tail");
		const u8 method = *t++;
		const u64 len = getVarint(t, end);
		if (len > (u64)(end - t))
			throw SerializationError("mapblock_codec: truncated tail");
		if (method == TAIL_RAW) {
			b->tail.assign((const char *)t, len);
		} else if (method == TAIL_ZSTD) {
			unsigned long long size = ZSTD_getFrameContentSize(t, len);
			if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN ||
					size > MAX_TAIL_SIZE)
				throw SerializationError("mapblock_codec: invalid tail");
			b->tail.resize(size);
			size_t r = ZSTD_decompress(b->tail.data(), size, t, len);
			if (ZSTD_isError(r) || r != size)
				throw SerializationError("mapblock_codec: invalid tail");
		} else {
			throw SerializationError("mapblock_codec: unknown tail encoding");
		}
		used = (t + len) - begin;
	} else {
		b->tail = EMPTY_TAIL;
	}

	writeRaw(*b, os);

	// Give back unused input
	is.clear();
	if (start != std::streampos(-1))
		is.seekg(start + std::streamoff(used));
}

#ifdef MAPBLOCK_CODEC_TRAINING
namespace training
{

namespace {
	std::unique_ptr<Stats> g_stats_storage;
}

void useFlatPriors()
{
	g_priors_override = std::make_unique<Priors>();
	g_priors_generation++;
}

void beginStats()
{
	g_stats_storage = std::make_unique<Stats>();
	g_stats = g_stats_storage.get();
}

void usePriorsFromStats()
{
	auto P = std::make_unique<Priors>();
	auto build = [](Slot *slots, const std::vector<u64> &c0, const std::vector<u64> &c1) {
		for (size_t i = 0; i < c0.size(); i++) {
			double pr = (c1[i] + 0.4) / (c0[i] + c1[i] + 0.8);
			slots[i].p = (u16)std::clamp<int>((int)(pr * 65536), 32, 65535 - 32);
			slots[i].n = 0;
		}
	};
	build(reinterpret_cast<Slot *>(&P->s), g_stats->s0, g_stats->s1);
	build(reinterpret_cast<Slot *>(&P->n), g_stats->n0, g_stats->n1);
	g_stats = nullptr;
	g_priors_override = std::move(P);
	g_priors_generation++;
}

std::string priorsSource()
{
	const Priors &P = activePriors();
	std::ostringstream os;
	auto dump = [&](const char *name, const Slot *slots, size_t count) {
		os << "const PriorEntry " << name << "[] = {\n";
		int col = 0;
		size_t written = 0;
		for (size_t i = 0; i < count; i++) {
			if (slots[i].p == 32768)
				continue;
			os << (col == 0 ? "\t" : " ") << "{" << i << "," << slots[i].p << "},";
			written++;
			if (++col == 8) {
				os << "\n";
				col = 0;
			}
		}
		if (written == 0)
			os << "\t{0,32768},";
		os << (col ? "\n" : "") << "};\n";
	};
	os << "// Luanti\n"
		"// SPDX-License-Identifier: LGPL-2.1-or-later\n\n"
		"// Generated by util/mapblock_codec/train.cpp - do not edit.\n"
		"// Initial probabilities of the mapblock codec models, as {slot index, P(1) * 65536}.\n"
		"// These are part of the on-disk format (serialization version 30).\n"
		"// Included inside mapblock_codec.cpp.\n\n"
		"#pragma once\n\n";
	os << "static_assert(sizeof(StaticModel) / sizeof(Slot) == " << STATIC_SLOTS << ");\n";
	os << "static_assert(sizeof(NodeModel) / sizeof(Slot) == " << NODE_SLOTS << ");\n\n";
	dump("STATIC_PRIORS", reinterpret_cast<const Slot *>(&P.s), STATIC_SLOTS);
	os << "\n";
	dump("NODE_PRIORS", reinterpret_cast<const Slot *>(&P.n), NODE_SLOTS);
	return os.str();
}

const double *costBytes()
{
	static double c[8];
	if (g_stats)
		memcpy(c, g_stats->cost, sizeof(c));
	return c;
}

} // namespace training
#endif

} // namespace mapblock_codec
