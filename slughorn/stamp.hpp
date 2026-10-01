#pragma once

// ================================================================================================
// stamp.hpp - stamp runs (FX-Map-style instancing) for slughorn
//
// A stamp layer is ONE layer of a CompositeShape that draws N instances of a few prototypes, each
// instance an affine copy with its own solid color, composited in paint order. It replaces the
// thousands of near-identical layers canvas noise loops produce (speckles, gravel, petals).
//
// Prototypes live in a canonical unit frame:
//
//   Rect    - the unit square [0,1]^2, analytic (no curves)
//   Ellipse - the unit circle at the origin, analytic (no curves)
//   Curve   - any path, normalized to its bbox [0,1]^2, packed into the atlas ONCE as an ordinary
//             shape (like a glyph) and sampled through the Slug bands
//
// An Instance carries the unit-frame -> canvas em-space affine (Matrix), its prototype index and a
// straight (non-premultiplied) color with every opacity folded in, sRGB-encoded like Layer::color.
//
// In a CompositeShape a stamp layer is a placeholder Layer: effectId == stamp::EFFECT_ID and
// effectParam == its index into Set::layers (no shape key). Consumers that do not know stamps
// skip it (it has no shape); stamp-aware ones (renderComposite() below, bake.hpp) expand it.
//
// SVG input (loadString(), needs SLUGHORN_THORVG): a recorder emits
//
//   <defs><symbol id="pN" data-stamp-kind="rect|ellipse|path" overflow="visible">...</symbol></defs>
//   <g data-stamp-run="k"><use href="#pN" transform="matrix(a b c d e f)" fill=".." fill-opacity=".."/>...</g>
//
// A small XML pre-pass lifts each run out before ThorVG sees the file and leaves a marker at its
// paint position, which becomes the stamp layer. Runs that cannot be represented exactly (nested
// under a transformed / clipped / translucent ancestor, unknown symbols, non-matrix transforms,
// ellipse symbols without overflow="visible" - SVG clips a symbol to its viewport, which cuts a
// unit circle at the origin to one quadrant) are left in place for ThorVG and reported.
// ================================================================================================

#include "slughorn.hpp"
#include "render.hpp"

#ifdef SLUGHORN_HAS_THORVG
#include "thorvg.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <array>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace slughorn {
namespace stamp {

inline constexpr uint32_t EFFECT_ID = 0x504D5453; // 'STMP'

enum class Kind : uint8_t { Curve = 0, Ellipse = 1, Rect = 2 };

struct Proto {
	Kind kind = Kind::Rect;

	// Curve: geometry in the unit frame, and the atlas key it is registered under (registerProtos).
	Atlas::Curves curves = {};
	std::vector<size_t> starts = {};
	Key key = Key(0u);
};

struct Instance {
	Matrix m = {};        // unit frame -> canvas em
	uint32_t proto = 0;

	// Solid: the straight, sRGB-encoded color with every opacity folded in. Gradient: a tint the
	// gradient color is multiplied by (rgb and alpha).
	Color color = {};

	// 0 = solid; > 0 = 1-based index into Set::gradients (defined in the PROTOTYPE unit frame).
	uint32_t gradient = 0;
};

struct Layer {
	std::vector<Instance> instances;
};

struct Set {
	std::vector<Proto> protos;
	std::vector<GradientInfo> gradients; // prototype-frame gradients, deduplicated
	std::vector<Layer> layers;

	// Set::gradients[i] as registered in the atlas by registerProtos() (1-based atlas ids).
	std::vector<uint32_t> atlasGradientIds;

	bool empty() const { return layers.empty(); }
};

inline bool isStampLayer(const slughorn::Layer& l) { return l.effectId == EFFECT_ID; }
inline size_t stampIndex(const slughorn::Layer& l) { return static_cast<size_t>(l.effectParam); }

inline slughorn::Layer placeholder(size_t index) {
	slughorn::Layer l;

	l.key = Key(std::string());
	l.color = {1_cv, 1_cv, 1_cv, 1_cv};
	l.effectId = EFFECT_ID;
	l.effectParam = static_cast<slug_t>(index);

	return l;
}

// Registers every Curve prototype as an atlas shape named prefix + "p_<n>" (unit-frame curves,
// autoMetrics) and every stamp gradient with atlas.addGradient() (ids in set.atlasGradientIds).
// Call before atlas.build().
inline void registerProtos(Set& set, Atlas& atlas, const std::string& prefix) {
	set.atlasGradientIds.clear();

	for(const auto& g : set.gradients) set.atlasGradientIds.push_back(atlas.addGradient(g));

	for(size_t i = 0; i < set.protos.size(); i++) {
		Proto& p = set.protos[i];

		if(p.kind != Kind::Curve || p.curves.empty()) continue;

		p.key = Key(prefix + "p_" + std::to_string(i));

		Atlas::ShapeInfo info;

		info.curves = p.curves;
		info.contourStarts = p.starts;
		info.autoMetrics = true;

		atlas.addShape(p.key, info);
	}
}

// ------------------------------------------------------------------------------------------------
// Geometry
// ------------------------------------------------------------------------------------------------
struct Box {
	slug_t x0, y0, x1, y1;
};

inline Box bounds(const Instance& in, Kind kind) {
	const Matrix& m = in.m;

	if(kind == Kind::Ellipse) {
		const slug_t hx = std::hypot(m.xx, m.xy);
		const slug_t hy = std::hypot(m.yx, m.yy);

		return {m.dx - hx, m.dy - hy, m.dx + hx, m.dy + hy};
	}

	Box b{std::numeric_limits<slug_t>::max(), std::numeric_limits<slug_t>::max(),
		std::numeric_limits<slug_t>::lowest(), std::numeric_limits<slug_t>::lowest()};

	for(const auto& [ux, uy] : {std::pair{0_cv, 0_cv}, {1_cv, 0_cv}, {0_cv, 1_cv}, {1_cv, 1_cv}}) {
		slug_t x, y;

		m.apply(ux, uy, x, y);

		b.x0 = std::min(b.x0, x); b.y0 = std::min(b.y0, y);
		b.x1 = std::max(b.x1, x); b.y1 = std::max(b.y1, y);
	}

	return b;
}

struct Inverse {
	slug_t a = 1_cv, b = 0_cv, c = 0_cv, d = 1_cv, e = 0_cv, f = 0_cv; // q = [a b; c d] p + (e, f)
	bool ok = false;
};

inline Inverse invert(const Matrix& m) {
	Inverse r;

	const double det = double(m.xx) * m.yy - double(m.xy) * m.yx;

	if(std::abs(det) < 1e-24) return r;

	r.a = slug_t(m.yy / det);
	r.b = slug_t(-m.xy / det);
	r.c = slug_t(-m.yx / det);
	r.d = slug_t(m.xx / det);
	r.e = -(r.a * m.dx + r.b * m.dy);
	r.f = -(r.c * m.dx + r.d * m.dy);
	r.ok = true;

	return r;
}

// Coverage of one instance at canvas-em point (x, y), for a raster of ppeX / ppeY pixels per em.
// Rect: separable box-filter overlap (exact for axis-aligned instances); Ellipse: distance-based
// AA (capped by the ellipse's pixel area for sub-pixel specks); Curve: the prototype's Slug bands.
class Evaluator {
public:
	Evaluator(const Atlas& atlas, const Set& set, bool linear=false): _atlas(atlas), _set(set), _samplers(set.protos.size()), _linear(linear) {
		if(linear) for(const auto& g : set.gradients) _linearGradients.push_back(render::toLinear(g));
	}

