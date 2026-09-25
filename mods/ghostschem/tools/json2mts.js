// Convert nodecore-optics-schematic JSON into a Luanti .mts schematic.
//
// One file, two uses:
//
//   Command line (Node 18+):
//     node json2mts.js staff-builder.schematic.json [staff-builder.mts] [--keep-existing]
//
//   In a web page, e.g. an "Export .mts" button in the builder:
//     <script src="json2mts.js"><\/script>
//
//   (That closing tag is written <\/script> on purpose: this file must never
//   contain the literal sequence, or inlining it into a page's own <script>
//   block ends the block right there, comment or not.)
//     <button onclick="GhostschemMts.download(schematicJson, 'staff-builder.mts')">
//       Export .mts</button>
//
// Both use CompressionStream, so the command line runs exactly the code a
// browser does. No dependencies.
//
// The byte layout comes from the engine's own writer, not from third-party
// docs - src/mapgen/mg_schematic.cpp, Schematic::serializeToMts():
//
//   u32  0x4d54534d 'MTSM'        u16  version (4)
//   s16  size.x, size.y, size.z
//   u8   slice probability  x size.y          (0x7F = always)
//   u16  name count, then per name: u16 length + bytes
//   zlib( u16 content id x N, u8 param1 x N, u8 param2 x N )
//
// All integers big-endian (src/util/serialize.h). The node payload is
// MapNode::serializeBulk() at format 28, which src/serialization.cpp compress()
// sends through zlib *with* its header (deflateInit2 with DEFAULT_WBITS) -
// what CompressionStream calls "deflate". Nodes are ordered X-fastest, then Y,
// then Z. param1 is a probability 0..127, plus 0x80 for per-node force_place.
//
// .mts has no field for node inventories, so item `stack`s cannot be carried.
// They are listed in the report rather than dropped silently.

