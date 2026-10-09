// Shared helpers for the mapblock compression benchmark.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <stdexcept>
#include <chrono>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

static const int NODES = 4096;

// A version-29 disk mapblock (the bytes *inside* the zstd stream), split up.
struct Block {
	u8 flags;
	u16 lighting;
	u32 timestamp;
	std::vector<std::string> names; // index == block-local id
	u16 content[NODES];
	u8 p1[NODES];
	u8 p2[NODES];
	std::string tail; // node metadata, static objects, node timers
};

inline u16 rd16(const u8 *p) { return (p[0] << 8) | p[1]; }
inline u32 rd32(const u8 *p) { return ((u32)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
inline void wr8(std::string &s, u8 v) { s.push_back((char)v); }
inline void wr16(std::string &s, u16 v) { wr8(s, v >> 8); wr8(s, v & 0xff); }
inline void wr32(std::string &s, u32 v) { wr16(s, v >> 16); wr16(s, v & 0xffff); }

inline Block parseRaw(const std::string &raw)
{
	Block b;
	const u8 *p = (const u8 *)raw.data(), *end = p + raw.size();
	auto need = [&](size_t n) { if ((size_t)(end - p) < n) throw std::runtime_error("short"); };
	need(7 + 3);
	b.flags = p[0]; b.lighting = rd16(p + 1); b.timestamp = rd32(p + 3); p += 7;
	if (*p++ != 0) throw std::runtime_error("nimap ver");
	u16 cnt = rd16(p); p += 2;
	b.names.resize(cnt);
	for (u16 i = 0; i < cnt; i++) {
		need(4);
		u16 id = rd16(p), len = rd16(p + 2); p += 4;
		need(len);
		if (id >= cnt) throw std::runtime_error("nimap id");
		b.names[id].assign((const char *)p, len); p += len;
	}
	need(2 + NODES * 4);
	if (p[0] != 2 || p[1] != 2) throw std::runtime_error("widths");
	p += 2;
	for (int i = 0; i < NODES; i++) b.content[i] = rd16(p + 2 * i);
	p += 2 * NODES;
	memcpy(b.p1, p, NODES); p += NODES;
	memcpy(b.p2, p, NODES); p += NODES;
	b.tail.assign((const char *)p, end - p);
	return b;
}

inline std::string writeRaw(const Block &b)
{
	std::string s;
	s.reserve(16500);
	wr8(s, b.flags); wr16(s, b.lighting); wr32(s, b.timestamp);
	wr8(s, 0); wr16(s, b.names.size());
	for (size_t i = 0; i < b.names.size(); i++) {
		wr16(s, i); wr16(s, b.names[i].size()); s += b.names[i];
	}
	wr8(s, 2); wr8(s, 2);
	for (int i = 0; i < NODES; i++) wr16(s, b.content[i]);
	s.append((const char *)b.p1, NODES);
	s.append((const char *)b.p2, NODES);
	s += b.tail;
	return s;
}

struct Timer {
	std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now();
	double sec() const {
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
	}
};