	// Exactly the port's shader (core/slug/slug.gdshaderinc, slug_stamp_layer) for a raster whose
	// screen pixel steps are (1/ppeX, 0) and (0, 1/ppeY) em: q = M^-1 em, qx / qy = q's change per
	// screen pixel along x / y, l = (|grad q.x|, |grad q.y|) per pixel.
	//   Rect    - a one-pixel box filter per prototype axis.
	//   Ellipse - (|q| - 1) over its screen gradient.
	//   Curve   - the prototype's Slug bands at q (the prototype layer's canvas em, offset 0), with
	//             pixels-per-em 1 / (|qx| + |qy|) per axis, inside the shape's bounds +- one pixel.
	slug_t coverage(const Instance& in, const Inverse& inv, slug_t x, slug_t y, slug_t ppeX, slug_t ppeY) {
		if(!inv.ok || in.proto >= _set.protos.size()) return 0_cv;

		const Proto& proto = _set.protos[in.proto];
		const slug_t qx = inv.a * x + inv.b * y + inv.e;
		const slug_t qy = inv.c * x + inv.d * y + inv.f;

		// dq per screen pixel: along x (dqx_*), along y (dqy_*).
		const slug_t dqxX = inv.a / ppeX, dqxY = inv.c / ppeX;
		const slug_t dqyX = inv.b / ppeY, dqyY = inv.d / ppeY;
		const slug_t lx = std::max(std::hypot(dqxX, dqyX), 1e-9_cv);
		const slug_t ly = std::max(std::hypot(dqxY, dqyY), 1e-9_cv);

		switch(proto.kind) {
			case Kind::Rect: {
				const slug_t cx = std::clamp(qx / lx + 0.5_cv, 0_cv, 1_cv) - std::clamp((qx - 1_cv) / lx + 0.5_cv, 0_cv, 1_cv);
				const slug_t cy = std::clamp(qy / ly + 0.5_cv, 0_cv, 1_cv) - std::clamp((qy - 1_cv) / ly + 0.5_cv, 0_cv, 1_cv);

				return cx * cy;
			}

			case Kind::Ellipse: {
				const slug_t r = std::hypot(qx, qy);
				const slug_t nx = qx / std::max(r, 1e-9_cv), ny = qy / std::max(r, 1e-9_cv);
				const slug_t gl = std::max(std::hypot(nx * dqxX + ny * dqxY, nx * dqyX + ny * dqyY), 1e-9_cv);

				return std::clamp(0.5_cv - (r - 1_cv) / gl, 0_cv, 1_cv);
			}

			case Kind::Curve: {
				const auto* s = sampler(in.proto);

				if(!s) return 0_cv;

				const slug_t qeX = std::max(std::abs(dqxX) + std::abs(dqyX), 1e-9_cv);
				const slug_t qeY = std::max(std::abs(dqxY) + std::abs(dqyY), 1e-9_cv);
				const Atlas::Shape& sh = s->shape;
				const slug_t bx0 = sh.bearingX, by0 = sh.bearingY - sh.height;
				const slug_t bx1 = sh.bearingX + sh.width, by1 = sh.bearingY;

				if(qx < bx0 - qeX || qy < by0 - qeY || qx > bx1 + qeX || qy > by1 + qeY) return 0_cv;

				return std::clamp(s->renderSampleBanded(qx, qy, 1_cv / qeX, 1_cv / qeY).fill, 0_cv, 1_cv);
			}
		}

		return 0_cv;
	}

	// The shader's fade input: the instance's prototype extent along each prototype axis over that
	// axis' change per screen pixel, the larger of the two (screen px).
	slug_t footprint(const Instance& in, const Inverse& inv, slug_t ppeX, slug_t ppeY) {
		if(!inv.ok || in.proto >= _set.protos.size()) return 0_cv;

		const slug_t lx = std::max(std::hypot(inv.a / ppeX, inv.b / ppeY), 1e-9_cv);
		const slug_t ly = std::max(std::hypot(inv.c / ppeX, inv.d / ppeY), 1e-9_cv);

		slug_t ex = 1_cv, ey = 1_cv;

		switch(_set.protos[in.proto].kind) {
			case Kind::Rect: break;
			case Kind::Ellipse: ex = ey = 2_cv; break;
			case Kind::Curve: {
				const auto* s = sampler(in.proto);

				if(s) { ex = s->shape.width; ey = s->shape.height; }

				break;
			}
		}

		return std::max(ex / lx, ey / ly);
	}

	// The instance's straight color at canvas-em point (x, y) (gradients evaluated in the unit frame).
	Color color(const Instance& in, const Inverse& inv, slug_t x, slug_t y) const {
		const Color tint = _linear ? render::toLinear(in.color) : in.color;

		if(!in.gradient || in.gradient > _set.gradients.size()) return tint;

		const GradientInfo& g = _linear ? _linearGradients[in.gradient - 1] : _set.gradients[in.gradient - 1];
		const slug_t qx = inv.a * x + inv.b * y + inv.e;
		const slug_t qy = inv.c * x + inv.d * y + inv.f;
		const Color gc = render::gradientColor(g, render::gradientT(g, qx, qy));

		return {gc.r * tint.r, gc.g * tint.g, gc.b * tint.b, gc.a * tint.a};
	}

	// Max curves in any single band (h + v) of a Curve prototype's shape; 1 for analytic ones.
	uint32_t bandCost(uint32_t proto) {
		if(proto >= _set.protos.size() || _set.protos[proto].kind != Kind::Curve) return 1;

		const auto* s = sampler(proto);

		if(!s) return 1;

		uint32_t h = 0, v = 0;

		for(size_t i = 0; i + 1 < s->hbandOffsets.size(); i++) h = std::max(h, s->hbandOffsets[i + 1] - s->hbandOffsets[i]);
		for(size_t i = 0; i + 1 < s->vbandOffsets.size(); i++) v = std::max(v, s->vbandOffsets[i + 1] - s->vbandOffsets[i]);

		return std::max<uint32_t>(1, h + v);
	}

private:
	const render::Sampler* sampler(uint32_t proto) {
		if(!_samplers[proto]) {
			const Proto& p = _set.protos[proto];

			if(!_atlas.isBuilt() || !_atlas.getShape(p.key)) return nullptr;

			_samplers[proto] = render::decode(_atlas, p.key);
		}

		return &*_samplers[proto];
	}

