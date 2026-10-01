// Included by slughorn-test-thorvg.cpp after slughorn-test-thorvg-stamp.inl.
//
// Mesh rasterizer (baked meshes back to pixels), the cutout-bake test, and labelled contact sheets:
//
//   slughorn-test-thorvg --contact-sheet OUT_DIR <file.svg> [...]
//
// One row per key: ThorVG raster | slughorn CPU decode (stamps included) | baked mesh rasterized
// (the cutout bake, alpha 0.5, for card keys) | |ThorVG - mesh| (x4), labelled with the key, its
// mode and errors; the worst keys first, then the stamped ones, then the rest. 12 rows a sheet,
// 256 px cells, PNG via stb_image_write. Also writes stamp-negative-control-01.png (same layout).

#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "stb_image_write.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// ------------------------------------------------------------------------------------------------
// Mesh rasterizer: 2 x 2 samples per pixel, triangles in index order, src-over (UV v up).
// ------------------------------------------------------------------------------------------------
static slughorn::Color paintColor(const slughorn::bake::Paint& p, float p0, float p1) {
	using PT = slughorn::bake::Paint::Type;

	if(p.type == PT::Solid) return p.color;

	slughorn::GradientInfo g;

	g.stops = p.stops;

	const slug_t t = p.type == PT::Linear ? slug_t(p0) : slug_t(std::sqrt(p0 * p0 + p1 * p1)) - p.innerRadius;

	return slughorn::render::gradientColor(g, t);
}

static std::vector<slug_t> rasterizeMesh(const slughorn::bake::BakedMesh& m, uint32_t W, uint32_t H, bool vUp=true) {
	std::vector<slug_t> img(size_t(W) * H * 4, 0_cv);
	std::vector<slug_t> cover(size_t(W) * H * 4 * 4, 0_cv); // per sample premultiplied

	for(size_t t = 0; t + 2 < m.indices.size(); t += 3) {
		const uint32_t ia = m.indices[t], ib = m.indices[t + 1], ic = m.indices[t + 2];
		auto P = [&](uint32_t i) {
			const double u = m.positions[i * 2], v = m.positions[i * 2 + 1];

			return std::pair<double, double>{u * W, (vUp ? 1.0 - v : v) * H};
		};

		const auto [ax, ay] = P(ia);
		const auto [bx, by] = P(ib);
		const auto [cx, cy] = P(ic);
		const double d = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);

		if(std::abs(d) < 1e-12) continue;

		const auto x0 = std::max<int64_t>(0, int64_t(std::floor(std::min({ax, bx, cx}))));
		const auto x1 = std::min<int64_t>(W - 1, int64_t(std::ceil(std::max({ax, bx, cx}))));
		const auto y0 = std::max<int64_t>(0, int64_t(std::floor(std::min({ay, by, cy}))));
		const auto y1 = std::min<int64_t>(H - 1, int64_t(std::ceil(std::max({ay, by, cy}))));

		const auto& paint = m.paints[m.paintIds[ia]];

		for(int64_t y = y0; y <= y1; y++) {
			for(int64_t x = x0; x <= x1; x++) {
				for(int s = 0; s < 4; s++) {
					const double px = double(x) + 0.25 + 0.5 * (s & 1), py = double(y) + 0.25 + 0.5 * (s >> 1);
					const double w1 = ((px - ax) * (cy - ay) - (cx - ax) * (py - ay)) / d;
					const double w2 = ((bx - ax) * (py - ay) - (px - ax) * (by - ay)) / d;
					const double w0 = 1 - w1 - w2;

					if(w0 < 0 || w1 < 0 || w2 < 0) continue;

					const float p0 = float(m.params[ia * 2] * w0 + m.params[ib * 2] * w1 + m.params[ic * 2] * w2);
					const float p1 = float(m.params[ia * 2 + 1] * w0 + m.params[ib * 2 + 1] * w1 + m.params[ic * 2 + 1] * w2);
					const slughorn::Color c = paintColor(paint, p0, p1);
					slug_t* o = &cover[((size_t(y) * W + size_t(x)) * 4 + size_t(s)) * 4];

					o[0] = c.r * c.a + o[0] * (1 - c.a);
					o[1] = c.g * c.a + o[1] * (1 - c.a);
					o[2] = c.b * c.a + o[2] * (1 - c.a);
					o[3] = c.a + o[3] * (1 - c.a);
				}
			}
		}
	}

	for(size_t p = 0; p < size_t(W) * H; p++) for(int k = 0; k < 4; k++) {
		slug_t acc = 0;

		for(int s = 0; s < 4; s++) acc += cover[(p * 4 + size_t(s)) * 4 + size_t(k)];

		img[p * 4 + size_t(k)] = acc / 4;
	}

	return img;
}