(function (root) {
	"use strict";

	const FORMAT = "nodecore-optics-schematic";
	const MAX_VERSION = 1;

	const MTS_SIGNATURE = 0x4d54534d;
	const MTS_VERSION = 4;
	const PROB_ALWAYS = 0x7f;
	const PROB_NEVER = 0x00;

	const DEFAULT_MAX_VOLUME = 512 * 1024; // same default as the mod
	const S16_MAX = 32767;
	const U16_MAX = 65535;

	function isInteger(v) {
		return typeof v === "number" && Number.isInteger(v);
	}

	// Validate and flatten into a dense X-fastest array, following exactly the
	// same rules as the in-game importer (formats.lua, gs.import_nodecore), so a
	// file converted here and a file loaded in-game describe the same thing.
	function parse(doc, options) {
		const opts = options || {};
		const maxVolume = opts.maxVolume || DEFAULT_MAX_VOLUME;

		if (typeof doc === "string") {
			doc = JSON.parse(doc);
		}
		if (!doc || typeof doc !== "object" || Array.isArray(doc)) {
			throw new Error("not a JSON object");
		}
		if (doc.format !== FORMAT) {
			throw new Error(`unexpected format ${doc.format} (expected ${FORMAT})`);
		}
		const version = Number(doc.version);
		if (!Number.isFinite(version)) {
			throw new Error("missing version");
		}
		if (version > MAX_VERSION) {
			throw new Error(`version ${doc.version} is newer than this converter ` +
				`understands (up to ${MAX_VERSION})`);
		}
		if (!Array.isArray(doc.nodes) || doc.nodes.length === 0) {
			throw new Error("schematic contains no nodes");
		}

		let min = null;
		let max = null;
		doc.nodes.forEach((entry, i) => {
			const n = i + 1;
			if (!entry || typeof entry !== "object") {
				throw new Error(`node ${n} is not an object`);
			}
			const p = entry.pos;
			if (!Array.isArray(p) || !isInteger(p[0]) || !isInteger(p[1]) ||
					!isInteger(p[2])) {
				throw new Error(`node ${n} has a malformed pos (want three integers)`);
			}
			if (typeof entry.name !== "string" || entry.name === "") {
				throw new Error(`node ${n} has no name`);
			}
			if (min === null) {
				min = { x: p[0], y: p[1], z: p[2] };
				max = { x: p[0], y: p[1], z: p[2] };
			} else {
				min.x = Math.min(min.x, p[0]); max.x = Math.max(max.x, p[0]);
				min.y = Math.min(min.y, p[1]); max.y = Math.max(max.y, p[1]);
				min.z = Math.min(min.z, p[2]); max.z = Math.max(max.z, p[2]);
			}
		});

		const size = {
			x: max.x - min.x + 1,
			y: max.y - min.y + 1,
			z: max.z - min.z + 1,
		};
		if (size.x > S16_MAX || size.y > S16_MAX || size.z > S16_MAX) {
			throw new Error(`bounding box ${size.x}x${size.y}x${size.z} does not ` +
				`fit an .mts (max ${S16_MAX} per axis)`);
		}
		const volume = size.x * size.y * size.z;
		if (volume > maxVolume) {
			throw new Error(`bounding box is ${size.x}x${size.y}x${size.z} = ` +
				`${volume} nodes, over the ${maxVolume} limit`);
		}

		// null = nothing listed here.
		const cells = new Array(volume).fill(null);
		// Keyed by cell, so a later duplicate replaces an earlier entry's stack
		// along with the entry itself.
		const stackAt = new Map();
		let filled = 0;
		let duplicates = 0;

		for (const entry of doc.nodes) {
			const [px, py, pz] = entry.pos;
			const x = px - min.x;
			const y = py - min.y;
			const z = pz - min.z;
			const index = x + y * size.x + z * size.x * size.y;

			if (cells[index] === null) {
				filled++;
			} else {
				duplicates++;
			}

			cells[index] = {
				name: entry.name,
				param2: isInteger(entry.param2) ? entry.param2 & 0xff : 0,
			};

			stackAt.delete(index);
			if (entry.stack && typeof entry.stack === "object" &&
					typeof entry.stack.name === "string" && entry.stack.name !== "") {
				stackAt.set(index, {
					pos: [px, py, pz],
					node: entry.name,
					item: entry.stack.name,
					count: Math.max(1, Math.floor(Number(entry.stack.count) || 1)),
				});
			}
		}

		const report = {
			name: doc.name,
			description: doc.description,
			size,
			filled,
			air: volume - filled,
			duplicates,
			droppedStacks: [...stackAt.values()],
			originOffset: { x: -min.x, y: -min.y, z: -min.z },
			terrainSkipped: null,
		};

		// Terrain modes are the builder's own concept and are not documented
		// anywhere checkable; "off" is honoured, anything else is reported.
		if (doc.terrain && typeof doc.terrain === "object" &&
				doc.terrain.mode !== undefined && doc.terrain.mode !== "off") {
			report.terrainSkipped = String(doc.terrain.mode);
		}

		return { size, cells, report };
	}

	// Pure byte encoder. `cells` is a dense X-fastest array of
	// {name, param2} or null; `unlisted` decides what null becomes.
	async function encode(size, cells, options) {
		const opts = options || {};
		// "air":  unlisted cells are air and clear whatever is in the world,
		//         matching the in-game importer.
		// "keep": unlisted cells are written with probability 0, which
		//         blitToVManip skips (MTSCHEM_PROB_NEVER), so the world shows
		//         through the gaps.
		const unlisted = opts.unlisted || "air";
		if (unlisted !== "air" && unlisted !== "keep") {
			throw new Error(`unlisted must be "air" or "keep", not ${unlisted}`);
		}

		const count = size.x * size.y * size.z;
		if (cells.length !== count) {
			throw new Error(`expected ${count} cells, got ${cells.length}`);
		}

		// Name table. "air" always gets id 0, whether or not it is used, so
		// unlisted cells have somewhere to point.
		const names = ["air"];
		const ids = new Map([["air", 0]]);
		for (const cell of cells) {
			if (cell && !ids.has(cell.name)) {
				ids.set(cell.name, names.length);
				names.push(cell.name);
			}
		}
		if (names.length > U16_MAX) {
			throw new Error(`${names.length} distinct node names; .mts allows ${U16_MAX}`);
		}

		const utf8 = new TextEncoder();
		const encodedNames = names.map((n) => {
			const bytes = utf8.encode(n);
			if (bytes.length > U16_MAX) {
				throw new Error(`node name too long: ${n.slice(0, 40)}...`);
			}
			return bytes;
		});

		// Node payload: content ids, then param1, then param2 (serializeBulk).
		const payload = new Uint8Array(count * 4);
		const view = new DataView(payload.buffer);
		const p1Base = count * 2;
		const p2Base = count * 3;
		for (let i = 0; i < count; i++) {
			const cell = cells[i];
			if (cell) {
				view.setUint16(i * 2, ids.get(cell.name), false);
				payload[p1Base + i] = PROB_ALWAYS;
				payload[p2Base + i] = cell.param2;
			} else {
				view.setUint16(i * 2, 0, false);
				payload[p1Base + i] = unlisted === "keep" ? PROB_NEVER : PROB_ALWAYS;
				payload[p2Base + i] = 0;
			}
		}
		const compressed = await zlibDeflate(payload);

		// Header.
		let headerLength = 4 + 2 + 6 + size.y + 2;
		for (const b of encodedNames) headerLength += 2 + b.length;

		const out = new Uint8Array(headerLength + compressed.length);
		const dv = new DataView(out.buffer);
		let o = 0;
		dv.setUint32(o, MTS_SIGNATURE, false); o += 4;
		dv.setUint16(o, MTS_VERSION, false); o += 2;
		dv.setInt16(o, size.x, false); o += 2;
		dv.setInt16(o, size.y, false); o += 2;
		dv.setInt16(o, size.z, false); o += 2;
		for (let y = 0; y < size.y; y++) out[o++] = PROB_ALWAYS;
		dv.setUint16(o, names.length, false); o += 2;
		for (const b of encodedNames) {
			dv.setUint16(o, b.length, false); o += 2;
			out.set(b, o); o += b.length;
		}
		out.set(compressed, o);

		return out;
	}

	// zlib-wrapped deflate, as the engine's compressZlib() writes and reads it.
	async function zlibDeflate(bytes) {
		if (typeof CompressionStream !== "function") {
			throw new Error("CompressionStream is not available " +
				"(needs a current browser or Node 18+)");
		}
		const stream = new Blob([bytes]).stream()
			.pipeThrough(new CompressionStream("deflate"));
		return new Uint8Array(await new Response(stream).arrayBuffer());
	}

	async function convert(doc, options) {
		const { size, cells, report } = parse(doc, options);
		const bytes = await encode(size, cells, options);
		report.unlisted = (options && options.unlisted) || "air";
		report.bytes = bytes.length;
		return { bytes, report };
	}

	function describe(report) {
		const lines = [];
		const s = report.size;
		lines.push(`${report.name ? `"${report.name}"` : FORMAT}: ` +
			`${s.x}x${s.y}x${s.z}, ${report.filled} nodes, ${report.air} ` +
			(report.unlisted === "keep" ? "unlisted cells left untouched" : "air"));
		if (report.duplicates > 0) {
			lines.push(`${report.duplicates} duplicate position(s); the last entry won`);
		}
		if (report.terrainSkipped) {
			lines.push(`terrain mode '${report.terrainSkipped}' skipped; ` +
				"only the listed nodes were converted");
		}
		if (report.droppedStacks.length > 0) {
			lines.push(`WARNING: .mts cannot store node inventories, so ` +
				`${report.droppedStacks.length} item stack(s) are NOT in this file:`);
			for (const st of report.droppedStacks) {
				lines.push(`  ${st.count} x ${st.item} in ${st.node} at ` +
					`(${st.pos.join(",")})`);
			}
			lines.push("Load the .json with /gs load to keep them.");
		}
		return lines.join("\n");
	}

	// Browser convenience: convert and hand the file to the user.
	async function download(doc, filename, options) {
		const { bytes, report } = await convert(doc, options);
		const blob = new Blob([bytes], { type: "application/octet-stream" });
		const url = URL.createObjectURL(blob);
		const a = document.createElement("a");
		a.href = url;
		a.download = filename || "schematic.mts";
		document.body.appendChild(a);
		a.click();
		a.remove();
		setTimeout(() => URL.revokeObjectURL(url), 0);
		return report;
	}

	const api = { parse, encode, convert, describe, download, FORMAT };

	if (typeof module === "object" && module.exports) {
		module.exports = api;
	}
	root.GhostschemMts = api;

	// Command line.
	if (typeof require === "function" && typeof module === "object" &&
			require.main === module) {
		const fs = require("fs");
		const path = require("path");

		const args = process.argv.slice(2);
		const flags = args.filter((a) => a.startsWith("--"));
		const files = args.filter((a) => !a.startsWith("--"));

		const unknown = flags.filter((f) => f !== "--keep-existing");
		if (files.length < 1 || files.length > 2 || unknown.length > 0) {
			console.error("usage: node json2mts.js <in.json> [out.mts] [--keep-existing]");
			console.error("");
			console.error("  --keep-existing  leave the world alone in cells the JSON");
			console.error("                   does not list, instead of clearing them to air");
			process.exit(2);
		}

		const input = files[0];
		const output = files[1] ||
			path.join(path.dirname(input),
				path.basename(input).replace(/(\.schematic)?\.json$/i, "") + ".mts");
		const unlisted = flags.includes("--keep-existing") ? "keep" : "air";

		convert(fs.readFileSync(input, "utf8"), { unlisted })
			.then(({ bytes, report }) => {
				fs.writeFileSync(output, bytes);
				console.log(describe(report));
				console.log(`wrote ${output} (${bytes.length} bytes)`);
			})
			.catch((err) => {
				console.error(`json2mts: ${err.message}`);
				process.exit(1);
			});
	}
})(typeof globalThis !== "undefined" ? globalThis : this);