	const Atlas& _atlas;
	const Set& _set;
	std::vector<std::optional<render::Sampler>> _samplers;
	bool _linear = false;
	std::vector<GradientInfo> _linearGradients;
};

// ------------------------------------------------------------------------------------------------
// CPU reference rendering
// ------------------------------------------------------------------------------------------------

// Composites one stamp layer: every instance, in paint order, over its own padded pixel box.
inline void renderStampLayer(render::Image& img, Evaluator& ev, const Set& set, const Layer& layer, const render::Window& win) {
	if(!img.width || !img.height) return;

	const slug_t ppeX = cv(img.width) / win.emWidth;
	const slug_t ppeY = cv(img.height) / win.emHeight;

	for(const Instance& in : layer.instances) {
		if(in.proto >= set.protos.size() || in.color.a <= 0_cv) continue;

		const Box b = bounds(in, set.protos[in.proto].kind);
		const Inverse inv = invert(in.m);

		const auto i0 = std::max<int64_t>(0, int64_t(std::floor((b.x0 - win.emX0) * ppeX)) - 1);
		const auto i1 = std::min<int64_t>(img.width, int64_t(std::ceil((b.x1 - win.emX0) * ppeX)) + 1);
		const auto j0 = std::max<int64_t>(0, int64_t(std::floor((b.y0 - win.emY0) * ppeY)) - 1);
		const auto j1 = std::min<int64_t>(img.height, int64_t(std::ceil((b.y1 - win.emY0) * ppeY)) + 1);

		for(int64_t j = j0; j < j1; j++) {
			for(int64_t i = i0; i < i1; i++) {
				const slug_t ex = win.emX0 + (cv(i) + 0.5_cv) / ppeX;
				const slug_t ey = win.emY0 + (cv(j) + 0.5_cv) / ppeY;
				const slug_t cov = ev.coverage(in, inv, ex, ey, ppeX, ppeY);

				if(cov > 0_cv) render::blendPixel(img, uint32_t(i), uint32_t(j), ev.color(in, inv, ex, ey), cov);
			}
		}
	}
}

// render::renderComposite() that also evaluates stamp layers (placeholders) from @p set. The atlas
// must be built and hold the Curve prototypes (registerProtos).
inline render::Image renderComposite(
	const Atlas& atlas,
	const CompositeShape& composite,
	const Set& set,
	uint32_t width,
	uint32_t height,
	slug_t emX0=0_cv,
	slug_t emY0=0_cv,
	slug_t emWidth=1_cv,
	slug_t emHeight=1_cv,
	bool linear=false
) {
	render::Image img{width, height, std::vector<slug_t>(size_t(width) * height * 4, 0_cv)};

	if(!width || !height || emWidth <= 0_cv || emHeight <= 0_cv) return img;

	const render::Window win{emX0, emY0, emWidth, emHeight, linear};
	Evaluator ev(atlas, set, linear);

	for(const auto& layer : composite.layers) {
		if(isStampLayer(layer)) {
			if(layer.drawMode == DrawMode::Visible && stampIndex(layer) < set.layers.size()) {
				renderStampLayer(img, ev, set, set.layers[stampIndex(layer)], win);
			}
		}

		else render::renderLayer(img, atlas, layer, win);
	}

	return img;
}

// ------------------------------------------------------------------------------------------------
// Cell grid (what a GPU stamp shader walks)
// ------------------------------------------------------------------------------------------------
//
// A G x G grid over the key in UV (u = em x / canvas width in em, v = 1 - em y / canvas height in
// em: v up). Cell (i, j) covers u in [i/G, (i+1)/G), v in [j/G, (j+1)/G); index j * G + i. An
// instance is listed, in paint order, in every cell its bbox padded by one texture pixel touches.
// G is the first of 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256 whose busiest cell
// holds <= targetPerCell instances (256 if none does; maxPerCell then reports what remains -
// splitDeep() then cuts the layer into consecutive sub-layers that each fit).
struct Grid {
	uint32_t G = 1;
	uint32_t maxPerCell = 0;
	std::vector<std::vector<uint32_t>> cells;

	// Per cell, premultiplied RGBA8 mean of the layer alone over transparent (4 x 4 point samples
	// per cell), in the color space of the instance colors (or linear when buildGrid's
	// `linearMeans` is set).
	std::vector<uint8_t> means;