// ------------------------------------------------------------------------------------------------
// Cutout bake test
// ------------------------------------------------------------------------------------------------
static const std::string SVG_CUTOUT = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128" viewBox="0 0 128 128">
	<defs>
		<linearGradient id="fade" gradientUnits="userSpaceOnUse" x1="8" y1="0" x2="120" y2="0">
			<stop offset="0" stop-color="#ff0000" stop-opacity="0"/>
			<stop offset="0.6" stop-color="#ffff00" stop-opacity="1"/>
			<stop offset="1" stop-color="#00ff00" stop-opacity="0.2"/>
		</linearGradient>
		<radialGradient id="glow" gradientUnits="userSpaceOnUse" cx="96" cy="96" r="28">
			<stop offset="0" stop-color="#ffffff" stop-opacity="1"/>
			<stop offset="1" stop-color="#0000ff" stop-opacity="0"/>
		</radialGradient>
		<symbol id="e" data-stamp-kind="ellipse" overflow="visible"><path d="M1 0C1 0.55228 0.55228 1 0 1C-0.55228 1 -1 0.55228 -1 0C-1 -0.55228 -0.55228 -1 0 -1C0.55228 -1 1 -0.55228 1 0Z"/></symbol>
	</defs>
	<path d="M8 8H72V56H8Z" fill="#336699" fill-opacity="0.4"/>
	<path d="M40 24H100V64H40Z" fill="#993366" fill-opacity="0.4"/>
	<path d="M8 72H120V84H8Z" fill="url(#fade)"/>
	<path d="M64 64H128V128H64Z" fill="url(#glow)"/>
	<path d="M8 96H40V120H8Z" fill="#00aa00" fill-opacity="0.3"/>
	<g data-stamp-run="0">
		<use href="#e" transform="matrix(10 0 0 10 24 104)" fill="#ffaa00" fill-opacity="0.35"/>
		<use href="#e" transform="matrix(8 0 0 8 30 110)" fill="#ffaa00" fill-opacity="0.35"/>
		<use href="#e" transform="matrix(6 0 0 6 110 20)" fill="#ffffff" fill-opacity="0.9"/>
	</g>
</svg>
)SVG";

void test_Cutout() {
	std::cout << "\n=== test_Cutout ===" << std::endl;

	StampCase sc;

	loadStamped(SVG_CUTOUT, sc);

	const uint32_t W = 512, H = 512;
	const auto ref = slughorn::stamp::renderComposite(sc.atlas, sc.comp, sc.set, W, H, 0_cv, 0_cv, 1_cv, sc.cfg.heightEm).data;

	std::vector<slughorn::bake::LayerSource> meta(sc.comp.layers.size());

	for(slug_t test : {0.5_cv, 0.25_cv}) {
		slughorn::bake::BakeConfig bc;

		bc.width = sc.cfg.width;
		bc.height = sc.cfg.height;
		bc.vUp = true;
		bc.alphaTest = test;
		bc.tolerancePx = 0.1_cv;

		const auto mesh = slughorn::bake::bakeCutout(sc.atlas, sc.comp, meta, bc, &sc.set);
		const auto img = rasterizeMesh(mesh, W, H);

		size_t refIn = 0, meshIn = 0, mismatch = 0, ctlMismatch = 0;
		double area = 0.0;

		for(size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
			auto P = [&](uint32_t k) { return std::pair<double, double>{mesh.positions[k * 2], mesh.positions[k * 2 + 1]}; };
			const auto [ax, ay] = P(mesh.indices[i]);
			const auto [bx, by] = P(mesh.indices[i + 1]);
			const auto [cx, cy] = P(mesh.indices[i + 2]);

			area += ((bx - ax) * (cy - ay) - (cx - ax) * (by - ay)) * 0.5;
		}

		for(size_t p = 0; p < size_t(W) * H; p++) {
			const bool r = ref[p * 4 + 3] >= test;
			const bool m = img[p * 4 + 3] >= 0.5_cv;
			const bool ctl = ref[p * 4 + 3] >= test + 0.25_cv; // negative control: the wrong threshold

			refIn += r;
			meshIn += m;
			mismatch += r != m;
			ctlMismatch += ctl != m;
		}

		const double refArea = double(refIn) / (double(W) * H);
		const double mis = double(mismatch) / (double(W) * H);
		const double ctl = double(ctlMismatch) / (double(W) * H);

		std::cout << "  alphaTest=" << test << ": bake area=" << area << " CPU area(alpha>=test)=" << refArea
			<< " pixel mismatch=" << mis * 100 << "% control(alpha>=test+0.25) mismatch=" << ctl * 100 << "%"
			<< " tris=" << mesh.trianglesAfter << " approxFaces=" << mesh.cutoutApproxFaces << std::endl;

		check("cutout bake area == CPU area of composite alpha >= alphaTest (1e-3 + 0.5%)", std::abs(area - refArea) <= 1e-3 + 0.005 * refArea);
		check("cutout raster matches the CPU alpha-test mask (< 0.5% pixels, edges)", mis < 0.005);
		check("negative control (threshold + 0.25) does not match", ctl > 0.02);
		check("cutout bake has no overlay", mesh.overlayIndexCount == 0);
		check("every paint opaque", std::all_of(mesh.paints.begin(), mesh.paints.end(), [](const auto& p) { return p.opaque; }));
	}
}

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
	std::string label;
	std::string sub;
	std::vector<slug_t> a, b, c, d;
	uint32_t w = 0, h = 0;
	double err = 0;
};

