#pragma once

// ================================================================================================
// ThorVG backend for slughorn
//
// Loads SVG files/strings with ThorVG (the vector library Godot
// embeds) and converts its paint tree into slughorn Atlas shapes, producing a CompositeShape with
// one Layer per painted ThorVG Shape (fill first, then stroke), back-to-front order preserved.
//
// API mirrors slughorn/nanosvg.hpp:
//
// decomposePath() - low-level: tvg::Shape (+ its world matrix) -> (curves, transform)
// loadShape() - mid-level: decompose + register in atlas
// loadPicture() - high-level: full tvg::Picture -> CompositeShape
// loadFile() - convenience: parse file + loadPicture
// loadString() - convenience: parse string + loadPicture
//
// Coordinates follow nanosvg.hpp exactly: SVG pixel space (Y down) normalized to em-space by
// scale = 1/picture width, curves shifted to each shape's local bbox origin, with the offset in
// Layer::transform. Multiply layer.transform.x/y by cfg.width to recover authoring pixels.
//
// USAGE
// -----
// In exactly one .cpp file, before including this header:
//
// #define SLUGHORN_THORVG_IMPLEMENTATION
// #include <slughorn/thorvg.hpp>
//
// (slughorn.cpp does this when built with SLUGHORN_THORVG=ON.)
//
// WHAT IS SUPPORTED
// -----------------
// - The whole ThorVG paint tree (Scene / Shape), each Shape with its accumulated transform
//   (every ancestor's Paint::transform(), including the viewBox scale ThorVG puts on the root)
//   and accumulated opacity (product of every ancestor's Paint::opacity()), visibility honored
// - Path commands MoveTo / LineTo / CubicTo / Close (cubics -> quadratics via CurveDecomposer);
//   rect / circle / ellipse / polygon all arrive as path commands
// - Fill rule: even-odd converted to nonzero at load time exactly like nanosvg.hpp (ray-cast
//   winding reversal of inner sub-paths); the authored rule is reported per layer in
//   LoadConfig::layers for consumers that rebuild regions (clipper.hpp, bake.hpp)
// - Solid fills (fill + fill-opacity), linear gradients, radial gradients (incl. elliptical via
//   gradientTransform / non-uniform paint transform), stop colors/opacity
// - Strokes (stroke-width / linecap / linejoin / miterlimit / dasharray / dashoffset) expanded
//   to fill contours with slughorn's own canvas::Path::strokePath(), stroked in the shape's
//   LOCAL space and then transformed (so non-uniform scale gives the correct elliptic pen).
//   Each stroke becomes its own layer after the fill layer.
// - Clip paths (SVG clipPath, ThorVG Paint::clip()) when built with SLUGHORN_CLIPPER2=ON: the clip
//   region is intersected into the layer geometry on the CPU (Clipper2), so clipped layers are
//   piecewise-linear at LoadConfig::clipTolerancePx. Without Clipper2 clips are skipped + logged.
// - Masks (SVG <mask>, Paint::mask()) with Clipper2: APPROXIMATED as a hard clip by the union of
//   the mask's shapes (alpha / luminance gradation is ignored; InvAlpha / InvLuma subtract);
//   logged. Without Clipper2 masks are skipped + logged.
//
// WHAT IS NOT SUPPORTED
// ---------------------
// - Gradient spread reflect / repeat (rendered as pad; reported in LayerInfo::spread)
// - Radial focal point (fx/fy != cx/cy): approximated by a centered radial; logged
// - Group opacity is multiplied into each child (exact only when the children do not overlap;
//   ThorVG itself composites the group offscreen)
// - Blend modes, scene effects / SVG filters (blur, drop shadow) - silently ignored (ThorVG's
//   public API exposes neither for reading)
// - Text (tvg::Text paints) and raster images (<image>) - skipped + logged. Text already
//   converted to paths is ordinary geometry and is supported.
// - Units: ThorVG resolves physical units itself; the dpi parameter is accepted for API parity
//   with nanosvg.hpp and ignored.
// ================================================================================================

#include "slughorn.hpp"
#include "canvas.hpp"

