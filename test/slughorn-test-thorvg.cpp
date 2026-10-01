//vimrun! ./slughorn-test-thorvg

// Verifies that slughorn/thorvg.hpp correctly converts ThorVG's SVG paint tree into slughorn
// curves, gradients and layers.
//
// The first tests mirror slughorn-test-nanosvg.cpp exactly (same fixtures, same expected
// metrics), so both SVG backends are held to the same ground truth. The parity tests then render
// whole composites through the CPU Slug decoder (render::renderComposite) and compare:
//
// - thorvg.hpp vs nanosvg.hpp on the SVG subset both support (must agree within tolerance), with
//   a negative control proving the metric actually fails on a wrong result;
// - thorvg.hpp vs ThorVG's own software rasterizer for the features nanosvg lacks (strokes,
//   dashes, clip paths, group opacity, nested transforms).
//
// When built with SLUGHORN_CLIPPER2 + SLUGHORN_TESSELLATE it also checks fill-rule-robust
// tessellation on a deliberately mis-wound glyph-like contour, and slughorn/bake.hpp's merge /
// planar bake.
//
// Usage:
//
// ./slughorn-test-thorvg - run unit tests
// ./slughorn-test-thorvg <file.svg> [...] - dump one or more SVG files as JSON
// ./slughorn-test-thorvg --compare <file.svg> [...] - Slug vs ThorVG raster parity per file

#ifndef SLUGHORN_HAS_THORVG
#  error "This test requires SLUGHORN_THORVG=ON"
#endif

#ifndef SLUGHORN_HAS_SERIAL
#  error "This test requires SLUGHORN_SERIAL=ON"
#endif

#include "slughorn/thorvg.hpp"
#include "slughorn/render.hpp"
#include "slughorn/serial.hpp"

#ifdef SLUGHORN_HAS_NANOSVG
#include "slughorn/nanosvg.hpp"
#endif

#if defined(SLUGHORN_HAS_CLIPPER2) && defined(SLUGHORN_HAS_TESSELLATE)
#include "slughorn/bake.hpp"
#include "slughorn/stamp.hpp"
#include "slughorn/tessellate.hpp"
#endif

#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace slughorn::literals;
using slughorn::slug_t;

// =============================================================================
// Minimal assertion helpers
// =============================================================================

static int s_pass = 0;
static int s_fail = 0;

static void check(const char* label, bool cond) {
	if(cond) {
		std::cout << "  PASS: " << label << std::endl;
		s_pass++;
	}

	else {
		std::cout << "  FAIL: " << label << std::endl;
		s_fail++;
	}
}

static void checkNear(const char* label, slug_t actual, slug_t expected, slug_t eps = 1e-3_cv) {
	const bool ok = std::abs(actual - expected) <= eps;

	if(ok) {
		std::cout << "  PASS: " << label << " (" << actual << ")" << std::endl;
	}

	else {
		std::cout << "  FAIL: " << label
			<< " expected=" << expected
			<< " actual=" << actual
			<< " delta=" << std::abs(actual - expected)
			<< std::endl
		;

		s_fail++;

		return;
	}

	s_pass++;
}

// Silence expected warnings in tests that provoke them.
static slughorn::thorvg::LoadConfig quietConfig() {
	slughorn::thorvg::LoadConfig cfg;

	cfg.log = [](int, std::string_view) {};

	return cfg;
}

// =============================================================================
// SVG fixtures (the first five are byte-identical to slughorn-test-nanosvg.cpp)
// =============================================================================

static const std::string SVG_TRIANGLE = R"(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
	<path fill="red" d="M 0,0 L 100,0 L 0,100 Z"/>
</svg>
)";

static const std::string SVG_LINEAR_GRADIENT = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
	<defs>
		<linearGradient id="lg" x1="0" y1="0" x2="1" y2="0">
			<stop offset="0" stop-color="#ff0000"/>
			<stop offset="1" stop-color="#0000ff"/>
		</linearGradient>
	</defs>
	<rect x="0" y="0" width="100" height="100" fill="url(#lg)"/>
</svg>
)SVG";

static const std::string SVG_RADIAL_GRADIENT = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
	<defs>
		<radialGradient id="rg" cx="0.5" cy="0.5" r="0.5">
			<stop offset="0" stop-color="#ffffff"/>
			<stop offset="1" stop-color="#000000"/>
		</radialGradient>
	</defs>
	<rect x="0" y="0" width="100" height="100" fill="url(#rg)"/>
</svg>
)SVG";

static const std::string SVG_RADIAL_OBB_NON_SQUARE = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 400 200">
	<defs>
		<radialGradient id="rg3" cx="50%" cy="50%" r="50%" gradientUnits="objectBoundingBox">
			<stop offset="0" stop-color="#ffffff"/>
			<stop offset="1" stop-color="#000000"/>
		</radialGradient>
	</defs>
	<rect x="0" y="0" width="400" height="200" fill="url(#rg3)"/>
</svg>
)SVG";

static const std::string SVG_RADIAL_NON_SQUARE = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 400 200">
	<defs>
		<radialGradient id="rg2" cx="0" cy="0" r="1"
			gradientUnits="userSpaceOnUse"
			gradientTransform="matrix(200,0,0,100,200,100)"
		>
			<stop offset="0" stop-color="#ffffff"/>
			<stop offset="1" stop-color="#000000"/>
		</radialGradient>
	</defs>
	<rect x="0" y="0" width="400" height="200" fill="url(#rg2)"/>
