#pragma once

// ================================================================================================
// bake.hpp - layer merging, planar mesh baking and per-key cost records
//
// Three steps that turn a loaded CompositeShape (e.g. from slughorn/thorvg.hpp) into something a
// realtime consumer can choose between: the Slug atlas itself, or a baked triangle mesh derived
// from the atlas's OWN contours (Slug -> mesh, never re-parsing the source).
//
// mergeLayers() - copies a composite from a staging atlas into a destination atlas, merging runs
//     of same-solid-paint layers into one shape when that cannot change painter's order (the
//     merged layer never moves past a layer it overlaps). Targets content like canvas pixel-noise
//     loops, where thousands of 1-2 px fillRect speckles would otherwise be thousands of layers.
//
// bakeMesh() - tessellates every layer's Atlas::getShapeContours() (fill-rule normalized with
//     Clipper2, tessellate::triangulate) and planarizes painter's order: walking top -> bottom,
//     each layer loses whatever opaque layers above it cover, so the opaque part of the key is
//     ONE non-overlapping planar triangle set; layers with alpha < 1 keep painter's order as an
//     overlay index range drawn after it. Per vertex it emits a paint id and a paint parameter
//     (linear gradient t, or radial gradient-space position) so gradients interpolate exactly
//     without splitting triangles.
//
// cost() - curve and band statistics from the built atlas plus the baked triangle count, and a
//     recommended mode ("mesh" / "slug" / "mean").
//
// Requires SLUGHORN_TESSELLATE=ON and SLUGHORN_CLIPPER2=ON. No I/O: exporters (the Python pack
// script, the slug.elf sandbox guest) serialize the plain structs below.
// ================================================================================================

#include "slughorn.hpp"
#include "clipper.hpp"
#include "render.hpp"
#include "tessellate.hpp"
#include "stamp.hpp"

#ifdef SLUGHORN_HAS_MESHOPT
#include <meshoptimizer.h>
#endif

#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <optional>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace slughorn {
namespace bake {

// Per-layer provenance that bake needs beyond Layer itself (thorvg::LayerInfo carries these).
struct LayerSource {
	FillRule fillRule = FillRule::NonZero;
	bool stroke = false;

	// 0 = pad, 1 = reflect, 2 = repeat (as authored; Slug renders pad).
	uint8_t spread = 0;
};

// ================================================================================================
// mergeLayers
// ================================================================================================
struct MergeResult {
	CompositeShape composite;
	std::vector<LayerSource> layers;

	size_t layersBefore = 0;
	size_t layersAfter = 0;

	// Total curves over all layers before / after merging. Merging concatenates curved members
	// (no change), but a group whose members are all straight-edged (fillRect speckles, polygons)
	// is replaced by its exact Clipper2 union, which drops shared and covered edges.
	size_t curvesBefore = 0;
	size_t curvesAfter = 0;
};

// A merged group stops accepting members once it holds this many curves: one Slug layer whose
// curves are spread over the whole key puts all of them into a few bands, and the shader's
// per-fragment cost is the band's curve count, so unbounded merging trades layer count for band
// density. 512 keeps a merged layer's densest band in the low hundreds even when the members are
// spread over the full canvas (bands are capped at 32 per axis by the 32-slot indirection).
inline constexpr size_t MAX_MERGED_CURVES = 512;

// Band count per axis for re-registered shapes: Atlas's own auto pick caps at 16; the 32-slot
// indirection table addresses up to 32 distinct bands, which halves band density for the big
// merged shapes this module produces.
inline constexpr int MAX_AUTO_BANDS = 32;

// Two solid colors are "the same paint" when every channel matches within this (1/4 of an 8-bit
// step), so values that round-tripped through 8-bit SVG colors compare equal.
inline constexpr slug_t COLOR_EPS = 1_cv / 1024_cv;

namespace detail {

struct Box {
	slug_t x0 = std::numeric_limits<slug_t>::max(), y0 = x0;
	slug_t x1 = std::numeric_limits<slug_t>::lowest(), y1 = x1;

	void add(slug_t x, slug_t y) {
		x0 = std::min(x0, x); y0 = std::min(y0, y);
		x1 = std::max(x1, x); y1 = std::max(y1, y);
	}

	void add(const Box& b) { add(b.x0, b.y0); add(b.x1, b.y1); }

	// Strict overlap: boxes that only touch along an edge do not overlap (abutting speckles).
	bool overlaps(const Box& b) const { return x0 < b.x1 && b.x0 < x1 && y0 < b.y1 && b.y0 < y1; }
};

// Shape curves in canvas em-space (local + placement), with explicit contour starts.
struct Placed {
	Atlas::Curves curves;
	std::vector<size_t> starts;
	Box box;
};

inline Placed place(const Atlas& atlas, const Layer& layer) {
	Placed p;

	const auto shape = atlas.getShape(layer.key);

	if(!shape) return p;

	const slug_t ox = layer.transform.x - shape->originX;
	const slug_t oy = layer.transform.y - shape->originY;

	p.curves = shape->curves;

	for(auto& c : p.curves) {
		c.x1 += ox; c.y1 += oy;
		c.x2 += ox; c.y2 += oy;
		c.x3 += ox; c.y3 += oy;

		p.box.add(c.x1, c.y1);
		p.box.add(c.x2, c.y2);
		p.box.add(c.x3, c.y3);
	}

	if(!shape->contourStarts.empty()) p.starts = shape->contourStarts;

	else {
		size_t at = 0;

		for(const auto& contour : clipper::splitContours(p.curves)) {
			p.starts.push_back(at);
			at += contour.size();
		}
	}

	return p;
}

// Sign of the enclosed area (control-polygon shoelace; only the sign is used).
inline slug_t signedArea(const Atlas::Curves& curves) {
	slug_t a = 0_cv;

	for(const auto& c : curves) {
		a += c.x1 * c.y2 - c.x2 * c.y1;
		a += c.x2 * c.y3 - c.x3 * c.y2;
	}

	return a;
}

// True when every curve is a straight segment (CurveDecomposer::lineTo duplicates the end point
// into the control point; a control point on either end point is equally straight).
inline bool isPolygonal(const Atlas::Curves& curves) {
	for(const auto& c : curves) {
		const bool atEnd = c.x2 == c.x3 && c.y2 == c.y3;
		const bool atStart = c.x2 == c.x1 && c.y2 == c.y1;

		if(!atEnd && !atStart) return false;
	}

	return true;
}

inline bool sameColor(const Color& a, const Color& b) {
	return
		std::abs(a.r - b.r) <= COLOR_EPS && std::abs(a.g - b.g) <= COLOR_EPS &&
		std::abs(a.b - b.b) <= COLOR_EPS && std::abs(a.a - b.a) <= COLOR_EPS
	;
}

}

// Copies @p in (whose shapes live in @p src, built or not) into @p dst with fresh keys from
// @p keys, merging layers when @p merge is set. A layer L joins an earlier group G when:
//
// - both are solid (gradientId == 0), DrawMode::Visible, same color (COLOR_EPS), same fill rule,
//   same blend mode, no effect id;
// - L does not overlap (bounding box) any group drawn between G and L - so moving L down to G's
//   slot cannot change what is visible;
// - for even-odd groups, L does not overlap G's own members (even-odd of a concatenation is the
//   union only for disjoint pieces). Nonzero members are orientation-normalized (positive area)
//   before concatenation, so overlapping nonzero members union correctly.
//
// Shapes are re-registered with autoMetrics and the default origin; gradients are copied.
inline MergeResult mergeLayers(
	const Atlas& src,
	const CompositeShape& in,
	const std::vector<LayerSource>& meta,
	Atlas& dst,
	KeyIterator& keys,
	bool merge=true,
	size_t maxMergedCurves=MAX_MERGED_CURVES
) {
	using namespace detail;

	struct Group {
		Layer layer;
		LayerSource source;
		bool mergeable = false;
		Box box;
		std::vector<Box> memberBoxes;
		Atlas::Curves curves;
		std::vector<size_t> starts;
		bool polygonal = true;
		bool stamp = false;
	};

	MergeResult result;
	std::vector<Group> groups;

	result.layersBefore = in.layers.size();

	for(size_t li = 0; li < in.layers.size(); li++) {
		const Layer& layer = in.layers[li];
		const LayerSource source = li < meta.size() ? meta[li] : LayerSource{};

		// Stamp layers (stamp.hpp placeholders) pass through untouched and block merging across
		// them (their extent is not known here).
		if(stamp::isStampLayer(layer)) {
			Group g;

			g.layer = layer;
			g.source = source;
			g.stamp = true;
			g.box = Box{-1e30_cv, -1e30_cv, 1e30_cv, 1e30_cv};
			g.memberBoxes.push_back(g.box);

			groups.push_back(std::move(g));

			continue;
		}

		Placed p = place(src, layer);

		if(p.curves.empty()) continue;

		result.curvesBefore += p.curves.size();

		const bool mergeable =
			merge &&
			layer.gradientId == 0 &&
			layer.drawMode == DrawMode::Visible &&
			layer.blendMode == BlendMode::SrcOver &&
			layer.effectId == 0 &&
			layer.scale == 1_cv
		;

		// Orientation-normalize so overlapping nonzero members add instead of cancel.
		if(mergeable && signedArea(p.curves) < 0_cv) CurveDecomposer::reverseCurves(p.curves, 0, p.curves.size());

		Group* target = nullptr;

		if(mergeable) {
			for(size_t gi = groups.size(); gi-- > 0;) {
				Group& g = groups[gi];

				if(
					g.mergeable &&
					g.source.fillRule == source.fillRule &&
					g.layer.blendMode == layer.blendMode &&
					sameColor(g.layer.color, layer.color)
				) {
					bool ok = true;

					if(source.fillRule == FillRule::EvenOdd && g.box.overlaps(p.box)) {
						for(const auto& mb : g.memberBoxes) if(mb.overlaps(p.box)) { ok = false; break; }
					}

					if(g.curves.size() + p.curves.size() > maxMergedCurves) ok = false;

					if(ok) {
						target = &g;

						break;
					}
				}

				// Can't move L below a group it might overlap.
				bool blocked = false;

				if(g.box.overlaps(p.box)) {
					for(const auto& mb : g.memberBoxes) if(mb.overlaps(p.box)) { blocked = true; break; }
				}

				if(blocked) break;
			}
		}

		if(target) {
			const size_t base = target->curves.size();

			for(size_t s : p.starts) target->starts.push_back(base + s);

			target->curves.insert(target->curves.end(), p.curves.begin(), p.curves.end());
			target->polygonal = target->polygonal && isPolygonal(p.curves);
			target->box.add(p.box);
			target->memberBoxes.push_back(p.box);
			target->source.stroke = target->source.stroke || source.stroke;

			continue;
		}

		Group g;

		g.layer = layer;
		g.source = source;
		g.mergeable = mergeable;
		g.box = p.box;
		g.memberBoxes.push_back(p.box);
		g.polygonal = isPolygonal(p.curves);
		g.curves = std::move(p.curves);
		g.starts = std::move(p.starts);

		groups.push_back(std::move(g));
	}

	const auto& gradients = src.getGradients();

	for(auto& g : groups) {
		if(g.stamp) {
			result.composite.layers.push_back(g.layer);
			result.layers.push_back(g.source);

			continue;
		}

		// Straight-edged multi-member groups: replace the concatenation by the exact union (no
		// flattening involved - every edge is already a line), when that is smaller.
		if(g.mergeable && g.polygonal && g.memberBoxes.size() > 1) {
			const auto region = clipper::normalize(
				clipper::toPaths(clipper::splitContours(g.curves, g.starts), 1e-6_cv),
				g.source.fillRule
			);

			std::vector<size_t> starts;
			Atlas::Curves curves = clipper::toCurves(region, &starts);

			if(!curves.empty() && curves.size() < g.curves.size()) {
				g.curves = std::move(curves);
				g.starts = std::move(starts);
				g.source.fillRule = FillRule::NonZero; // normalized
			}
		}

		result.curvesAfter += g.curves.size();

		// Re-localize to the group's bbox corner (default origin, autoMetrics).
		const slug_t ox = g.box.x0, oy = g.box.y0;

		for(auto& c : g.curves) {
			c.x1 -= ox; c.y1 -= oy;
			c.x2 -= ox; c.y2 -= oy;
			c.x3 -= ox; c.y3 -= oy;
		}

		// Gradients are expressed in the layer's local em-space; non-mergeable layers keep their
		// original local frame, so shift the gradient by however far the frame moved.
		Layer out = g.layer;

		const auto srcShape = src.getShape(g.layer.key);
		const slug_t shiftX = srcShape ? (g.layer.transform.x - srcShape->originX) - ox : 0_cv;
		const slug_t shiftY = srcShape ? (g.layer.transform.y - srcShape->originY) - oy : 0_cv;

		Atlas::ShapeInfo info;

		const int bands = static_cast<int>(std::clamp<size_t>(g.curves.size() / 2, 1, MAX_AUTO_BANDS));

		info.curves = std::move(g.curves);
		info.contourStarts = std::move(g.starts);
		info.autoMetrics = true;
		info.numBandsX = bands;
		info.numBandsY = bands;

		out.key = keys.next();
		out.transform = Transform{ ox, oy, g.layer.transform.z };

		if(g.layer.gradientId > 0 && g.layer.gradientId <= gradients.size()) {
			GradientInfo gi = gradients[g.layer.gradientId - 1];

			// local_new = local_old - shift  =>  f(local_old) = f(local_new + shift).
			Matrix& m = gi.transform;

			if(gi.type == GradientInfo::Type::Linear) {
				m.dx += m.xx * shiftX + m.xy * shiftY;
			}

			else {
				// Radial / AffineRadial / Sweep keep their center in dx/dy.
				m.dx -= shiftX;
				m.dy -= shiftY;
			}

			out.gradientId = dst.addGradient(gi);
		}

		dst.addShape(out.key, info);

		result.composite.layers.push_back(out);
		result.layers.push_back(g.source);
	}

	result.composite.advance = in.advance;
	result.layersAfter = result.composite.layers.size();

	return result;
}

// ================================================================================================
// bakeMesh
// ================================================================================================
struct Paint {
	enum class Type : uint8_t { Solid = 0, Linear = 1, Radial = 2 };

