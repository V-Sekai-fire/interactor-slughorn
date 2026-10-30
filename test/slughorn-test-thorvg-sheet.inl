// Included by slughorn-test-thorvg.cpp after slughorn-test-thorvg-stamp.inl.
//
// Labelled contact sheets:
//
//   slughorn-test-thorvg --contact-sheet OUT_DIR <file.svg> [...]
//
// One row per key: ThorVG raster | slug DIRECT decode | SHIPPED decode (stamp + merged layers, the
// slug-runtime view) | ERROR of the decode (x4), worst ERROR first. Labels carry the diff-2
// decomposition (FLOOR / ERROR / RESIDUAL, mean |delta| / % pixels off by > 0.25, each before >
// after rung 1), the baked mesh's cost and triangle count, and the DETERMINISTIC precondition -
// see decompose(). 12 rows a sheet, 256 px cells, PNG via stb_image_write. Also writes
// content-classes-NN.png (one row per content class of the vector subset) and
// stamp-negative-control-01.png. No blur, masks or threshold changes.

#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "stb_image_write.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// ------------------------------------------------------------------------------------------------
// Contact sheets
// ------------------------------------------------------------------------------------------------
namespace sheet {

// 5 x 7 bitmap font: digits, upper case (lower case maps to it) and . : = | % - _ / ( ) + , <space>
static const char* glyph(char c) {
	static const std::pair<char, const char*> G[] = {
		{'0', "01110100011001110101110011000101110"}, {'1', "00100011000010000100001000010001110"},
		{'2', "01110100010000100010001000100011111"}, {'3', "11111000100010000010000011000101110"},
		{'4', "00010001100101010010111110001000010"}, {'5', "11111100001111000001000011000101110"},
		{'6', "00110010001000011110100011000101110"}, {'7', "11111000010001000100010000100001000"},
		{'8', "01110100011000101110100011000101110"}, {'9', "01110100011000101111000010001001100"},
		{'A', "01110100011000111111100011000110001"}, {'B', "11110100011000111110100011000111110"},
		{'C', "01110100011000010000100001000101110"}, {'D', "11100100101000110001100011001011100"},
		{'E', "11111100001000011110100001000011111"}, {'F', "11111100001000011110100001000010000"},
		{'G', "01110100011000010111100011000101111"}, {'H', "10001100011000111111100011000110001"},
		{'I', "01110001000010000100001000010001110"}, {'J', "00111000100001000010000101001001100"},
		{'K', "10001100101010011000101001001010001"}, {'L', "10000100001000010000100001000011111"},
		{'M', "10001110111010110101100011000110001"}, {'N', "10001100011100110101100111000110001"},
		{'O', "01110100011000110001100011000101110"}, {'P', "11110100011000111110100001000010000"},
		{'Q', "01110100011000110001101011001001101"}, {'R', "11110100011000111110101001001010001"},
		{'S', "01111100001000001110000010000111110"}, {'T', "11111001000010000100001000010000100"},
		{'U', "10001100011000110001100011000101110"}, {'V', "10001100011000110001100010101000100"},
		{'W', "10001100011000110101101011010101010"}, {'X', "10001100010101000100010101000110001"},
		{'Y', "10001100011000101010001000010000100"}, {'Z', "11111000010001000100010001000011111"},
		{'.', "00000000000000000000000000110001100"}, {':', "00000011000110000000011000110000000"},
		{'=', "00000000001111100000111110000000000"}, {'|', "00100001000010000100001000010000100"},
		{'%', "11000110010001000100010001001100011"}, {'-', "00000000000000011111000000000000000"},
		{'_', "00000000000000000000000000000011111"}, {'/', "00000000010001000100010001000000000"},
		{'(', "00010001000100001000010000010000010"}, {')', "01000001000001000010000100010001000"},
		{'+', "00000001000010011111001000010000000"}, {',', "00000000000000000000001100010001000"},
		{'>', "10000010000010000010001000100010000"}, {'<', "00001000100010001000001000001000001"},
	};

	if(c >= 'a' && c <= 'z') c = char(c - 32);

	for(const auto& [k, v] : G) if(k == c) return v;

	return nullptr;
}

struct Canvas {
	uint32_t w, h;
	std::vector<uint8_t> rgb;

	Canvas(uint32_t W, uint32_t Hh): w(W), h(Hh), rgb(size_t(W) * Hh * 3, 40) {}

	void put(int64_t x, int64_t y, uint8_t r, uint8_t g, uint8_t b) {
		if(x < 0 || y < 0 || x >= int64_t(w) || y >= int64_t(h)) return;

		uint8_t* p = &rgb[(size_t(y) * w + size_t(x)) * 3];

		p[0] = r; p[1] = g; p[2] = b;
	}

	void text(int64_t x, int64_t y, const std::string& s, int scale=2, uint8_t r=235, uint8_t g=235, uint8_t b=235) {
		for(char c : s) {
			if(const char* gph = glyph(c)) {
				for(int j = 0; j < 7; j++) for(int i = 0; i < 5; i++) if(gph[j * 5 + i] == '1') {
					for(int sy = 0; sy < scale; sy++) for(int sx = 0; sx < scale; sx++) put(x + i * scale + sx, y + j * scale + sy, r, g, b);
				}
			}

			x += 6 * scale;
		}
	}

