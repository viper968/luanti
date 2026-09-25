// Tests for tools/json2mts.js.
//
// The decoder here is deliberately independent of the encoder: it inflates with
// node:zlib rather than DecompressionStream, and reads fields at the offsets
// src/mapgen/mg_schematic.cpp deserializeFromMts() uses. The authoritative
// check - that the engine itself reads these files the same way the in-game
// importer does - lives in tests/ingame.lua (/gs selftest).
//
// Run: node tests/json2mts_spec.js

"use strict";

const fs = require("fs");
const path = require("path");
const zlib = require("zlib");
const { execFileSync } = require("child_process");

const tool = path.join(__dirname, "..", "tools", "json2mts.js");
const fixtures = path.join(__dirname, "fixtures");
const mts = require(tool);

let checks = 0;
let failed = 0;
function check(label, ok, detail) {
	checks++;
	if (!ok) failed++;
	console.log(`${label.padEnd(58)} ${ok ? "ok" : "FAIL"}` +
		(!ok && detail ? `  (${detail})` : ""));
}

function decode(bytes) {
	const b = Buffer.from(bytes);
	let o = 0;
	const out = {};
	out.signature = b.readUInt32BE(o); o += 4;
	out.version = b.readUInt16BE(o); o += 2;
	out.size = { x: b.readInt16BE(o), y: b.readInt16BE(o + 2), z: b.readInt16BE(o + 4) };
	o += 6;
	out.slices = [...b.subarray(o, o + out.size.y)]; o += out.size.y;
	const nameCount = b.readUInt16BE(o); o += 2;
	out.names = [];
	for (let i = 0; i < nameCount; i++) {
		const len = b.readUInt16BE(o); o += 2;
		out.names.push(b.subarray(o, o + len).toString("utf8")); o += len;
	}
	out.payload = zlib.inflateSync(b.subarray(o)); // throws unless zlib-wrapped
	const n = out.size.x * out.size.y * out.size.z;
	out.count = n;
	out.cell = (i) => ({
		name: out.names[out.payload.readUInt16BE(i * 2)],
		param1: out.payload[n * 2 + i],
		param2: out.payload[n * 3 + i],
	});
	return out;
}

const doc = (nodes, extra) =>
	Object.assign({ format: "nodecore-optics-schematic", version: 1, nodes }, extra);