#include <thorvg.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <ostream>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slughorn {
namespace thorvg {

// ================================================================================================
// LogCallback / ShapePolicy / ShapeRule / LoadConfig (same shape as nanosvg.hpp)
// ================================================================================================
// The message is valid only for the duration of the callback.
using LogCallback = std::function<void(int level, std::string_view msg)>;

// Controls how a matched ShapeRule overrides default load behavior.
enum class ShapePolicy : uint32_t {
	Default = 0,
	ForceInclude = 1 << 0, // include even if paint type is unsupported (fill none -> geometry)
	ForceExclude = 1 << 1, // exclude even if paint type is supported
	GeometryOnly = 1 << 2, // add curves to atlas but mark the layer DrawMode::Geometry
};

inline ShapePolicy operator|(ShapePolicy a, ShapePolicy b) {
	return static_cast<ShapePolicy>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline bool operator&(ShapePolicy a, ShapePolicy b) {
	return static_cast<uint32_t>(a) & static_cast<uint32_t>(b);
}

// Matches SVG element ids against a regex and applies a policy override.
// Rules are evaluated in order; the first match wins. Elements without an id match against "".
struct ShapeRule {
	std::regex id;

	ShapePolicy policy = ShapePolicy::Default;

	// When set, overrides LoadConfig::origin for shapes matching this rule.
	std::optional<Atlas::ShapeInfo::Origin> origin = std::nullopt;
};

// Gradient spread as authored (slughorn's gradient strip always clamps, i.e. renders Pad).
enum class Spread : uint8_t { Pad = 0, Reflect = 1, Repeat = 2 };

// Per-layer provenance, parallel to the returned CompositeShape::layers.
struct LayerInfo {
	// SVG id of the source element ("" if it had none).
	std::string id = {};

	// Authored fill rule of the geometry in this layer (stroke layers are always NonZero). The
	// atlas curves are already converted to nonzero; even-odd is winding-agnostic, so a consumer
	// may re-normalize the layer's contours with this rule (clipper::normalize).
	FillRule fillRule = FillRule::NonZero;

	// True when the layer is a stroke expanded to fill contours.
	bool stroke = false;

	// True when a clip path / mask was intersected into the geometry.
	bool clipped = false;

	// Gradient spread as authored (Pad for solid layers).
	Spread spread = Spread::Pad;

	// Accumulated paint opacity (already multiplied into the color / gradient stop alphas).
	slug_t opacity = 1_cv;
};

struct LoadConfig {
	// Input fields.
	LogCallback log = {};

	std::vector<ShapeRule> rules = {};

	// If true (default), build() derives width/height/bearing from the actual curve bounding box.
	// If false, curves stay in full-canvas em-space and the shape spans the whole viewport.
	bool autoMetrics = true;

	// Origin applied to all shapes unless overridden by a matching ShapeRule::origin.
	Atlas::ShapeInfo::Origin origin = {};

	// Expand strokes into fill layers (canvas::Path::strokePath).
	bool strokes = true;

	// CurveDecomposer tolerance for path cubics, in em-space. TOLERANCE_EXACT (default) emits the
	// fixed two-quadratics-per-cubic leaf, matching nanosvg.hpp bit for bit (its error grows with
	// the curve: ~0.003 of a circle's radius).
	slug_t tolerance = TOLERANCE_EXACT;

	// The same tolerance in authoring pixels; when > 0 it overrides `tolerance`.
	slug_t tolerancePx = 0_cv;

	// When true, the tolerance bounds each cubic's distance to its quadratics
	// (CurveDecomposer::errorBound) rather than its flatness: the same accuracy from far fewer
	// curves.
	bool curveErrorBound = false;

	// Centerline flattening tolerance for stroke expansion, in authoring pixels.
	slug_t strokeTolerancePx = 0.05_cv;

	// Flattening tolerance for clip-path / mask booleans, in authoring pixels.
	slug_t clipTolerancePx = 0.05_cv;

	// ThorVG's SVG loader clips every document to its viewport (svgSceneBuild adds a (0, 0, w, h)
	// clip to the root scene), so every shape carries a clip, and a clipped layer is intersected
	// with Clipper2 - i.e. flattened to clipTolerancePx polylines. When true, a clip that is an
	// axis-aligned rectangle containing a nonzero layer's geometry (control-point hull) is not
	// applied: it cannot cut anything, and the layer keeps its curves. Masks, inverse masks,
	// non-rect clips and even-odd layers (which need the Clipper2 pass) are always applied.
	bool skipContainingRectClips = false;

	// Output fields.
	//
	// Populated by loadPicture/loadFile/loadString after a successful parse.
	slug_t width = 0.0f;
	slug_t height = 0.0f;
	slug_t heightEm = 0_cv;

	std::vector<LayerInfo> layers = {};
};

inline std::ostream& operator<<(std::ostream& os, const LoadConfig& c) {
	return os
		<< "LoadConfig("
		<< "width=" << c.width
		<< " height=" << c.height
		<< " heightEm=" << c.heightEm
		<< " autoMetrics=" << c.autoMetrics
		<< " layers=" << c.layers.size()
		<< ")"
	;
}

// ================================================================================================
// toMatrix - tvg::Matrix (row-major 3x3) -> slughorn::Matrix (2x3 affine; perspective dropped)
// ================================================================================================
inline Matrix toMatrix(const tvg::Matrix& m) {
	return { .xx = m.e11, .yx = m.e21, .xy = m.e12, .yy = m.e22, .dx = m.e13, .dy = m.e23 };
}

// The accumulated paint -> picture-space matrix of @p paint (its own transform included).
Matrix worldMatrix(const tvg::Paint* paint);

// ================================================================================================
// decomposePath
//
// Decompose the FILL geometry of one tvg::Shape into slughorn curves. @p world maps the shape's
// local coordinates into picture pixels (use worldMatrix(shape)); @p scale normalizes pixels into
// em-space (1/picture width). Even-odd paths are converted to nonzero. Sub-path boundaries are
// recorded in ShapeInfo::contourStarts.
//
// Returns an empty ShapeInfo if the path has no curves or a zero-area bounding box. The returned
// Transform follows nanosvg::decomposePath's origin conventions exactly.
// ================================================================================================
std::pair<Atlas::ShapeInfo, Transform> decomposePath(
	const tvg::Shape* shape,
	const Matrix& world,
	slug_t scale=1_cv,
	Atlas::ShapeInfo::Origin origin={},
	bool autoMetrics=true,
	slug_t tolerance=TOLERANCE_EXACT
);

// ================================================================================================
// loadShape - decompose + addShape (see nanosvg::loadShape for canvasHeightEm)
// ================================================================================================
std::optional<Transform> loadShape(
	const tvg::Shape* shape,
	const Matrix& world,
	Atlas& atlas,
	Key key,
	slug_t scale=1_cv,
	Atlas::ShapeInfo::Origin origin={},
	bool autoMetrics=true,
	slug_t canvasHeightEm=0_cv
);

// ================================================================================================
// loadPicture
//
// Convert a loaded tvg::Picture into a CompositeShape. Keys come from the element id when present
// (stroke layers: id + ".stroke") unless keys.force is set, otherwise from keys.next().
// @p idNames optionally maps ThorVG's djb2 paint ids back to the SVG id strings (loadFile and
// loadString build it from the source text); without it every layer is keyed via keys.next().
// ================================================================================================
CompositeShape loadPicture(
	tvg::Picture* picture,
	Atlas& atlas,
	KeyIterator& keys,
	LoadConfig* config=nullptr,
	const std::vector<std::pair<uint32_t, std::string>>* idNames=nullptr
);

// ================================================================================================
// loadFile / loadString - convenience wrappers (initialize ThorVG, parse, convert, release)
// ================================================================================================
CompositeShape loadFile(
	const std::filesystem::path& path,
	Atlas& atlas,
	KeyIterator& keys,
	slug_t dpi=96_cv,
	LoadConfig* config=nullptr
);

CompositeShape loadString(
	std::string_view svg,
	Atlas& atlas,
	KeyIterator& keys,
	slug_t dpi=96_cv,
	LoadConfig* config=nullptr
);

// Collects (djb2(id), id) for every id="..." attribute in an SVG document - the inverse of the
// hash ThorVG's SVG loader stores in Paint::id.
std::vector<std::pair<uint32_t, std::string>> collectIds(std::string_view svg);

}
}

// ================================================================================================
// IMPLEMENTATION
// ================================================================================================
#ifdef SLUGHORN_THORVG_IMPLEMENTATION

#ifdef SLUGHORN_HAS_CLIPPER2
#include "clipper.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