	// Premultiplied RGBA float image over a checkerboard, scaled into a cell.
	void image(int64_t x0, int64_t y0, const std::vector<slug_t>& img, uint32_t iw, uint32_t ih, uint32_t cell, double gain=1.0) {
		const double s = double(cell) / double(std::max(iw, ih));

		for(uint32_t y = 0; y < cell; y++) {
			for(uint32_t x = 0; x < cell; x++) {
				const auto sx = uint32_t(double(x) / s), sy = uint32_t(double(y) / s);

				if(sx >= iw || sy >= ih) continue;

				const slug_t* p = &img[(size_t(sy) * iw + sx) * 4];
				const double bg = ((x / 8 + y / 8) % 2) ? 0.85 : 0.65;
				auto ch = [&](int k) { return uint8_t(std::lround(std::clamp((double(p[k]) * gain + bg * (1 - std::min(1.0, double(p[3]) * gain))), 0.0, 1.0) * 255)); };

				put(x0 + x, y0 + y, ch(0), ch(1), ch(2));
			}
		}
	}
};

struct Row {
	std::vector<std::string> text;          // first line white, the rest blue
	std::vector<std::vector<slug_t>> cols;  // premultiplied RGBA images, w x h
	std::vector<double> gains;              // per column (1 when missing)
	uint32_t w = 0, h = 0;
	double err = 0;
};

static std::vector<std::string> write(const std::string& dir, const std::string& stem, const std::vector<Row>& rows, const std::vector<std::string>& headers) {
	const uint32_t cell = 256, pad = 8, perSheet = 12, headH = 30;
	std::vector<std::string> paths;

	size_t lines = 1, columns = headers.size();

	for(const Row& row : rows) { lines = std::max(lines, row.text.size()); columns = std::max(columns, row.cols.size()); }

	const auto labelH = uint32_t(4 + 20 * lines);

	// Wide enough for the longest label line (12 px a character at scale 2).
	size_t longest = 0;

	for(const Row& row : rows) for(const auto& t : row.text) longest = std::max(longest, t.size());
	for(const auto& h : headers) longest = std::max(longest, h.size());

	for(size_t first = 0, n = 1; first < rows.size(); first += perSheet, n++) {
		const size_t count = std::min<size_t>(perSheet, rows.size() - first);
		Canvas cv(uint32_t(std::max<size_t>(pad + columns * (cell + pad), 2 * pad + 12 * longest)), uint32_t(headH + count * (labelH + cell + pad)));

		for(size_t c = 0; c < headers.size(); c++) cv.text(int64_t(pad + c * (cell + pad)), 8, headers[c], 2, 255, 220, 120);

		for(size_t r = 0; r < count; r++) {
			const Row& row = rows[first + r];
			const int64_t y = int64_t(headH + r * (labelH + cell + pad));

			for(size_t l = 0; l < row.text.size(); l++) {
				if(l == 0) cv.text(pad, y + 4, row.text[l], 2);
				else cv.text(pad, y + 4 + int64_t(20 * l), row.text[l], 2, 180, 200, 255);
			}

			for(size_t c = 0; c < row.cols.size(); c++) {
				if(!row.cols[c].empty()) cv.image(int64_t(pad + c * (cell + pad)), y + labelH, row.cols[c], row.w, row.h, cell, c < row.gains.size() ? row.gains[c] : 1.0);
			}
		}

		char name[64];

		std::snprintf(name, sizeof(name), "%s-%02zu.png", stem.c_str(), n);

		const std::string path = dir + "/" + name;

		stbi_write_png(path.c_str(), int(cv.w), int(cv.h), 3, cv.rgb.data(), int(cv.w * 3));
		paths.push_back(path);
	}

	return paths;
}

static std::vector<slug_t> absDiff(const std::vector<slug_t>& a, const std::vector<slug_t>& b) {
	std::vector<slug_t> d(a.size(), 0_cv);

	for(size_t p = 0; p + 3 < a.size() && p + 3 < b.size(); p += 4) {
		for(int k = 0; k < 3; k++) d[p + size_t(k)] = std::abs(a[p + size_t(k)] - b[p + size_t(k)]);

		d[p + 3] = std::max({d[p], d[p + 1], d[p + 2], std::abs(a[p + 3] - b[p + 3])});
		d[p] = std::max(d[p], std::abs(a[p + 3] - b[p + 3]));
	}

	return d;
}

static std::string fmt(double v, int prec=4) {
	char b[32];

	std::snprintf(b, sizeof(b), "%.*f", prec, v);

	return b;
}

}

// ------------------------------------------------------------------------------------------------
// Diff-2 decomposition (the residual ladder), per key against the ThorVG raster of the key's SVG
// (<use href + xlink:href> reduced to href, which ThorVG 1.0.3 would draw twice):
//
//   DIRECT    slug's CPU decode of the key's direct content: thorvg::loadString with <use> expanded
//             by ThorVG - no stamp layers, no merging, no bake - with curves held to 0.01 px
//             (cubic error bound, stroke and clip flattening), rectangular clips that contain a
//             layer not flattening it. Slug's best rendition of the content.
//   FLOOR     ThorVG vs DIRECT: what the two rasterizers disagree on for this content (AA model,
//             8-bit output, ThorVG's own defects). Also with thorvg-01's exact stroke widths.
//   SHIPPED   what slug.elf ships: stamp layers + merged layers (the slug-runtime decode), and the
//             slug-baked mesh (planar; the alpha-0.5 cutout for card keys).
//   ERROR     DIRECT vs SHIPPED - ours to fix (rung 1, the prototype tolerance, lives here).
//   RESIDUAL  ThorVG vs SHIPPED.
//
// DETERMINISTIC (a precondition, not a floor): the ThorVG raster, DIRECT and the shipped decode each
// reproduce bit-exactly from a second independent load.
// ------------------------------------------------------------------------------------------------
static slughorn::thorvg::LoadConfig directConfig() {
	auto cfg = quietConfig();

	cfg.tolerancePx = 0.01_cv;
	cfg.curveErrorBound = true;
	cfg.strokeTolerancePx = 0.01_cv;
	cfg.clipTolerancePx = 0.01_cv;
	cfg.skipContainingRectClips = true;

	return cfg;
}