	// Per cell, the largest bbox side (canvas em) of any instance listed in it: a view-independent
	// bound for the shader's fade footprint (footprint px <= cellMax * pixels per em).
	std::vector<float> cellMax;
};

inline float srgbToLinear(float c) {
	c = std::clamp(c, 0.0f, 1.0f);

	return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

inline Grid buildGrid(
	const Atlas& atlas,
	const Set& set,
	const Layer& layer,
	slug_t canvasEmW,
	slug_t canvasEmH,
	slug_t pxPerEm,
	uint32_t targetPerCell=32,
	bool linearMeans=false,
	bool computeMeans=true
) {
	static constexpr uint32_t CANDIDATES[] = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256};

	struct UVBox { double u0, v0, u1, v1; };

	std::vector<UVBox> boxes;

	boxes.reserve(layer.instances.size());

	const double pad = pxPerEm > 0_cv ? 1.0 / double(pxPerEm) : 0.0;

	for(const Instance& in : layer.instances) {
		const Kind k = in.proto < set.protos.size() ? set.protos[in.proto].kind : Kind::Rect;
		const Box b = bounds(in, k);

		boxes.push_back({
			(double(b.x0) - pad) / canvasEmW,
			1.0 - (double(b.y1) + pad) / canvasEmH,
			(double(b.x1) + pad) / canvasEmW,
			1.0 - (double(b.y0) - pad) / canvasEmH
		});
	}

	Grid g;

	auto range = [](double a, double b, uint32_t G) {
		const auto lo = static_cast<int64_t>(std::floor(a * G));
		const auto hi = static_cast<int64_t>(std::floor(b * G));

		return std::pair<uint32_t, uint32_t>{
			uint32_t(std::clamp<int64_t>(lo, 0, G - 1)), uint32_t(std::clamp<int64_t>(hi, 0, G - 1))
		};
	};

	for(uint32_t G : CANDIDATES) {
		std::vector<uint32_t> counts(size_t(G) * G, 0);
		uint32_t mx = 0;

		for(const UVBox& b : boxes) {
			if(b.u1 < 0 || b.v1 < 0 || b.u0 > 1 || b.v0 > 1) continue;

			const auto [i0, i1] = range(b.u0, b.u1, G);
			const auto [j0, j1] = range(b.v0, b.v1, G);

			for(uint32_t j = j0; j <= j1; j++) for(uint32_t i = i0; i <= i1; i++) mx = std::max(mx, ++counts[size_t(j) * G + i]);
		}

		g.G = G;
		g.maxPerCell = mx;

		if(mx <= targetPerCell) break;
	}

	g.cells.assign(size_t(g.G) * g.G, {});
	g.cellMax.assign(size_t(g.G) * g.G, 0.0f);

	for(uint32_t n = 0; n < boxes.size(); n++) {
		const UVBox& b = boxes[n];

		if(b.u1 < 0 || b.v1 < 0 || b.u0 > 1 || b.v0 > 1) continue;

		const auto [i0, i1] = range(b.u0, b.u1, g.G);
		const auto [j0, j1] = range(b.v0, b.v1, g.G);

		const Instance& in = layer.instances[n];
		const Box eb = bounds(in, in.proto < set.protos.size() ? set.protos[in.proto].kind : Kind::Rect);
		const float side = float(std::max(eb.x1 - eb.x0, eb.y1 - eb.y0));

		for(uint32_t j = j0; j <= j1; j++) for(uint32_t i = i0; i <= i1; i++) {
			g.cells[size_t(j) * g.G + i].push_back(n);
			g.cellMax[size_t(j) * g.G + i] = std::max(g.cellMax[size_t(j) * g.G + i], side);
		}
	}

	if(!computeMeans) return g;

	// Mean colors: 4 x 4 point samples per cell, the cell's instances composited in paint order.
	Evaluator ev(atlas, set, linearMeans);
	std::vector<Inverse> inv(layer.instances.size());

	for(size_t n = 0; n < layer.instances.size(); n++) inv[n] = invert(layer.instances[n].m);

	g.means.assign(size_t(g.G) * g.G * 4, 0);

	for(uint32_t j = 0; j < g.G; j++) {
		for(uint32_t i = 0; i < g.G; i++) {
			const auto& list = g.cells[size_t(j) * g.G + i];

			if(list.empty()) continue;

			double acc[4] = {0, 0, 0, 0};

			for(int sy = 0; sy < 4; sy++) {
				for(int sx = 0; sx < 4; sx++) {
					const double u = (i + (sx + 0.5) / 4.0) / g.G;
					const double v = (j + (sy + 0.5) / 4.0) / g.G;
					const slug_t ex = slug_t(u * canvasEmW);
					const slug_t ey = slug_t((1.0 - v) * canvasEmH);

					double px[4] = {0, 0, 0, 0};

					for(uint32_t n : list) {
						const Instance& in = layer.instances[n];
						const double cov = ev.coverage(in, inv[n], ex, ey, pxPerEm, pxPerEm);

						if(cov <= 0) continue;

						const Color c = ev.color(in, inv[n], ex, ey);

						const double a = c.a * cov;

						px[0] = c.r * a + px[0] * (1 - a);
						px[1] = c.g * a + px[1] * (1 - a);
						px[2] = c.b * a + px[2] * (1 - a);
						px[3] = a + px[3] * (1 - a);
					}

					for(int k = 0; k < 4; k++) acc[k] += px[k];
				}
			}

			for(int k = 0; k < 4; k++) {
				g.means[(size_t(j) * g.G + i) * 4 + size_t(k)] = uint8_t(std::lround(std::clamp(acc[k] / 16.0, 0.0, 1.0) * 255.0));
			}
		}
	}

	return g;
}

// ------------------------------------------------------------------------------------------------
// Expansion to ordinary contours (canvas em) - what bake.hpp tessellates for stamped keys.
// ------------------------------------------------------------------------------------------------
inline Atlas::Contours instanceContours(const Set& set, const Instance& in) {
	Atlas::Contours out;

	if(in.proto >= set.protos.size()) return out;

	const Proto& p = set.protos[in.proto];

	auto xf = [&](Atlas::Curve c) {
		in.m.apply(c.x1, c.y1, c.x1, c.y1);
		in.m.apply(c.x2, c.y2, c.x2, c.y2);
		in.m.apply(c.x3, c.y3, c.x3, c.y3);

		return c;
	};

	Atlas::Curves curves;
	CurveDecomposer d(curves);

	if(p.kind == Kind::Rect) {
		d.moveTo(0_cv, 0_cv); d.lineTo(1_cv, 0_cv); d.lineTo(1_cv, 1_cv); d.lineTo(0_cv, 1_cv); d.close();
	}

	else if(p.kind == Kind::Ellipse) {
		const slug_t k = 0.5522847498_cv;

		d.moveTo(1_cv, 0_cv);
		d.cubicTo(1_cv, k, k, 1_cv, 0_cv, 1_cv);
		d.cubicTo(-k, 1_cv, -1_cv, k, -1_cv, 0_cv);
		d.cubicTo(-1_cv, -k, -k, -1_cv, 0_cv, -1_cv);
		d.cubicTo(k, -1_cv, 1_cv, -k, 1_cv, 0_cv);
	}

	else {
		const size_t n = std::max<size_t>(p.starts.size(), 1);

		for(size_t s = 0; s < n; s++) {
			const size_t b = p.starts.empty() ? 0 : p.starts[s];
			const size_t e = (s + 1 < p.starts.size()) ? p.starts[s + 1] : p.curves.size();

			Atlas::Curves c;

			for(size_t i = b; i < e && i < p.curves.size(); i++) c.push_back(xf(p.curves[i]));

			if(!c.empty()) out.push_back(std::move(c));
		}

		return out;
	}

	for(auto& c : curves) c = xf(c);

	out.push_back(std::move(curves));

	return out;
}


// ------------------------------------------------------------------------------------------------
// Depth splitting
// ------------------------------------------------------------------------------------------------
//
// A stamp shader walks one cell's list per fragment with a fixed loop bound (maxPerCell, 32). A
// finer grid cannot help where instances genuinely pile up on one point, so a layer whose busiest
// cell still exceeds the bound at the finest grid is cut into CONSECUTIVE sub-layers (paint order
// kept) that each fit. Placeholders in @p composite are renumbered accordingly. Returns the
// deepest overlap seen (instances on one finest-grid cell), the "max depth".
inline constexpr size_t LIMIT_U16 = 65536;

inline uint32_t splitDeep(Set& set, CompositeShape& composite, slug_t canvasEmW, slug_t canvasEmH, slug_t pxPerEm, uint32_t maxPerCell=32) {
	uint32_t deepest = 0;

	const Atlas none;
	std::vector<Layer> outLayers;
	std::vector<std::vector<size_t>> remap(set.layers.size());

	for(size_t li = 0; li < set.layers.size(); li++) {
		Layer& layer = set.layers[li];
		const Grid g = buildGrid(none, set, layer, canvasEmW, canvasEmH, pxPerEm, maxPerCell, false, false);

		deepest = std::max(deepest, g.maxPerCell);

		if(g.maxPerCell <= maxPerCell) {
			remap[li].push_back(outLayers.size());
			outLayers.push_back(std::move(layer));

			continue;
		}

		// Greedy cut on the finest grid: a new sub-layer starts when an instance would push one of
		// its cells past the bound.
		const uint32_t G = g.G;
		const double pad = pxPerEm > 0_cv ? 1.0 / double(pxPerEm) : 0.0;
		std::vector<uint32_t> counts(size_t(G) * G, 0);
		std::vector<size_t> cells;
		Layer cur;

		for(Instance& in : layer.instances) {
			cells.clear();

			const Box b = bounds(in, in.proto < set.protos.size() ? set.protos[in.proto].kind : Kind::Rect);
			const double u0 = (double(b.x0) - pad) / canvasEmW, u1 = (double(b.x1) + pad) / canvasEmW;
			const double v0 = 1.0 - (double(b.y1) + pad) / canvasEmH, v1 = 1.0 - (double(b.y0) - pad) / canvasEmH;

			if(!(u1 < 0 || v1 < 0 || u0 > 1 || v0 > 1)) {
				auto cl = [&](double x) { return uint32_t(std::clamp<int64_t>(int64_t(std::floor(x * G)), 0, int64_t(G) - 1)); };

				for(uint32_t j = cl(v0); j <= cl(v1); j++) for(uint32_t i = cl(u0); i <= cl(u1); i++) cells.push_back(size_t(j) * G + i);
			}

			bool full = false;

			for(size_t c : cells) if(counts[c] + 1 > maxPerCell) { full = true; break; }

			if(full && !cur.instances.empty()) {
				remap[li].push_back(outLayers.size());
				outLayers.push_back(std::move(cur));
				cur = Layer{};
				std::fill(counts.begin(), counts.end(), 0u);
			}

			for(size_t c : cells) counts[c]++;

			cur.instances.push_back(in);
		}

		if(!cur.instances.empty()) {
			remap[li].push_back(outLayers.size());
			outLayers.push_back(std::move(cur));
		}
	}

	// Encoding limits (RG16UI cells): per stamp layer < 65536 instances and < 65536 u16 elements of
	// headers + lists. Halve offending layers (consecutive halves keep paint order) until they fit.
	{
		std::vector<Layer> fitted;
		std::vector<std::vector<size_t>> remap2(outLayers.size());

		std::function<void(Layer&&, size_t)> fit = [&](Layer&& l, size_t origin) {
			bool ok = l.instances.size() < LIMIT_U16;

			if(ok) {
				const Grid g = buildGrid(none, set, l, canvasEmW, canvasEmH, pxPerEm, maxPerCell, false, false);
				size_t elements = size_t(2) * g.G * g.G;

				for(const auto& c : g.cells) elements += c.size();

				ok = elements < LIMIT_U16;
			}

			if(ok || l.instances.size() < 2) {
				remap2[origin].push_back(fitted.size());
				fitted.push_back(std::move(l));

				return;
			}

			const auto half = static_cast<std::ptrdiff_t>(l.instances.size() / 2);
			Layer a, b;

			a.instances.assign(l.instances.begin(), l.instances.begin() + half);
			b.instances.assign(l.instances.begin() + half, l.instances.end());

			fit(std::move(a), origin);
			fit(std::move(b), origin);
		};

		for(size_t i = 0; i < outLayers.size(); i++) fit(std::move(outLayers[i]), i);

		for(auto& r : remap) {
			std::vector<size_t> nr;

			for(size_t i : r) for(size_t j : remap2[i]) nr.push_back(j);

			r = std::move(nr);
		}

		outLayers = std::move(fitted);
	}

	set.layers = std::move(outLayers);

	std::vector<slughorn::Layer> layers;

	for(const auto& l : composite.layers) {
		if(!isStampLayer(l) || stampIndex(l) >= remap.size()) {
			layers.push_back(l);

			continue;
		}

		for(size_t ni : remap[stampIndex(l)]) {
			slughorn::Layer p = l;

			p.effectParam = static_cast<slug_t>(ni);
			layers.push_back(p);
		}
	}

	composite.layers = std::move(layers);

	return deepest;
}

// ------------------------------------------------------------------------------------------------
// Repeat wrapping
// ------------------------------------------------------------------------------------------------
//
// For keys sampled with repeat wrapping: every instance whose AA-padded box crosses a canvas edge
// gets copies shifted by the canvas size toward the opposite edge(s), right after it in paint
// order, so a tiled texture has no seam - unless the recorder already drew that copy (an instance
// with the same prototype, paint and linear part at exactly that offset), which is then not
// doubled. Returns how many copies were added.
inline size_t wrapDuplicates(Set& set, slug_t canvasEmW, slug_t canvasEmH, slug_t pxPerEm) {
	const slug_t pad = pxPerEm > 0_cv ? 1_cv / pxPerEm : 0_cv;
	size_t added = 0;

	auto key = [](const Instance& in, slug_t dx, slug_t dy) {
		auto q = [](double v) { return std::llround(v * 4096.0); };

		return std::array<int64_t, 12>{
			int64_t(in.proto), int64_t(in.gradient), q(in.m.xx), q(in.m.yx), q(in.m.xy), q(in.m.yy),
			q(in.m.dx + dx), q(in.m.dy + dy), q(in.color.r), q(in.color.g), q(in.color.b), q(in.color.a)
		};
	};

	for(Layer& layer : set.layers) {
		std::map<std::array<int64_t, 12>, int> present;

		for(const Instance& in : layer.instances) present[key(in, 0_cv, 0_cv)]++;

		std::vector<Instance> out;

		out.reserve(layer.instances.size());

		for(const Instance& in : layer.instances) {
			out.push_back(in);

			const Box b = bounds(in, in.proto < set.protos.size() ? set.protos[in.proto].kind : Kind::Rect);

			for(int sy = -1; sy <= 1; sy++) {
				for(int sx = -1; sx <= 1; sx++) {
					if(!sx && !sy) continue;

					const slug_t dx = slug_t(sx) * canvasEmW, dy = slug_t(sy) * canvasEmH;

					// The copy must land on the canvas (padded).
					if(b.x1 + dx + pad < 0_cv || b.x0 + dx - pad > canvasEmW || b.y1 + dy + pad < 0_cv || b.y0 + dy - pad > canvasEmH) continue;

					if(present.count(key(in, dx, dy))) continue;

					Instance c = in;

					c.m.dx += dx;
					c.m.dy += dy;
					out.push_back(c);
					present[key(c, 0_cv, 0_cv)]++;
					added++;
				}
			}
		}

		layer.instances = std::move(out);
	}

	return added;
}

// ------------------------------------------------------------------------------------------------
// SVG pre-pass + loader
// ------------------------------------------------------------------------------------------------
namespace detail {

struct Tag {
	std::string name;
	std::vector<std::pair<std::string, std::string>> attrs;
	bool closing = false;
	bool selfClosing = false;
	size_t begin = 0, end = 0;