async function main() {
	// 1. Header and layout, on the reference file.
	const staff = JSON.parse(fs.readFileSync(
		path.join(fixtures, "staff-builder.schematic.json"), "utf8"));
	const { bytes, report } = await mts.convert(staff);
	const d = decode(bytes);

	check("signature is 'MTSM'", d.signature === 0x4d54534d,
		d.signature.toString(16));
	check("version is 4", d.version === 4, d.version);
	check("size is 14x5x13",
		d.size.x === 14 && d.size.y === 5 && d.size.z === 13,
		JSON.stringify(d.size));
	check("every Y slice is 'always' (0x7F)",
		d.slices.length === 5 && d.slices.every((v) => v === 0x7f));
	check("payload is zlib-wrapped and 4 bytes per node",
		d.payload.length === d.count * 4, d.payload.length);
	check("air is name id 0", d.names[0] === "air", d.names[0]);
	check("name table has air plus the 7 node types", d.names.length === 8,
		d.names.join(","));

	// A node in the source lands where the X-fastest index says it does.
	// Source (5, 1, 0) with min (-3, -1, -6) -> local (8, 2, 6).
	const idx = 8 + 2 * 14 + 6 * 14 * 5;
	const form = d.cell(idx);
	check("(5,1,0) lands at local (8,2,6) as the hatchet form",
		form.name === "nc_woodwork:form", form.name);
	const prism = d.cell((4 + 3) + (1 + 1) * 14 + (1 + 6) * 14 * 5);
	check("param2 survives (prism at (4,1,1) has 14)",
		prism.name === "nc_optics:prism" && prism.param2 === 14,
		`${prism.name} p2=${prism.param2}`);

	let listed = 0, air = 0, badProb = 0;
	for (let i = 0; i < d.count; i++) {
		const c = d.cell(i);
		if (c.name === "air") air++; else listed++;
		if (c.param1 !== 0x7f) badProb++;
	}
	check("235 listed nodes and 675 air", listed === 235 && air === 675,
		`${listed} / ${air}`);
	check("default mode: every cell has probability 'always'", badProb === 0,
		`${badProb} cells`);
	check("report lists the 3 dropped stacks", report.droppedStacks.length === 3,
		report.droppedStacks.length);
	check("report records the builder origin offset",
		report.originOffset.x === 3 && report.originOffset.y === 1 &&
		report.originOffset.z === 6, JSON.stringify(report.originOffset));

	// 2. The committed fixtures still match what the tool produces. Compare the
	//    decoded content, not raw bytes: compressed output may legitimately
	//    differ between zlib builds.
	for (const [json, file, opts] of [
		["staff-builder.schematic.json", "staff-builder.mts", {}],
		["keep-existing.schematic.json", "keep-existing.air.mts", {}],
		["keep-existing.schematic.json", "keep-existing.keep.mts", { unlisted: "keep" }],
	]) {
		const fresh = decode((await mts.convert(
			fs.readFileSync(path.join(fixtures, json), "utf8"), opts)).bytes);
		const committed = decode(fs.readFileSync(path.join(fixtures, file)));
		const same = fresh.names.join("\n") === committed.names.join("\n") &&
			fresh.payload.equals(committed.payload) &&
			JSON.stringify(fresh.size) === JSON.stringify(committed.size);
		check(`fixture ${file} is current`, same,
			"regenerate with tools/json2mts.js");
	}

	// 3. --keep-existing writes probability 0 in the gaps only.
	const keep = decode((await mts.convert(doc([
		{ pos: [0, 0, 0], name: "a" },
		{ pos: [2, 0, 0], name: "b" },
	]), { unlisted: "keep" })).bytes);
	check("keep mode: listed cells are 'always'",
		keep.cell(0).param1 === 0x7f && keep.cell(2).param1 === 0x7f);
	check("keep mode: the gap is probability 0 (never placed)",
		keep.cell(1).name === "air" && keep.cell(1).param1 === 0);

	// 4. Duplicates: last entry wins, and so does its stack (or lack of one).
	const dup = await mts.convert(doc([
		{ pos: [0, 0, 0], name: "first", stack: { name: "x", count: 1 } },
		{ pos: [0, 0, 0], name: "second" },
	]));
	check("duplicate: last entry wins", decode(dup.bytes).cell(0).name === "second");
	check("duplicate: replaced entry's stack is not reported as dropped",
		dup.report.droppedStacks.length === 0, dup.report.droppedStacks.length);
	check("duplicate: counted", dup.report.duplicates === 1);

	// 5. Terrain is reported, never guessed.
	const flat = await mts.convert(doc([{ pos: [0, 0, 0], name: "a" }],
		{ terrain: { mode: "flat" } }));
	check("terrain mode other than off is reported",
		flat.report.terrainSkipped === "flat");

	// 6. Same rejections as the in-game importer.
	const rejects = [
		["not an object", "[1,2]"],
		["wrong format", { format: "other", version: 1, nodes: [] }],
		["future version", doc([{ pos: [0, 0, 0], name: "a" }], { version: 99 })],
		["no nodes", doc([])],
		["malformed pos", doc([{ pos: [0, 0], name: "a" }])],
		["non-integer pos", doc([{ pos: [0, 0.5, 0], name: "a" }])],
		["missing name", doc([{ pos: [0, 0, 0] }])],
		["oversized bounding box", doc([
			{ pos: [0, 0, 0], name: "a" }, { pos: [5000, 5000, 5000], name: "b" }])],
		["axis too long for s16", doc([
			{ pos: [0, 0, 0], name: "a" }, { pos: [40000, 0, 0], name: "b" }]),
			{ maxVolume: 1e9 }],
	];
	for (const [label, input, opts] of rejects) {
		let err = null;
		try { await mts.convert(input, opts); } catch (e) { err = e; }
		check(`reject: ${label}`, err !== null, "accepted bad input");
	}

	// 7. Web pages often inline scripts. The HTML parser ends a <script> block
	//    at the first "</script" it sees, even inside a JS comment, so the file
	//    must never contain one (nor "<!--", which changes script parsing).
	const source = fs.readFileSync(tool, "utf8");
	check("safe to inline: no literal </script in the file",
		!/<\/script/i.test(source));
	check("safe to inline: no <!-- in the file", !source.includes("<!--"));

	// 8. The command line.
	const tmp = fs.mkdtempSync(path.join(require("os").tmpdir(), "json2mts-"));
	const out = path.join(tmp, "out.mts");
	const stdout = execFileSync("node",
		[tool, path.join(fixtures, "staff-builder.schematic.json"), out],
		{ encoding: "utf8" });
	check("CLI writes the file", fs.existsSync(out));
	check("CLI warns about dropped stacks",
		stdout.includes("cannot store node inventories"));
	let code = 0;
	try {
		execFileSync("node", [tool], { stdio: "pipe" });
	} catch (e) {
		code = e.status;
	}
	check("CLI with no arguments exits 2 with usage", code === 2, code);
	fs.rmSync(tmp, { recursive: true, force: true });

	console.log();
	console.log(failed === 0
		? `all ${checks} json2mts checks passed`
		: `${failed} of ${checks} json2mts checks FAILED`);
	process.exit(failed === 0 ? 0 : 1);
}

main().catch((e) => {
	console.error(e);
	process.exit(1);
});