</svg>
)SVG";

static const std::string SVG_THREE_TRIANGLES = R"(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 300 300">
	<path fill="red" d="M 0,0 L 100,0 L 0,100 Z"/>
	<path fill="green" d="M 100,100 L 200,100 L 100,200 Z"/>
	<path fill="blue" d="M 200,200 L 300,200 L 200,300 Z"/>
</svg>
)";

// --- Parity fixtures: the canvas-recording subset both loaders support. ---

// Rects, circle, ellipse, fill-opacity, transform matrices, cubic paths.
static const std::string SVG_PARITY_SHAPES = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" width="256" height="128" viewBox="0 0 256 128">
	<rect x="0" y="0" width="256" height="128" fill="#203040"/>
	<rect x="8" y="8" width="60" height="40" fill="#e0c020"/>
	<circle cx="120" cy="40" r="30" fill="#30a0e0" fill-opacity="0.6"/>
	<ellipse cx="200" cy="40" rx="40" ry="20" fill="#e04060"/>
	<g transform="matrix(0.8,0.3,-0.3,0.8,60,70)">
		<rect x="0" y="0" width="50" height="30" fill="#80ff80"/>
	</g>
	<path d="M150 120 C 170 70, 230 70, 250 120 Z" fill="#ffffff" fill-opacity="0.8"/>
	<path fill-rule="evenodd" fill="#ff8000" d="M 100 80 h 40 v 40 h -40 Z M 110 90 h 20 v 20 h -20 Z"/>
</svg>
)SVG";

// userSpaceOnUse linear + radial gradients (with a gradientTransform) on transformed shapes.
static const std::string SVG_PARITY_GRADIENTS = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200" viewBox="0 0 200 200">
	<defs>
		<linearGradient id="lin" gradientUnits="userSpaceOnUse" x1="10" y1="10" x2="190" y2="90">
			<stop offset="0" stop-color="#ff0000"/>
			<stop offset="0.5" stop-color="#ffff00"/>
			<stop offset="1" stop-color="#0000ff"/>
		</linearGradient>
		<radialGradient id="rad" gradientUnits="userSpaceOnUse" cx="100" cy="150" r="40"
			gradientTransform="translate(100 150) scale(1.5 0.8) translate(-100 -150)">
			<stop offset="0" stop-color="#ffffff"/>
			<stop offset="1" stop-color="#008000"/>
		</radialGradient>
	</defs>
	<rect x="10" y="10" width="180" height="80" fill="url(#lin)"/>
	<g transform="translate(5 0)">
		<rect x="20" y="105" width="160" height="90" fill="url(#rad)"/>
	</g>
</svg>
)SVG";

// Features nanosvg does not support (or renders differently): strokes with caps/joins/dashes,
// clip paths, group opacity, nested transforms. Compared against ThorVG's own rasterizer.
static const std::string SVG_RASTER_STROKES = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200" viewBox="0 0 200 200">
	<rect x="0" y="0" width="200" height="200" fill="#ffffff"/>
	<polyline points="20,30 100,30 60,90" fill="none" stroke="#c03030" stroke-width="10"
		stroke-linejoin="round" stroke-linecap="round"/>
	<rect x="120" y="20" width="60" height="60" fill="#3060c0" stroke="#000000" stroke-width="6"
		stroke-linejoin="miter"/>
	<path d="M 20 130 C 60 100, 100 160, 180 120" fill="none" stroke="#208020" stroke-width="8"
		stroke-dasharray="20 10" stroke-linecap="butt"/>
	<g transform="translate(100 170) rotate(30) scale(2 1)">
		<line x1="-20" y1="0" x2="20" y2="0" stroke="#806000" stroke-width="5"/>
	</g>
</svg>
)SVG";

static const std::string SVG_RASTER_CLIP_OPACITY = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200" viewBox="0 0 200 200">
	<defs>
		<clipPath id="clip">
			<circle cx="100" cy="100" r="60"/>
		</clipPath>
	</defs>
	<rect x="0" y="0" width="200" height="200" fill="#202020"/>
	<g clip-path="url(#clip)">
		<rect x="40" y="40" width="80" height="120" fill="#f0a000"/>
		<rect x="120" y="40" width="40" height="120" fill="#00a0f0"/>
	</g>
	<g opacity="0.5">
		<rect x="10" y="150" width="60" height="40" fill="#ff00ff"/>
	</g>
	<g transform="translate(150 20)">
		<g transform="scale(0.5)">
			<rect x="0" y="0" width="80" height="80" fill="#00ff00"/>
		</g>
	</g>
</svg>
)SVG";

// =============================================================================
// test_Shape (mirror of slughorn-test-nanosvg.cpp)
// =============================================================================

