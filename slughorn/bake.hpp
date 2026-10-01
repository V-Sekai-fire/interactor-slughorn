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

#include <algorithm>
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
	};

	MergeResult result;
	std::vector<Group> groups;

	result.layersBefore = in.layers.size();

	for(size_t li = 0; li < in.layers.size(); li++) {
		const Layer& layer = in.layers[li];
		const LayerSource source = li < meta.size() ? meta[li] : LayerSource{};

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

// Bakes @p composite (shapes in @p atlas) into one planar mesh. See BakedMesh for the layout.
// Sweep gradients are emitted as Radial paints with the raw (x, y) offset from the center (the
// consumer would need atan2); slughorn's SVG loaders never produce them.
inline BakedMesh bakeMesh(
	const Atlas& atlas,
	const CompositeShape& composite,
	const std::vector<LayerSource>& meta,
	const BakeConfig& cfg
) {
	BakedMesh mesh;

	mesh.tolerancePx = cfg.tolerancePx;

	const slug_t ppe = cfg.width; // pixels per em

	struct Region {
		size_t layer;
		clipper::Paths paths;
		bool opaque;
	};

	std::vector<Region> regions;
	const auto& gradients = atlas.getGradients();

	for(size_t li = 0; li < composite.layers.size(); li++) {
		const Layer& layer = composite.layers[li];

		if(layer.drawMode != DrawMode::Visible) continue;

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

	// Tile the canvas: every region is cut (Clipper2 RectClip, holes stay holes) into a grid of
	// tiles sized to ~PLANAR_TILE_VERTICES vertices, and planarization + triangulation run per
	// tile. Without tiling, a big merged region pays for every occluder above it anywhere on the
	// canvas, which is quadratic in practice (the station's text / speckle atlases). Clipping to
	// the canvas rectangle is also the canvas's own semantics (nothing draws outside it).
	struct Piece {
		size_t tile;
		clipper::Paths paths;
		detail::Box box;
	};

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

	std::vector<std::vector<Piece>> pieces(regions.size());

	for(size_t ri = 0; ri < regions.size(); ri++) {
		const detail::Box b = boxOf(regions[ri].paths);
		const int precision = clipper::precisionFor(regions[ri].paths);

		const auto i0 = static_cast<size_t>(std::clamp(std::floor(double(b.x0) / tw), 0.0, double(G - 1)));
		const auto i1 = static_cast<size_t>(std::clamp(std::floor(double(b.x1) / tw), 0.0, double(G - 1)));
		const auto j0 = static_cast<size_t>(std::clamp(std::floor(double(b.y0) / th), 0.0, double(G - 1)));
		const auto j1 = static_cast<size_t>(std::clamp(std::floor(double(b.y1) / th), 0.0, double(G - 1)));

		for(size_t j = j0; j <= j1; j++) {
			for(size_t i = i0; i <= i1; i++) {
				const Clipper2Lib::RectD rect(tw * double(i), th * double(j), tw * double(i + 1), th * double(j + 1));

				clipper::Paths piece = Clipper2Lib::RectClip(rect, regions[ri].paths, precision);

				if(piece.empty()) continue;

				const detail::Box pb = boxOf(piece);

				pieces[ri].push_back({j * G + i, std::move(piece), pb});
			}
		}
	}

	// Planarize, per tile: top -> bottom, subtract every opaque piece above. Pieces are
	// normalized (every covered point has winding exactly 1, holes included), so a plain
	// concatenation of several IS their union under the nonzero rule - no incremental union.
	if(cfg.planarize) {
		struct Occluder {
			clipper::Paths paths;
			detail::Box box;
		};

		std::vector<std::vector<Occluder>> occluders(G * G);

		for(size_t ri = regions.size(); ri-- > 0;) {
			for(Piece& pc : pieces[ri]) {
				auto& occ = occluders[pc.tile];
				clipper::Paths full = regions[ri].opaque ? pc.paths : clipper::Paths{};
				clipper::Paths clips;

				for(const auto& o : occ) {
					if(o.box.x0 <= pc.box.x1 && pc.box.x0 <= o.box.x1 && o.box.y0 <= pc.box.y1 && pc.box.y0 <= o.box.y1) {
						clips.insert(clips.end(), o.paths.begin(), o.paths.end());
					}
				}

				if(!clips.empty()) pc.paths = clipper::difference(pc.paths, clips);

				if(regions[ri].opaque) occ.push_back({std::move(full), pc.box});
			}
		}
	}

	std::vector<uint32_t> overlay;

	for(size_t ri = 0; ri < regions.size(); ri++) {
		const Region& r = regions[ri];

		// Triangulate this region's surviving pieces (tile by tile).
		tessellate::Mesh2D tri;

		for(const Piece& pc : pieces[ri]) {
			if(pc.paths.empty()) continue;

			const tessellate::Mesh2D m = detail::triangulateTiled(pc.paths);
			const auto off = static_cast<uint32_t>(tri.positions.size() / 2);

			tri.positions.insert(tri.positions.end(), m.positions.begin(), m.positions.end());

			for(uint32_t idx : m.indices) tri.indices.push_back(off + idx);
		}

		if(tri.indices.empty()) continue;

		const Layer& layer = composite.layers[r.layer];
		const LayerSource src = r.layer < meta.size() ? meta[r.layer] : LayerSource{};
		const auto shape = atlas.getShape(layer.key);

		const auto paintId = static_cast<uint16_t>(mesh.paints.size());

		mesh.paints.push_back(detail::paintOf(atlas, layer, src, cfg.opaqueAlpha));

		const GradientInfo* grad = (layer.gradientId > 0 && layer.gradientId <= gradients.size())
			? &gradients[layer.gradientId - 1]
			: nullptr
		;

		const auto base = static_cast<uint32_t>(mesh.positions.size() / 2);

		const slug_t s = layer.scale;
		const slug_t ox = (layer.transform.x - shape->originX) * s;
		const slug_t oy = (layer.transform.y - shape->originY) * s;

		for(size_t v = 0; v + 1 < tri.positions.size(); v += 2) {
			const slug_t x = tri.positions[v], y = tri.positions[v + 1];

			mesh.positions.push_back(static_cast<float>(x / cfg.width));
			mesh.positions.push_back(static_cast<float>(cfg.vUp ? 1_cv - y / cfg.height : y / cfg.height));
			mesh.paintIds.push_back(paintId);

			slug_t p0 = 0_cv, p1 = 0_cv;

			if(grad) {
				// Back to the layer's local em-space, where GradientInfo lives.
				const slug_t lx = (x / ppe - ox) / s;
				const slug_t ly = (y / ppe - oy) / s;
				const Matrix& m = grad->transform;

				switch(grad->type) {
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
						// Circular: normalize by (r1 - r0) so t = length(param) - r0/(r1-r0).
						const slug_t span = m.xx - grad->innerRadius;
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

		if(grad && grad->type == GradientInfo::Type::Radial) {
			const slug_t span = grad->transform.xx - grad->innerRadius;

			mesh.paints.back().innerRadius = span != 0_cv ? grad->innerRadius / span : 0_cv;
		}

		auto& dstIdx = (r.opaque && cfg.planarize) ? mesh.indices : overlay;

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

	// "mesh" | "slug" | "mean"
	std::string mode;
};

// Mode thresholds (see recommend()). Tunables, calibrated on the sakuragaoka-station keys.
//
// 1. "mesh" when the planar mesh is a card-sized triangle count: as cheap as any small prop,
//    exact at any distance up to the bake tolerance, no per-fragment curve work at all.
// 2. "slug" when the mesh would be big but the Slug shader's expected per-fragment work (mean
//    curve evaluations, quad overdraw included) and its quad count stay modest.
// 3. "mesh" again up to a hard triangle budget: heavy, but still cheaper than heavy Slug.
// 4. "mean": too heavy either way - draw the key's mean color (or a low-res raster) instead.
inline constexpr size_t MESH_TRIANGLES_MAX = 4096;
inline constexpr double SLUG_WORK_MEAN_MAX = 256.0;
inline constexpr size_t SLUG_LAYERS_MAX = 1024;
inline constexpr size_t MESH_TRIANGLES_HARD_MAX = 65536;

inline std::string recommend(const Cost& c) {
	if(c.trianglesAfter <= MESH_TRIANGLES_MAX) return "mesh";
	if(c.slugWorkMean <= SLUG_WORK_MEAN_MAX && c.layersAfter <= SLUG_LAYERS_MAX) return "slug";
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
	slug_t canvasEmH=0_cv
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

	for(const auto& layer : composite.layers) {
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