	Type type = Type::Solid;

	// Solid: the color. Gradients: unused (stops carry color; layer opacity already folded in).
	Color color = {};

	std::vector<GradientStop> stops = {};

	uint8_t spread = 0; // 0 pad, 1 reflect, 2 repeat

	// Radial: t = length(param) - innerRadius.
	slug_t innerRadius = 0_cv;

	bool opaque = false;
};

// How bakeFinal() treats the final composite's alpha (see there).
enum class AlphaMode : uint8_t { Opaque = 0, Transparent = 1, AlphaTest = 2 };

struct BakeConfig {
	// Authoring size of the key in pixels (thorvg::LoadConfig width/height). Em-space is
	// normalized by width (em = px / width), so width is also pixels-per-em.
	slug_t width = 1_cv;
	slug_t height = 1_cv;

	// Chord tolerance for flattening the atlas contours, in authoring/texture pixels.
	slug_t tolerancePx = 0.25_cv;

	// A paint counts as opaque (occludes what is below it) when every alpha >= this.
	slug_t opaqueAlpha = 1_cv - 0.5_cv / 255_cv;

	// Planarize painter's order (subtract higher opaque layers from lower ones).
	bool planarize = true;

	// Output v axis: false = v down (v = y / height, SVG / image orientation), true = v up
	// (v = 1 - y / height, the three.js / OpenGL texture convention).
	bool vUp = false;

	// > 0: alpha-tested (cutout) bake - see bakeCutout(). 0: the planar base + overlay bake.
	slug_t alphaTest = 0_cv;

	// bakeFinal(): the material's alpha handling, its opacity (multiplies the final alpha) and
	// the cell size (px) for faces under two or more gradients (0: CUTOUT_CELL_PX).
	AlphaMode alphaMode = AlphaMode::AlphaTest;
	slug_t opacity = 1_cv;
	slug_t cellPx = 0_cv;

	// bakeFinal(): bake only this rectangle of the canvas (authoring px, x0 < x1 and y0 < y1);
	// empty = the whole canvas.
	slug_t windowX0 = 0_cv, windowY0 = 0_cv, windowX1 = 0_cv, windowY1 = 0_cv;

	// bakeFinal() LOD: features (connected components of a layer, stamp instances) whose size
	// 2 * area / perimeter is below this many px are not geometry; they are folded into the mean
	// colour of the face they sit on. 0 keeps everything.
	slug_t featurePx = 0_cv;

	// bakeFinal(): stop (BakedMesh::aborted) once the mesh passes this many triangles; 0 = no cap.
	size_t maxTriangles = 0;

	// bakeFinal() LOD: simplify every region's outline (Clipper2 SimplifyPaths, px) before the
	// arrangement, so faces still share their boundaries exactly. Curves reach the bake already
	// flattened (the loader's clip pass), so this - not tolerancePx - is what coarsens polylines.
	slug_t simplifyPx = 0_cv;
};

struct BakedMesh {
	// Per vertex. Positions are UV in [0, 1] over the key's authoring rectangle: u = x / width,
	// v = y / height (v = 0 at the TOP edge, SVG Y-down) or, with BakeConfig::vUp,
	// v = 1 - y / height (v = 0 at the bottom). Every triangle is wound counter-clockwise in the
	// emitted (u, v) plane (positive signed area with u right, v up-the-axis).
	std::vector<float> positions;   // 2 per vertex (u, v)
	std::vector<uint16_t> paintIds; // 1 per vertex, index into paints
	std::vector<float> params;      // 2 per vertex: linear (t, 0); radial (gx, gy); solid (0, 0)

	// Triangle list: [0, opaqueIndexCount) is the planar opaque set (order irrelevant, no
	// overlaps), [opaqueIndexCount, opaqueIndexCount + overlayIndexCount) is the overlay set,
	// which must be drawn afterwards in index order with alpha blending.
	std::vector<uint32_t> indices;
	uint32_t opaqueIndexCount = 0;
	uint32_t overlayIndexCount = 0;

	std::vector<Paint> paints;

	// Triangle counts: every layer tessellated on its own (stacked, no planarization) vs the
	// planarized output.
	size_t trianglesBefore = 0;
	size_t trianglesAfter = 0;

	// Layers whose geometry is a stroke expansion (strokes reach bake already converted to fill
	// contours by the loader; nothing un-expanded can reach the tessellator - the atlas only
	// stores fill contours).
	size_t strokeLayers = 0;

	slug_t tolerancePx = 0_cv;

	// Cutout bakes: the alpha test used (0 for the planar bake) and how many arrangement faces had
	// to be approximated at their centroid (stacks with two or more gradients).
	slug_t alphaTest = 0_cv;
	size_t cutoutApproxFaces = 0;

	// bakeFinal(): the mode it baked, the area (authoring px^2) of the window and, in Opaque mode,
	// of the faces whose final alpha is below 1 - transparent at the end on an opaque material,
	// shown with their straight rgb (black where nothing was drawn).
	AlphaMode alphaMode = AlphaMode::AlphaTest;
	double canvasArea = 0.0;
	double transparentArea = 0.0;

	// bakeFinal() LOD: the feature size it kept (px), how many features (and how much area, px^2)
	// it folded into face means, and whether it stopped at BakeConfig::maxTriangles.
	slug_t featurePx = 0_cv;
	size_t foldedFeatures = 0;
	double foldedArea = 0.0;
	bool aborted = false;
};

namespace detail {

// earcut eliminates holes by bridging them into the outer ring one at a time, which is quadratic
// in the hole count - a background minus thousands of planarized speckles stalls it. Large
// regions are therefore cut into a grid of tiles (Clipper2 RectClip keeps holes as holes) and each
// tile triangulated on its own. Costs a few extra triangles along tile seams.
inline constexpr size_t TILE_VERTICES = 384;

}

// Planarization tile budget: the canvas is cut into a G x G grid with about this many region
// vertices per tile (G <= 32).
inline constexpr size_t PLANAR_TILE_VERTICES = 2048;

namespace detail {

inline tessellate::Mesh2D triangulateTiled(const clipper::Paths& region) {
	const size_t nv = clipper::vertexCount(region);

	if(nv <= TILE_VERTICES * 2) return tessellate::triangulate(region);

	double x0 = std::numeric_limits<double>::max(), y0 = x0;
	double x1 = std::numeric_limits<double>::lowest(), y1 = x1;

	for(const auto& p : region) for(const auto& pt : p) {
		x0 = std::min(x0, pt.x); y0 = std::min(y0, pt.y);
		x1 = std::max(x1, pt.x); y1 = std::max(y1, pt.y);
	}

	const auto n = static_cast<size_t>(std::ceil(std::sqrt(double(nv) / double(TILE_VERTICES))));
	const double tw = (x1 - x0) / double(n), th = (y1 - y0) / double(n);
	const int precision = clipper::precisionFor(region);

	tessellate::Mesh2D out;

	for(size_t j = 0; j < n; j++) {
		for(size_t i = 0; i < n; i++) {
			const Clipper2Lib::RectD rect(
				x0 + tw * double(i), y0 + th * double(j),
				i + 1 == n ? x1 : x0 + tw * double(i + 1),
				j + 1 == n ? y1 : y0 + th * double(j + 1)
			);

			const clipper::Paths tile = Clipper2Lib::RectClip(rect, region, precision);

			if(tile.empty()) continue;

			const tessellate::Mesh2D m = tessellate::triangulate(tile);
			const auto base = static_cast<uint32_t>(out.positions.size() / 2);

			out.positions.insert(out.positions.end(), m.positions.begin(), m.positions.end());

			for(uint32_t idx : m.indices) out.indices.push_back(base + idx);
		}
	}

	return out;
}

inline Paint paintOf(const Atlas& atlas, const Layer& layer, const LayerSource& src, slug_t opaqueAlpha) {
	Paint p;

	p.spread = src.spread;

	const auto& gradients = atlas.getGradients();

	if(layer.gradientId > 0 && layer.gradientId <= gradients.size()) {
		const GradientInfo& g = gradients[layer.gradientId - 1];

		p.type = g.type == GradientInfo::Type::Linear ? Paint::Type::Linear : Paint::Type::Radial;
		p.stops = g.stops;
		p.innerRadius = g.innerRadius;
		p.opaque = layer.color.a >= opaqueAlpha;

		for(auto& s : p.stops) {
			s.color.a *= layer.color.a;

			if(s.color.a < opaqueAlpha) p.opaque = false;
		}
	}

	else {
		p.color = layer.color;
		p.opaque = layer.color.a >= opaqueAlpha;
	}

	return p;
}

}

// ================================================================================================
// bakeFinal - the FINAL composite as one opaque-or-alpha planar bake (no overlay)
//
// Every alpha source is folded into the layer stack and only the final composite is judged, the
// way three.js shades a textured material: rgb = the colour key's composite (straight, as WebGL
// un-premultiplies a canvas texture: 0 where its alpha is 0; white when there is no colour key),
// alpha = colour alpha * opacity * (alpha map's straight GREEN channel, when there is an alpha-map
// key - diffuseColor.a *= texture(alphaMap, uv).g). Per AlphaMode:
//
//   Opaque       every face kept, opaque, with the straight rgb (three.js ignores alpha on an
//                opaque material); faces whose final alpha < 1 are measured in
//                BakedMesh::transparentArea ("transparent at the end on an opaque material").
//   Transparent  faces with final alpha > 0 kept, with that alpha in their paint (base-colour
//                alpha): exact per vertex under one gradient, per cell under two or more.
//   AlphaTest    faces with final alpha >= alphaTest kept, opaque, with the straight rgb (the
//                cutout: what alphaTest discards is gone).
//
// The colour key's layers (stamp instances included) and the alpha-map key's layers (scaled onto
// the colour key's UV square) are cut into one planar arrangement: every face of it is covered by
// a fixed stack. Each face's composite is known in closed form:
//
// - stacks of solid paints: constant;
// - exactly one gradient (linear / radial / elliptical; in either key): the final alpha is
//   piecewise linear in the gradient parameter t between stops, so alpha-test iso-lines are lines
//   of constant t - straight lines (half-planes, exact) for a linear gradient, ellipses (an annulus
//   polygon at the bake tolerance) for a radial one; the kept part gets a gradient paint whose
//   stops are the composite at every original stop and crossing (exact at those t, linear in
//   between);
// - two or more gradients (or a sweep): no closed form; cut into BakeConfig::cellPx cells, each
//   kept / dropped and painted with the composite at its centre (BakedMesh::cutoutApproxFaces).
// ================================================================================================

// An alpha-map key: its composite's straight green multiplies the final alpha.
struct AlphaSource {
	const Atlas* atlas = nullptr;
	const CompositeShape* composite = nullptr;
	const std::vector<LayerSource>* meta = nullptr;
	const stamp::Set* stamps = nullptr;