void test_Shape() {
	std::cout << "\n=== test_Shape ===" << std::endl;

	slughorn::Atlas atlas;
	slughorn::KeyIterator keys;
	slughorn::thorvg::LoadConfig cfg;

	auto composite = slughorn::thorvg::loadString(SVG_TRIANGLE, atlas, keys, 96_cv, &cfg);

	check("1 layer loaded", composite.layers.size() == 1);
	checkNear("cfg.width == 100", cfg.width, 100_cv);
	checkNear("cfg.heightEm == 1", cfg.heightEm, 1_cv);

	if(composite.layers.empty()) return;

	const auto& layer = composite.layers[0];

	checkNear("transform.x == 0", layer.transform.x, 0_cv);
	checkNear("transform.y == 0", layer.transform.y, 0_cv);
	check("color is red", layer.color.r > 0.99_cv && layer.color.g < 0.01_cv && layer.color.b < 0.01_cv);

	const auto pre = atlas.getShape(layer.key);

	check("curve count >= 3", pre && pre->curves.size() >= 3);

	bool inRange = true;

	if(pre) for(const auto& c : pre->curves) {
		for(slug_t v : {c.x1, c.y1, c.x2, c.y2, c.x3, c.y3}) if(v < -1e-3_cv || v > 1.001_cv) inRange = false;
	}

	check("all curves in [0,1]", inRange);

	atlas.build();

	const auto s = atlas.getShape(layer.key);

	check("shape in atlas", s.has_value());

	if(s) {
		checkNear("width == 1", s->width, 1_cv);
		checkNear("height == 1", s->height, 1_cv);
		checkNear("bearingX == 0", s->bearingX, 0_cv);
		checkNear("bearingY == 1", s->bearingY, 1_cv);

		auto q = s->computeQuad(layer.transform);

		checkNear("quad.x0 == 0", q.x0, 0_cv);
		checkNear("quad.y0 == 0", q.y0, 0_cv);
		checkNear("quad.x1 == 1", q.x1, 1_cv);
		checkNear("quad.y1 == 1", q.y1, 1_cv);
	}
}

// =============================================================================
// test_Gradients (mirror of slughorn-test-nanosvg.cpp; same expected B matrices)
// =============================================================================

void test_Gradients() {
	auto testGradient = [](const char* label, std::string_view svg, auto&& checks) {
		std::cout << "\n=== test_Gradients (" << label << ") ===" << std::endl;

		slughorn::Atlas atlas;
		slughorn::KeyIterator keys;

		auto composite = slughorn::thorvg::loadString(svg, atlas, keys);

		check("1 layer loaded", composite.layers.size() == 1);
		check("layer has gradientId", composite.layers.size() >= 1 && composite.layers[0].gradientId != 0);

		atlas.build();

		const auto& grads = atlas.getGradients();

		check("one gradient registered", grads.size() == 1);

		if(!grads.empty()) checks(grads[0]);
	};

	testGradient("linear", SVG_LINEAR_GRADIENT, [](const auto& g) {
		check("type == Linear", g.type == slughorn::GradientInfo::Type::Linear);
		check("2 stops", g.stops.size() == 2);

		if(g.stops.size() >= 2) {
			checkNear("stop[0].t == 0", g.stops[0].t, 0_cv);
			checkNear("stop[0].r == 1", g.stops[0].color.r, 1_cv);
			checkNear("stop[1].t == 1", g.stops[1].t, 1_cv);
			checkNear("stop[1].b == 1", g.stops[1].color.b, 1_cv);
		}

		// t = x on the unit-em square.
		checkNear("t(0,0.5) == 0", g.transform.xx * 0_cv + g.transform.xy * 0.5_cv + g.transform.dx, 0_cv);
		checkNear("t(1,0.5) == 1", g.transform.xx * 1_cv + g.transform.xy * 0.5_cv + g.transform.dx, 1_cv);
	});

	testGradient("radial square", SVG_RADIAL_GRADIENT, [](const auto& g) {
		check("type == AffineRadial", g.type == slughorn::GradientInfo::Type::AffineRadial);
		checkNear("b01 ~= 0", g.transform.xy, 0_cv);
		checkNear("b10 ~= 0", g.transform.yx, 0_cv);
		check("b00 > 0", g.transform.xx > 0_cv);
		checkNear("b00 == b11", g.transform.xx, g.transform.yy);
		checkNear("b00 ~= 2", g.transform.xx, 2_cv, 0.01_cv);
	});

	testGradient("radial non-square", SVG_RADIAL_NON_SQUARE, [](const auto& g) {
		check("type == AffineRadial", g.type == slughorn::GradientInfo::Type::AffineRadial);
		checkNear("b00 ~= 2", g.transform.xx, 2_cv, 0.01_cv);
		checkNear("b11 ~= 4", g.transform.yy, 4_cv, 0.01_cv);
		checkNear("b01 ~= 0", g.transform.xy, 0_cv);
		checkNear("b10 ~= 0", g.transform.yx, 0_cv);
		checkNear("center.x ~= 0.5", g.transform.dx, 0.5_cv, 0.01_cv);
		checkNear("center.y ~= 0.25", g.transform.dy, 0.25_cv, 0.01_cv);
	});

	testGradient("radial OBB non-square", SVG_RADIAL_OBB_NON_SQUARE, [](const auto& g) {
		check("type == AffineRadial", g.type == slughorn::GradientInfo::Type::AffineRadial);
		check("b00 != b11 (anisotropic)", std::abs(g.transform.xx - g.transform.yy) > 1e-3_cv);
		checkNear("b00 ~= 2", g.transform.xx, 2_cv, 0.05_cv);
		checkNear("b11 ~= 4", g.transform.yy, 4_cv, 0.05_cv);
		checkNear("center.x ~= 0.5", g.transform.dx, 0.5_cv, 0.01_cv);
		checkNear("center.y ~= 0.25", g.transform.dy, 0.25_cv, 0.01_cv);
	});
}