struct DirectRun {
	std::vector<slug_t> img;
	size_t layers = 0, curves = 0;
	std::vector<std::string> warnings;
};

static DirectRun directRender(const std::string& svg, uint32_t W, uint32_t H) {
	DirectRun r;
	slughorn::Atlas atlas;
	slughorn::KeyIterator keys("d", true);
	auto cfg = directConfig();

	cfg.log = [&](int, std::string_view m) { r.warnings.emplace_back(m); };

	const auto comp = slughorn::thorvg::loadString(svg, atlas, keys, 96_cv, &cfg);

	atlas.build();

	for(const auto& [k, sh] : atlas.getShapes()) r.curves += sh.curves.size();

	r.layers = comp.layers.size();
	r.img = slughorn::render::renderComposite(atlas, comp, W, H, 0_cv, 0_cv, 1_cv, cfg.heightEm).data;

	return r;
}

// One key through slug.elf's pipeline: stamp-aware load, deep runs split, same-paint layers merged,
// atlas built, CPU decode (stamp layers included) at W x H. The knobs are the ladder's variants.
struct Variant {
	slughorn::stamp::ProtoTolerance proto = slughorn::stamp::ProtoTolerance::Device;
	bool merge = true;
	bool rectSkip = false;         // rectangular clips containing a layer do not flatten it
	slug_t mainTolerancePx = 0_cv; // > 0: the canvas's own cubics error-bounded at this tolerance
};

struct KeyRun {
	StampCase sc;
	slughorn::bake::MergeResult merged;
	std::vector<slug_t> decode;
	uint32_t W = 0, H = 0;
	size_t protoCurves = 0;
	size_t atlasCurves = 0;
	bool ok = false;
};

static void runKey(const std::string& svg, KeyRun& r, uint32_t W, const Variant& v={}) {
	slughorn::KeyIterator keys("k", true);

	r.sc.cfg.log = [](int, std::string_view) {};
	r.sc.cfg.skipContainingRectClips = v.rectSkip;

	if(v.mainTolerancePx > 0_cv) {
		r.sc.cfg.tolerancePx = v.mainTolerancePx;
		r.sc.cfg.curveErrorBound = true;
	}

	slughorn::Atlas staging;
	auto loaded = slughorn::stamp::loadString(svg, staging, keys, &r.sc.cfg, r.sc.set, "proto/", &r.sc.notes, v.proto);

	if(r.sc.cfg.width <= 0_cv) return;

	slughorn::stamp::splitDeep(r.sc.set, loaded, 1_cv, r.sc.cfg.heightEm, r.sc.cfg.width);

	std::vector<slughorn::bake::LayerSource> meta;

	for(const auto& li : r.sc.cfg.layers) meta.push_back({li.fillRule, li.stroke, static_cast<uint8_t>(li.spread)});

	slughorn::KeyIterator lk("l");

	r.merged = slughorn::bake::mergeLayers(staging, loaded, meta, r.sc.atlas, lk, v.merge);

	slughorn::stamp::registerProtos(r.sc.set, r.sc.atlas, "proto/");
	r.sc.atlas.build();

	for(const auto& p : r.sc.set.protos) r.protoCurves += p.curves.size();
	for(const auto& [k, sh] : r.sc.atlas.getShapes()) r.atlasCurves += sh.curves.size();

	r.W = W;
	r.H = static_cast<uint32_t>(std::max<long>(1, std::lround(W * r.sc.cfg.heightEm)));
	r.decode = slughorn::stamp::renderComposite(r.sc.atlas, r.merged.composite, r.sc.set, r.W, r.H, 0_cv, 0_cv, 1_cv, r.sc.cfg.heightEm).data;
	r.ok = true;
}

// The slug-baked mesh of the key, and its cost record when asked for.
static slughorn::bake::BakedMesh bakeShown(KeyRun& r, slughorn::bake::Cost* cost=nullptr) {
	slughorn::bake::BakeConfig bc;

	bc.width = r.sc.cfg.width;
	bc.height = r.sc.cfg.height;
	bc.vUp = true;

	auto planar = slughorn::bake::bakeMesh(r.sc.atlas, r.merged.composite, r.merged.layers, bc, &r.sc.set);

	if(cost) *cost = slughorn::bake::cost(r.sc.atlas, r.merged.composite, planar, r.merged.layersBefore, 1_cv, r.sc.cfg.heightEm, &r.sc.set, r.sc.cfg.width);

	return planar;
}

static std::string md(const Diff& d) {
	return sheet::fmt(d.mean) + "/" + sheet::fmt(d.bad * 100, 2) + "%";
}