	// Its authoring size (pixels); it is stretched onto the colour key's UV square.
	slug_t width = 1_cv;
	slug_t height = 1_cv;
};

namespace detail {

// How one region paints, in (colour key) authoring pixels.
struct CutPaint {
	const GradientInfo* g = nullptr; // nullptr = solid
	Color color = {};                // solid straight color, or the gradient tint
	Matrix toLocal = {};             // px -> the space GradientInfo lives in
};

inline Color paintColorAt(const CutPaint& p, double px, double py) {
	if(!p.g) return p.color;

	slug_t lx, ly;

	p.toLocal.apply(slug_t(px), slug_t(py), lx, ly);

	const Color c = render::gradientColor(*p.g, render::gradientT(*p.g, lx, ly));

	return {c.r * p.color.r, c.g * p.color.g, c.b * p.color.b, c.a * p.color.a};
}

inline Color paintColorAtT(const CutPaint& p, double t) {
	const Color c = render::gradientColor(*p.g, slug_t(t));

	return {c.r * p.color.r, c.g * p.color.g, c.b * p.color.b, c.a * p.color.a};
}

struct Premul {
	double r = 0, g = 0, b = 0, a = 0;

	void over(const Color& c) { // this = c over this
		const double ca = c.a;

		r = c.r * ca + r * (1 - ca);
		g = c.g * ca + g * (1 - ca);
		b = c.b * ca + b * (1 - ca);
		a = ca + a * (1 - ca);
	}
};

// Convex polygon (px) of points with lo <= alpha*x + beta*y + gamma <= hi, inside @p box.
inline clipper::Path slab(double alpha, double beta, double gamma, double lo, double hi, double x0, double y0, double x1, double y1) {
	clipper::Path poly{{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};

	auto clip = [&](double sgn, double bound) { // keep sgn * (f - bound) >= 0
		clipper::Path out;

		for(size_t i = 0; i < poly.size(); i++) {
			const auto& p = poly[i];
			const auto& q = poly[(i + 1) % poly.size()];
			const double fp = sgn * (alpha * p.x + beta * p.y + gamma - bound);
			const double fq = sgn * (alpha * q.x + beta * q.y + gamma - bound);

			if(fp >= 0) out.push_back(p);
			if((fp >= 0) != (fq >= 0)) {
				const double u = fp / (fp - fq);

				out.push_back({p.x + (q.x - p.x) * u, p.y + (q.y - p.y) * u});
			}
		}

		poly = std::move(out);
	};

	if(std::isfinite(lo)) clip(1.0, lo);
	if(std::isfinite(hi) && !poly.empty()) clip(-1.0, hi);

	return poly;
}

// The final colour of a face: straight rgb of the colour stack, final alpha.
struct Final {
	double r = 0, g = 0, b = 0, a = 0;
};

}

// Cell size (authoring px) for faces under two or more gradients (no closed-form iso-line).
inline constexpr double CUTOUT_CELL_PX = 1.0;

// Folded features take local means over cells no finer than canvas / FOLD_GRID.
inline constexpr double FOLD_GRID = 32.0;

// bakeFinal() gives up before tiling when the outlines carry more than this many vertices per
// triangle of BakeConfig::maxTriangles.
inline constexpr size_t HOPELESS_VERTICES_PER_TRIANGLE = 16;

// A final alpha below this is "not opaque" (Opaque mode's transparentArea).
inline constexpr double FINAL_OPAQUE_ALPHA = 1.0 - 0.5 / 255.0;

// A composite ramp is refined until linear interpolation between its stops stays this close to
// the composite (per channel), at most 2^RAMP_MAX_DEPTH stops an interval.
inline constexpr double RAMP_TOLERANCE = 1.0 / 255.0;
inline constexpr int RAMP_MAX_DEPTH = 6;

inline BakedMesh bakeFinal(
	const Atlas& atlas,
	const CompositeShape* composite,       // nullptr: no colour key (white)
	const std::vector<LayerSource>& meta,
	const BakeConfig& cfg,
	const stamp::Set* stamps=nullptr,
	const AlphaSource* alphaMap=nullptr
) {
	using detail::CutPaint;
	using detail::Premul;
	using detail::Final;

	BakedMesh mesh;

	mesh.tolerancePx = cfg.tolerancePx;
	mesh.alphaTest = cfg.alphaMode == AlphaMode::AlphaTest ? cfg.alphaTest : 0_cv;
	mesh.alphaMode = cfg.alphaMode;

	const double W = cfg.width, H = cfg.height;

	// The bake window (px): the whole canvas unless BakeConfig::window is set.
	const bool windowed = cfg.windowX1 > cfg.windowX0 && cfg.windowY1 > cfg.windowY0;
	const double wx0 = windowed ? std::max(0.0, double(cfg.windowX0)) : 0.0, wy0 = windowed ? std::max(0.0, double(cfg.windowY0)) : 0.0;
	const double wx1 = windowed ? std::min(W, double(cfg.windowX1)) : W, wy1 = windowed ? std::min(H, double(cfg.windowY1)) : H;

	if(!(wx1 > wx0 && wy1 > wy0)) return mesh;

	mesh.canvasArea = (wx1 - wx0) * (wy1 - wy0);

	struct Region {
		clipper::Paths paths;
		CutPaint paint;
		bool alpha = false; // an alpha-map region
	};

	std::vector<Region> regions;

	auto inWindow = [&](const clipper::Paths& paths) {
		for(const auto& p : paths) for(const auto& pt : p) if(pt.x >= wx0 && pt.x <= wx1 && pt.y >= wy0 && pt.y <= wy1) return true;

		// No vertex inside: still overlapping when the boxes overlap (a big shape around the window).
		double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;

		for(const auto& p : paths) for(const auto& pt : p) { x0 = std::min(x0, pt.x); y0 = std::min(y0, pt.y); x1 = std::max(x1, pt.x); y1 = std::max(y1, pt.y); }

		return x0 <= wx1 && x1 >= wx0 && y0 <= wy1 && y1 >= wy0;
	};

	// LOD outline simplification (re-normalized: simplifying can make a ring cross itself).
	auto simplify = [&](clipper::Paths& paths) {
		if(cfg.simplifyPx <= 0_cv || paths.empty()) return;

		paths = clipper::normalize(Clipper2Lib::SimplifyPaths<double>(paths, double(cfg.simplifyPx), true), FillRule::NonZero);
	};

	// One composite's regions. em -> px is (x * sx, y * sy).
	auto addComposite = [&](const Atlas& at, const CompositeShape& comp, const std::vector<LayerSource>& lmeta, const stamp::Set* st,
			double sx, double sy, bool alpha) {
		const auto& gradients = at.getGradients();

		for(size_t li = 0; li < comp.layers.size(); li++) {
			const Layer& layer = comp.layers[li];

			if(layer.drawMode != DrawMode::Visible) continue;

			if(stamp::isStampLayer(layer)) {
				if(!st || stamp::stampIndex(layer) >= st->layers.size()) continue;

				for(const stamp::Instance& in : st->layers[stamp::stampIndex(layer)].instances) {
					if(in.color.a <= 0_cv) continue;

					// Cheap reject before flattening: the instance's box against the window.
					const stamp::Box ib = stamp::bounds(in, *st);

					if(double(ib.x1) * sx < wx0 || double(ib.x0) * sx > wx1 || double(ib.y1) * sy < wy0 || double(ib.y0) * sy > wy1) continue;

					auto paths = clipper::normalize(
						clipper::toPaths(stamp::instanceContours(*st, in), cfg.tolerancePx, Matrix{.xx = slug_t(sx), .yy = slug_t(sy)}),
						FillRule::NonZero
					);

					if(paths.empty()) continue;

					CutPaint p;

					p.color = in.color;

					if(in.gradient && in.gradient <= st->gradients.size()) {
						const stamp::Inverse inv = stamp::invert(in.m);

						p.g = &st->gradients[in.gradient - 1];
						p.toLocal = Matrix{.xx = slug_t(inv.a / sx), .yx = slug_t(inv.c / sx), .xy = slug_t(inv.b / sy), .yy = slug_t(inv.d / sy), .dx = inv.e, .dy = inv.f};
					}

					regions.push_back({std::move(paths), p, alpha});
				}

				continue;
			}

			const auto shape = at.getShape(layer.key);

			if(!shape) continue;

			const LayerSource src = li < lmeta.size() ? lmeta[li] : LayerSource{};

			if(src.stroke && !alpha) mesh.strokeLayers++;

			const double s = layer.scale;
			const double ox = (double(layer.transform.x) - shape->originX) * s;
			const double oy = (double(layer.transform.y) - shape->originY) * s;

			auto paths = clipper::normalize(
				clipper::toPaths(at.getShapeContours(layer.key), cfg.tolerancePx,
					Matrix{.xx = slug_t(s * sx), .yy = slug_t(s * sy), .dx = slug_t(ox * sx), .dy = slug_t(oy * sy)}),
				src.fillRule
			);

			if(paths.empty() || (windowed && !inWindow(paths))) continue;

			CutPaint p;

			p.color = layer.color;

			if(layer.gradientId > 0 && layer.gradientId <= gradients.size()) {
				p.g = &gradients[layer.gradientId - 1];
				// local = (px / scale - o) / s
				p.toLocal = Matrix{.xx = slug_t(1.0 / (sx * s)), .yy = slug_t(1.0 / (sy * s)), .dx = slug_t(-ox / s), .dy = slug_t(-oy / s)};
			}

			regions.push_back({std::move(paths), p, alpha});
		}
	};

	// The canvas itself: white under no colour key; under a colour key in Opaque mode a clear
	// background, so faces nothing covers still exist (they come out black, as WebGL un-premultiplies).
	const clipper::Paths canvasRect{{{wx0, wy0}, {wx1, wy0}, {wx1, wy1}, {wx0, wy1}}};
	size_t firstLayerRegion = 0;

	if(!composite) {
		CutPaint white;

		white.color = {1_cv, 1_cv, 1_cv, 1_cv};
		regions.push_back({canvasRect, white, false});
		firstLayerRegion = regions.size();
	}

	else {
		if(cfg.alphaMode == AlphaMode::Opaque) {
			CutPaint clear;

			clear.color = {0_cv, 0_cv, 0_cv, 0_cv};
			regions.push_back({canvasRect, clear, false});
		}

		firstLayerRegion = regions.size();
		addComposite(atlas, *composite, meta, stamps, W, W, false);
	}

	const bool haveAlpha = alphaMap && alphaMap->atlas && alphaMap->composite && alphaMap->width > 0_cv;

	if(haveAlpha) {
		// Alpha key em -> its px (x Wa) -> colour px (x W / Wa, y H / Ha).
		const double wa = alphaMap->width, ha = alphaMap->height;
		static const std::vector<LayerSource> noMeta;

		addComposite(*alphaMap->atlas, *alphaMap->composite, alphaMap->meta ? *alphaMap->meta : noMeta, alphaMap->stamps, W, wa * H / ha, true);
	}

	for(const auto& r : regions) mesh.trianglesBefore += clipper::vertexCount(r.paths);

	// LOD: every connected component (a layer's piece with its holes, a stamp instance) smaller
	// than featurePx is folded - recorded at its centroid with its area and colour, and dropped from
	// the geometry. Each face later takes the area-weighted mean of what was folded inside it,
	// composited at the fold's own place in paint order (so a fold hidden under an opaque layer
	// above it changes nothing).
	struct Fold {
		double x, y, area;
		uint32_t region;
		Color color;
		bool used = false;
	};

	std::vector<Fold> folds;

	mesh.featurePx = cfg.featurePx;

	if(cfg.featurePx > 0_cv) {
		const double fpx = cfg.featurePx;

		for(size_t ri = firstLayerRegion; ri < regions.size(); ri++) {
			Region& r = regions[ri];

			if(r.paths.empty()) continue;

			clipper::Paths keep;

			for(const clipper::Paths& comp : clipper::components(r.paths)) {
				const double A = std::abs(clipper::area(comp));
				const double P = clipper::perimeter(comp.front());

				if(P > 0 && 2.0 * A / P >= fpx) { keep.insert(keep.end(), comp.begin(), comp.end()); continue; }

				double cx = 0, cy = 0;

				clipper::centroid(comp.front(), cx, cy);

				folds.push_back({cx, cy, A, uint32_t(ri), detail::paintColorAt(r.paint, cx, cy)});
				mesh.foldedFeatures++;
				mesh.foldedArea += A;
			}

			r.paths = std::move(keep);
		}
	}

	// LOD outline simplification, after folding (a feature simplified away would vanish without
	// leaving its colour in the mean).
	if(cfg.simplifyPx > 0_cv) for(size_t ri = firstLayerRegion; ri < regions.size(); ri++) simplify(regions[ri].paths);

	// Tiles over the window, as in bakeMesh().
	auto boxOf = [](const clipper::Paths& paths) {
		detail::Box b;

		for(const auto& p : paths) for(const auto& pt : p) b.add(static_cast<slug_t>(pt.x), static_cast<slug_t>(pt.y));

		return b;
	};

	size_t totalVerts = 0;

	for(const auto& r : regions) totalVerts += clipper::vertexCount(r.paths);

	// Hopeless against the budget before any arrangement work: every outline vertex left after
	// folding and simplification costs about a triangle, and planarization rarely hides more than
	// most of them.
	if(cfg.maxTriangles > 0 && totalVerts > cfg.maxTriangles * HOPELESS_VERTICES_PER_TRIANGLE) {
		mesh.aborted = true;

		return mesh;
	}

	const auto G = std::clamp<size_t>(static_cast<size_t>(std::ceil(std::sqrt(double(totalVerts) / double(PLANAR_TILE_VERTICES)))), 1, 32);
	const double tw = (wx1 - wx0) / double(G), th = (wy1 - wy0) / double(G);

	struct Face {
		clipper::Paths paths;
		detail::Box box;
		std::vector<uint32_t> stack; // region indices, bottom -> top
	};

	auto overlap = [](const detail::Box& a, const detail::Box& b) {
		return a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1;
	};

	std::vector<detail::Box> rbox(regions.size());
	std::vector<int> rprec(regions.size());

	for(size_t ri = 0; ri < regions.size(); ri++) {
		rbox[ri] = boxOf(regions[ri].paths);
		rprec[ri] = clipper::precisionFor(regions[ri].paths);
	}

	std::vector<std::vector<uint32_t>> tileFolds(G * G);

	for(uint32_t k = 0; k < folds.size(); k++) {
		const auto i = size_t(std::clamp(int64_t(std::floor((folds[k].x - wx0) / tw)), int64_t(0), int64_t(G) - 1));
		const auto j = size_t(std::clamp(int64_t(std::floor((folds[k].y - wy0) / th)), int64_t(0), int64_t(G) - 1));

		tileFolds[j * G + i].push_back(k);
	}

	// One tile's arrangement: every region overlapping it, clipped to it, inserted in paint order
	// (colour regions, then alpha-map regions; the two stacks composite separately).
	auto buildTile = [&](size_t ti) {
		const size_t i = ti % G, j = ti / G;
		const double x0 = wx0 + tw * double(i), y0 = wy0 + th * double(j);
		const double x1 = i + 1 == G ? wx1 : wx0 + tw * double(i + 1), y1 = j + 1 == G ? wy1 : wy0 + th * double(j + 1);
		const Clipper2Lib::RectD rect(x0, y0, x1, y1);
		std::vector<Face> faces;

		for(uint32_t ri = 0; ri < regions.size(); ri++) {
			const detail::Box& rb = rbox[ri];

			if(double(rb.x1) < x0 || double(rb.x0) > x1 || double(rb.y1) < y0 || double(rb.y0) > y1) continue;

			const clipper::Paths piece = Clipper2Lib::RectClip(rect, regions[ri].paths, rprec[ri]);

			if(piece.empty()) continue;

			const detail::Box pb = boxOf(piece);
			std::vector<Face> next;
			clipper::Paths covered;

			next.reserve(faces.size() + 2);

			for(Face& f : faces) {
				if(!overlap(f.box, pb)) { next.push_back(std::move(f)); continue; }

				clipper::Paths inter = clipper::intersect(f.paths, piece);

				if(inter.empty()) { next.push_back(std::move(f)); continue; }

				covered.insert(covered.end(), f.paths.begin(), f.paths.end());

				clipper::Paths rest = clipper::difference(f.paths, piece);

				if(!rest.empty()) next.push_back({rest, boxOf(rest), f.stack});

				std::vector<uint32_t> stack = f.stack;

				stack.push_back(ri);
				next.push_back({inter, boxOf(inter), std::move(stack)});
			}

			clipper::Paths uncovered = covered.empty() ? piece : clipper::difference(piece, covered);

			if(!uncovered.empty()) next.push_back({uncovered, boxOf(uncovered), {ri}});

			faces = std::move(next);
		}

		return faces;
	};

	const AlphaMode mode = cfg.alphaMode;
	const double opacity = std::clamp(double(cfg.opacity), 0.0, 1.0);
	const double test = mode == AlphaMode::AlphaTest ? double(cfg.alphaTest) : 1e-6;
	const double cellPx = cfg.cellPx > 0_cv ? double(cfg.cellPx) : CUTOUT_CELL_PX;

	// The face's final colour at a point (stack colours given).
	auto finish = [&](const Premul& c, const Premul& a) {
		Final f;

		if(c.a > 0) { f.r = c.r / c.a; f.g = c.g / c.a; f.b = c.b / c.a; }

		const double factor = haveAlpha ? (a.a > 0 ? std::clamp(a.g / a.a, 0.0, 1.0) : 0.0) : 1.0;

		f.a = c.a * opacity * factor;

		return f;
	};

	auto finalOf = [&](const std::vector<uint32_t>& stack, const std::vector<Color>& colors) {
		Premul c, a;

		for(size_t k = 0; k < stack.size(); k++) {
			if(regions[stack[k]].alpha) a.over(colors[k]);
			else c.over(colors[k]);
		}

		return finish(c, a);
	};

	// The stack with one folded feature inserted at its own paint position.
	auto finalWith = [&](const std::vector<uint32_t>& stack, const std::vector<Color>& colors, uint32_t extra, const Color& extraColor) {
		Premul c, a;
		bool done = false;

		for(size_t k = 0; k <= stack.size(); k++) {
			if(!done && (k == stack.size() || stack[k] > extra)) {
				(regions[extra].alpha ? a : c).over(extraColor);
				done = true;
			}

			if(k < stack.size()) (regions[stack[k]].alpha ? a : c).over(colors[k]);
		}

		return finish(c, a);
	};

	// Area-weighted mean of the face (fractions fr) and the face with each fold: straight colour in
	// Opaque mode (what it shows), premultiplied otherwise.
	auto mixFolds = [&](const Final& base, const std::vector<Final>& withs, const std::vector<double>& fr) {
		if(withs.empty()) return base;

		Final out = base;

		if(mode == AlphaMode::Opaque) {
			for(size_t i = 0; i < withs.size(); i++) {
				out.r += fr[i] * (withs[i].r - base.r);
				out.g += fr[i] * (withs[i].g - base.g);
				out.b += fr[i] * (withs[i].b - base.b);
				out.a += fr[i] * (withs[i].a - base.a);
			}

			return out;
		}

		double pr = base.r * base.a, pg = base.g * base.a, pb = base.b * base.a, pa = base.a;

		for(size_t i = 0; i < withs.size(); i++) {
			pr += fr[i] * (withs[i].r * withs[i].a - base.r * base.a);
			pg += fr[i] * (withs[i].g * withs[i].a - base.g * base.a);
			pb += fr[i] * (withs[i].b * withs[i].a - base.b * base.a);
			pa += fr[i] * (withs[i].a - base.a);
		}

		out.a = std::clamp(pa, 0.0, 1.0);

		if(pa > 1e-9) { out.r = pr / pa; out.g = pg / pa; out.b = pb / pa; }

		return out;
	};

	auto keeps = [&](double a) { return mode == AlphaMode::Opaque || (a >= test && a > 0); };
	auto paintAlpha = [&](double a) { return mode == AlphaMode::Transparent ? std::clamp(a, 0.0, 1.0) : 1.0; };

	std::map<std::array<int64_t, 4>, uint16_t> solidIds;
	std::map<std::vector<int64_t>, uint16_t> rampIds;

	struct Kept {
		clipper::Paths paths;
		uint16_t paint;
		const CutPaint* gradient; // for the per-vertex parameter
	};

	std::vector<Kept> kept;

	auto solidPaint = [&](const Final& f) {
		const double pa = paintAlpha(f.a);
		const std::array<int64_t, 4> k{std::llround(f.r * 65536), std::llround(f.g * 65536), std::llround(f.b * 65536), std::llround(pa * 65536)};
		auto it = solidIds.find(k);

		if(it != solidIds.end()) return it->second;

		Paint p;

		p.color = {slug_t(f.r), slug_t(f.g), slug_t(f.b), slug_t(pa)};
		p.opaque = pa >= FINAL_OPAQUE_ALPHA;

		const auto id = static_cast<uint16_t>(mesh.paints.size());

		mesh.paints.push_back(p);
		solidIds[k] = id;

		return id;
	};

	auto pathsArea = [](const clipper::Paths& p) { return std::abs(clipper::area(p)); };

	for(size_t ti = 0; ti < G * G; ti++) {
		const std::vector<Face> faces = buildTile(ti);

		// One face (or one fold cell of a face): `fin` are the folds it takes, `frac` their share of its
		// area.
		auto processFace = [&](const Face& f, const std::vector<const Fold*>& fin, const std::vector<double>& frac) {
			int gradientCount = 0;
			int gradientAt = -1;

			for(size_t k = 0; k < f.stack.size(); k++) {
				if(regions[f.stack[k]].paint.g) { gradientCount++; gradientAt = int(k); }
			}

			const bool sweep = gradientAt >= 0 && regions[f.stack[size_t(gradientAt)]].paint.g->type == GradientInfo::Type::Sweep;

			std::vector<Color> colors(f.stack.size());

			std::vector<Final> withs(fin.size());

			auto folded = [&](const Final& base) {
				if(fin.empty()) return base;

				for(size_t i = 0; i < fin.size(); i++) withs[i] = finalWith(f.stack, colors, fin[i]->region, fin[i]->color);

				return mixFolds(base, withs, frac);
			};

			auto finalAt = [&](double x, double y) {
				for(size_t k = 0; k < f.stack.size(); k++) colors[k] = detail::paintColorAt(regions[f.stack[k]].paint, x, y);

				return folded(finalOf(f.stack, colors));
			};

			if(gradientCount == 0) {
				const Final c = finalAt((double(f.box.x0) + f.box.x1) * 0.5, (double(f.box.y0) + f.box.y1) * 0.5);

				if(mode == AlphaMode::Opaque && c.a < FINAL_OPAQUE_ALPHA) mesh.transparentArea += pathsArea(f.paths);

				if(keeps(c.a)) kept.push_back({f.paths, solidPaint(c), nullptr});

				return;
			}

			if(gradientCount > 1 || sweep) {
				// No closed form: cellPx cells, each kept / dropped at its centre with its centre colour.
				mesh.cutoutApproxFaces++;

				const int precision = clipper::precisionFor(f.paths);

				for(double y = std::floor(double(f.box.y0) / cellPx) * cellPx; y < double(f.box.y1); y += cellPx) {
					for(double x = std::floor(double(f.box.x0) / cellPx) * cellPx; x < double(f.box.x1); x += cellPx) {
						const Final c = finalAt(x + cellPx * 0.5, y + cellPx * 0.5);
						const bool opaqueBad = mode == AlphaMode::Opaque && c.a < FINAL_OPAQUE_ALPHA;

						if(!keeps(c.a) && !opaqueBad) continue;

						clipper::Paths cell = Clipper2Lib::RectClip(Clipper2Lib::RectD(x, y, x + cellPx, y + cellPx), f.paths, precision);

						if(cell.empty()) continue;

						if(opaqueBad) mesh.transparentArea += pathsArea(cell);

						if(keeps(c.a)) kept.push_back({std::move(cell), solidPaint(c), nullptr});
					}
				}

				return;
			}

			// One gradient (in either stack): every other region is solid.
			const CutPaint& gp = regions[f.stack[size_t(gradientAt)]].paint;

			for(size_t k = 0; k < f.stack.size(); k++) colors[k] = regions[f.stack[k]].paint.color;

			auto finalAtT = [&](double t) {
				colors[size_t(gradientAt)] = detail::paintColorAtT(gp, t);

				return folded(finalOf(f.stack, colors));
			};

			// Knots: 0, 1 and every stop (t is clamped to [0, 1]).
			std::vector<double> knots{0.0, 1.0};

			for(const auto& st : gp.g->stops) knots.push_back(std::clamp(double(st.t), 0.0, 1.0));

			std::sort(knots.begin(), knots.end());
			knots.erase(std::unique(knots.begin(), knots.end()), knots.end());

			// Kept t-intervals (final alpha piecewise linear between knots); Opaque keeps all.
			std::vector<std::pair<double, double>> keep;
			std::vector<double> samples(knots);
			bool anyClear = false;

			auto add = [&](double a, double b) {
				if(!keep.empty() && std::abs(keep.back().second - a) < 1e-12) keep.back().second = b;
				else keep.push_back({a, b});
			};

			for(size_t k = 0; k + 1 < knots.size(); k++) {
				const double ta = knots[k], tb = knots[k + 1];
				const double fa = finalAtT(ta).a, fb = finalAtT(tb).a;

				if(fa < FINAL_OPAQUE_ALPHA || fb < FINAL_OPAQUE_ALPHA) anyClear = true;

				if(mode == AlphaMode::Opaque) { add(ta, tb); continue; }

				const double aa = fa - test, ab = fb - test;

				if(aa >= 0 && ab >= 0) add(ta, tb);

				else if(aa >= 0 || ab >= 0) {
					const double tc = ta + (tb - ta) * (aa / (aa - ab));

					samples.push_back(tc);

					if(aa >= 0) add(ta, tc);
					else add(tc, tb);
				}
			}

			// Opaque mode: the face is transparent at the end somewhere along its ramp. Its share of
			// the area is not known in closed form; the face counts whole (an upper bound).
			if(mode == AlphaMode::Opaque && anyClear) mesh.transparentArea += pathsArea(f.paths);

			if(keep.empty()) return;

			// Clamp semantics: an interval touching 0 / 1 extends to -inf / +inf.
			for(auto& [a, b] : keep) {
				if(a <= 0.0) a = -std::numeric_limits<double>::infinity();
				if(b >= 1.0) b = std::numeric_limits<double>::infinity();
			}

			// Composite gradient paint: the final colour at every knot and crossing.
			std::sort(samples.begin(), samples.end());
			samples.erase(std::unique(samples.begin(), samples.end()), samples.end());

			Paint paint;

			paint.type = gp.g->type == GradientInfo::Type::Linear ? Paint::Type::Linear : Paint::Type::Radial;
			paint.opaque = true;
			paint.innerRadius = gp.g->innerRadius;

			// Between samples the ramp interpolates straight colour and alpha linearly, but the
			// composite is not linear there once the gradient's alpha varies over other layers
			// (products of the two). Refine each interval until the displayed colour (premultiplied
			// for Transparent, straight otherwise) is within RAMP_TOLERANCE of the composite.
			struct RampStop { double t; Final c; double pa; };

			auto stopAt = [&](double t) { const Final c = finalAtT(t); return RampStop{t, c, paintAlpha(c.a)}; };
			auto shown = [&](const Final& c, double pa) {
				return mode == AlphaMode::Transparent ? std::array<double, 4>{c.r * pa, c.g * pa, c.b * pa, pa} : std::array<double, 4>{c.r, c.g, c.b, 1.0};
			};

			std::vector<RampStop> ramp;

			std::function<void(const RampStop&, const RampStop&, int)> refine = [&](const RampStop& a, const RampStop& b, int depth) {
				if(depth < RAMP_MAX_DEPTH) {
					const RampStop m = stopAt((a.t + b.t) * 0.5);
					const Final lin{(a.c.r + b.c.r) * 0.5, (a.c.g + b.c.g) * 0.5, (a.c.b + b.c.b) * 0.5, 0.0};
					const auto want = shown(m.c, m.pa), got = shown(lin, (a.pa + b.pa) * 0.5);
					double err = 0;

					for(int k = 0; k < 4; k++) err = std::max(err, std::abs(want[size_t(k)] - got[size_t(k)]));

					if(err > RAMP_TOLERANCE) {
						refine(a, m, depth + 1);
						refine(m, b, depth + 1);

						return;
					}
				}

				ramp.push_back(b);
			};

			for(size_t k = 0; k < samples.size(); k++) {
				const RampStop st = stopAt(samples[k]);

				if(k == 0) ramp.push_back(st);
				else refine(ramp.back(), st, 0);
			}

			for(const RampStop& st : ramp) {
				paint.stops.push_back({slug_t(st.t), Color{slug_t(st.c.r), slug_t(st.c.g), slug_t(st.c.b), slug_t(st.pa)}});

				if(st.pa < FINAL_OPAQUE_ALPHA) paint.opaque = false;
			}

			if(gp.g->type == GradientInfo::Type::Radial) {
				const slug_t span = gp.g->transform.xx - gp.g->innerRadius;

				paint.innerRadius = span != 0_cv ? gp.g->innerRadius / span : 0_cv;
			}

			// Identical composite ramps share one paint.
			std::vector<int64_t> sig{int64_t(paint.type), std::llround(double(paint.innerRadius) * 65536)};

			for(const auto& st : paint.stops) {
				for(double v : {double(st.t), double(st.color.r), double(st.color.g), double(st.color.b), double(st.color.a)}) sig.push_back(std::llround(v * 65536));
			}

			uint16_t paintId;

			if(auto it = rampIds.find(sig); it != rampIds.end()) paintId = it->second;

			else {
				paintId = static_cast<uint16_t>(mesh.paints.size());
				mesh.paints.push_back(paint);
				rampIds.emplace(std::move(sig), paintId);
			}

			// Clip the face to the kept t-set.
			const double bx0 = double(f.box.x0) - 1, by0 = double(f.box.y0) - 1, bx1 = double(f.box.x1) + 1, by1 = double(f.box.y1) + 1;
			clipper::Paths keepRegion;

			if(keep.size() == 1 && !std::isfinite(keep[0].first) && !std::isfinite(keep[0].second)) keepRegion = f.paths;

			else if(gp.g->type == GradientInfo::Type::Linear) {
				const Matrix& L = gp.toLocal;
				const Matrix& m = gp.g->transform;
				const double alpha = m.xx * L.xx + m.xy * L.yx;
				const double beta = m.xx * L.xy + m.xy * L.yy;
				const double gamma = m.xx * L.dx + m.xy * L.dy + m.dx;

				clipper::Paths slabs;

				for(const auto& [a, b] : keep) {
					clipper::Path poly = detail::slab(alpha, beta, gamma, a, b, bx0, by0, bx1, by1);

					if(poly.size() >= 3) slabs.push_back(std::move(poly));
				}

				keepRegion = clipper::intersect(f.paths, slabs);
			}

			else {
				// Radial: t + inner' = |Q(px)|, Q affine; level sets are ellipses in px.
				const Matrix& L = gp.toLocal;
				const Matrix& m = gp.g->transform;
				double b00, b01, b10, b11, cx, cy, inner;

				if(gp.g->type == GradientInfo::Type::AffineRadial) {
					b00 = m.xx; b01 = m.xy; b10 = m.yx; b11 = m.yy; cx = m.dx; cy = m.dy; inner = gp.g->innerRadius;
				}

				else {
					const double span = double(m.xx) - gp.g->innerRadius;
					const double k = span != 0 ? 1.0 / span : 0.0;

					b00 = k; b01 = 0; b10 = 0; b11 = k; cx = m.dx; cy = m.dy; inner = gp.g->innerRadius * k;
				}

				// Q(px) = B (L px + Lt - c)
				const double q00 = b00 * L.xx + b01 * L.yx, q01 = b00 * L.xy + b01 * L.yy;
				const double q10 = b10 * L.xx + b11 * L.yx, q11 = b10 * L.xy + b11 * L.yy;
				const double qx = b00 * (L.dx - cx) + b01 * (L.dy - cy);
				const double qy = b10 * (L.dx - cx) + b11 * (L.dy - cy);
				const double det = q00 * q11 - q01 * q10;

				if(std::abs(det) < 1e-18) return;

				auto ellipse = [&](double r) {
					clipper::Path poly;

					// px radius of the ellipse's longest axis decides the segment count.
					const double rpx = r * std::sqrt((q00 * q00 + q01 * q01 + q10 * q10 + q11 * q11) / (det * det));
					const double tol = std::max(1e-3, double(cfg.tolerancePx));
					const int n = std::clamp(int(std::ceil(PI_CV / std::acos(std::max(-1.0, 1.0 - tol / std::max(rpx, tol))))), 16, 2048);

					for(int k = 0; k < n; k++) {
						const double a = 2.0 * PI_CV * k / n;
						const double ux = r * std::cos(a) - qx, uy = r * std::sin(a) - qy;

						poly.push_back({(q11 * ux - q01 * uy) / det, (-q10 * ux + q00 * uy) / det});
					}

					return poly;
				};

				const clipper::Path boxPoly{{bx0, by0}, {bx1, by0}, {bx1, by1}, {bx0, by1}};
				clipper::Paths annuli;

				for(const auto& [a, b] : keep) {
					const double r0 = std::isfinite(a) ? a + inner : -1.0;
					const double r1 = std::isfinite(b) ? b + inner : std::numeric_limits<double>::infinity();

					if(r1 <= 0) continue;

					clipper::Paths outer = std::isfinite(r1) ? clipper::Paths{ellipse(r1)} : clipper::Paths{boxPoly};

					if(r0 > 0) outer = clipper::difference(clipper::normalize(outer, FillRule::NonZero), clipper::normalize({ellipse(r0)}, FillRule::NonZero));
					else outer = clipper::normalize(outer, FillRule::NonZero);

					annuli = clipper::unite(annuli, outer);
				}

				keepRegion = clipper::intersect(f.paths, annuli);
			}

			if(!keepRegion.empty()) kept.push_back({std::move(keepRegion), paintId, &gp});
		};

		for(const Face& f0 : faces) {
			// The folds whose centroid lies in this face, grouped by fold cells: each cell of the face
			// that holds folds takes its own area-weighted mean, so the density of what was folded
			// survives at the cell scale; the rest of the face is untouched. A cell is 2 x featurePx
			// but never finer than 1/32 of the canvas: finer cells would cost more triangles (a piece
			// and a hole each) than the features they replace.
			std::map<std::pair<int64_t, int64_t>, std::vector<const Fold*>> cells;
			const double cs = std::max({2.0 * double(cfg.featurePx), cellPx, std::max(W, H) / FOLD_GRID});

			for(uint32_t k : tileFolds[ti]) {
				Fold& fd = folds[k];

				if(fd.used || fd.x < f0.box.x0 || fd.x > f0.box.x1 || fd.y < f0.box.y0 || fd.y > f0.box.y1) continue;
				if(!clipper::contains(f0.paths, fd.x, fd.y)) continue;

				fd.used = true;
				cells[{int64_t(std::floor((fd.x - wx0) / cs)), int64_t(std::floor((fd.y - wy0) / cs))}].push_back(&fd);
			}

			if(cells.empty()) { processFace(f0, {}, {}); continue; }

			const int precision = clipper::precisionFor(f0.paths);
			clipper::Paths used;

			for(const auto& [cell, list] : cells) {
				const double x0 = wx0 + double(cell.first) * cs, y0 = wy0 + double(cell.second) * cs;
				Face piece;

				piece.paths = Clipper2Lib::RectClip(Clipper2Lib::RectD(x0, y0, x0 + cs, y0 + cs), f0.paths, precision);

				if(piece.paths.empty()) continue;

				piece.box = boxOf(piece.paths);
				piece.stack = f0.stack;

				const double A = pathsArea(piece.paths);
				std::vector<double> fr;
				double sum = 0;

				for(const Fold* fd : list) { fr.push_back(A > 0 ? fd->area / A : 0.0); sum += fr.back(); }

				if(sum > 1.0) for(double& v : fr) v /= sum;

				processFace(piece, list, fr);
				used.push_back({{x0, y0}, {x0 + cs, y0}, {x0 + cs, y0 + cs}, {x0, y0 + cs}});
			}

			Face rest;

			rest.paths = clipper::difference(f0.paths, clipper::normalize(used, FillRule::NonZero));

			if(rest.paths.empty()) continue;

			rest.box = boxOf(rest.paths);
			rest.stack = f0.stack;
			processFace(rest, {}, {});
		}

		// Emit this tile's kept faces (no overlay), then drop them.
		for(const Kept& k : kept) {
			const tessellate::Mesh2D tri = detail::triangulateTiled(k.paths);
			const auto base = static_cast<uint32_t>(mesh.positions.size() / 2);

			for(size_t v = 0; v + 1 < tri.positions.size(); v += 2) {
				const double x = tri.positions[v], y = tri.positions[v + 1];

				mesh.positions.push_back(static_cast<float>(x / W));
				mesh.positions.push_back(static_cast<float>(cfg.vUp ? 1.0 - y / H : y / H));
				mesh.paintIds.push_back(k.paint);

				slug_t p0 = 0_cv, p1 = 0_cv;

				if(k.gradient) {
					slug_t lx, ly;

					k.gradient->toLocal.apply(slug_t(x), slug_t(y), lx, ly);

					const Matrix& m = k.gradient->g->transform;

					switch(k.gradient->g->type) {
						case GradientInfo::Type::Linear:
							p0 = m.xx * lx + m.xy * ly + m.dx;
							break;

						case GradientInfo::Type::AffineRadial:
							p0 = m.xx * (lx - m.dx) + m.xy * (ly - m.dy);
							p1 = m.yx * (lx - m.dx) + m.yy * (ly - m.dy);
							break;

						case GradientInfo::Type::Radial: {
							const slug_t span = m.xx - k.gradient->g->innerRadius;
							const slug_t kk = span != 0_cv ? 1_cv / span : 0_cv;

							p0 = (lx - m.dx) * kk;
							p1 = (ly - m.dy) * kk;
							break;
						}

						default:
							break;
					}
				}

				mesh.params.push_back(static_cast<float>(p0));
				mesh.params.push_back(static_cast<float>(p1));
			}

			for(size_t t = 0; t + 2 < tri.indices.size(); t += 3) {
				uint32_t a = base + tri.indices[t], b = base + tri.indices[t + 1], c = base + tri.indices[t + 2];

				const float* P = mesh.positions.data();
				const double area2 =
					(double(P[b * 2]) - P[a * 2]) * (double(P[c * 2 + 1]) - P[a * 2 + 1]) -
					(double(P[c * 2]) - P[a * 2]) * (double(P[b * 2 + 1]) - P[a * 2 + 1])
				;

				if(area2 == 0.0) continue;
				if(area2 < 0.0) std::swap(b, c);

				mesh.indices.push_back(a);
				mesh.indices.push_back(b);
				mesh.indices.push_back(c);
			}
		}

		kept.clear();

		if(cfg.maxTriangles > 0 && mesh.indices.size() / 3 > cfg.maxTriangles) {
			mesh.aborted = true;
			break;
		}
	}

	mesh.opaqueIndexCount = static_cast<uint32_t>(mesh.indices.size());
	mesh.overlayIndexCount = 0;
	mesh.trianglesAfter = mesh.indices.size() / 3;

	return mesh;
}

// ================================================================================================
// LOD levels for bakeFinal(): level 0 is the key at its own tolerance with features below the
// floor folded (the floor: what no view can resolve); each further level doubles the flattening
// tolerance, the cell size and the folded feature size every two levels (half-octave steps). After
// LOD_LEVELS levels comes the mean: the whole window in its area-weighted mean colour.
// ================================================================================================
struct LodLevel {
	int level = 0;
	slug_t tolerancePx = 0.25_cv;
	slug_t featurePx = 0_cv;
	slug_t cellPx = 1_cv;
};

inline constexpr int LOD_LEVELS = 15;

// Half-octave steps: level L scales by k = 2^(L / 2) (1, 1.41, 2, 2.83, ... 128).
inline LodLevel lodLevel(int level, slug_t baseTolerancePx, slug_t featureFloorPx=0_cv) {
	LodLevel l;
	const slug_t k = slug_t(std::exp2(0.5 * double(std::clamp(level, 0, 60))));

	l.level = level;
	l.tolerancePx = baseTolerancePx * k;
	l.cellPx = slug_t(CUTOUT_CELL_PX) * k;
	l.featurePx = level == 0 ? featureFloorPx : std::max(featureFloorPx, 0.5_cv * k);

	return l;
}

inline void applyLod(BakeConfig& cfg, const LodLevel& l) {
	cfg.tolerancePx = l.tolerancePx;
	cfg.featurePx = l.featurePx;
	cfg.cellPx = l.cellPx;
	cfg.simplifyPx = l.level > 0 ? l.tolerancePx : 0_cv;
}

#ifdef SLUGHORN_HAS_MESHOPT
// meshoptimizer on a bakeFinal() mesh (no overlay): welds identical vertices (position, paint,
// parameter), then simplifies toward @p targetTriangles without passing @p maxErrorPx (authoring
// px, absolute). Vertices that share a position but not a paint stay apart, so meshoptimizer sees
// every colour boundary as an attribute seam and only ever slides along it: the boundaries stay
// shared and exact, no gaps open between colours. Gradient parameters are linear in position, so
// they stay exact on the new triangles. Returns the error meshoptimizer reports (px).
inline double simplifyBaked(BakedMesh& m, double widthPx, double heightPx, size_t targetTriangles, double maxErrorPx, bool vUp=true) {
	const size_t nv = m.positions.size() / 2;

	if(m.indices.empty() || nv == 0) return 0.0;

	struct Vtx { float u, v, paint, p0, p1; };

	std::vector<Vtx> verts(nv);

	for(size_t i = 0; i < nv; i++) verts[i] = {m.positions[i * 2], m.positions[i * 2 + 1], float(m.paintIds[i]), m.params[i * 2], m.params[i * 2 + 1]};

	std::vector<unsigned int> remap(nv);
	std::vector<unsigned int> idx(m.indices.begin(), m.indices.end());
	const size_t unique = meshopt_generateVertexRemap(remap.data(), idx.data(), idx.size(), verts.data(), nv, sizeof(Vtx));

	std::vector<Vtx> uv(unique);

	meshopt_remapVertexBuffer(uv.data(), verts.data(), nv, sizeof(Vtx), remap.data());
	meshopt_remapIndexBuffer(idx.data(), idx.data(), idx.size(), remap.data());

	// Authoring-pixel positions (z = 0) so the error is in px.
	std::vector<float> pos(unique * 3);

	for(size_t i = 0; i < unique; i++) {
		pos[i * 3] = float(double(uv[i].u) * widthPx);
		pos[i * 3 + 1] = float((vUp ? 1.0 - double(uv[i].v) : double(uv[i].v)) * heightPx);
		pos[i * 3 + 2] = 0.0f;
	}

	std::vector<unsigned int> out(idx.size());
	float err = 0.0f;
	const size_t n = meshopt_simplify(out.data(), idx.data(), idx.size(), pos.data(), unique, sizeof(float) * 3,
		std::min(idx.size(), targetTriangles * 3), float(maxErrorPx), meshopt_SimplifyErrorAbsolute, &err);

	out.resize(n);

	// Compact the kept vertices.
	std::vector<unsigned int> keep(unique, ~0u);
	BakedMesh r = m;

	r.positions.clear();
	r.paintIds.clear();
	r.params.clear();
	r.indices.clear();

	for(unsigned int i : out) {
		if(keep[i] == ~0u) {
			keep[i] = unsigned(r.positions.size() / 2);
			r.positions.push_back(uv[i].u);
			r.positions.push_back(uv[i].v);
			r.paintIds.push_back(uint16_t(uv[i].paint));
			r.params.push_back(uv[i].p0);
			r.params.push_back(uv[i].p1);
		}

		r.indices.push_back(keep[i]);
	}

	r.opaqueIndexCount = uint32_t(r.indices.size());
	r.overlayIndexCount = 0;
	r.trianglesAfter = r.indices.size() / 3;
	m = std::move(r);

	return double(err);
}
#endif

// The alpha-tested (cutout) bake: bakeFinal with AlphaMode::AlphaTest.
inline BakedMesh bakeCutout(
	const Atlas& atlas,
	const CompositeShape& composite,
	const std::vector<LayerSource>& meta,
	const BakeConfig& cfg,
	const stamp::Set* stamps=nullptr
) {
	BakeConfig c = cfg;

	c.alphaMode = AlphaMode::AlphaTest;

	return bakeFinal(atlas, &composite, meta, c, stamps);
}


// Bakes @p composite (shapes in @p atlas) into one planar mesh. See BakedMesh for the layout.
// Sweep gradients are emitted as Radial paints with the raw (x, y) offset from the center (the
// consumer would need atan2); slughorn's SVG loaders never produce them.
inline BakedMesh bakeMesh(
	const Atlas& atlas,
	const CompositeShape& composite,
	const std::vector<LayerSource>& meta,
	const BakeConfig& cfg,
	const stamp::Set* stamps=nullptr
) {
	if(cfg.alphaTest > 0_cv) return bakeCutout(atlas, composite, meta, cfg, stamps);

	BakedMesh mesh;

	mesh.tolerancePx = cfg.tolerancePx;

	const slug_t ppe = cfg.width; // pixels per em

	struct Region {
		size_t layer;
		clipper::Paths paths;
		bool opaque;
		bool stamp = false; // one expanded stamp instance
		Color color = {};
		const stamp::Instance* inst = nullptr;
	};

	std::vector<Region> regions;
	const auto& gradients = atlas.getGradients();

	for(size_t li = 0; li < composite.layers.size(); li++) {
		const Layer& layer = composite.layers[li];

		if(layer.drawMode != DrawMode::Visible) continue;

		// Stamp layers expand to one region per instance, in paint order.
		if(stamp::isStampLayer(layer)) {
			if(!stamps || stamp::stampIndex(layer) >= stamps->layers.size()) continue;

			const Matrix toPx = Matrix::scale(ppe, ppe);

			for(const stamp::Instance& in : stamps->layers[stamp::stampIndex(layer)].instances) {
				if(in.color.a <= 0_cv) continue;

				auto paths = clipper::normalize(
					clipper::toPaths(stamp::instanceContours(*stamps, in), cfg.tolerancePx, toPx),
					FillRule::NonZero
				);

				if(paths.empty()) continue;

				mesh.trianglesBefore += detail::triangulateTiled(paths).indices.size() / 3;

				bool opaque = in.color.a >= cfg.opaqueAlpha;

				if(in.gradient && in.gradient <= stamps->gradients.size()) {
					for(const auto& st : stamps->gradients[in.gradient - 1].stops) if(st.color.a * in.color.a < cfg.opaqueAlpha) opaque = false;
				}

				regions.push_back({li, std::move(paths), opaque, true, in.color, &in});
			}

			continue;
		}

		const auto shape = atlas.getShape(layer.key);

		if(!shape) continue;

		const LayerSource src = li < meta.size() ? meta[li] : LayerSource{};

		if(src.stroke) mesh.strokeLayers++;

		const slug_t s = layer.scale;
		const slug_t ox = (layer.transform.x - shape->originX) * s;
		const slug_t oy = (layer.transform.y - shape->originY) * s;

		// local em -> authoring px
		const Matrix xf{ .xx = s * ppe, .yy = s * ppe, .dx = ox * ppe, .dy = oy * ppe };

		auto paths = clipper::normalize(
			clipper::toPaths(atlas.getShapeContours(layer.key), cfg.tolerancePx, xf),
			src.fillRule
		);

		if(paths.empty()) continue;

		const Paint paint = detail::paintOf(atlas, layer, src, cfg.opaqueAlpha);

		mesh.trianglesBefore += detail::triangulateTiled(paths).indices.size() / 3;

		regions.push_back({li, std::move(paths), paint.opaque});
	}

	// Tile the canvas into a grid sized to ~PLANAR_TILE_VERTICES region vertices a tile, and run
	// clipping (Clipper2 RectClip, holes stay holes), planarization and triangulation one tile at a
	// time, emitting and dropping each tile's work before the next. Without tiling a big merged
	// region pays for every occluder above it anywhere on the canvas (quadratic in practice); and
	// streaming keeps the peak live-allocation count to one tile's (the sandbox caps it). Tiles are
	// disjoint, so the overlay's order across tiles is free; within a tile it is paint order.
	// Clipping to the canvas rectangle is also the canvas's own semantics.
	auto boxOf = [](const clipper::Paths& paths) {
		detail::Box b;

		for(const auto& p : paths) for(const auto& pt : p) b.add(static_cast<slug_t>(pt.x), static_cast<slug_t>(pt.y));

		return b;
	};

	size_t totalVerts = 0;

	for(const auto& r : regions) totalVerts += clipper::vertexCount(r.paths);

	const auto G = std::clamp<size_t>(
		static_cast<size_t>(std::ceil(std::sqrt(double(totalVerts) / double(PLANAR_TILE_VERTICES)))), 1, 32
	);

	const double tw = double(cfg.width) / double(G), th = double(cfg.height) / double(G);

	std::vector<detail::Box> rbox(regions.size());
	std::vector<int> rprec(regions.size());

	for(size_t ri = 0; ri < regions.size(); ri++) {
		rbox[ri] = boxOf(regions[ri].paths);
		rprec[ri] = clipper::precisionFor(regions[ri].paths);
	}

	std::vector<uint32_t> overlay;

	// Solid paints are shared: every region of the same color and opacity class gets one id.
	std::map<std::array<int64_t, 5>, uint16_t> solidIds;

	auto solidPaint = [&](const Paint& p) -> uint16_t {
		const std::array<int64_t, 5> k{
			std::llround(double(p.color.r) * 65536), std::llround(double(p.color.g) * 65536),
			std::llround(double(p.color.b) * 65536), std::llround(double(p.color.a) * 65536), p.opaque ? 1 : 0
		};

		auto it = solidIds.find(k);

		if(it != solidIds.end()) return it->second;

		const auto id = static_cast<uint16_t>(mesh.paints.size());

		mesh.paints.push_back(p);
		solidIds[k] = id;

		return id;
	};

	// Per region (lazily): its paint id and how a vertex maps into its gradient's space.
	struct RegionPaint {
		bool ready = false;
		uint16_t id = 0;
		const GradientInfo* grad = nullptr;
		bool stampFrame = false;
		stamp::Inverse inv;
		slug_t s = 1_cv, ox = 0_cv, oy = 0_cv;
	};

	std::vector<RegionPaint> rpaint(regions.size());

	auto paintFor = [&](size_t ri) -> const RegionPaint& {
		RegionPaint& rp = rpaint[ri];

		if(rp.ready) return rp;

		rp.ready = true;

		const Region& r = regions[ri];
		const Layer& layer = composite.layers[r.layer];
		const LayerSource src = r.layer < meta.size() ? meta[r.layer] : LayerSource{};
		const auto shape = r.stamp ? std::optional<Atlas::Shape>{} : atlas.getShape(layer.key);

		rp.grad = (!r.stamp && layer.gradientId > 0 && layer.gradientId <= gradients.size()) ? &gradients[layer.gradientId - 1] : nullptr;
		rp.s = layer.scale;
		rp.ox = shape ? (layer.transform.x - shape->originX) * rp.s : 0_cv;
		rp.oy = shape ? (layer.transform.y - shape->originY) * rp.s : 0_cv;

		// Stamp instance with a prototype-frame gradient: params come from the unit frame.
		if(r.stamp && r.inst && r.inst->gradient && stamps && r.inst->gradient <= stamps->gradients.size()) {
			rp.grad = &stamps->gradients[r.inst->gradient - 1];
			rp.stampFrame = true;
			rp.inv = stamp::invert(r.inst->m);
		}

		if(r.stamp && rp.grad) {
			// One paint per (gradient, tint).
			const std::array<int64_t, 5> k{
				-int64_t(r.inst->gradient), std::llround(double(r.color.r) * 65536), std::llround(double(r.color.g) * 65536),
				std::llround(double(r.color.b) * 65536), std::llround(double(r.color.a) * 65536)
			};

			auto it = solidIds.find(k);

			if(it != solidIds.end()) rp.id = it->second;

			else {
				Paint p;

				p.type = rp.grad->type == GradientInfo::Type::Linear ? Paint::Type::Linear : Paint::Type::Radial;
				p.stops = rp.grad->stops;
				p.innerRadius = rp.grad->innerRadius;
				p.opaque = r.opaque;

				for(auto& st : p.stops) {
					st.color.r *= r.color.r;
					st.color.g *= r.color.g;
					st.color.b *= r.color.b;
					st.color.a *= r.color.a;
				}

				rp.id = static_cast<uint16_t>(mesh.paints.size());
				mesh.paints.push_back(p);
				solidIds[k] = rp.id;
			}
		}

		else if(r.stamp) {
			Paint p;

			p.color = r.color;
			p.opaque = r.opaque;
			rp.id = solidPaint(p);
		}

		else if(!rp.grad) rp.id = solidPaint(detail::paintOf(atlas, layer, src, cfg.opaqueAlpha));

		else {
			rp.id = static_cast<uint16_t>(mesh.paints.size());
			mesh.paints.push_back(detail::paintOf(atlas, layer, src, cfg.opaqueAlpha));
		}

		if(rp.grad && rp.grad->type == GradientInfo::Type::Radial) {
			const slug_t span = rp.grad->transform.xx - rp.grad->innerRadius;

			mesh.paints[rp.id].innerRadius = span != 0_cv ? rp.grad->innerRadius / span : 0_cv;
		}

		return rp;
	};

	auto emit = [&](const tessellate::Mesh2D& tri, size_t ri) {
		const RegionPaint& rp = paintFor(ri);
		const auto base = static_cast<uint32_t>(mesh.positions.size() / 2);

		for(size_t v = 0; v + 1 < tri.positions.size(); v += 2) {
			const slug_t x = tri.positions[v], y = tri.positions[v + 1];

			mesh.positions.push_back(static_cast<float>(x / cfg.width));
			mesh.positions.push_back(static_cast<float>(cfg.vUp ? 1_cv - y / cfg.height : y / cfg.height));
			mesh.paintIds.push_back(rp.id);

			slug_t p0 = 0_cv, p1 = 0_cv;

			if(rp.grad) {
				// Back to the space GradientInfo lives in: the layer's local em-space, or a stamp
				// instance's prototype unit frame.
				slug_t lx = (x / ppe - rp.ox) / rp.s;
				slug_t ly = (y / ppe - rp.oy) / rp.s;

				if(rp.stampFrame) {
					const slug_t ex = x / ppe, ey = y / ppe;

					lx = rp.inv.a * ex + rp.inv.b * ey + rp.inv.e;
					ly = rp.inv.c * ex + rp.inv.d * ey + rp.inv.f;
				}

				const Matrix& m = rp.grad->transform;

				switch(rp.grad->type) {
					case GradientInfo::Type::Linear:
						p0 = m.xx * lx + m.xy * ly + m.dx;
						break;

					case GradientInfo::Type::AffineRadial: {
						const slug_t dx = lx - m.dx, dy = ly - m.dy;

						p0 = m.xx * dx + m.xy * dy;
						p1 = m.yx * dx + m.yy * dy;

						break;
					}

					case GradientInfo::Type::Radial: {
						// Circular: normalize by the span so t = length(param) - inner / span.
						const slug_t span = m.xx - rp.grad->innerRadius;
						const slug_t k = span != 0_cv ? 1_cv / span : 0_cv;

						p0 = (lx - m.dx) * k;
						p1 = (ly - m.dy) * k;

						break;
					}

					case GradientInfo::Type::Sweep:
						p0 = lx - m.dx;
						p1 = ly - m.dy;
						break;
				}
			}

			mesh.params.push_back(static_cast<float>(p0));
			mesh.params.push_back(static_cast<float>(p1));
		}

		auto& dstIdx = (regions[ri].opaque && cfg.planarize) ? mesh.indices : overlay;

		for(size_t t = 0; t + 2 < tri.indices.size(); t += 3) {
			uint32_t a = base + tri.indices[t], b = base + tri.indices[t + 1], c = base + tri.indices[t + 2];

			const float* P = mesh.positions.data();
			const double area2 =
				(double(P[b * 2]) - P[a * 2]) * (double(P[c * 2 + 1]) - P[a * 2 + 1]) -
				(double(P[c * 2]) - P[a * 2]) * (double(P[b * 2 + 1]) - P[a * 2 + 1])
			;

			// Drop triangles that are degenerate at float precision; enforce CCW in (u, v).
			if(area2 == 0.0) continue;
			if(area2 < 0.0) std::swap(b, c);

			dstIdx.push_back(a);
			dstIdx.push_back(b);
			dstIdx.push_back(c);
		}
	};

	struct Piece {
		size_t region;
		clipper::Paths paths;
		detail::Box box;
	};

	for(size_t j = 0; j < G; j++) {
		for(size_t i = 0; i < G; i++) {
			const double x0 = tw * double(i), y0 = th * double(j), x1 = tw * double(i + 1), y1 = th * double(j + 1);
			const Clipper2Lib::RectD rect(x0, y0, x1, y1);

			std::vector<Piece> pieces;

			for(size_t ri = 0; ri < regions.size(); ri++) {
				const detail::Box& b = rbox[ri];

				if(double(b.x1) < x0 || double(b.x0) > x1 || double(b.y1) < y0 || double(b.y0) > y1) continue;

				clipper::Paths piece = Clipper2Lib::RectClip(rect, regions[ri].paths, rprec[ri]);

				if(piece.empty()) continue;

				const detail::Box pb = boxOf(piece);

				pieces.push_back({ri, std::move(piece), pb});
			}

			// Planarize: top -> bottom, subtract every opaque piece above. Pieces are normalized
			// (every covered point has winding exactly 1, holes included), so a plain concatenation
			// of several IS their union under the nonzero rule - no incremental union.
			if(cfg.planarize) {
				std::vector<std::pair<clipper::Paths, detail::Box>> occ;

				for(size_t k = pieces.size(); k-- > 0;) {
					Piece& pc = pieces[k];
					clipper::Paths full = regions[pc.region].opaque ? pc.paths : clipper::Paths{};
					clipper::Paths clips;

					for(const auto& [paths, box] : occ) {
						if(box.x0 <= pc.box.x1 && pc.box.x0 <= box.x1 && box.y0 <= pc.box.y1 && pc.box.y0 <= box.y1) {
							clips.insert(clips.end(), paths.begin(), paths.end());
						}
					}

					if(!clips.empty()) pc.paths = clipper::difference(pc.paths, clips);

					if(regions[pc.region].opaque) occ.push_back({std::move(full), pc.box});
				}
			}

			for(const Piece& pc : pieces) {
				if(pc.paths.empty()) continue;

				const tessellate::Mesh2D tri = detail::triangulateTiled(pc.paths);

				if(!tri.indices.empty()) emit(tri, pc.region);
			}
		}
	}

	mesh.opaqueIndexCount = static_cast<uint32_t>(mesh.indices.size());
	mesh.overlayIndexCount = static_cast<uint32_t>(overlay.size());
	mesh.indices.insert(mesh.indices.end(), overlay.begin(), overlay.end());
	mesh.trianglesAfter = mesh.indices.size() / 3;

	return mesh;
}

// ================================================================================================
// cost
// ================================================================================================
struct Cost {
	size_t curves = 0;