namespace slughorn {
namespace thorvg {

namespace {

template<typename... Args>
void warn(const LoadConfig& config, int level, const Args&... args) {
	if(config.log) {
		config.log(level, slughorn::detail::to_sstr(args...));
	}

	else {
		std::cerr << "slughorn::thorvg [" << level << "]: ";

		((std::cerr << args), ...);

		std::cerr << std::endl;
	}
}

const ShapeRule* findRule(const LoadConfig& cfg, const std::string& id) {
	for(const auto& rule : cfg.rules) if(std::regex_match(id, rule.id)) return &rule;

	return nullptr;
}

// Same ray-cast helper as nanosvg.hpp (curve approximated by its chord).
size_t rayCrossings(const Atlas::Curves& curves, size_t begin, size_t end, slug_t px, slug_t py) {
	size_t count = 0;

	for(size_t i = begin; i < end; i++) {
		const auto& c = curves[i];

		if((c.y1 <= py) == (c.y3 <= py)) continue;

		const slug_t t = (py - c.y1) / (c.y3 - c.y1);
		const slug_t xi = c.x1 + t * (c.x3 - c.x1);

		if(xi > px) count++;
	}

	return count;
}

// Geometry in picture pixel space, with explicit sub-path starts (index 0 explicit).
struct PxGeometry {
	Atlas::Curves curves;
	std::vector<size_t> starts;
};

// Walks a shape's path commands through @p world into picture pixels. Every sub-path is closed
// (fill semantics); even-odd sub-paths are winding-flipped exactly like nanosvg.hpp does.
PxGeometry fillGeometry(const tvg::Shape* shape, const Matrix& world, slug_t tolerancePx, bool errorBound=false) {
	PxGeometry g;

	const tvg::PathCommand* cmds = nullptr;
	const tvg::Point* pts = nullptr;
	uint32_t cmdCnt = 0, ptsCnt = 0;

	if(shape->path(&cmds, &cmdCnt, &pts, &ptsCnt) != tvg::Result::Success || !cmdCnt) return g;

	CurveDecomposer dec(g.curves);

	dec.tolerance = tolerancePx;
	dec.errorBound = errorBound;

	const bool evenodd = shape->fillRule() == tvg::FillRule::EvenOdd;

	size_t subStart = 0;
	bool open = false;
	slug_t startX = 0_cv, startY = 0_cv;

	auto finish = [&]() {
		if(!open) return;

		dec.close();

		if(g.curves.size() > subStart) {
			g.starts.push_back(subStart);

			if(evenodd && subStart > 0 && rayCrossings(g.curves, 0, subStart, startX, startY) % 2 != 0) {
				dec.reverseFrom(subStart);
			}
		}

		open = false;
	};

	auto map = [&](const tvg::Point& p, slug_t& x, slug_t& y) { world.apply(p.x, p.y, x, y); };

	uint32_t pi = 0;

	for(uint32_t ci = 0; ci < cmdCnt; ci++) {
		switch(cmds[ci]) {
			case tvg::PathCommand::MoveTo: {
				if(pi + 1 > ptsCnt) return g;

				finish();

				map(pts[pi++], startX, startY);
				dec.moveTo(startX, startY);

				subStart = g.curves.size();
				open = true;

				break;
			}

			case tvg::PathCommand::LineTo: {
				if(pi + 1 > ptsCnt) return g;

				slug_t x, y;

				map(pts[pi++], x, y);

				if(!open) { dec.moveTo(dec._x, dec._y); subStart = g.curves.size(); open = true; }

				if(std::abs(x - dec._x) > 1e-9_cv || std::abs(y - dec._y) > 1e-9_cv) dec.lineTo(x, y);

				break;
			}

			case tvg::PathCommand::CubicTo: {
				if(pi + 3 > ptsCnt) return g;

				slug_t c1x, c1y, c2x, c2y, x, y;

				map(pts[pi], c1x, c1y);
				map(pts[pi + 1], c2x, c2y);
				map(pts[pi + 2], x, y);

				pi += 3;

				if(!open) { dec.moveTo(dec._x, dec._y); subStart = g.curves.size(); open = true; }

				dec.cubicTo(c1x, c1y, c2x, c2y, x, y);

				break;
			}

			case tvg::PathCommand::Close: {
				// After close the current point returns to the sub-path start (SVG semantics);
				// the decomposer already tracks that in _sx/_sy.
				finish();

				dec._x = dec._sx;
				dec._y = dec._sy;

				break;
			}
		}
	}

	finish();

	return g;
}

// Flattens one sub-path's commands (local space) into polylines, for dashing.
struct LocalPolyline {
	std::vector<std::pair<slug_t, slug_t>> pts;
	bool closed = false;
};

void flattenCubic(
	slug_t x0, slug_t y0, slug_t x1, slug_t y1, slug_t x2, slug_t y2, slug_t x3, slug_t y3,
	slug_t tol, int depth, std::vector<std::pair<slug_t, slug_t>>& out
) {
	const slug_t dx = x3 - x0, dy = y3 - y0;
	const slug_t d1 = std::abs((x1 - x3) * dy - (y1 - y3) * dx);
	const slug_t d2 = std::abs((x2 - x3) * dy - (y2 - y3) * dx);

	if(depth >= 12 || (d1 + d2) * (d1 + d2) <= tol * tol * (dx * dx + dy * dy) * 4_cv) {
		out.push_back({x3, y3});

		return;
	}

	const slug_t x01 = (x0 + x1) * 0.5_cv, y01 = (y0 + y1) * 0.5_cv;
	const slug_t x12 = (x1 + x2) * 0.5_cv, y12 = (y1 + y2) * 0.5_cv;
	const slug_t x23 = (x2 + x3) * 0.5_cv, y23 = (y2 + y3) * 0.5_cv;
	const slug_t xa = (x01 + x12) * 0.5_cv, ya = (y01 + y12) * 0.5_cv;
	const slug_t xb = (x12 + x23) * 0.5_cv, yb = (y12 + y23) * 0.5_cv;
	const slug_t xm = (xa + xb) * 0.5_cv, ym = (ya + yb) * 0.5_cv;

	flattenCubic(x0, y0, x01, y01, xa, ya, xm, ym, tol, depth + 1, out);
	flattenCubic(xm, ym, xb, yb, x23, y23, x3, y3, tol, depth + 1, out);
}

std::vector<LocalPolyline> localPolylines(const tvg::Shape* shape, slug_t tol) {
	std::vector<LocalPolyline> out;

	const tvg::PathCommand* cmds = nullptr;
	const tvg::Point* pts = nullptr;
	uint32_t cmdCnt = 0, ptsCnt = 0;

	if(shape->path(&cmds, &cmdCnt, &pts, &ptsCnt) != tvg::Result::Success) return out;

	uint32_t pi = 0;
	slug_t sx = 0_cv, sy = 0_cv;

	auto cur = [&]() -> LocalPolyline& {
		if(out.empty() || out.back().closed) {
			out.push_back({});
			out.back().pts.push_back({sx, sy});
		}

		return out.back();
	};

	for(uint32_t ci = 0; ci < cmdCnt; ci++) {
		switch(cmds[ci]) {
			case tvg::PathCommand::MoveTo:
				if(pi + 1 > ptsCnt) return out;

				sx = pts[pi].x; sy = pts[pi].y; pi++;
				out.push_back({});
				out.back().pts.push_back({sx, sy});

				break;

			case tvg::PathCommand::LineTo:
				if(pi + 1 > ptsCnt) return out;

				cur().pts.push_back({pts[pi].x, pts[pi].y});
				pi++;

				break;

			case tvg::PathCommand::CubicTo: {
				if(pi + 3 > ptsCnt) return out;

				auto& pl = cur();
				const auto [x0, y0] = pl.pts.back();

				flattenCubic(x0, y0, pts[pi].x, pts[pi].y, pts[pi + 1].x, pts[pi + 1].y, pts[pi + 2].x, pts[pi + 2].y, tol, 0, pl.pts);
				pi += 3;

				break;
			}

			case tvg::PathCommand::Close:
				if(!out.empty() && !out.back().closed) {
					auto& pl = out.back();

					if(pl.pts.front() != pl.pts.back()) pl.pts.push_back(pl.pts.front());

					pl.closed = true;
				}

				break;
		}
	}

	return out;
}

// Splits polylines into dash segments (SVG stroke-dasharray / dashoffset semantics: the pattern
// restarts on every sub-path; an odd-length pattern is repeated to make it even).
std::vector<std::vector<std::pair<slug_t, slug_t>>> dashPolylines(
	const std::vector<LocalPolyline>& lines,
	std::vector<slug_t> pattern,
	slug_t offset
) {
	std::vector<std::vector<std::pair<slug_t, slug_t>>> out;

	if(pattern.size() % 2) {
		const auto n = pattern.size();

		for(size_t i = 0; i < n; i++) pattern.push_back(pattern[i]);
	}

	slug_t period = 0_cv;

	for(auto v : pattern) period += std::max(0_cv, v);

	if(period <= 1e-6_cv) return out;

	for(const auto& pl : lines) {
		if(pl.pts.size() < 2) continue;

		// Position within the pattern at the start of this sub-path.
		slug_t phase = std::fmod(offset, period);

		if(phase < 0_cv) phase += period;

		size_t idx = 0;

		while(phase >= pattern[idx]) {
			phase -= pattern[idx];
			idx = (idx + 1) % pattern.size();
		}

		slug_t remain = pattern[idx] - phase; // length left in the current dash/gap
		bool on = (idx % 2) == 0;

		std::vector<std::pair<slug_t, slug_t>> current;

		if(on) current.push_back(pl.pts[0]);

		for(size_t i = 1; i < pl.pts.size(); i++) {
			auto [ax, ay] = pl.pts[i - 1];
			const auto [bx, by] = pl.pts[i];

			slug_t segLen = std::hypot(bx - ax, by - ay);

			while(segLen > 0_cv) {
				if(remain >= segLen) {
					remain -= segLen;

					if(on) current.push_back({bx, by});

					segLen = 0_cv;
				}

				else {
					const slug_t t = remain / segLen;
					const slug_t mx = ax + (bx - ax) * t;
					const slug_t my = ay + (by - ay) * t;

					if(on) {
						current.push_back({mx, my});

						if(current.size() >= 2) out.push_back(std::move(current));

						current.clear();
					}

					else current.push_back({mx, my});

					on = !on;
					idx = (idx + 1) % pattern.size();
					remain = pattern[idx];
					segLen -= std::hypot(mx - ax, my - ay);
					ax = mx; ay = my;
				}
			}
		}

		if(on && current.size() >= 2) out.push_back(std::move(current));
	}

	return out;
}

canvas::LineJoin toJoin(tvg::StrokeJoin j) {
	switch(j) {
		case tvg::StrokeJoin::Round: return canvas::LineJoin::Round;
		case tvg::StrokeJoin::Bevel: return canvas::LineJoin::Bevel;
		default: return canvas::LineJoin::Miter;
	}
}

canvas::LineCap toCap(tvg::StrokeCap c) {
	switch(c) {
		case tvg::StrokeCap::Round: return canvas::LineCap::Round;
		case tvg::StrokeCap::Square: return canvas::LineCap::Square;
		default: return canvas::LineCap::Butt;
	}
}

// Stroke-to-fill expansion in the shape's LOCAL space via canvas::Path::strokePath(), then mapped
// into picture pixels through @p world.
PxGeometry strokeGeometry(const tvg::Shape* shape, const Matrix& world, slug_t tolerancePx) {
	PxGeometry g;

	const slug_t width = shape->strokeWidth();

	if(width <= 0_cv) return g;

	const slug_t det = std::abs(world.xx * world.yy - world.xy * world.yx);
	const slug_t pxPerLocal = det > 0_cv ? std::sqrt(det) : 1_cv;
	const slug_t tolLocal = std::max(1e-6_cv, tolerancePx / pxPerLocal);

	canvas::Path path;

	path.decomposer().tolerance = tolLocal;

	const float* dash = nullptr;
	float dashOffset = 0.0f;
	const uint32_t dashCnt = shape->strokeDash(&dash, &dashOffset);

	if(dashCnt > 0 && dash) {
		std::vector<slug_t> pattern(dash, dash + dashCnt);

		const auto segments = dashPolylines(localPolylines(shape, tolLocal), pattern, dashOffset);

		for(const auto& seg : segments) {
			path.moveTo(seg[0].first, seg[0].second);

			for(size_t i = 1; i < seg.size(); i++) path.lineTo(seg[i].first, seg[i].second);
		}
	}

	else {
		const tvg::PathCommand* cmds = nullptr;
		const tvg::Point* pts = nullptr;
		uint32_t cmdCnt = 0, ptsCnt = 0;

		if(shape->path(&cmds, &cmdCnt, &pts, &ptsCnt) != tvg::Result::Success) return g;

		uint32_t pi = 0;

		for(uint32_t ci = 0; ci < cmdCnt; ci++) {
			switch(cmds[ci]) {
				case tvg::PathCommand::MoveTo:
					if(pi + 1 > ptsCnt) break;

					path.moveTo(pts[pi].x, pts[pi].y);
					pi++;

					break;

				case tvg::PathCommand::LineTo:
					if(pi + 1 > ptsCnt) break;

					path.lineTo(pts[pi].x, pts[pi].y);
					pi++;

					break;

				case tvg::PathCommand::CubicTo:
					if(pi + 3 > ptsCnt) break;

					path.bezierTo(pts[pi].x, pts[pi].y, pts[pi + 1].x, pts[pi + 1].y, pts[pi + 2].x, pts[pi + 2].y);
					pi += 3;

					break;

				case tvg::PathCommand::Close:
					path.closePath();

					break;
			}
		}
	}

	if(!path.hasPendingPath()) return g;

	if(!path.strokePath(
		width, false,
		toJoin(shape->strokeJoin()),
		toCap(shape->strokeCap()),
		shape->strokeMiterlimit()
	)) return g;

	g.curves = path.curves();
	g.starts = path.mergedContourStarts();

	for(auto& c : g.curves) {
		world.apply(c.x1, c.y1, c.x1, c.y1);
		world.apply(c.x2, c.y2, c.x2, c.y2);
		world.apply(c.x3, c.y3, c.x3, c.y3);
	}

	return g;
}

struct Finalized {
	Atlas::ShapeInfo info;
	Transform transform;
	slug_t offX = 0_cv, offY = 0_cv; // em-space offset subtracted from the curves
	bool ok = false;
};

// Pixel geometry -> em-space ShapeInfo + layer Transform (nanosvg::decomposePath conventions).
Finalized finalize(PxGeometry&& g, slug_t scale, Atlas::ShapeInfo::Origin origin, bool autoMetrics) {
	Finalized f;

	if(g.curves.empty()) return f;

	slug_t minX = std::numeric_limits<slug_t>::max(), minY = minX;
	slug_t maxX = std::numeric_limits<slug_t>::lowest(), maxY = maxX;

	for(auto& c : g.curves) {
		c.x1 *= scale; c.y1 *= scale;
		c.x2 *= scale; c.y2 *= scale;
		c.x3 *= scale; c.y3 *= scale;

		minX = std::min({minX, c.x1, c.x2, c.x3});
		minY = std::min({minY, c.y1, c.y2, c.y3});
		maxX = std::max({maxX, c.x1, c.x2, c.x3});
		maxY = std::max({maxY, c.y1, c.y2, c.y3});
	}

	if(maxX <= minX || maxY <= minY) return f;

	const slug_t offX = autoMetrics ? minX : 0_cv;
	const slug_t offY = autoMetrics ? minY : 0_cv;

	if(autoMetrics) for(auto& c : g.curves) {
		c.x1 -= offX; c.y1 -= offY;
		c.x2 -= offX; c.y2 -= offY;
		c.x3 -= offX; c.y3 -= offY;
	}

	Atlas::ShapeInfo::Origin infoOrigin = origin;

	if(origin.type == Atlas::ShapeInfo::Origin::Type::Pivot) {
		infoOrigin.x = origin.x * scale - offX;
		infoOrigin.y = origin.y * scale - offY;
	}

	else if(origin.type == Atlas::ShapeInfo::Origin::Type::Custom) {
		infoOrigin.x = origin.x * scale;
		infoOrigin.y = origin.y * scale;
	}

	f.transform = !autoMetrics ? Transform{} :
		(origin.type == Atlas::ShapeInfo::Origin::Type::Centered)
		? Transform{ (minX + maxX) * 0.5_cv, (minY + maxY) * 0.5_cv }
		: (origin.type == Atlas::ShapeInfo::Origin::Type::Pivot)
		? Transform{ origin.x * scale, origin.y * scale }
		: (origin.type == Atlas::ShapeInfo::Origin::Type::Custom)
		? Transform{ minX + origin.x * scale, minY + origin.y * scale }
		: Transform{ minX, minY }
	;

	f.info.curves = std::move(g.curves);
	f.info.contourStarts = std::move(g.starts);
	f.info.origin = infoOrigin;
	f.offX = offX;
	f.offY = offY;
	f.ok = true;

	return f;
}

void registerShape(Atlas& atlas, Key key, Atlas::ShapeInfo& info, bool autoMetrics, slug_t canvasHeightEm) {
	info.autoMetrics = autoMetrics;

	if(!autoMetrics) {
		const slug_t h = canvasHeightEm > 0_cv ? canvasHeightEm : 1_cv;

		info.bearingX = 0_cv;
		info.bearingY = h;
		info.width = 1_cv;
		info.height = h;
	}

	atlas.addShape(key, info);
}

Color toColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a, slug_t opacity) {
	return { cv(r) / 255_cv, cv(g) / 255_cv, cv(b) / 255_cv, cv(a) / 255_cv * opacity };
}

Spread toSpread(tvg::FillSpread s) {
	switch(s) {
		case tvg::FillSpread::Reflect: return Spread::Reflect;
		case tvg::FillSpread::Repeat: return Spread::Repeat;
		default: return Spread::Pad;
	}
}

// Result of converting a tvg::Fill: either a gradient or (degenerate linear) a solid color.
struct PaintResult {
	bool ok = false;
	bool solid = false;
	Color color = {};
	GradientInfo gradient = {};
	Spread spread = Spread::Pad;
};

// Converts a ThorVG gradient into a slughorn GradientInfo in the layer's local em-space.
//
// G = world * fill->transform() maps gradient space -> picture pixels; a local em point e maps
// to pixels p = (e + off) / scale. H = G^-1 composed with that is affine e -> gradient space, so
// the linear t and the radial B matrix both follow in closed form.
PaintResult convertFill(
	const tvg::Fill* fill,
	const Matrix& world,
	slug_t scale,
	slug_t offX, slug_t offY,
	slug_t opacity,
	const LoadConfig& cfg,
	const std::string& id
) {
	PaintResult r;

	const tvg::Fill::ColorStop* stops = nullptr;
	const uint32_t n = fill->colorStops(&stops);

	if(!n || !stops) {
		warn(cfg, 1, "skipping gradient with no stops (shape id=\"", id, "\")");

		return r;
	}

	r.spread = toSpread(fill->spread());

	if(r.spread != Spread::Pad) warn(cfg, 1,
		"gradient spread reflect/repeat is rendered as pad (shape id=\"", id, "\")"
	);

	r.gradient.stops.reserve(n);

	for(uint32_t i = 0; i < n; i++) r.gradient.stops.push_back({
		cv(stops[i].offset), toColor(stops[i].r, stops[i].g, stops[i].b, stops[i].a, opacity)
	});

	const Matrix G = world * toMatrix(fill->transform());

	const double det = double(G.xx) * G.yy - double(G.xy) * G.yx;

	if(std::abs(det) < 1e-12) {
		warn(cfg, 1, "degenerate gradient transform, skipping (shape id=\"", id, "\")");

		return r;
	}

	// Ginv (pixels -> gradient space).
	const double ia = G.yy / det, ib = -G.xy / det, id_ = -G.yx / det, ie = G.xx / det;
	const double ic = -(ia * G.dx + ib * G.dy);
	const double if_ = -(id_ * G.dx + ie * G.dy);

	// H (local em -> gradient space): H_lin = Ginv_lin / scale, H_t = Ginv(off / scale).
	const double s = scale;
	const double ha = ia / s, hb = ib / s, hd = id_ / s, he = ie / s;
	const double hc = ia * (offX / s) + ib * (offY / s) + ic;
	const double hf = id_ * (offX / s) + ie * (offY / s) + if_;

	if(fill->type() == tvg::Type::LinearGradient) {
		float x1 = 0, y1 = 0, x2 = 0, y2 = 0;

		static_cast<const tvg::LinearGradient*>(fill)->linear(&x1, &y1, &x2, &y2);

		const double dx = double(x2) - x1, dy = double(y2) - y1;
		const double l2 = dx * dx + dy * dy;

		if(l2 < 1e-12) {
			// Zero-length axis: ThorVG paints the last stop.
			r.solid = true;
			r.color = r.gradient.stops.back().color;
			r.ok = true;

			return r;
		}

		r.gradient.type = GradientInfo::Type::Linear;
		r.gradient.transform = Matrix{
			.xx = cv((dx * ha + dy * hd) / l2),
			.xy = cv((dx * hb + dy * he) / l2),
			.dx = cv((dx * (hc - x1) + dy * (hf - y1)) / l2),
		};
		r.ok = true;

		return r;
	}

	if(fill->type() == tvg::Type::RadialGradient) {
		float cx = 0, cy = 0, rad = 0, fx = 0, fy = 0, fr = 0;

		static_cast<const tvg::RadialGradient*>(fill)->radial(&cx, &cy, &rad, &fx, &fy, &fr);

		if(std::abs(fx - cx) > 1e-4f || std::abs(fy - cy) > 1e-4f) warn(cfg, 1,
			"radial gradient focal point is approximated by its center (shape id=\"", id, "\")"
		);

		const double span = double(rad) - fr;

		if(span <= 1e-9) {
			warn(cfg, 1, "degenerate radial gradient radius, skipping (shape id=\"", id, "\")");

			return r;
		}

		// Center in local em: solve H e = C.
		const double hdet = ha * he - hb * hd;

		if(std::abs(hdet) < 1e-18) {
			warn(cfg, 1, "degenerate radial gradient transform, skipping (shape id=\"", id, "\")");

			return r;
		}

		const double rx = cx - hc, ry = cy - hf;
		const double ex = ( he * rx - hb * ry) / hdet;
		const double ey = (-hd * rx + ha * ry) / hdet;

		double b00 = ha / span, b01 = hb / span, b10 = hd / span, b11 = he / span;

		// Shader discriminator wants b11 > 0; length(B d) == length(-B d).
		if(b11 < 0.0) { b00 = -b00; b01 = -b01; b10 = -b10; b11 = -b11; }

		r.gradient.type = GradientInfo::Type::AffineRadial;
		r.gradient.transform = buildAffineRadialGradientMatrix(cv(ex), cv(ey), cv(b00), cv(b01), cv(b10), cv(b11));
		r.gradient.innerRadius = cv(double(fr) / span);
		r.ok = true;

		return r;
	}

	warn(cfg, 1, "skipping unsupported fill type (shape id=\"", id, "\")");

	return r;
}

uint32_t djb2(const char* str) {
	// Mirrors ThorVG's djb2Encode() (unsigned long, truncated to Paint::id's uint32_t).
	unsigned long hash = 5381;
	int c;

	while((c = *str++)) hash = ((hash << 5) + hash) + static_cast<unsigned long>(c);

	return static_cast<uint32_t>(hash);
}

struct ThorvgInit {
	bool ok;