// =============================================================================
// test_CompositeShape (mirror of slughorn-test-nanosvg.cpp)
// =============================================================================

void test_CompositeShape() {
	std::cout << "\n=== test_CompositeShape ===" << std::endl;

	slughorn::Atlas atlas;
	slughorn::KeyIterator keys;

	auto composite = slughorn::thorvg::loadString(SVG_THREE_TRIANGLES, atlas, keys);

	atlas.build();

	check("3 layers loaded", composite.layers.size() == 3);

	const slug_t third = 1_cv / 3_cv;
	const slug_t offsets[3] = { 0_cv, third, 2_cv * third };

	for(size_t i = 0; i < composite.layers.size() && i < 3; i++) {
		const auto& layer = composite.layers[i];
		const auto s = atlas.getShape(layer.key);
		const std::string n = std::to_string(i);

		check(("shape in atlas [" + n + "]").c_str(), s.has_value());

		if(!s) continue;

		checkNear(("width ~= 1/3 [" + n + "]").c_str(), s->width, third);
		checkNear(("height ~= 1/3 [" + n + "]").c_str(), s->height, third);
		checkNear(("transform.x [" + n + "]").c_str(), layer.transform.x, offsets[i]);
		checkNear(("transform.y [" + n + "]").c_str(), layer.transform.y, offsets[i]);
	}
}

// =============================================================================
// test_IdsRulesStrokes
// =============================================================================

void test_IdsRulesStrokes() {
	std::cout << "\n=== test_IdsRulesStrokes ===" << std::endl;

	const std::string svg = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 300 100">
	<rect id="keep" x="0" y="0" width="100" height="100" fill="#ff0000"/>
	<rect id="exclude" x="100" y="0" width="100" height="100" fill="#00ff00"/>
	<rect id="stroked" x="210" y="10" width="80" height="80" fill="#0000ff" stroke="#000000" stroke-width="4"/>
</svg>
)SVG";

	slughorn::Atlas atlas;
	slughorn::KeyIterator keys;
	auto cfg = quietConfig();

	cfg.rules.push_back({std::regex("exclude"), slughorn::thorvg::ShapePolicy::ForceExclude});

	auto composite = slughorn::thorvg::loadString(svg, atlas, keys, 96_cv, &cfg);

	check("3 layers (keep, stroked fill, stroked stroke)", composite.layers.size() == 3);
	check("layer info parallel to layers", cfg.layers.size() == composite.layers.size());

	if(composite.layers.size() == 3 && cfg.layers.size() == 3) {
		check("key 'keep'", composite.layers[0].key == slughorn::Key("keep"));
		check("key 'stroked'", composite.layers[1].key == slughorn::Key("stroked"));
		check("key 'stroked.stroke'", composite.layers[2].key == slughorn::Key("stroked.stroke"));
		check("layer 2 is a stroke", cfg.layers[2].stroke && !cfg.layers[1].stroke);
		check("id recorded", cfg.layers[1].id == "stroked");

		// Stroke outline spans the rect +- half the width: x 208..292 px.
		const auto s = atlas.getShape(composite.layers[2].key);

		if(s) {
			atlas.build();

			const auto b = atlas.getShape(composite.layers[2].key);

			checkNear("stroke width em ~= 84/300", b->width, 84_cv / 300_cv, 2e-3_cv);
			checkNear("stroke x em ~= 208/300", composite.layers[2].transform.x, 208_cv / 300_cv, 2e-3_cv);
		}
	}
}

// =============================================================================
// Image comparison helpers
// =============================================================================

struct Diff {
	double mean = 0.0;   // mean |delta| over all RGBA channels
	double bad = 0.0;    // fraction of pixels whose max channel |delta| > 0.25
	size_t flips = 0;    // pixels whose max channel |delta| > 0.5
	double maxd = 0.0;   // largest channel |delta| anywhere
};

static Diff compare(const std::vector<slug_t>& a, const std::vector<slug_t>& b) {
	Diff d;

	if(a.size() != b.size() || a.empty()) return {1.0, 1.0, a.size(), 1.0};

	size_t badCount = 0;

	for(size_t p = 0; p < a.size() / 4; p++) {
		double mx = 0.0;

		for(size_t c = 0; c < 4; c++) {
			const double e = std::abs(double(a[p * 4 + c]) - double(b[p * 4 + c]));

			d.mean += e;
			mx = std::max(mx, e);
		}

		if(mx > 0.25) badCount++;
		if(mx > 0.5) d.flips++;
		d.maxd = std::max(d.maxd, mx);
	}

	d.mean /= double(a.size());
	d.bad = double(badCount) / double(a.size() / 4);

	return d;
}

static std::vector<slug_t> slugRender(
	const slughorn::Atlas& atlas,
	const slughorn::CompositeShape& composite,
	slug_t heightEm,
	uint32_t w,
	uint32_t h
) {
	return slughorn::render::renderComposite(atlas, composite, w, h, 0_cv, 0_cv, 1_cv, heightEm).data;
}