	// Largest curve list of any single horizontal / vertical band, over every layer's shape.
	uint32_t maxBandCurvesH = 0;
	uint32_t maxBandCurvesV = 0;

	// Upper bound on curve evaluations for one fragment covered by every layer:
	// sum over layers of (max hband + max vband).
	size_t slugWork = 0;

	// Expected curve evaluations per fragment, averaged over the key's canvas: sum over layers of
	// (layer quad area / canvas area) * (mean hband + mean vband curve count). This is what the
	// mode recommendation uses; slugWork is the worst case.
	double slugWorkMean = 0.0;

	size_t layersBefore = 0;
	size_t layersAfter = 0;
	size_t strokeLayers = 0;
	size_t gradientLayers = 0;

	size_t trianglesBefore = 0;
	size_t trianglesAfter = 0;

	// Stamp layers (stamp.hpp): how many, their instances, and the per-pixel work of the worst
	// cell: sum over stamp layers of maxPerCell x (prototype curves per band (h + v), or 1 for the
	// analytic rect / ellipse). stampGrid is the largest G picked (buildGrid).
	size_t stampLayers = 0;
	size_t stampInstances = 0;
	uint32_t stampMaxPerCell = 0;
	uint32_t stampGrid = 0;
	double stampWork = 0.0;