static std::vector<std::string> write(const std::string& dir, const std::string& stem, const std::vector<Row>& rows, const std::vector<std::string>& headers) {
	const uint32_t cell = 256, labelH = 44, pad = 8, perSheet = 12, headH = 30;
	std::vector<std::string> paths;

	for(size_t first = 0, n = 1; first < rows.size(); first += perSheet, n++) {
		const size_t count = std::min<size_t>(perSheet, rows.size() - first);
		Canvas cv(pad + 4 * (cell + pad), uint32_t(headH + count * (labelH + cell + pad)));

		for(size_t c = 0; c < headers.size(); c++) cv.text(int64_t(pad + c * (cell + pad)), 8, headers[c], 2, 255, 220, 120);

		for(size_t r = 0; r < count; r++) {
			const Row& row = rows[first + r];
			const int64_t y = int64_t(headH + r * (labelH + cell + pad));

			cv.text(pad, y + 4, row.label, 2);
			cv.text(pad, y + 24, row.sub, 2, 180, 200, 255);

			const std::vector<slug_t>* cols[4] = {&row.a, &row.b, &row.c, &row.d};

			for(int c = 0; c < 4; c++) if(!cols[c]->empty()) cv.image(int64_t(pad + c * (cell + pad)), y + labelH, *cols[c], row.w, row.h, cell, c == 3 ? 4.0 : 1.0);
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

int contactSheets(int argc, char** argv) {
	const std::string dir = argv[2];
	std::vector<sheet::Row> rows;
	std::vector<std::pair<double, size_t>> order;
	std::vector<bool> stamped;

	for(int i = 3; i < argc; i++) {
		std::ifstream in(argv[i], std::ios::binary);
		std::stringstream ss;

		ss << in.rdbuf();

		const std::string svg = ss.str();
		const std::string key = std::filesystem::path(argv[i]).stem().string();

		// The guest's pipeline: stamp-aware load, merge, build, bake, cost.
		StampCase sc;
		slughorn::KeyIterator keys("k", true);

		sc.cfg.log = [](int, std::string_view) {};

		slughorn::Atlas staging;
		auto loaded = slughorn::stamp::loadString(svg, staging, keys, &sc.cfg, sc.set, "proto/", &sc.notes);

		if(sc.cfg.width <= 0_cv) continue;

		slughorn::stamp::splitDeep(sc.set, loaded, 1_cv, sc.cfg.heightEm, sc.cfg.width);

		std::vector<slughorn::bake::LayerSource> meta;

		for(const auto& li : sc.cfg.layers) meta.push_back({li.fillRule, li.stroke, static_cast<uint8_t>(li.spread)});

		slughorn::KeyIterator lk("l");
		auto merged = slughorn::bake::mergeLayers(staging, loaded, meta, sc.atlas, lk);

		slughorn::stamp::registerProtos(sc.set, sc.atlas, "proto/");
		sc.atlas.build();

		const uint32_t W = 256;
		const auto H = static_cast<uint32_t>(std::max<long>(1, std::lround(W * sc.cfg.heightEm)));

		const auto raster = thorvgRaster(svg, W, H);
		const auto decode = slughorn::stamp::renderComposite(sc.atlas, merged.composite, sc.set, W, H, 0_cv, 0_cv, 1_cv, sc.cfg.heightEm).data;

		// Card keys (much of the canvas transparent) show the cutout bake; the rest the planar bake.
		size_t clear = 0;

		for(size_t p = 0; p < size_t(W) * H; p++) clear += decode[p * 4 + 3] < 0.5_cv;

		const bool card = double(clear) / (double(W) * H) > 0.05;

		slughorn::bake::BakeConfig bc;

		bc.width = sc.cfg.width;
		bc.height = sc.cfg.height;
		bc.vUp = true;

		const auto planar = slughorn::bake::bakeMesh(sc.atlas, merged.composite, merged.layers, bc, &sc.set);
		const auto cost = slughorn::bake::cost(sc.atlas, merged.composite, planar, merged.layersBefore, 1_cv, sc.cfg.heightEm, &sc.set, sc.cfg.width);

		slughorn::bake::BakedMesh shown = planar;

		if(card) {
			bc.alphaTest = 0.5_cv;
			shown = slughorn::bake::bakeMesh(sc.atlas, merged.composite, merged.layers, bc, &sc.set);
		}

		auto meshImg = rasterizeMesh(shown, W, H);

		// For cards compare against the alpha-tested raster.
		std::vector<slug_t> refForMesh = raster;

		if(card) {
			for(size_t p = 0; p < size_t(W) * H; p++) {
				slug_t* q = &refForMesh[p * 4];

				if(q[3] >= 0.5_cv) { const slug_t a = q[3]; q[0] /= a; q[1] /= a; q[2] /= a; q[3] = 1_cv; }
				else { q[0] = q[1] = q[2] = q[3] = 0_cv; }
			}
		}

		const Diff dDecode = compare(decode, raster);
		const Diff dMesh = compare(meshImg, refForMesh);

		sheet::Row row;

		row.label = key + " " + cost.mode + (card ? " CARD(CUTOUT .5)" : "") + (sc.set.layers.empty() ? "" : " STAMPED");
		row.sub = "DECODE " + sheet::fmt(dDecode.mean) + " " + sheet::fmt(dDecode.bad * 100, 2) + "%  MESH " + sheet::fmt(dMesh.mean) + " " +
			sheet::fmt(dMesh.bad * 100, 2) + "%  TRIS " + std::to_string(shown.trianglesAfter);
		row.a = raster;
		row.b = decode;
		row.c = meshImg;
		row.d = sheet::absDiff(refForMesh, meshImg);
		row.w = W;
		row.h = H;
		row.err = std::max(dDecode.mean, dMesh.mean);

		std::cout << key << " mode=" << cost.mode << " card=" << card << " stamped=" << !sc.set.layers.empty()
			<< " decode=" << dDecode.mean << "/" << dDecode.bad * 100 << "% mesh=" << dMesh.mean << "/" << dMesh.bad * 100 << "%" << std::endl;

		stamped.push_back(!sc.set.layers.empty());
		order.push_back({row.err, rows.size()});
		rows.push_back(std::move(row));
	}

	// Worst 12 first, then stamped keys, then the rest (each by error).
	std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

	std::vector<size_t> seq;
	std::vector<bool> used(rows.size(), false);

	for(size_t k = 0; k < std::min<size_t>(12, order.size()); k++) { seq.push_back(order[k].second); used[order[k].second] = true; }
	for(const auto& [e, i] : order) if(!used[i] && stamped[i]) { seq.push_back(i); used[i] = true; }
	for(const auto& [e, i] : order) if(!used[i]) { seq.push_back(i); used[i] = true; }

	std::vector<sheet::Row> sorted;

	for(size_t i : seq) sorted.push_back(std::move(rows[i]));

	const auto paths = sheet::write(dir, "thorvg-slug-bake", sorted,
		{"THORVG RASTER", "SLUG CPU DECODE", "BAKED MESH", "ABS DIFF X4"});

	for(const auto& p : paths) std::cout << "sheet: " << p << std::endl;

	// Stamp negative control sheet: the synthetic case, one instance shifted.
	{
		StampCase sc;

		loadStamped(SVG_STAMPED, sc);

		const uint32_t W = 256, H = 192;
		const auto expanded = expandedRender(SVG_STAMPED, W, H);
		const auto good = slughorn::stamp::renderComposite(sc.atlas, sc.comp, sc.set, W, H, 0_cv, 0_cv, 1_cv, sc.cfg.heightEm).data;

		shiftOneInstance(sc.set, expanded, W, H, sc.cfg.heightEm);

		const auto bad = slughorn::stamp::renderComposite(sc.atlas, sc.comp, sc.set, W, H, 0_cv, 0_cv, 1_cv, sc.cfg.heightEm).data;
		const Diff dg = compare(good, expanded), db = compare(bad, expanded);

		sheet::Row row;

		row.label = "SYNTHETIC STAMPS: ONE INSTANCE SHIFTED";
		row.sub = "PARITY " + sheet::fmt(dg.maxd, 3) + " MAX  CONTROL " + sheet::fmt(db.maxd, 3) + " MAX " + (stampOk(db, size_t(W) * H) ? "MISSED" : "CAUGHT");
		row.a = thorvgRaster(SVG_STAMPED, W, H);
		row.b = good;
		row.c = bad;
		row.d = sheet::absDiff(expanded, bad);
		row.w = W;
		row.h = H;

		const auto cp = sheet::write(dir, "stamp-negative-control", {row}, {"THORVG RASTER", "STAMPS", "STAMPS, 1 SHIFTED", "ABS DIFF X4"});

		for(const auto& p : cp) std::cout << "sheet: " << p << std::endl;
	}

	return 0;
}