// ThorVG's own software rasterizer (premultiplied RGBA floats), the ground truth for features
// nanosvg lacks.
static std::vector<slug_t> thorvgRaster(const std::string& svg, uint32_t w, uint32_t h) {
	std::vector<slug_t> out(size_t(w) * h * 4, 0_cv);

	if(tvg::Initializer::init(0) != tvg::Result::Success) return out;

	{
		std::vector<uint32_t> buffer(size_t(w) * h, 0u);

		tvg::SwCanvas* canvas = tvg::SwCanvas::gen();

		canvas->target(buffer.data(), w, w, h, tvg::ColorSpace::ABGR8888);

		tvg::Picture* picture = tvg::Picture::gen();

		picture->load(svg.data(), static_cast<uint32_t>(svg.size()), "svg", nullptr, true);
		picture->size(float(w), float(h));

		canvas->add(picture);
		canvas->draw(true);
		canvas->sync();

		for(size_t i = 0; i < buffer.size(); i++) {
			const uint32_t px = buffer[i];

			out[i * 4 + 0] = cv(px & 0xFF) / 255_cv;
			out[i * 4 + 1] = cv((px >> 8) & 0xFF) / 255_cv;
			out[i * 4 + 2] = cv((px >> 16) & 0xFF) / 255_cv;
			out[i * 4 + 3] = cv((px >> 24) & 0xFF) / 255_cv;
		}

		delete canvas;
	}

	tvg::Initializer::term();

	return out;
}

static void reportDiff(const char* what, const Diff& d) {
	std::cout << "  " << what << ": mean|d|=" << d.mean << " bad=" << (d.bad * 100.0) << "%" << std::endl;
}

// =============================================================================
// test_ParityNanoSVG - thorvg.hpp vs nanosvg.hpp through the same CPU Slug decoder
// =============================================================================

#ifdef SLUGHORN_HAS_NANOSVG
void test_ParityNanoSVG() {
	std::cout << "\n=== test_ParityNanoSVG ===" << std::endl;

	struct Case { const char* name; const std::string* svg; };

	const Case cases[] = {
		{"triangle", &SVG_TRIANGLE},
		{"three triangles", &SVG_THREE_TRIANGLES},
		{"shapes", &SVG_PARITY_SHAPES},
		{"gradients", &SVG_PARITY_GRADIENTS},
		{"linear OBB", &SVG_LINEAR_GRADIENT},
		{"radial non-square", &SVG_RADIAL_NON_SQUARE},
	};

	for(const auto& c : cases) {
		slughorn::Atlas ta, na;
		slughorn::KeyIterator tk, nk;
		slughorn::thorvg::LoadConfig tcfg;
		slughorn::nanosvg::LoadConfig ncfg;

		auto tc = slughorn::thorvg::loadString(*c.svg, ta, tk, 96_cv, &tcfg);
		auto nc = slughorn::nanosvg::loadString(*c.svg, na, nk, 96_cv, &ncfg);

		ta.build();
		na.build();

		const uint32_t W = 128;
		const auto H = static_cast<uint32_t>(std::lround(W * tcfg.heightEm));

		const auto ti = slugRender(ta, tc, tcfg.heightEm, W, H);
		const auto ni = slugRender(na, nc, ncfg.heightEm, W, H);

		const Diff d = compare(ti, ni);

		std::cout << "  [" << c.name << "] layers thorvg=" << tc.layers.size() << " nanosvg=" << nc.layers.size() << std::endl;
		reportDiff(c.name, d);

		check((std::string(c.name) + ": same layer count").c_str(), tc.layers.size() == nc.layers.size());
		check((std::string(c.name) + ": mean |delta| < 0.01").c_str(), d.mean < 0.01);
		check((std::string(c.name) + ": < 1% pixels differ by > 0.25").c_str(), d.bad < 0.01);
	}

	// Negative control: a shape moved by 6 px must FAIL the same thresholds, proving the metric
	// discriminates (this is the check that can fail).
	{
		const std::string moved = R"(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
	<path fill="red" d="M 6,0 L 100,0 L 6,100 Z"/>
</svg>
)";

		slughorn::Atlas ta, na;
		slughorn::KeyIterator tk, nk;

		auto tc = slughorn::thorvg::loadString(SVG_TRIANGLE, ta, tk);
		auto nc = slughorn::nanosvg::loadString(moved, na, nk);

		ta.build();
		na.build();

		const Diff d = compare(slugRender(ta, tc, 1_cv, 128, 128), slugRender(na, nc, 1_cv, 128, 128));

		reportDiff("negative control (6px shift)", d);
		check("negative control exceeds tolerance", d.mean >= 0.01 || d.bad >= 0.01);
	}
}
#endif

// =============================================================================
// test_ParityRaster - thorvg.hpp through Slug vs ThorVG's own rasterizer
// =============================================================================