	// "mesh" | "stamp" | "slug" | "mean"
	std::string mode;
};

// Mode thresholds (see recommend()). Tunables, calibrated on the sakuragaoka-station keys.
//
// 1. "mesh" when the planar mesh is a card-sized triangle count: as cheap as any small prop,
//    exact at any distance up to the bake tolerance, no per-fragment curve work at all.
// 2. "slug" when the mesh would be big but the Slug shader's expected per-fragment work (mean
//    curve evaluations, quad overdraw included) and its quad count stay modest.
// 2b. "stamp" for keys with stamp layers whose Slug work plus stamp work (worst cell) stays
//    under the same per-fragment budget: Slug for the ordinary layers + the stamp shader.
// 3. "mesh" again up to a hard triangle budget: heavy, but still cheaper than heavy Slug.
// 4. "mean": too heavy either way - draw the key's mean color (or a low-res raster) instead.
inline constexpr size_t MESH_TRIANGLES_MAX = 4096;
inline constexpr double SLUG_WORK_MEAN_MAX = 256.0;
inline constexpr size_t SLUG_LAYERS_MAX = 1024;
inline constexpr size_t MESH_TRIANGLES_HARD_MAX = 65536;

inline std::string recommend(const Cost& c) {
	if(c.trianglesAfter <= MESH_TRIANGLES_MAX) return "mesh";

	if(c.stampLayers > 0) {
		if(c.slugWorkMean + c.stampWork <= SLUG_WORK_MEAN_MAX && c.layersAfter <= SLUG_LAYERS_MAX) return "stamp";
	}

	else if(c.slugWorkMean <= SLUG_WORK_MEAN_MAX && c.layersAfter <= SLUG_LAYERS_MAX) return "slug";
	if(c.trianglesAfter <= MESH_TRIANGLES_HARD_MAX) return "mesh";

	return "mean";
}

// @p atlas must be built (band statistics come from the packed band texture). @p canvasEmW/H is
// the key's canvas in em (1 x heightEm for SVG loads); 0 derives it from the composite's bounds.
inline Cost cost(
	const Atlas& atlas,
	const CompositeShape& composite,
	const BakedMesh& mesh,
	size_t layersBefore,
	slug_t canvasEmW=0_cv,
	slug_t canvasEmH=0_cv,
	const stamp::Set* stamps=nullptr,
	slug_t pxPerEm=0_cv
) {
	if(canvasEmW <= 0_cv || canvasEmH <= 0_cv) {
		const auto bb = composite.boundingBox(atlas);

		canvasEmW = bb ? bb->x1 - bb->x0 : 1_cv;
		canvasEmH = bb ? bb->y1 - bb->y0 : 1_cv;
	}

	const double canvasArea = std::max(1e-12, double(canvasEmW) * double(canvasEmH));

	Cost c;

	c.layersBefore = layersBefore;
	c.layersAfter = composite.layers.size();
	c.strokeLayers = mesh.strokeLayers;
	c.trianglesBefore = mesh.trianglesBefore;
	c.trianglesAfter = mesh.trianglesAfter;

	static const stamp::Set noStamps{};

	stamp::Evaluator ev(atlas, stamps ? *stamps : noStamps);

	if(stamps) for(const auto& p : stamps->protos) c.curves += p.curves.size();

	for(const auto& layer : composite.layers) {
		if(stamp::isStampLayer(layer)) {
			if(!stamps || stamp::stampIndex(layer) >= stamps->layers.size()) continue;

			const stamp::Layer& sl = stamps->layers[stamp::stampIndex(layer)];
			const auto grid = stamp::buildGrid(atlas, *stamps, sl, canvasEmW, canvasEmH,
				pxPerEm > 0_cv ? pxPerEm : 1_cv / canvasEmW);

			uint32_t factor = 1;

			for(const auto& in : sl.instances) factor = std::max(factor, ev.bandCost(in.proto));

			c.stampLayers++;
			c.stampInstances += sl.instances.size();
			c.stampMaxPerCell = std::max(c.stampMaxPerCell, grid.maxPerCell);
			c.stampGrid = std::max(c.stampGrid, grid.G);
			c.stampWork += double(grid.maxPerCell) * double(factor);

			continue;
		}

		if(layer.gradientId) c.gradientLayers++;

		const auto shape = atlas.getShape(layer.key);

		if(!shape || shape->curves.empty()) continue;

		c.curves += shape->curves.size();

		const render::Sampler s = render::decode(atlas, layer.key);

		uint32_t h = 0, v = 0;
		double hSum = 0.0, vSum = 0.0;

		for(size_t i = 0; i + 1 < s.hbandOffsets.size(); i++) {
			const uint32_t n = s.hbandOffsets[i + 1] - s.hbandOffsets[i];

			h = std::max(h, n);
			hSum += n;
		}

		for(size_t i = 0; i + 1 < s.vbandOffsets.size(); i++) {
			const uint32_t n = s.vbandOffsets[i + 1] - s.vbandOffsets[i];

			v = std::max(v, n);
			vSum += n;
		}

		const double hMean = s.hbandOffsets.size() > 1 ? hSum / double(s.hbandOffsets.size() - 1) : 0.0;
		const double vMean = s.vbandOffsets.size() > 1 ? vSum / double(s.vbandOffsets.size() - 1) : 0.0;
		const double quadArea = double(shape->width) * double(shape->height) * double(layer.scale) * double(layer.scale);

		c.slugWorkMean += std::min(1.0, quadArea / canvasArea) * (hMean + vMean);

		c.maxBandCurvesH = std::max(c.maxBandCurvesH, h);
		c.maxBandCurvesV = std::max(c.maxBandCurvesV, v);
		c.slugWork += h + v;
	}

	c.mode = recommend(c);

	return c;
}

}
}