	ThorvgInit(): ok(tvg::Initializer::init(0) == tvg::Result::Success) {}
	~ThorvgInit() { if(ok) tvg::Initializer::term(); }
};

}

// ================================================================================================
// worldMatrix
// ================================================================================================
Matrix worldMatrix(const tvg::Paint* paint) {
	Matrix m = Matrix::identity();

	for(const tvg::Paint* p = paint; p; p = p->parent()) {
		m = toMatrix(const_cast<tvg::Paint*>(p)->transform()) * m;
	}

	return m;
}

std::vector<std::pair<uint32_t, std::string>> collectIds(std::string_view svg) {
	std::vector<std::pair<uint32_t, std::string>> out;

	for(size_t i = 0; i + 2 < svg.size(); i++) {
		if(svg[i] != 'i' || svg[i + 1] != 'd') continue;

		// Attribute name boundary: preceded by whitespace.
		if(i == 0 || !std::isspace(static_cast<unsigned char>(svg[i - 1]))) continue;

		size_t j = i + 2;

		while(j < svg.size() && std::isspace(static_cast<unsigned char>(svg[j]))) j++;

		if(j >= svg.size() || svg[j] != '=') continue;

		j++;

		while(j < svg.size() && std::isspace(static_cast<unsigned char>(svg[j]))) j++;

		if(j >= svg.size() || (svg[j] != '"' && svg[j] != '\'')) continue;

		const char q = svg[j];
		const size_t end = svg.find(q, j + 1);

		if(end == std::string_view::npos) break;

		std::string id(svg.substr(j + 1, end - j - 1));

		out.push_back({djb2(id.c_str()), std::move(id)});

		i = end;
	}

	return out;
}

// ================================================================================================
// decomposePath / loadShape
// ================================================================================================
std::pair<Atlas::ShapeInfo, Transform> decomposePath(
	const tvg::Shape* shape,
	const Matrix& world,
	slug_t scale,
	Atlas::ShapeInfo::Origin origin,
	bool autoMetrics,
	slug_t tolerance
) {
	// CurveDecomposer works in pixels here; convert the em tolerance.
	const slug_t tolPx = tolerance >= TOLERANCE_EXACT ? TOLERANCE_EXACT : tolerance / scale;

	auto f = finalize(fillGeometry(shape, world, tolPx), scale, origin, autoMetrics);

	if(!f.ok) return { {}, {} };

	return { std::move(f.info), f.transform };
}

std::optional<Transform> loadShape(
	const tvg::Shape* shape,
	const Matrix& world,
	Atlas& atlas,
	Key key,
	slug_t scale,
	Atlas::ShapeInfo::Origin origin,
	bool autoMetrics,
	slug_t canvasHeightEm
) {
	auto [info, transform] = decomposePath(shape, world, scale, origin, autoMetrics);

	if(info.curves.empty()) return std::nullopt;

	registerShape(atlas, key, info, autoMetrics, canvasHeightEm);

	return transform;
}

// ================================================================================================
// loadPicture
// ================================================================================================
CompositeShape loadPicture(
	tvg::Picture* picture,
	Atlas& atlas,
	KeyIterator& keys,
	LoadConfig* config,
	const std::vector<std::pair<uint32_t, std::string>>* idNames
) {
	static const LoadConfig dflt{};
	const LoadConfig& cfg = config ? *config : dflt;

	CompositeShape composite;

	if(!picture) return composite;

	float pw = 0.0f, ph = 0.0f;

	picture->size(&pw, &ph);

	if(pw <= 0.0f) {
		warn(cfg, 2, "loadPicture: picture width is zero, cannot normalize");

		return composite;
	}

	const slug_t scale = 1_cv / cv(pw);
	const slug_t heightEm = cv(ph) / cv(pw);

	if(config) {
		config->width = pw;
		config->height = ph;
		config->heightEm = heightEm;
		config->layers.clear();
	}

	composite.advance = 1_cv;

	std::map<uint32_t, std::string> names;

	if(idNames) for(const auto& [h, s] : *idNames) names.emplace(h, s);

	// ThorVG's SVG builder wraps a clipped / masked element in a single-child Scene and puts the
	// element's id on that wrapper, so climb through single-child wrappers to find it.
	auto nameOf = [&](const tvg::Paint* p) -> std::string {
		while(p && !p->id) {
			const tvg::Paint* parent = p->parent();

			if(!parent || parent->type() != tvg::Type::Scene) return {};
			if(static_cast<const tvg::Scene*>(parent)->paints().size() != 1) return {};

			p = parent;
		}

		if(!p) return {};

		auto it = names.find(p->id);

		return it == names.end() ? std::string{} : it->second;
	};

	// Collect shapes in pre-order (= painter's order for siblings).
	std::vector<const tvg::Paint*> paints;

	tvg::Accessor* accessor = tvg::Accessor::gen();

	accessor->set(picture, [](const tvg::Paint* paint, void* data) {
		static_cast<std::vector<const tvg::Paint*>*>(data)->push_back(paint);

		return true;
	}, &paints);

	delete accessor;

#ifdef SLUGHORN_HAS_CLIPPER2
	// Clip regions in picture pixels, cached per (clip shape, its parent world).
	auto shapeRegion = [&](const tvg::Shape* s, const Matrix& world) {
		auto g = fillGeometry(s, world, cv(cfg.clipTolerancePx));

		return clipper::normalize(
			clipper::toPaths(clipper::splitContours(g.curves, g.starts), cfg.clipTolerancePx),
			s->fillRule() == tvg::FillRule::EvenOdd ? FillRule::EvenOdd : FillRule::NonZero
		);
	};
#endif

	for(const tvg::Paint* paint : paints) {
		if(!paint) continue;

		const tvg::Type type = paint->type();

		if(type == tvg::Type::Text) {
			warn(cfg, 1, "text element skipped (convert text to paths first) id=\"", nameOf(paint), "\"");

			continue;
		}

		if(type == tvg::Type::Picture && paint != picture) {
			warn(cfg, 1, "embedded image / picture skipped id=\"", nameOf(paint), "\"");

			continue;
		}

		if(type != tvg::Type::Shape) continue;

		const auto* shape = static_cast<const tvg::Shape*>(paint);

		if(!shape) continue;

		// Visibility + accumulated opacity along the ancestor chain; collect clips / masks.
		bool visible = true;
		slug_t opacity = 1_cv;

		std::vector<const tvg::Paint*> chain;

		for(const tvg::Paint* p = paint; p; p = p->parent()) {
			if(!p->visible()) visible = false;

			opacity *= cv(p->opacity()) / 255_cv;
			chain.push_back(p);
		}

		// Inside a nested Picture that we refused (bitmap) - not reachable for shapes; skip check.
		if(!visible || opacity <= 1e-4_cv) continue;

		const std::string id = nameOf(paint);
		const ShapeRule* rule = findRule(cfg, id);
		const ShapePolicy policy = rule ? rule->policy : ShapePolicy::Default;

		if(policy & ShapePolicy::ForceExclude) continue;

		const Matrix world = worldMatrix(paint);

		// Clip / mask region (picture pixels): the intersection of every clip (and Alpha/Luma mask)
		// on the ancestor chain, minus every InvAlpha/InvLuma mask.
		bool hasComp = false;

#ifdef SLUGHORN_HAS_CLIPPER2
		bool hasClip = false;
		bool hasMask = false;
		clipper::Paths clipRegion;
		std::vector<clipper::Paths> subtracts;

		auto applyRegion = [&](const clipper::Paths& r) {
			clipRegion = hasClip ? clipper::intersect(clipRegion, r) : r;
			hasClip = true;
		};
#endif

		for(const tvg::Paint* p : chain) {
			const Matrix parentWorld = p->parent() ? worldMatrix(p->parent()) : Matrix::identity();

			if(const tvg::Shape* clip = p->clip()) {
#ifdef SLUGHORN_HAS_CLIPPER2
				if(clip->clip()) warn(cfg, 1, "nested clip-of-clip ignored (shape id=\"", id, "\")");

				const Matrix cw = parentWorld * toMatrix(const_cast<tvg::Shape*>(clip)->transform());

				applyRegion(shapeRegion(clip, cw));
				hasComp = true;
#else
				warn(cfg, 1, "clip path skipped (build with SLUGHORN_CLIPPER2=ON) id=\"", id, "\"");
#endif
			}

			const tvg::Paint* maskTarget = nullptr;
			const tvg::MaskMethod method = p->mask(&maskTarget);

			if(method != tvg::MaskMethod::None && maskTarget) {
#ifdef SLUGHORN_HAS_CLIPPER2
				const bool inverse = method == tvg::MaskMethod::InvAlpha || method == tvg::MaskMethod::InvLuma;
				const bool supported = inverse || method == tvg::MaskMethod::Alpha || method == tvg::MaskMethod::Luma;

				if(!supported) {
					warn(cfg, 1, "mask method ", static_cast<int>(method), " ignored (shape id=\"", id, "\")");

					continue;
				}

				warn(cfg, 1, "mask approximated as a hard clip by its shapes' union (shape id=\"", id, "\")");

				// Union of every visible shape in the mask target, each with its world relative
				// to the masked paint's parent.
				const Matrix targetWorld = parentWorld * toMatrix(const_cast<tvg::Paint*>(maskTarget)->transform());

				std::vector<const tvg::Paint*> mpaints;
				tvg::Accessor* ma = tvg::Accessor::gen();

				ma->set(const_cast<tvg::Paint*>(maskTarget), [](const tvg::Paint* q, void* data) {
					static_cast<std::vector<const tvg::Paint*>*>(data)->push_back(q);

					return true;
				}, &mpaints);

				delete ma;

				clipper::Paths region;

				for(const tvg::Paint* q : mpaints) {
					if(q->type() != tvg::Type::Shape) continue;

					Matrix m = Matrix::identity();

					for(const tvg::Paint* a = q; a && a != maskTarget; a = a->parent()) {
						m = toMatrix(const_cast<tvg::Paint*>(a)->transform()) * m;
					}

					region = clipper::unite(region, shapeRegion(static_cast<const tvg::Shape*>(q), targetWorld * m));
				}

				if(inverse) subtracts.push_back(std::move(region));
				else applyRegion(region);

				hasComp = true;
				hasMask = true;
#else
				warn(cfg, 1, "mask skipped (build with SLUGHORN_CLIPPER2=ON) id=\"", id, "\"");
#endif
			}
		}

		const Atlas::ShapeInfo::Origin shapeOrigin = (rule && rule->origin) ? *rule->origin : cfg.origin;

		const tvg::FillRule fr = shape->fillRule();
		const FillRule fillRule = fr == tvg::FillRule::EvenOdd ? FillRule::EvenOdd : FillRule::NonZero;

		// Emits one layer from pixel-space geometry + paint.
		auto emit = [&](PxGeometry&& g, bool isStroke, const tvg::Fill* gradFill, Color solid, bool geometryOnly, const Key& key) {
			LayerInfo li;

			li.id = id;
			li.fillRule = isStroke ? FillRule::NonZero : fillRule;
			li.stroke = isStroke;
			li.opacity = opacity;

#ifdef SLUGHORN_HAS_CLIPPER2
			// A rectangular clip around the whole geometry is a no-op: keep the curves. Not for
			// even-odd layers: their Clipper2 pass is also what makes a self-intersecting or
			// overlapping even-odd path exact (the sub-path winding flip only covers nesting).
			bool noop = false;

			if(hasComp && cfg.skipContainingRectClips && hasClip && !hasMask && subtracts.empty() && !g.curves.empty() &&
				li.fillRule == FillRule::NonZero) {
				double rx0 = 0, ry0 = 0, rx1 = 0, ry1 = 0;

				if(clipper::isAxisRect(clipRegion, rx0, ry0, rx1, ry1)) {
					double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;

					for(const auto& c : g.curves) {
						for(const auto& [x, y] : {std::pair<slug_t, slug_t>{c.x1, c.y1}, {c.x2, c.y2}, {c.x3, c.y3}}) {
							x0 = std::min(x0, double(x)); y0 = std::min(y0, double(y));
							x1 = std::max(x1, double(x)); y1 = std::max(y1, double(y));
						}
					}

					noop = x0 >= rx0 && y0 >= ry0 && x1 <= rx1 && y1 <= ry1;
				}
			}

			if(hasComp && !noop) {
				auto region = clipper::normalize(
					clipper::toPaths(clipper::splitContours(g.curves, g.starts), cfg.clipTolerancePx),
					li.fillRule
				);

				if(hasClip) region = clipper::intersect(region, clipRegion);

				for(const auto& sub : subtracts) region = clipper::difference(region, sub);

				g.starts.clear();
				g.curves = clipper::toCurves(region, &g.starts);
				li.clipped = true;
				li.fillRule = FillRule::NonZero; // normalized
			}
#endif

			auto f = finalize(std::move(g), scale, shapeOrigin, cfg.autoMetrics);

			if(!f.ok) return;

			uint32_t gradientId = 0;
			Color color = solid;

			if(gradFill) {
				auto pr = convertFill(gradFill, world, scale, f.offX, f.offY, opacity, cfg, id);

				if(!pr.ok) return;

				li.spread = pr.spread;

				if(pr.solid) color = pr.color;

				else {
					color = { 1_cv, 1_cv, 1_cv, 1_cv };
					gradientId = atlas.addGradient(pr.gradient);
				}
			}

			registerShape(atlas, key, f.info, cfg.autoMetrics, heightEm);

			composite.layers.push_back(Layer{
				.key = key,
				.color = color,
				.transform = f.transform,
				.gradientId = gradientId,
				.drawMode = geometryOnly ? DrawMode::Geometry : DrawMode::Visible
			});

			if(config) config->layers.push_back(std::move(li));
		};

		const bool geometryOnlyPolicy = policy & ShapePolicy::GeometryOnly;

		// ---- Fill ----
		{
			const tvg::Fill* gradFill = shape->fill();
			uint8_t r = 0, g = 0, b = 0, a = 0;

			shape->fill(&r, &g, &b, &a);

			const bool hasSolid = !gradFill && a > 0;

			if(gradFill || hasSolid || (policy & ShapePolicy::ForceInclude)) {
				const Color solid = toColor(r, g, b, a, opacity);
				const bool geometryOnly = geometryOnlyPolicy || (!gradFill && !hasSolid);

				if(gradFill || solid.a >= 1e-4_cv || geometryOnly) {
					const Key key = (!keys.force && !id.empty()) ? Key(id) : keys.next();
					const slug_t tolPx = cfg.tolerancePx > 0_cv ? cfg.tolerancePx : cfg.tolerance >= TOLERANCE_EXACT ? TOLERANCE_EXACT : cfg.tolerance / scale;

					emit(fillGeometry(shape, world, tolPx, cfg.curveErrorBound), false, gradFill, solid, geometryOnly, key);
				}
			}
		}

		// ---- Stroke ----
		if(shape->strokeWidth() > 0.0f) {
			const tvg::Fill* gradStroke = shape->strokeFill();
			uint8_t r = 0, g = 0, b = 0, a = 0;

			shape->strokeFill(&r, &g, &b, &a);

			const bool painted = gradStroke || a > 0;

			if(painted && !cfg.strokes) {
				warn(cfg, 1, "stroke skipped (LoadConfig::strokes=false) id=\"", id, "\"");
			}

			else if(painted) {
				const Color solid = toColor(r, g, b, a, opacity);

				if(gradStroke || solid.a >= 1e-4_cv) {
					const Key key = (!keys.force && !id.empty()) ? Key(id + ".stroke") : keys.next();

					emit(strokeGeometry(shape, world, cfg.strokeTolerancePx), true, gradStroke, solid, geometryOnlyPolicy, key);
				}
			}
		}
	}

	return composite;
}

// ================================================================================================
// loadFile / loadString
// ================================================================================================
CompositeShape loadString(
	std::string_view svg,
	Atlas& atlas,
	KeyIterator& keys,
	slug_t dpi,
	LoadConfig* config
) {
	static const LoadConfig dflt{};
	const LoadConfig& cfg = config ? *config : dflt;

	(void)dpi;

	ThorvgInit init;

	if(!init.ok) {
		warn(cfg, 2, "loadString: ThorVG initialization failed");

		return {};
	}

	tvg::Picture* picture = tvg::Picture::gen();

	picture->ref();

	if(picture->load(svg.data(), static_cast<uint32_t>(svg.size()), "svg", nullptr, true) != tvg::Result::Success) {
		warn(cfg, 2, "loadString: failed to parse SVG");

		picture->unref();

		return {};
	}

	const auto ids = collectIds(svg);

	CompositeShape result = loadPicture(picture, atlas, keys, config, &ids);

	picture->unref();

	return result;
}

CompositeShape loadFile(
	const std::filesystem::path& path,
	Atlas& atlas,
	KeyIterator& keys,
	slug_t dpi,
	LoadConfig* config
) {
	static const LoadConfig dflt{};
	const LoadConfig& cfg = config ? *config : dflt;

	std::ifstream in(path, std::ios::binary);

	if(!in) {
		warn(cfg, 2, "loadFile: failed to open '", path.string(), "'");

		return {};
	}

	std::stringstream ss;

	ss << in.rdbuf();

	const std::string text = ss.str();

	if(text.empty()) {
		warn(cfg, 2, "loadFile: failed to parse '", path.string(), "'");

		return {};
	}

	return loadString(text, atlas, keys, dpi, config);
}

}
}

#endif