void test_ParityRaster() {
	std::cout << "\n=== test_ParityRaster ===" << std::endl;

	struct Case { const char* name; const std::string* svg; bool needsClipper; };

	const Case cases[] = {
		{"shapes", &SVG_PARITY_SHAPES, false},
		{"gradients", &SVG_PARITY_GRADIENTS, false},
		{"strokes", &SVG_RASTER_STROKES, false},
		{"clip + group opacity", &SVG_RASTER_CLIP_OPACITY, true},
	};

	for(const auto& c : cases) {
#ifndef SLUGHORN_HAS_CLIPPER2
		if(c.needsClipper) {
			std::cout << "  [" << c.name << "] skipped (needs SLUGHORN_CLIPPER2)" << std::endl;

			continue;
		}
#endif

		slughorn::Atlas atlas;
		slughorn::KeyIterator keys;
		auto cfg = quietConfig();

		auto comp = slughorn::thorvg::loadString(*c.svg, atlas, keys, 96_cv, &cfg);

		atlas.build();

		const uint32_t W = 160;
		const auto H = static_cast<uint32_t>(std::lround(W * cfg.heightEm));

		const Diff d = compare(slugRender(atlas, comp, cfg.heightEm, W, H), thorvgRaster(*c.svg, W, H));

		std::cout << "  [" << c.name << "] layers=" << comp.layers.size() << std::endl;
		reportDiff(c.name, d);

		check((std::string(c.name) + ": mean |delta| < 0.02").c_str(), d.mean < 0.02);
		check((std::string(c.name) + ": < 3% pixels differ by > 0.25").c_str(), d.bad < 0.03);
	}
}

// =============================================================================
// test_MisWound - fill-rule-robust tessellation of a mis-wound glyph-like contour
// =============================================================================

#if defined(SLUGHORN_HAS_CLIPPER2) && defined(SLUGHORN_HAS_TESSELLATE)

static double meshArea(const slughorn::tessellate::Mesh2D& m) {
	double a = 0.0;

	for(size_t i = 0; i + 2 < m.indices.size(); i += 3) {
		const auto p = [&](uint32_t k) { return std::pair<double, double>{m.positions[k * 2], m.positions[k * 2 + 1]}; };
		const auto [ax, ay] = p(m.indices[i]);
		const auto [bx, by] = p(m.indices[i + 1]);
		const auto [cx, cy] = p(m.indices[i + 2]);

		a += std::abs((bx - ax) * (cy - ay) - (cx - ax) * (by - ay)) * 0.5;
	}

	return a;
}

static slughorn::Atlas::Curves square(slug_t x0, slug_t y0, slug_t x1, slug_t y1, bool ccw) {
	slughorn::Atlas::Curves c;
	slughorn::CurveDecomposer d(c);

	d.moveTo(x0, y0);

	if(ccw) { d.lineTo(x1, y0); d.lineTo(x1, y1); d.lineTo(x0, y1); }
	else { d.lineTo(x0, y1); d.lineTo(x1, y1); d.lineTo(x1, y0); }

	d.close();

	return c;
}

void test_MisWound() {
	std::cout << "\n=== test_MisWound ===" << std::endl;

	// A glyph-like "O": 10x10 outline with a 4x4 counter (expected area 100 - 16 = 84).
	// Mis-wound #1: the counter is wound the SAME way as the outline (a common font defect).
	const slughorn::Atlas::Contours sameWinding = {square(0, 0, 10, 10, true), square(3, 3, 7, 7, true)};

	// Mis-wound #2: whole glyph reversed (outline clockwise, counter counter-clockwise).
	const slughorn::Atlas::Contours reversed = {square(0, 0, 10, 10, false), square(3, 3, 7, 7, true)};

	using slughorn::FillRule;
	namespace tess = slughorn::tessellate;

	const double naive1 = meshArea(tess::tessellate(sameWinding, 1e-3_cv));
	const double naive2 = meshArea(tess::tessellate(reversed, 1e-3_cv));
	const double eo1 = meshArea(tess::tessellate(sameWinding, 1e-3_cv, FillRule::EvenOdd));
	const double nz2 = meshArea(tess::tessellate(reversed, 1e-3_cv, FillRule::NonZero));
	const double eo2 = meshArea(tess::tessellate(reversed, 1e-3_cv, FillRule::EvenOdd));
	const double nz1 = meshArea(tess::tessellate(sameWinding, 1e-3_cv, FillRule::NonZero));

	std::cout << "  signed-area tessellate: same-winding=" << naive1 << " reversed=" << naive2 << std::endl;

	check("signed-area classification is fooled by mis-winding (documents the bug)", std::abs(naive1 - 84.0) > 1.0 || std::abs(naive2 - 84.0) > 1.0);
	checkNear("even-odd: same-winding counter is a hole (84)", cv(eo1), 84_cv, 1e-2_cv);
	checkNear("even-odd: reversed glyph (84)", cv(eo2), 84_cv, 1e-2_cv);
	checkNear("nonzero: reversed glyph keeps its counter (84)", cv(nz2), 84_cv, 1e-2_cv);
	checkNear("nonzero: same-winding counter fills (100)", cv(nz1), 100_cv, 1e-2_cv);

	// Overlapping contours must resolve per the rule, never double-cover: same-wound overlap is a
	// union under nonzero (36 + 36 - 9 = 63); opposite-wound overlap cancels to winding 0
	// (72 - 18 = 54), as does even-odd for either.
	const slughorn::Atlas::Contours overlapSame = {square(0, 0, 6, 6, true), square(3, 3, 9, 9, true)};
	const slughorn::Atlas::Contours overlapOpp = {square(0, 0, 6, 6, true), square(3, 3, 9, 9, false)};

	checkNear("nonzero same-wound overlap = union (63)", cv(meshArea(tess::tessellate(overlapSame, 1e-3_cv, FillRule::NonZero))), 63_cv, 1e-2_cv);
	checkNear("nonzero opposite-wound overlap cancels (54)", cv(meshArea(tess::tessellate(overlapOpp, 1e-3_cv, FillRule::NonZero))), 54_cv, 1e-2_cv);
	checkNear("even-odd overlap (54)", cv(meshArea(tess::tessellate(overlapSame, 1e-3_cv, FillRule::EvenOdd))), 54_cv, 1e-2_cv);
}