static bool identical(const std::vector<slug_t>& a, const std::vector<slug_t>& b) {
	return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

// One measured case (a key, or a content-class sample).
struct Decomposition {
	std::string name;
	std::string mode;
	bool card = false, stamped = false, deterministic = true;
	size_t protoCurvesBefore = 0, protoCurvesAfter = 0, shippedCurves = 0, directCurves = 0, triangles = 0;
	Diff floor, floorExact;
	Diff errDecodeBefore, errDecodeAfter;
	Diff resDecodeBefore, resDecodeAfter;
	Diff errNoMerge, errRectSkip, errFineMain;
	size_t rectSkipCurves = 0, fineMainCurves = 0;
	std::vector<std::string> warnings;
	sheet::Row row;
};

static Decomposition decompose(const std::string& name, const std::string& svg, uint32_t W, bool diagnostics) {
	Decomposition dc;

	dc.name = name;

	KeyRun after;

	runKey(svg, after, W);

	if(!after.ok) { dc.mode = "FAILED"; return dc; }

	const uint32_t H = after.H;
	const std::string ref = slughorn::stamp::dedupeUseHref(svg);

	dc.stamped = !after.sc.set.layers.empty();

	// ThorVG (stock, and with thorvg-01's exact stroke widths) and DIRECT, each twice.
	const auto raster = thorvgRaster(ref, W, H);
	const auto rasterExact = thorvgRaster(ref, W, H, true);
	const DirectRun direct = directRender(ref, W, H);

	{
		KeyRun again;

		runKey(svg, again, W);

		dc.deterministic = identical(thorvgRaster(ref, W, H), raster) && identical(directRender(ref, W, H).img, direct.img) &&
			identical(again.decode, after.decode);
	}

	dc.warnings = direct.warnings;
	dc.directCurves = direct.curves;
	dc.floor = compare(direct.img, raster);
	dc.floorExact = compare(direct.img, rasterExact);

	size_t clear = 0;

	for(size_t p = 0; p < size_t(W) * H; p++) clear += direct.img[p * 4 + 3] < 0.5_cv;

	dc.card = double(clear) / (double(W) * H) > 0.05;

	// The baked mesh's cost: the mode and the triangle count the sheet reports for the key.
	slughorn::bake::Cost cost;
	const auto shownAfter = bakeShown(after, &cost);

	dc.mode = cost.mode;
	dc.triangles = shownAfter.trianglesAfter;
	dc.shippedCurves = after.atlasCurves;
	dc.protoCurvesAfter = after.protoCurves;
	dc.errDecodeAfter = compare(after.decode, direct.img);
	dc.resDecodeAfter = compare(after.decode, raster);

	// Before rung 1: prototypes at the legacy fixed tolerance (keys without prototypes: the same
	// pipeline either way).
	dc.protoCurvesBefore = after.protoCurves;
	dc.errDecodeBefore = dc.errDecodeAfter;
	dc.resDecodeBefore = dc.resDecodeAfter;

	if(dc.stamped) {
		KeyRun before;
		Variant v;

		v.proto = slughorn::stamp::ProtoTolerance::Legacy;
		runKey(svg, before, W, v);

		dc.protoCurvesBefore = before.protoCurves;
		dc.errDecodeBefore = compare(before.decode, direct.img);
		dc.resDecodeBefore = compare(before.decode, raster);
	}

	// The next rungs' candidates: what each would take out of ERROR (decode).
	if(diagnostics) {
		KeyRun a, b, c;
		Variant va, vb, vc;

		va.merge = false;
		vb.rectSkip = true;
		vc.mainTolerancePx = 0.05_cv;

		runKey(svg, a, W, va);
		runKey(svg, b, W, vb);
		runKey(svg, c, W, vc);

		dc.errNoMerge = compare(a.decode, direct.img);
		dc.errRectSkip = compare(b.decode, direct.img);
		dc.errFineMain = compare(c.decode, direct.img);
		dc.rectSkipCurves = b.atlasCurves;
		dc.fineMainCurves = c.atlasCurves;
	}

	sheet::Row& row = dc.row;

	row.text = {
		name + " " + dc.mode + (dc.card ? " CARD" : "") +
			(dc.stamped ? " STAMPED, PROTO CURVES " + std::to_string(dc.protoCurvesBefore) + " > " + std::to_string(dc.protoCurvesAfter) : "") +
			(dc.deterministic ? "   DETERMINISTIC PASS" : "   DETERMINISTIC FAIL"),
		"FLOOR    THORVG VS DIRECT " + md(dc.floor) + "   (THORVG-01 EXACT STROKES " + md(dc.floorExact) + ")",
		"ERROR    DECODE " + md(dc.errDecodeBefore) + " > " + md(dc.errDecodeAfter) +
			(dc.stamped ? "   (BEFORE > AFTER RUNG 1)" : "   (NO PROTOS)"),
		"RESIDUAL DECODE " + md(dc.resDecodeBefore) + " > " + md(dc.resDecodeAfter) +
			"   TRIS " + std::to_string(dc.triangles),
	};
	row.cols = {raster, direct.img, after.decode, sheet::absDiff(direct.img, after.decode)};
	row.gains = {1.0, 1.0, 1.0, 4.0};
	row.w = W;
	row.h = H;
	row.err = dc.errDecodeAfter.mean;

	return dc;
}

static void printDecomposition(const Decomposition& d) {
	auto c = [](const Diff& x) { return std::to_string(x.mean) + "," + std::to_string(x.bad); };

	std::cout << '"' << d.name << '"' << "," << d.mode << "," << d.card << "," << d.stamped << "," << d.deterministic << ","
		<< d.protoCurvesBefore << "," << d.protoCurvesAfter << "," << d.directCurves << "," << d.shippedCurves << "," << d.triangles << ","
		<< c(d.floor) << "," << c(d.floorExact) << ","
		<< c(d.errDecodeBefore) << "," << c(d.errDecodeAfter) << ","
		<< c(d.resDecodeBefore) << "," << c(d.resDecodeAfter) << ","
		<< c(d.errNoMerge) << "," << c(d.errRectSkip) << "," << d.rectSkipCurves << "," << c(d.errFineMain) << "," << d.fineMainCurves << ","
		<< d.floor.maxd << "," << d.errDecodeAfter.maxd << std::endl;
}

static const char* DECOMPOSITION_CSV_HEADER =
	"name,mode,card,stamped,deterministic,proto_curves_before,proto_curves_after,direct_curves,shipped_curves,triangles,"
	"floor_mean,floor_bad,floor_exact_mean,floor_exact_bad,"
	"err_decode_before_mean,err_decode_before_bad,err_decode_after_mean,err_decode_after_bad,"
	"res_decode_before_mean,res_decode_before_bad,res_decode_after_mean,res_decode_after_bad,"
	"err_nomerge_mean,err_nomerge_bad,err_rectskip_mean,err_rectskip_bad,rectskip_curves,err_finemain_mean,err_finemain_bad,finemain_curves,"
	"floor_maxd,err_decode_maxd";

// The vector subset, one class a sample (128 x 128, drawn at 256): what the floor of each content
// class is, and whether slug represents it (DIRECT) at all.
static const std::vector<std::pair<std::string, std::string>>& contentClasses() {
	static const std::string H = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="128" height="128" viewBox="0 0 128 128">)SVG";
	static const std::vector<std::pair<std::string, std::string>> C = {
		{"rects, pixel-aligned", H + R"SVG(<rect width="64" height="64" fill="#336699"/><rect x="8" y="8" width="32" height="24" fill="#cc3300"/><rect x="64" width="64" height="64" fill="#ffcc00" fill-opacity="0.3"/><rect x="72" y="8" width="48" height="48" fill="#0066ff" fill-opacity="0.5"/><rect x="80" y="16" width="32" height="32" fill="#ff3366" fill-opacity="0.25"/><rect x="88" y="24" width="16" height="16" fill="#33cc66" fill-opacity="0.7"/><rect y="64" width="128" height="64" fill="#f0e0b0"/></svg>)SVG"},
		{"rects, subpixel + rotated", H + R"SVG(<rect x="3.3" y="5.7" width="40.4" height="20.25" fill="#336699"/><rect x="50.5" y="10.1" width="30.3" height="50.6" fill="#cc3300" fill-opacity="0.6"/><rect x="64" y="64" width="40" height="30" transform="rotate(23 84 79)" fill="#208040"/><rect x="10" y="80" width="0.6" height="40" fill="#000"/><rect x="20" y="80" width="1.5" height="40" fill="#000"/></svg>)SVG"},
		{"ellipses + circles", H + R"SVG(<circle cx="32" cy="32" r="28" fill="#c03060"/><ellipse cx="90" cy="34" rx="30" ry="12" fill="#3060c0" fill-opacity="0.7"/><ellipse cx="64" cy="92" rx="40" ry="18" transform="rotate(-30 64 92)" fill="#30a060"/><circle cx="110" cy="110" r="2.5" fill="#000"/><circle cx="100" cy="116" r="0.8" fill="#000"/></svg>)SVG"},
		{"arcs (A)", H + R"SVG(<path d="M10 64 A54 30 0 0 1 118 64 A54 30 0 0 1 10 64Z" fill="#a04080"/><path d="M20 20 A20 20 0 1 0 60 20Z" fill="#4080a0"/><path d="M70 100 a25 15 30 1 1 40 -10" fill="none" stroke="#202020" stroke-width="3"/></svg>)SVG"},
		{"quadratic paths (Q/T), nonzero", H + R"SVG(<path d="M8 120 Q64 -40 120 120 T 120 60 Z" fill="#c06020"/><path d="M10 10 Q60 60 10 110 Q60 60 110 110 Q60 60 110 10 Q60 60 10 10Z" fill="#2060c0" fill-opacity="0.6"/></svg>)SVG"},
		{"cubic paths (C/S), nonzero", H + R"SVG(<path d="M10 64 C10 0 118 0 118 64 S10 128 10 64Z" fill="#6020c0"/><path d="M20 20 C120 20 20 120 120 120 C20 120 120 20 20 20Z" fill="#20c060" fill-opacity="0.5"/></svg>)SVG"},
		{"both fill rules (evenodd, self-overlap)", H + R"SVG(<path d="M64 4 L100 120 L4 44 L124 44 L28 120Z" fill="#c02020" fill-rule="evenodd"/><path d="M10 70 C10 20 118 20 118 70 C118 120 10 120 10 70Z M30 70 C30 40 98 40 98 70 C98 100 30 100 30 70Z" fill="#2040c0" fill-opacity="0.5" fill-rule="evenodd"/></svg>)SVG"},
		{"strokes: caps", H + R"SVG(<g fill="none" stroke="#203040" stroke-width="12"><path d="M20 20 L108 20" stroke-linecap="butt"/><path d="M20 50 L108 50" stroke-linecap="round"/><path d="M20 80 L108 80" stroke-linecap="square"/><path d="M20 110 Q64 90 108 110" stroke-linecap="round" stroke-width="5"/></g></svg>)SVG"},
		{"strokes: joins + miterlimit", H + R"SVG(<g fill="none" stroke="#403020" stroke-width="8"><path d="M10 40 L40 10 L70 40 L100 10" stroke-linejoin="miter"/><path d="M10 80 L40 50 L70 80 L100 50" stroke-linejoin="round"/><path d="M10 120 L40 90 L70 120 L100 90" stroke-linejoin="bevel"/><path d="M110 20 L120 120 L114 20" stroke-linejoin="miter" stroke-miterlimit="10" stroke-width="3"/></g></svg>)SVG"},
		{"strokes: dashes", H + R"SVG(<g fill="none" stroke="#305070" stroke-width="6"><path d="M10 20 L118 20" stroke-dasharray="12 6"/><path d="M10 50 L118 50" stroke-dasharray="4 8" stroke-dashoffset="3" stroke-linecap="round"/><circle cx="64" cy="94" r="28" stroke-dasharray="10 5 2 5"/></g></svg>)SVG"},
		{"strokes: unit frame under x40", H + R"SVG(<g transform="matrix(40 0 0 40 20 20)" fill="none" stroke="#205030"><path d="M0 0.5C0.6 0 1.4 1 2 0.5" stroke-width="0.05" stroke-linecap="round"/><path d="M0 1.2L2 1.2" stroke-width="0.02"/><path d="M0 1.8L2 1.8" stroke-width="0.1"/></g></svg>)SVG"},
		{"linear gradient + transform", H + R"SVG(<defs><linearGradient id="g" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="128" y2="0" gradientTransform="rotate(30 64 64) scale(1 0.6)"><stop offset="0" stop-color="#ff2000"/><stop offset="0.5" stop-color="#20ff40" stop-opacity="0.6"/><stop offset="1" stop-color="#2040ff"/></linearGradient></defs><rect width="128" height="128" fill="url(#g)"/></svg>)SVG"},
		{"radial gradient + transform", H + R"SVG(<defs><radialGradient id="g" gradientUnits="userSpaceOnUse" cx="64" cy="64" r="50" gradientTransform="translate(64 64) rotate(20) scale(1 0.5) translate(-64 -64)"><stop offset="0" stop-color="#ffffff"/><stop offset="0.7" stop-color="#e08040" stop-opacity="0.8"/><stop offset="1" stop-color="#102060"/></radialGradient></defs><rect width="128" height="128" fill="url(#g)"/></svg>)SVG"},
		{"opacity: fill / stroke / element", H + R"SVG(<rect x="10" y="10" width="70" height="70" fill="#c03030" opacity="0.6"/><rect x="40" y="40" width="70" height="70" fill="#3030c0" fill-opacity="0.5"/><circle cx="64" cy="64" r="40" fill="none" stroke="#30a030" stroke-width="10" stroke-opacity="0.5"/></svg>)SVG"},
		{"clip-path + transforms", H + R"SVG(<defs><clipPath id="c" clipPathUnits="userSpaceOnUse"><circle cx="64" cy="64" r="46"/></clipPath><clipPath id="h" clipPathUnits="userSpaceOnUse"><path d="M-500 -500L600 -500L600 600L-500 600ZM40 40L88 40L88 88L40 88Z" clip-rule="evenodd"/></clipPath></defs><g clip-path="url(#c)"><rect width="128" height="64" fill="#a03070"/><rect y="64" width="128" height="64" fill="#3070a0" transform="skewX(15)"/></g><rect x="20" y="20" width="88" height="88" fill="#e0c040" fill-opacity="0.6" clip-path="url(#h)"/></svg>)SVG"},
		{"nested transforms (rotate, skew, scale)", H + R"SVG(<g transform="translate(64 64) rotate(35)"><g transform="skewX(20) scale(1.5 0.7)"><rect x="-30" y="-20" width="60" height="40" fill="#2080c0"/><circle cx="20" cy="0" r="10" fill="#c02080"/></g></g></svg>)SVG"},
		{"stamp run: use/symbol, mixed prototypes", H + R"SVG(<defs><symbol id="e" data-stamp-kind="ellipse" overflow="visible"><path d="M1 0C1 0.552285 0.552285 1 0 1C-0.552285 1 -1 0.552285 -1 0C-1 -0.552285 -0.552285 -1 0 -1C0.552285 -1 1 -0.552285 1 0Z"/></symbol><symbol id="r" data-stamp-kind="rect" overflow="visible"><path d="M0 0L1 0L1 1L0 1Z"/></symbol><symbol id="s" data-stamp-kind="stroke" overflow="visible"><path d="M0 0.5C0.3 0 0.7 1 1 0.5" fill="none" stroke-width="0.08" stroke-linecap="round"/></symbol></defs><g data-stamp-run="0"><use href="#e" transform="matrix(20 0 0 12 40 40)" fill="#c04040"/><use href="#r" transform="matrix(30 10 -8 24 70 20)" fill="#4040c0" fill-opacity="0.6"/><use href="#s" transform="matrix(60 0 0 60 30 70)" stroke="#204020"/><use href="#e" transform="matrix(3 0 0 3 100 110)" fill="#000"/></g></svg>)SVG"},
		{"group opacity, overlapping children", H + R"SVG(<g opacity="0.5"><rect x="10" y="10" width="70" height="70" fill="#c03030"/><rect x="48" y="48" width="70" height="70" fill="#3030c0"/></g></svg>)SVG"},
		{"mask (luminance)", H + R"SVG(<defs><linearGradient id="m" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="128" y2="0"><stop offset="0" stop-color="#fff"/><stop offset="1" stop-color="#000"/></linearGradient><mask id="k"><rect width="128" height="128" fill="url(#m)"/></mask></defs><rect width="128" height="128" fill="#c03060" mask="url(#k)"/></svg>)SVG"},
		{"gradient spread reflect / repeat", H + R"SVG(<defs><linearGradient id="a" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="32" y2="0" spreadMethod="reflect"><stop offset="0" stop-color="#ff0000"/><stop offset="1" stop-color="#0000ff"/></linearGradient><linearGradient id="b" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="32" y2="0" spreadMethod="repeat"><stop offset="0" stop-color="#ff0000"/><stop offset="1" stop-color="#0000ff"/></linearGradient></defs><rect width="128" height="64" fill="url(#a)"/><rect y="64" width="128" height="64" fill="url(#b)"/></svg>)SVG"},
		{"radial gradient, focal point", H + R"SVG(<defs><radialGradient id="g" gradientUnits="userSpaceOnUse" cx="64" cy="64" r="56" fx="40" fy="50"><stop offset="0" stop-color="#ffffff"/><stop offset="1" stop-color="#204080"/></radialGradient></defs><rect width="128" height="128" fill="url(#g)"/></svg>)SVG"},
	};

	return C;
}

// --contact-sheet OUT_DIR <svg...>: the decomposition per key (worst ERROR first) and per content
// class, plus the stamp negative control. CSV rows on stdout.
int contactSheets(int argc, char** argv) {
	const std::string dir = argv[2];
	const uint32_t W = 256;
	const std::vector<std::string> headers = {"THORVG RASTER", "SLUG DIRECT", "SHIPPED DECODE", "ERR DECODE X4"};

	std::cout << DECOMPOSITION_CSV_HEADER << std::endl;

	// Content classes.
	{
		std::vector<sheet::Row> rows;

		for(const auto& [name, svg] : contentClasses()) {
			Decomposition d = decompose("CLASS " + name, svg, W, false);

			printDecomposition(d);

			for(const auto& w : d.warnings) std::cout << "  warning: " << w << std::endl;

			rows.push_back(std::move(d.row));
		}

		for(const auto& p : sheet::write(dir, "content-classes", rows, headers)) std::cout << "sheet: " << p << std::endl;
	}

	std::vector<Decomposition> keys;

	for(int i = 3; i < argc; i++) {
		std::ifstream in(argv[i], std::ios::binary);
		std::stringstream ss;

		ss << in.rdbuf();

		Decomposition d = decompose(std::filesystem::path(argv[i]).stem().string(), ss.str(), W, true);

		printDecomposition(d);

		for(const auto& w : d.warnings) std::cout << "  warning: " << w << std::endl;

		keys.push_back(std::move(d));
	}

	// Worst ERROR (after rung 1) first.
	std::stable_sort(keys.begin(), keys.end(), [](const Decomposition& a, const Decomposition& b) { return a.row.err > b.row.err; });

	std::vector<sheet::Row> rows;

	for(auto& d : keys) rows.push_back(std::move(d.row));

	for(const auto& p : sheet::write(dir, "thorvg-slug-bake", rows, headers)) std::cout << "sheet: " << p << std::endl;

	// Stamp negative control sheet: the synthetic case, one instance shifted.
	{
		StampCase sc;

		loadStamped(SVG_STAMPED, sc);

		const uint32_t SW = 256, SH = 192;
		const auto expanded = expandedRender(SVG_STAMPED, SW, SH);
		const auto good = slughorn::stamp::renderComposite(sc.atlas, sc.comp, sc.set, SW, SH, 0_cv, 0_cv, 1_cv, sc.cfg.heightEm).data;

		shiftOneInstance(sc.set, expanded, SW, SH, sc.cfg.heightEm);

		const auto bad = slughorn::stamp::renderComposite(sc.atlas, sc.comp, sc.set, SW, SH, 0_cv, 0_cv, 1_cv, sc.cfg.heightEm).data;
		const Diff dg = compare(good, expanded), db = compare(bad, expanded);

		sheet::Row row;

		row.text = {
			"SYNTHETIC STAMPS: ONE INSTANCE SHIFTED",
			"PARITY " + sheet::fmt(dg.maxd, 3) + " MAX  CONTROL " + sheet::fmt(db.maxd, 3) + " MAX " + (stampOk(db, size_t(SW) * SH) ? "MISSED" : "CAUGHT")
		};
		row.cols = {thorvgRaster(SVG_STAMPED, SW, SH), good, bad, sheet::absDiff(expanded, bad)};
		row.gains = {1.0, 1.0, 1.0, 4.0};
		row.w = SW;
		row.h = SH;

		for(const auto& p : sheet::write(dir, "stamp-negative-control", {row}, {"THORVG RASTER", "STAMPS", "STAMPS, 1 SHIFTED", "ABS DIFF X4"})) {
			std::cout << "sheet: " << p << std::endl;
		}
	}

	return 0;
}

// --debug-key OUT_DIR WIDTH file.svg: full-size PNGs (over white) of the ThorVG raster, the stamp
// decode, the <use>-expanded decode (no stamps) and |raster - decode| x4, plus the 12 worst pixels.
static void writePng(const std::string& path, const std::vector<slug_t>& img, uint32_t W, uint32_t H, double gain=1.0, bool diff=false) {
	std::vector<uint8_t> rgb(size_t(W) * H * 3);

	for(size_t p = 0; p < size_t(W) * H; p++) {
		for(int k = 0; k < 3; k++) {
			const double v = diff ? double(img[p * 4 + size_t(k)]) * gain : double(img[p * 4 + size_t(k)]) + (1.0 - double(img[p * 4 + 3]));

			rgb[p * 3 + size_t(k)] = uint8_t(std::lround(std::clamp(v, 0.0, 1.0) * 255));
		}
	}

	stbi_write_png(path.c_str(), int(W), int(H), 3, rgb.data(), int(W * 3));
}

int debugKey(int argc, char** argv) {
	const std::string dir = argv[2];
	const auto W = uint32_t(std::atoi(argv[3]));

	for(int i = 4; i < argc; i++) {
		std::ifstream in(argv[i], std::ios::binary);
		std::stringstream ss;

		ss << in.rdbuf();

		const std::string svg = ss.str();
		const std::string key = std::filesystem::path(argv[i]).stem().string();

		KeyRun r;

		runKey(svg, r, W);

		if(!r.ok) continue;

		const uint32_t H = r.H;
		const auto raster = thorvgRaster(slughorn::stamp::dedupeUseHref(svg), W, H, std::getenv("SLUG_DEBUG_EXACT") != nullptr);
		const auto expanded = std::getenv("SLUG_DEBUG_DIRECT") ? directRender(slughorn::stamp::dedupeUseHref(svg), W, H).img : expandedRender(slughorn::stamp::dedupeUseHref(svg), W, H);

		writePng(dir + "/" + key + "-thorvg.png", raster, W, H);
		writePng(dir + "/" + key + "-decode.png", r.decode, W, H);
		writePng(dir + "/" + key + "-expanded.png", expanded, W, H);
		writePng(dir + "/" + key + "-diff.png", sheet::absDiff(raster, r.decode), W, H, 4.0, true);
		writePng(dir + "/" + key + "-diff-expanded.png", sheet::absDiff(raster, expanded), W, H, 4.0, true);

		const Diff d = compare(r.decode, raster), e = compare(expanded, raster), se = compare(r.decode, expanded);

		std::cout << key << " decode-vs-thorvg mean|d|=" << d.mean << " bad=" << d.bad * 100 << "% flips=" << d.flips << " max=" << d.maxd
			<< " | expanded-vs-thorvg mean|d|=" << e.mean << " bad=" << e.bad * 100 << "% flips=" << e.flips << " max=" << e.maxd
			<< " | decode-vs-expanded mean|d|=" << se.mean << " bad=" << se.bad * 100 << "% flips=" << se.flips << " max=" << se.maxd << std::endl;

		std::vector<std::pair<double, size_t>> worst;
		const bool vsExpanded = std::getenv("SLUG_DEBUG_VS_EXPANDED") != nullptr;
		const auto& refImg = vsExpanded ? expanded : raster;

		if(vsExpanded) writePng(dir + "/" + key + "-diff-stamp-vs-expanded.png", sheet::absDiff(expanded, r.decode), W, H, 4.0, true);

		for(size_t p = 0; p < size_t(W) * H; p++) {
			double m = 0;

			for(int k = 0; k < 4; k++) m = std::max(m, std::abs(double(r.decode[p * 4 + size_t(k)]) - double(refImg[p * 4 + size_t(k)])));

			worst.push_back({m, p});
		}

		std::partial_sort(worst.begin(), worst.begin() + std::min<std::ptrdiff_t>(12, std::ptrdiff_t(worst.size())), worst.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

		for(size_t k = 0; k < std::min<size_t>(12, worst.size()); k++) {
			const size_t p = worst[k].second;

			std::cout << "  px (" << p % W << "," << p / W << ") |d|=" << worst[k].first
				<< " thorvg=(" << raster[p * 4] << "," << raster[p * 4 + 1] << "," << raster[p * 4 + 2] << "," << raster[p * 4 + 3] << ")"
				<< " decode=(" << r.decode[p * 4] << "," << r.decode[p * 4 + 1] << "," << r.decode[p * 4 + 2] << "," << r.decode[p * 4 + 3] << ")"
				<< " expanded=(" << expanded[p * 4] << "," << expanded[p * 4 + 1] << "," << expanded[p * 4 + 2] << "," << expanded[p * 4 + 3] << ")" << std::endl;
		}

		for(const auto& n : r.sc.notes) std::cout << "  note: " << n << std::endl;

		// Instances whose bounds hold the worst pixel.
		if(!worst.empty()) {
			const size_t p = worst[0].second;
			const slug_t ex = (slug_t(p % W) + 0.5_cv) / slug_t(W), ey = (slug_t(p / W) + 0.5_cv) / slug_t(W);

			for(size_t li = 0; li < r.sc.set.layers.size(); li++) {
				for(const auto& in : r.sc.set.layers[li].instances) {
					const auto b = slughorn::stamp::bounds(in, r.sc.set);

					if(ex < b.x0 || ex > b.x1 || ey < b.y0 || ey > b.y1) continue;

					const auto& pr = r.sc.set.protos[in.proto];

					std::cout << "  at worst px: layer " << li << " proto " << in.proto << " kind " << int(pr.kind) << " curves " << pr.curves.size()
						<< " extent [" << pr.x0 << "," << pr.y0 << " - " << pr.x1 << "," << pr.y1 << "] m=[" << in.m.xx << " " << in.m.yx << " " << in.m.xy << " " << in.m.yy
						<< " " << in.m.dx << " " << in.m.dy << "] color=(" << in.color.r << "," << in.color.g << "," << in.color.b << "," << in.color.a << ")" << std::endl;
				}
			}
		}

		for(size_t k = 0; k < r.sc.set.protos.size(); k++) {
			const auto& pr = r.sc.set.protos[k];
			double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;

			for(const auto& c : pr.curves) for(const auto& [x, y] : {std::pair<slug_t, slug_t>{c.x1, c.y1}, {c.x3, c.y3}}) {
				x0 = std::min(x0, double(x)); y0 = std::min(y0, double(y)); x1 = std::max(x1, double(x)); y1 = std::max(y1, double(y));
			}

			double area = 0;

			for(const auto& contour : slughorn::clipper::splitContours(pr.curves, pr.starts)) {
				for(const auto& c : contour) area += double(c.x1) * double(c.y3) - double(c.x3) * double(c.y1);
			}

			std::cout << "  proto " << k << " kind=" << int(pr.kind) << " curves=" << pr.curves.size() << " contours=" << pr.starts.size()
				<< " bbox=[" << x0 << "," << y0 << " - " << x1 << "," << y1 << "] signedArea(chords)=" << area * 0.5 << std::endl;

			if(std::getenv("SLUG_DUMP_PROTO")) {
				size_t si = 0;

				for(size_t ci = 0; ci < pr.curves.size(); ci++) {
					if(si < pr.starts.size() && pr.starts[si] == ci) { std::cout << "    -- contour " << si << std::endl; si++; }

					const auto& c = pr.curves[ci];

					std::cout << "    " << c.x1 << "," << c.y1 << " " << c.x2 << "," << c.y2 << " " << c.x3 << "," << c.y3 << std::endl;
				}
			}
		}

		for(size_t li = 0; li < r.sc.set.layers.size(); li++) {
			for(const auto& in : r.sc.set.layers[li].instances) {
				std::cout << "  inst layer " << li << " proto " << in.proto << " m=[" << in.m.xx << " " << in.m.yx << " " << in.m.xy << " " << in.m.yy << " " << in.m.dx << " " << in.m.dy
					<< "] color=(" << in.color.r << "," << in.color.g << "," << in.color.b << "," << in.color.a << ") grad=" << in.gradient << std::endl;
			}
		}
	}

	return 0;
}