	const std::string* attr(std::string_view n) const {
		for(const auto& [k, v] : attrs) if(k == n) return &v;

		return nullptr;
	}
};

inline bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Next element tag at or after @p pos; skips comments, CDATA, <? ?> and <! >.
inline bool nextTag(std::string_view s, size_t& pos, Tag& t) {
	constexpr char DQ = '"';
	constexpr char SQ = 39; // single quote

	while(true) {
		const size_t lt = s.find('<', pos);

		if(lt == std::string_view::npos) return false;

		if(s.compare(lt, 4, "<!--") == 0) {
			const size_t e = s.find("-->", lt + 4);

			if(e == std::string_view::npos) return false;

			pos = e + 3;

			continue;
		}

		if(s.compare(lt, 9, "<![CDATA[") == 0) {
			const size_t e = s.find("]]>", lt + 9);

			if(e == std::string_view::npos) return false;

			pos = e + 3;

			continue;
		}

		if(lt + 1 < s.size() && (s[lt + 1] == '?' || s[lt + 1] == '!')) {
			const size_t e = s.find('>', lt + 1);

			if(e == std::string_view::npos) return false;

			pos = e + 1;

			continue;
		}

		t = Tag{};
		t.begin = lt;

		size_t i = lt + 1;

		if(i < s.size() && s[i] == '/') { t.closing = true; i++; }

		while(i < s.size() && !isSpace(s[i]) && s[i] != '>' && s[i] != '/') t.name += s[i++];

		while(i < s.size()) {
			while(i < s.size() && isSpace(s[i])) i++;

			if(i >= s.size()) return false;

			if(s[i] == '>') { i++; break; }

			if(s[i] == '/' && i + 1 < s.size() && s[i + 1] == '>') { t.selfClosing = true; i += 2; break; }

			std::string key;

			while(i < s.size() && !isSpace(s[i]) && s[i] != '=' && s[i] != '>' && s[i] != '/') key += s[i++];

			while(i < s.size() && isSpace(s[i])) i++;

			std::string value;

			if(i < s.size() && s[i] == '=') {
				i++;

				while(i < s.size() && isSpace(s[i])) i++;

				if(i < s.size() && (s[i] == DQ || s[i] == SQ)) {
					const char q = s[i++];
					const size_t e = s.find(q, i);

					if(e == std::string_view::npos) return false;

					value = std::string(s.substr(i, e - i));
					i = e + 1;
				}
			}

			else if(key.empty()) i++;

			if(!key.empty()) t.attrs.emplace_back(std::move(key), std::move(value));
		}

		t.end = i;
		pos = i;

		return true;
	}
}

inline std::vector<double> numbers(std::string_view s) {
	std::vector<double> out;
	std::string buf(s);
	const char* p = buf.c_str();

	while(*p) {
		if(std::isdigit(static_cast<unsigned char>(*p)) || *p == '-' || *p == '+' || *p == '.') {
			char* e = nullptr;
			const double v = std::strtod(p, &e);

			if(e == p) { p++; continue; }

			out.push_back(v);
			p = e;
		}

		else p++;
	}

	return out;
}

// SVG transform list -> Matrix (matrix / translate / scale / rotate). Empty = identity.
inline bool parseTransform(std::string_view s, Matrix& out) {
	out = Matrix::identity();

	size_t i = 0;

	while(i < s.size()) {
		while(i < s.size() && (isSpace(s[i]) || s[i] == ',')) i++;

		if(i >= s.size()) break;

		const size_t open = s.find('(', i);
		const size_t close = open == std::string_view::npos ? open : s.find(')', open);

		if(close == std::string_view::npos) return false;

		std::string name(s.substr(i, open - i));

		while(!name.empty() && isSpace(name.back())) name.pop_back();

		const auto v = numbers(s.substr(open + 1, close - open - 1));
		Matrix m = Matrix::identity();

		if(name == "matrix" && v.size() == 6) {
			m = {.xx = slug_t(v[0]), .yx = slug_t(v[1]), .xy = slug_t(v[2]), .yy = slug_t(v[3]), .dx = slug_t(v[4]), .dy = slug_t(v[5])};
		}

		else if(name == "translate" && (v.size() == 1 || v.size() == 2)) {
			m = Matrix::translate(slug_t(v[0]), slug_t(v.size() > 1 ? v[1] : 0.0));
		}

		else if(name == "scale" && (v.size() == 1 || v.size() == 2)) {
			m = Matrix::scale(slug_t(v[0]), slug_t(v.size() > 1 ? v[1] : v[0]));
		}

		else if(name == "rotate" && v.size() == 1) {
			m = Matrix::rotate(slug_t(v[0] * double(PI_CV) / 180.0));
		}

		else return false;

		out = out * m;
		i = close + 1;
	}

	return true;
}

inline bool parseColor(std::string_view s, Color& c) {
	while(!s.empty() && isSpace(s.front())) s.remove_prefix(1);
	while(!s.empty() && isSpace(s.back())) s.remove_suffix(1);

	auto hex = [](char h) -> int {
		if(h >= '0' && h <= '9') return h - '0';
		if(h >= 'a' && h <= 'f') return h - 'a' + 10;
		if(h >= 'A' && h <= 'F') return h - 'A' + 10;
		return -1;
	};

	if(!s.empty() && s[0] == '#') {
		if(s.size() == 7) {
			int v[6];

			for(int k = 0; k < 6; k++) if((v[k] = hex(s[size_t(k) + 1])) < 0) return false;

			c = {cv(v[0] * 16 + v[1]) / 255_cv, cv(v[2] * 16 + v[3]) / 255_cv, cv(v[4] * 16 + v[5]) / 255_cv, 1_cv};

			return true;
		}

		if(s.size() == 4) {
			int v[3];

			for(int k = 0; k < 3; k++) if((v[k] = hex(s[size_t(k) + 1])) < 0) return false;

			c = {cv(v[0] * 17) / 255_cv, cv(v[1] * 17) / 255_cv, cv(v[2] * 17) / 255_cv, 1_cv};

			return true;
		}

		return false;
	}

	if(s.substr(0, 4) == "rgb(" || s.substr(0, 5) == "rgba(") {
		const auto v = numbers(s.substr(s.find('(')));

		if(v.size() < 3) return false;

		c = {slug_t(v[0] / 255.0), slug_t(v[1] / 255.0), slug_t(v[2] / 255.0), slug_t(v.size() > 3 ? v[3] : 1.0)};

		return true;
	}

	if(s == "black") { c = {0_cv, 0_cv, 0_cv, 1_cv}; return true; }
	if(s == "white") { c = {1_cv, 1_cv, 1_cv, 1_cv}; return true; }

	return false;
}

inline double toNumber(const std::string* v, double dflt) {
	if(!v || v->empty()) return dflt;

	return std::strtod(v->c_str(), nullptr);
}

// url(#id) -> id ("" if not a url)
inline std::string urlId(std::string_view v) {
	const size_t a = v.find("url(");

	if(a == std::string_view::npos) return {};

	const size_t b = v.find('#', a);
	const size_t e = v.find(')', a);

	if(b == std::string_view::npos || e == std::string_view::npos || b > e) return {};

	return std::string(v.substr(b + 1, e - b - 1));
}

}

// One <use> of a run: its symbol, its transform (user space), and the presentation attributes it
// passes down (the run <g>'s, overridden by the use's own).
struct UseRef {
	std::string symbol;
	Matrix m = Matrix::identity();
	std::vector<std::pair<std::string, std::string>> attrs;
};

struct Run {
	size_t begin = 0, end = 0; // byte range of <g data-stamp-run>...</g>
	bool ok = true;
	std::string why;
	std::vector<UseRef> uses;
};

struct SymbolDef {
	Kind kind = Kind::Rect;
	bool overflowVisible = false;
	bool hasViewBox = false;
	size_t contentBegin = 0, contentEnd = 0;
};

struct PrePass {
	std::string svg;                       // rewritten text (runs replaced by markers)
	std::vector<Run> runs;                 // every run found (ok or not)
	std::map<std::string, SymbolDef> symbols;
	std::map<std::string, std::pair<size_t, size_t>> elements; // id -> [begin, end) of the element
	std::map<std::string, std::string> hrefs;                  // gradient id -> href'd gradient id
	double width = 0, height = 0;          // root <svg> size (user units)
	Matrix viewBox = Matrix::identity();   // user space -> picture pixels
};

inline constexpr std::string_view MARKER_PREFIX = "__slug_stamp_";

// Presentation attributes a <use> or its run <g> may pass down to the prototype.
inline bool isPaintAttr(std::string_view k) {
	return k == "fill" || k == "fill-opacity" || k == "fill-rule" || k == "opacity" || k == "stroke" ||
		k == "stroke-opacity" || k == "stroke-width" || k == "stroke-linecap" || k == "stroke-linejoin" ||
		k == "stroke-miterlimit" || k == "stroke-dasharray" || k == "stroke-dashoffset";
}

// The XML pre-pass: finds stamp symbols and runs, lifts the representable runs out of the text and
// leaves `<path id="__slug_stamp_<k>" .../>` markers at their paint positions.
inline PrePass prePass(std::string_view svg) {
	using namespace detail;

	PrePass out;

	struct Open { Tag tag; int runIndex = -1; std::string symbolId; std::string id; };

	std::vector<Open> stack;
	size_t pos = 0;
	Tag t;

	while(nextTag(svg, pos, t)) {
		if(t.closing) {
			while(!stack.empty()) {
				Open o = std::move(stack.back());

				stack.pop_back();

				if(o.runIndex >= 0) out.runs[size_t(o.runIndex)].end = t.end;
				if(!o.symbolId.empty()) out.symbols[o.symbolId].contentEnd = t.begin;
				if(!o.id.empty()) out.elements[o.id].second = t.end;

				if(o.tag.name == t.name) break;
			}

			continue;
		}

		Open o;

		o.tag = t;

		if(const std::string* id = t.attr("id")) {
			o.id = t.selfClosing ? std::string() : *id;
			out.elements[*id] = {t.begin, t.end};

			const std::string* href = t.attr("href");

			if(!href) href = t.attr("xlink:href");

			if(href && href->size() > 1 && (*href)[0] == '#' && t.name.find("Gradient") != std::string::npos) out.hrefs[*id] = href->substr(1);
		}

		if(t.name == "svg" && stack.empty()) {
			out.width = toNumber(t.attr("width"), 0);
			out.height = toNumber(t.attr("height"), 0);

			if(const std::string* vb = t.attr("viewBox")) {
				const auto v = numbers(*vb);

				if(v.size() == 4 && v[2] > 0 && v[3] > 0) {
					if(out.width <= 0) out.width = v[2];
					if(out.height <= 0) out.height = v[3];

					const double s = std::min(out.width / v[2], out.height / v[3]);
					const double tx = (out.width - v[2] * s) * 0.5 - v[0] * s;
					const double ty = (out.height - v[3] * s) * 0.5 - v[1] * s;

					out.viewBox = {.xx = slug_t(s), .yy = slug_t(s), .dx = slug_t(tx), .dy = slug_t(ty)};
				}
			}
		}

		else if(t.name == "symbol" && t.attr("data-stamp-kind") && t.attr("id")) {
			SymbolDef def;
			const std::string& k = *t.attr("data-stamp-kind");

			def.kind = k == "ellipse" ? Kind::Ellipse : k == "rect" ? Kind::Rect : Kind::Curve;

			const std::string* ov = t.attr("overflow");
			const std::string* st = t.attr("style");

			def.overflowVisible = (ov && *ov == "visible") || (st && (st->find("overflow:visible") != std::string::npos ||
				st->find("overflow: visible") != std::string::npos));
			def.hasViewBox = t.attr("viewBox") != nullptr;
			def.contentBegin = t.end;
			def.contentEnd = t.end;

			out.symbols[*t.attr("id")] = def;
			o.symbolId = t.selfClosing ? std::string() : *t.attr("id");
		}

		else if(t.name == "g" && t.attr("data-stamp-run")) {
			Run r;

			r.begin = t.begin;
			r.end = t.end;

			for(const Open& a : stack) {
				if(a.tag.name == "svg") continue;

				for(const char* bad : {"transform", "clip-path", "mask", "opacity", "filter", "style"}) {
					if(a.tag.attr(bad)) { r.ok = false; r.why = std::string("ancestor <") + a.tag.name + "> has " + bad; }
				}
			}

			for(const char* bad : {"transform", "clip-path", "mask", "opacity", "filter", "style"}) {
				if(t.attr(bad)) { r.ok = false; r.why = std::string("run <g> has ") + bad; }
			}

			o.runIndex = int(out.runs.size());
			out.runs.push_back(std::move(r));
		}

		else if(!stack.empty() && stack.back().runIndex >= 0) {
			Run& r = out.runs[size_t(stack.back().runIndex)];
			const Tag& g = stack.back().tag;

			if(t.name != "use") {
				r.ok = false;
				r.why = "run holds a <" + t.name + ">";
			}

			else {
				const std::string* href = t.attr("href");

				if(!href) href = t.attr("xlink:href");

				UseRef u;
				bool ok = href && href->size() > 1 && (*href)[0] == '#';

				if(ok && t.attr("transform")) ok = parseTransform(*t.attr("transform"), u.m);

				if(ok && (t.attr("x") || t.attr("y"))) {
					u.m = u.m * Matrix::translate(slug_t(toNumber(t.attr("x"), 0)), slug_t(toNumber(t.attr("y"), 0)));
				}

				if(ok && (t.attr("style") || t.attr("clip-path") || t.attr("mask") || t.attr("filter"))) ok = false;

				if(!ok) {
					r.ok = false;
					r.why = "unsupported <use> attributes";
				}

				else {
					u.symbol = href->substr(1);

					for(const auto& [k, v] : g.attrs) if(isPaintAttr(k)) u.attrs.emplace_back(k, v);

					for(const auto& [k, v] : t.attrs) {
						if(!isPaintAttr(k)) continue;

						bool replaced = false;

						for(auto& kv : u.attrs) if(kv.first == k) { kv.second = v; replaced = true; }

						if(!replaced) u.attrs.emplace_back(k, v);
					}

					r.uses.push_back(std::move(u));
				}
			}
		}

		if(!t.selfClosing) stack.push_back(std::move(o));
	}

	// Symbol checks.
	for(Run& r : out.runs) {
		if(!r.ok) continue;

		for(const UseRef& u : r.uses) {
			auto it = out.symbols.find(u.symbol);

			if(it == out.symbols.end()) { r.ok = false; r.why = "unknown stamp symbol #" + u.symbol; break; }
			if(it->second.hasViewBox) { r.ok = false; r.why = "symbol #" + u.symbol + " has a viewBox"; break; }

			if(!it->second.overflowVisible) {
				r.ok = false;
				r.why = "symbol #" + u.symbol + " lacks overflow=visible (SVG clips a symbol to its viewport)";
				break;
			}
		}
	}

	// Rewrite: representable runs -> markers.
	std::string& o = out.svg;
	size_t at = 0;

	o.reserve(svg.size());

	for(size_t k = 0; k < out.runs.size(); k++) {
		const Run& r = out.runs[k];

		if(!r.ok || r.end <= r.begin) continue;

		o.append(svg.substr(at, r.begin - at));
		o += "<path id=\"" + std::string(MARKER_PREFIX) + std::to_string(k) + "\" d=\"M0 0H1V1H0Z\" fill=\"#000\"/>";
		at = r.end;
	}

	o.append(svg.substr(at));

	return out;
}

#ifdef SLUGHORN_HAS_THORVG

namespace detail {

inline bool sameGradient(const GradientInfo& a, const GradientInfo& b) {
	auto eq = [](const Matrix& x, const Matrix& y) {
		return x.xx == y.xx && x.yx == y.yx && x.xy == y.xy && x.yy == y.yy && x.dx == y.dx && x.dy == y.dy;
	};

	if(a.type != b.type || !eq(a.transform, b.transform) || a.innerRadius != b.innerRadius || a.stops.size() != b.stops.size()) return false;

	for(size_t i = 0; i < a.stops.size(); i++) {
		const auto& p = a.stops[i];
		const auto& q = b.stops[i];

		if(p.t != q.t || p.color.r != q.color.r || p.color.g != q.color.g || p.color.b != q.color.b || p.color.a != q.color.a) return false;
	}

	return true;
}

}

// Loads @p svg like thorvg::loadString(), with stamp runs lifted into stamp layers of @p set (one
// placeholder Layer at each run's paint position).
//
// Every distinct (symbol, paint signature) is resolved ONCE through ThorVG itself: the symbol's
// content inside a <g> carrying the use's paint attributes (solid colors normalized to black,
// gradients kept by url), in a 1 x 1 picture whose unit is the prototype frame. That one load
// yields the prototype's parts in paint order - fills, strokes already expanded to fill contours
// (stroke prototypes become Curve prototypes), gradients converted into the prototype frame - so
// mixed prototypes, url() fills and stroke symbols all follow ThorVG's own semantics. Rect and
// ellipse symbols that resolve to a single fill stay analytic.
//
// Curve prototypes are registered in @p atlas under protoPrefix + "p_<n>", gradients with
// atlas.addGradient(). Problems go to @p notes, one line each (fallback runs included).
inline CompositeShape loadString(
	std::string_view svg,
	Atlas& atlas,
	KeyIterator& keys,
	thorvg::LoadConfig* config,
	Set& set,
	const std::string& protoPrefix,
	std::vector<std::string>* notes=nullptr
) {
	using namespace detail;

	PrePass pp = prePass(svg);

	for(size_t k = 0; k < pp.runs.size(); k++) {
		if(!pp.runs[k].ok && notes) notes->push_back("stamp run " + std::to_string(k) + " left to ThorVG: " + pp.runs[k].why);
	}

	thorvg::LoadConfig local;
	thorvg::LoadConfig& cfg = config ? *config : local;

	CompositeShape comp = thorvg::loadString(pp.svg, atlas, keys, 96_cv, &cfg);

	if(cfg.width <= 0_cv) return comp;

	// user space -> canvas em
	const Matrix toEm = Matrix::scale(1_cv / cfg.width, 1_cv / cfg.width) * pp.viewBox;

	struct Part {
		uint32_t proto = 0;
		uint32_t gradient = 0;  // 1-based into set.gradients
		bool stroke = false;
		Color color = {};       // as ThorVG resolved it with black / opaque paints
	};

	std::map<std::string, std::vector<Part>> resolved; // signature -> parts
	std::map<int, uint32_t> analyticProto;             // Kind -> proto index

	auto gradientText = [&](const std::string& id) {
		std::string text;
		std::string cur = id;

		for(int guard = 0; guard < 8 && !cur.empty(); guard++) {
			auto it = pp.elements.find(cur);

			if(it == pp.elements.end()) break;

			text += std::string(svg.substr(it->second.first, it->second.second - it->second.first));

			auto h = pp.hrefs.find(cur);

			cur = h == pp.hrefs.end() ? std::string() : h->second;
		}

		return text;
	};

	auto resolve = [&](const UseRef& u) -> const std::vector<Part>* {
		const SymbolDef& def = pp.symbols.at(u.symbol);

		std::string wrap = "<g";
		std::string defs;

		for(const auto& [k, v] : u.attrs) {
			if(k == "fill-opacity" || k == "stroke-opacity" || k == "opacity") continue;

			std::string value = v;

			if(k == "fill" || k == "stroke") {
				const std::string g = urlId(v);

				if(!g.empty()) defs += gradientText(g);
				else if(v != "none") value = "#000";
			}

			wrap += " " + k + "=\"" + value + "\"";
		}

		wrap += ">";

		const std::string signature = u.symbol + "|" + wrap;

		auto hit = resolved.find(signature);

		if(hit != resolved.end()) return &hit->second;

		const std::string mini =
			"<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"1\" height=\"1\" viewBox=\"0 0 1 1\"><defs>" +
			defs + "</defs>" + wrap + std::string(svg.substr(def.contentBegin, def.contentEnd - def.contentBegin)) + "</g></svg>";

		Atlas tmp;
		KeyIterator tk("q", true);
		thorvg::LoadConfig mc;

		mc.autoMetrics = false;
		mc.log = [](int, std::string_view) {};

		const CompositeShape parts = thorvg::loadString(mini, tmp, tk, 96_cv, &mc);
		std::vector<Part> out;

		const bool analytic = (def.kind == Kind::Rect || def.kind == Kind::Ellipse) && parts.layers.size() == 1 &&
			!(mc.layers.size() == 1 && mc.layers[0].stroke);

		for(size_t li = 0; li < parts.layers.size(); li++) {
			const slughorn::Layer& l = parts.layers[li];
			Part p;

			p.stroke = li < mc.layers.size() && mc.layers[li].stroke;
			p.color = l.color;

			if(l.gradientId > 0 && l.gradientId <= tmp.getGradients().size()) {
				const GradientInfo& g = tmp.getGradients()[l.gradientId - 1];
				uint32_t gi = 0;

				for(size_t k = 0; k < set.gradients.size(); k++) if(sameGradient(set.gradients[k], g)) { gi = uint32_t(k + 1); break; }

				if(!gi) {
					set.gradients.push_back(g);
					gi = uint32_t(set.gradients.size());
				}

				p.gradient = gi;
			}

			if(analytic) {
				const int key = int(def.kind);
				auto a = analyticProto.find(key);

				if(a == analyticProto.end()) {
					Proto pr;

					pr.kind = def.kind;
					set.protos.push_back(pr);
					a = analyticProto.emplace(key, uint32_t(set.protos.size() - 1)).first;
				}

				p.proto = a->second;
			}

			else {
				const auto sh = tmp.getShape(l.key);

				if(!sh || sh->curves.empty()) continue;

				Proto pr;

				pr.kind = Kind::Curve;
				pr.curves = sh->curves;
				pr.starts = sh->contourStarts;
				set.protos.push_back(std::move(pr));
				p.proto = uint32_t(set.protos.size() - 1);
			}

			out.push_back(p);
		}

		return &(resolved[signature] = std::move(out));
	};

	auto attr = [](const UseRef& u, std::string_view k) -> const std::string* {
		for(const auto& [kk, v] : u.attrs) if(kk == k) return &v;

		return nullptr;
	};

	std::vector<slughorn::Layer> layers;
	std::vector<thorvg::LayerInfo> infos;

	for(size_t li = 0; li < comp.layers.size(); li++) {
		const thorvg::LayerInfo info = li < cfg.layers.size() ? cfg.layers[li] : thorvg::LayerInfo{};

		if(info.id.rfind(MARKER_PREFIX, 0) != 0) {
			layers.push_back(comp.layers[li]);
			infos.push_back(info);

			continue;
		}

		const size_t runIndex = std::strtoul(info.id.c_str() + MARKER_PREFIX.size(), nullptr, 10);

		if(runIndex >= pp.runs.size()) continue;

		Layer sl;

		for(const UseRef& u : pp.runs[runIndex].uses) {
			const std::vector<Part>* parts = resolve(u);

			if(!parts) continue;

			const double opacity = std::clamp(toNumber(attr(u, "opacity"), 1.0), 0.0, 1.0);

			for(const Part& part : *parts) {
				const char* paintKey = part.stroke ? "stroke" : "fill";
				const char* opacityKey = part.stroke ? "stroke-opacity" : "fill-opacity";
				const std::string* paint = attr(u, paintKey);

				if(paint && *paint == "none") continue;

				Color useColor{0_cv, 0_cv, 0_cv, 1_cv};

				if(paint && urlId(*paint).empty()) parseColor(*paint, useColor);

				Instance in;

				in.m = toEm * u.m;
				in.proto = part.proto;
				in.gradient = part.gradient;

				const slug_t alpha = part.color.a * useColor.a * slug_t(std::clamp(toNumber(attr(u, opacityKey), 1.0), 0.0, 1.0) * opacity);

				if(part.gradient) in.color = {1_cv, 1_cv, 1_cv, alpha};

				else {
					// ThorVG painted the part with the black stand-in unless the symbol fixes its own color.
					const bool own = part.color.r != 0_cv || part.color.g != 0_cv || part.color.b != 0_cv;

					in.color = own ? Color{part.color.r, part.color.g, part.color.b, alpha} : Color{useColor.r, useColor.g, useColor.b, alpha};
				}

				if(in.color.a > 0_cv) sl.instances.push_back(in);
			}
		}

		thorvg::LayerInfo si;

		si.id = info.id;

		layers.push_back(placeholder(set.layers.size()));
		infos.push_back(si);
		set.layers.push_back(std::move(sl));
	}

	comp.layers = std::move(layers);
	cfg.layers = std::move(infos);

	registerProtos(set, atlas, protoPrefix);

	return comp;
}

#endif

}
}