// =============================================================================
// test_Bake - merge + planar mesh + cost
// =============================================================================

void test_Bake() {
	std::cout << "\n=== test_Bake ===" << std::endl;

	// A noise-loop-like key: a background, 200 2px speckles in two colors interleaved, and an
	// opaque bar on top that occludes part of the speckles, plus a translucent overlay.
	std::string svg = R"(<svg xmlns="http://www.w3.org/2000/svg" width="128" height="64" viewBox="0 0 128 64">)";

	svg += R"(<rect x="0" y="0" width="128" height="64" fill="#336699"/>)";

	for(int i = 0; i < 200; i++) {
		const int x = (i * 37) % 120;
		const int y = (i * 53) % 56 + (i % 2) * 3;

		svg += "<rect x=\"" + std::to_string(x) + "\" y=\"" + std::to_string(y) +
			"\" width=\"2\" height=\"2\" fill=\"" + (i % 2 ? "#ffffff" : "#000000") + "\"/>";
	}

	svg += R"(<rect x="40" y="20" width="48" height="24" fill="#ff0000"/>)";
	svg += R"(<rect x="0" y="0" width="64" height="64" fill="#00ff00" fill-opacity="0.5"/>)";
	svg += "</svg>";

	slughorn::Atlas staging;
	slughorn::KeyIterator skeys;
	auto cfg = quietConfig();

	auto loaded = slughorn::thorvg::loadString(svg, staging, skeys, 96_cv, &cfg);

	std::vector<slughorn::bake::LayerSource> meta;

	for(const auto& li : cfg.layers) meta.push_back({li.fillRule, li.stroke, static_cast<uint8_t>(li.spread)});

	slughorn::Atlas atlas;
	slughorn::KeyIterator keys("noise");

	auto merged = slughorn::bake::mergeLayers(staging, loaded, meta, atlas, keys);

	std::cout << "  layers before=" << merged.layersBefore << " after=" << merged.layersAfter << std::endl;

	check("203 layers loaded", merged.layersBefore == 203);
	check("speckles merged (<= 6 layers)", merged.layersAfter <= 6);

	atlas.build();

	// Merging must not change the rendered result.
	{
		slughorn::Atlas ref;
		slughorn::KeyIterator rk;
		auto rc = slughorn::thorvg::loadString(svg, ref, rk, 96_cv, nullptr);

		ref.build();

		const Diff d = compare(slugRender(atlas, merged.composite, 0.5_cv, 128, 64), slugRender(ref, rc, 0.5_cv, 128, 64));

		reportDiff("merged vs unmerged render", d);
		check("merge preserves the image", d.mean < 0.002 && d.bad < 0.002);
	}

	slughorn::bake::BakeConfig bc;

	bc.width = cfg.width;
	bc.height = cfg.height;
	bc.tolerancePx = 0.25_cv;

	const auto mesh = slughorn::bake::bakeMesh(atlas, merged.composite, merged.layers, bc);

	std::cout << "  triangles before=" << mesh.trianglesBefore << " after=" << mesh.trianglesAfter
		<< " opaque idx=" << mesh.opaqueIndexCount << " overlay idx=" << mesh.overlayIndexCount << std::endl;

	// Planarity: the opaque triangles tile the full 128x64 rect exactly once (UV area 1).
	double opaqueArea = 0.0, overlayArea = 0.0;

	for(size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
		const auto P = [&](uint32_t k) { return std::pair<double, double>{mesh.positions[k * 2], mesh.positions[k * 2 + 1]}; };
		const auto [ax, ay] = P(mesh.indices[i]);
		const auto [bx, by] = P(mesh.indices[i + 1]);
		const auto [cx, cy] = P(mesh.indices[i + 2]);
		const double a = std::abs((bx - ax) * (cy - ay) - (cx - ax) * (by - ay)) * 0.5;

		(i < mesh.opaqueIndexCount ? opaqueArea : overlayArea) += a;
	}

	checkNear("opaque set covers UV area exactly once (1.0)", cv(opaqueArea), 1_cv, 1e-3_cv);
	checkNear("overlay is the translucent half (0.5)", cv(overlayArea), 0.5_cv, 1e-3_cv);
	check("one paint per surviving layer", mesh.paints.size() <= merged.layersAfter);
	check("vertex streams consistent", mesh.positions.size() == mesh.params.size() && mesh.paintIds.size() * 2 == mesh.positions.size());

	const auto c = slughorn::bake::cost(atlas, merged.composite, mesh, merged.layersBefore);

	std::cout << "  cost: curves=" << c.curves << " maxBandH=" << c.maxBandCurvesH << " maxBandV=" << c.maxBandCurvesV
		<< " slugWork=" << c.slugWork << " mode=" << c.mode << std::endl;

	check("cost has a mode", c.mode == "mesh" || c.mode == "slug" || c.mode == "mean");

	// Gradient parameter: linear t at every vertex equals x / width for a left->right gradient.
	{
		slughorn::Atlas gs, ga;
		slughorn::KeyIterator gk, gk2("g");
		slughorn::thorvg::LoadConfig gcfg;

		auto gl = slughorn::thorvg::loadString(SVG_LINEAR_GRADIENT, gs, gk, 96_cv, &gcfg);

		std::vector<slughorn::bake::LayerSource> gm(gl.layers.size());

		auto gmerged = slughorn::bake::mergeLayers(gs, gl, gm, ga, gk2);

		ga.build();

		slughorn::bake::BakeConfig gbc;

		gbc.width = gcfg.width;
		gbc.height = gcfg.height;

		const auto gmesh = slughorn::bake::bakeMesh(ga, gmerged.composite, gmerged.layers, gbc);

		double maxErr = 0.0;

		for(size_t v = 0; v < gmesh.paintIds.size(); v++) {
			maxErr = std::max(maxErr, std::abs(double(gmesh.params[v * 2]) - double(gmesh.positions[v * 2])));
		}

		check("linear paint emitted", !gmesh.paints.empty() && gmesh.paints[0].type == slughorn::bake::Paint::Type::Linear);
		checkNear("per-vertex t == u (max error)", cv(maxErr), 0_cv, 1e-4_cv);
	}
}
#endif

#if defined(SLUGHORN_HAS_CLIPPER2) && defined(SLUGHORN_HAS_TESSELLATE)
#include "slughorn-test-thorvg-stamp.inl"
#include "slughorn-test-thorvg-sheet.inl"
#endif

// =============================================================================
// dumpSVGFile
// =============================================================================

void dumpSVGFile(const std::filesystem::path& path) {
	std::cerr << "=== SVG dump: " << path << " ===" << std::endl;

	slughorn::Atlas atlas;
	slughorn::KeyIterator keys;

	auto composite = slughorn::thorvg::loadFile(path, atlas, keys);

	atlas.addCompositeShape(slughorn::Key("composite"), composite);
	atlas.build();

	std::cerr << "PackingStats: " << atlas.getPackingStats() << std::endl;

	slughorn::serial::writeJSON(atlas, std::cout);

	std::cout << std::endl;
}

// =============================================================================
// main
// =============================================================================

// Compare mode: each SVG through thorvg.hpp + Slug (CPU) vs ThorVG's own rasterizer, at 256 px
// wide. Prints one line per file; exit status 1 if any file exceeds the raster-parity tolerance.
int compareSVGFiles(int argc, char** argv) {
	int failures = 0;

	for(int i = 2; i < argc; i++) {
		std::ifstream in(argv[i], std::ios::binary);
		std::stringstream ss;

		ss << in.rdbuf();

		const std::string svg = ss.str();

		slughorn::Atlas atlas;
		slughorn::KeyIterator keys("k", true);
		auto cfg = quietConfig();
		std::vector<std::string> warnings;

		cfg.log = [&](int, std::string_view m) { warnings.emplace_back(m); };

		auto comp = slughorn::thorvg::loadString(svg, atlas, keys, 96_cv, &cfg);

		if(cfg.width <= 0_cv) {
			std::cout << argv[i] << ": LOAD FAILED" << std::endl;
			failures++;

			continue;
		}

		atlas.build();

		const uint32_t W = 256;
		const auto H = static_cast<uint32_t>(std::max<long>(1, std::lround(W * cfg.heightEm)));
		const Diff d = compare(slugRender(atlas, comp, cfg.heightEm, W, H), thorvgRaster(svg, W, H));
		const bool ok = d.mean < 0.02 && d.bad < 0.03;

		if(!ok) failures++;

		std::cout << (ok ? "ok   " : "FAIL ") << std::filesystem::path(argv[i]).filename().string()
			<< " layers=" << comp.layers.size()
			<< " mean|d|=" << d.mean << " bad=" << (d.bad * 100.0) << "%"
			<< " warnings=" << warnings.size() << std::endl;
	}

	return failures ? 1 : 0;
}

int main(int argc, char** argv) {
	if(argc >= 3 && std::string(argv[1]) == "--compare") return compareSVGFiles(argc, argv);
#if defined(SLUGHORN_HAS_CLIPPER2) && defined(SLUGHORN_HAS_TESSELLATE)
	if(argc >= 3 && std::string(argv[1]) == "--stamp-compare") return stampCompareFiles(argc, argv);
	if(argc >= 4 && std::string(argv[1]) == "--contact-sheet") return contactSheets(argc, argv);
#endif

	if(argc >= 2) {
		for(int i = 1; i < argc; i++) dumpSVGFile(argv[i]);

		return 0;
	}

	test_Shape();
	test_Gradients();
	test_CompositeShape();
	test_IdsRulesStrokes();
#ifdef SLUGHORN_HAS_NANOSVG
	test_ParityNanoSVG();
#endif
	test_ParityRaster();
#if defined(SLUGHORN_HAS_CLIPPER2) && defined(SLUGHORN_HAS_TESSELLATE)
	test_MisWound();
	test_Bake();
	test_StampThorvgCaveats();
	test_Stamp();
	test_Cutout();
#endif

	std::cout
		<< "\n=== Results: "
		<< s_pass << " passed, "
		<< s_fail << " failed ==="
		<< std::endl
	;

	return s_fail > 0 ? 1 : 0;
}
