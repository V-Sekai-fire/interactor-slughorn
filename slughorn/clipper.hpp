#pragma once

// ================================================================================================
// clipper.hpp - polygon boolean helpers for slughorn curves (Clipper2-backed)
//
// slughorn stores shapes as closed loops of quadratic Beziers rendered with a nonzero winding
// rule. A few consumers need real region algebra on top of that:
//
// - fill-rule normalization: a contour set plus its authoring fill rule (nonzero / even-odd) is
//   reduced to a clean, non-self-intersecting, consistently-oriented polygon set. This is how
//   tessellation stops trusting signed area / authored winding (mis-wound font contours, SVG
//   even-odd paths) - see tessellate::tessellate(contours, tolerance, FillRule).
//
// - clip paths: SVG clipPath geometry intersected into a layer (slughorn/thorvg.hpp).
//
// - planarization: subtracting the coverage of higher layers from lower ones so a baked mesh is a
//   single non-overlapping triangle set (slughorn/bake.hpp).
//
// Curves are flattened to polylines (the same midpoint subdivision tessellate.hpp and canvas.hpp
// use), so every result is piecewise linear at the requested tolerance. Converting back to
// slughorn curves (toCurves) emits line segments.
//
// Requires SLUGHORN_CLIPPER2=ON (compiles the vendored ext/clipper2 sources into slughorn).
// ================================================================================================

#include "slughorn.hpp"

#if defined(_MSC_VER)
// C4702 (unreachable code) fires from Clipper2's inline templates at codegen time and is
// attributed to the including TU regardless of /external:W0 - same situation as nanosvg.hpp.
__pragma(warning(disable: 4702))
#endif

// Clipper2 1.5.4's clipper.core.h uses std::back_inserter without including <iterator>; MSVC's STL
// pulls it in transitively, libstdc++ / libc++ do not.
#include <iterator>

#include <clipper2/clipper.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace slughorn {
namespace clipper {

using Point = Clipper2Lib::PointD;
using Path = Clipper2Lib::PathD;
using Paths = Clipper2Lib::PathsD;

inline Clipper2Lib::FillRule toClipper(FillRule rule) {
	return rule == FillRule::EvenOdd ? Clipper2Lib::FillRule::EvenOdd : Clipper2Lib::FillRule::NonZero;
}

// Decimal places Clipper2 keeps when it scales PathsD into its int64 domain. Chosen from the
// coordinate magnitude so em-space input ([0, 1]) keeps 1e-8 resolution and pixel-space input
// (thousands) keeps 1e-4, both far inside Clipper2's int64 range.
inline int precisionFor(const Paths& paths) {
	double maxAbs = 1.0;

	for(const auto& p : paths) for(const auto& pt : p) {
		maxAbs = std::max({maxAbs, std::abs(pt.x), std::abs(pt.y)});
	}

	const int digits = static_cast<int>(std::ceil(std::log10(maxAbs)));

	return std::clamp(8 - digits, 2, 8);
}

inline int precisionFor(const Paths& a, const Paths& b) {
	return std::min(precisionFor(a), precisionFor(b));
}

namespace detail {

inline void flattenQuad(
	double p0x, double p0y,
	double p1x, double p1y,
	double p2x, double p2y,
	double tol, size_t depth,
	Path& out
) {
	const double dx = p2x - p0x, dy = p2y - p0y;
	const double lenSq = dx * dx + dy * dy;
	const double cross = (p1x - p0x) * dy - (p1y - p0y) * dx;

	if(lenSq < 1e-24 || (cross * cross) <= (tol * tol * lenSq) || depth >= 10) {
		out.push_back({p2x, p2y});

		return;
	}

	const double m01x = (p0x + p1x) * 0.5, m01y = (p0y + p1y) * 0.5;
	const double m12x = (p1x + p2x) * 0.5, m12y = (p1y + p2y) * 0.5;
	const double mx = (m01x + m12x) * 0.5, my = (m01y + m12y) * 0.5;

	flattenQuad(p0x, p0y, m01x, m01y, mx, my, tol, depth + 1, out);
	flattenQuad(mx, my, m12x, m12y, p2x, p2y, tol, depth + 1, out);
}

}

// Splits a flat curve list into contours, using explicit subpath starts when known (index 0
// explicit, the ShapeInfo::contourStarts convention) and otherwise the same coordinate-gap
// heuristic Atlas::getShapeContours() falls back to.
inline Atlas::Contours splitContours(const Atlas::Curves& curves, const std::vector<size_t>& starts={}) {
	Atlas::Contours out;

	if(curves.empty()) return out;

	if(!starts.empty()) {
		for(size_t i = 0; i < starts.size(); i++) {
			const size_t b = starts[i];
			const size_t e = (i + 1 < starts.size()) ? starts[i + 1] : curves.size();

			if(b >= e || b >= curves.size()) continue;

			out.emplace_back(
				curves.begin() + static_cast<std::ptrdiff_t>(b),
				curves.begin() + static_cast<std::ptrdiff_t>(std::min(e, curves.size()))
			);
		}

		return out;
	}

	Atlas::Curves cur;

	for(size_t i = 0; i < curves.size(); i++) {
		cur.push_back(curves[i]);

		const bool last = i + 1 == curves.size();

		if(last || std::abs(curves[i].x3 - curves[i + 1].x1) > 1e-6_cv || std::abs(curves[i].y3 - curves[i + 1].y1) > 1e-6_cv) {
			out.push_back(std::move(cur));
			cur.clear();
		}
	}

	return out;
}

// Flattens contours into closed polylines. Each control point is first mapped through @p xf
// (affine maps commute with Bezier evaluation, so this is exact), then flattened in the OUTPUT
// space, so @p tolerance is a chord error in output units.
// True when @p paths is exactly one axis-aligned rectangle (4 corners, any orientation); its
// extent goes to x0..y1.
inline bool isAxisRect(const Paths& paths, double& x0, double& y0, double& x1, double& y1) {
	if(paths.size() != 1 || paths[0].size() != 4) return false;

	const Path& p = paths[0];

	x0 = x1 = p[0].x;
	y0 = y1 = p[0].y;

	for(const auto& pt : p) {
		x0 = std::min(x0, pt.x); x1 = std::max(x1, pt.x);
		y0 = std::min(y0, pt.y); y1 = std::max(y1, pt.y);
	}

	for(const auto& pt : p) {
		if((pt.x != x0 && pt.x != x1) || (pt.y != y0 && pt.y != y1)) return false;
	}

	return x1 > x0 && y1 > y0;
}

inline Paths toPaths(const Atlas::Contours& contours, slug_t tolerance, const Matrix& xf=Matrix::identity()) {
	Paths paths;

	paths.reserve(contours.size());

	const double tol = std::max(1e-9, static_cast<double>(tolerance));

	for(const auto& contour : contours) {
		if(contour.empty()) continue;

		Path p;

		auto map = [&](slug_t x, slug_t y) {
			slug_t ox, oy;

			xf.apply(x, y, ox, oy);

			return Point{static_cast<double>(ox), static_cast<double>(oy)};
		};

		const Point start = map(contour.front().x1, contour.front().y1);

		p.push_back(start);

		for(const auto& c : contour) {
			const Point a = map(c.x1, c.y1);
			const Point b = map(c.x2, c.y2);
			const Point d = map(c.x3, c.y3);

			detail::flattenQuad(a.x, a.y, b.x, b.y, d.x, d.y, tol, 0, p);
		}

		if(p.size() > 1 && std::abs(p.front().x - p.back().x) < 1e-12 && std::abs(p.front().y - p.back().y) < 1e-12) {
			p.pop_back();
		}

		if(p.size() >= 3) paths.push_back(std::move(p));
	}

	return paths;
}

// Resolves @p paths under @p rule into a clean region: no self-intersections, no overlaps, outer
// boundaries and holes consistently oriented (so the result is correct under BOTH nonzero and
// even-odd from here on). Whatever winding the input had is irrelevant afterwards.
inline Paths normalize(const Paths& paths, FillRule rule) {
	if(paths.empty()) return {};

	return Clipper2Lib::Union(paths, toClipper(rule), precisionFor(paths));
}

// Boolean ops on already-normalized regions.
inline Paths unite(const Paths& a, const Paths& b) {
	if(a.empty()) return b;
	if(b.empty()) return a;

	return Clipper2Lib::Union(a, b, Clipper2Lib::FillRule::NonZero, precisionFor(a, b));
}

inline Paths intersect(const Paths& a, const Paths& b) {
	if(a.empty() || b.empty()) return {};

	return Clipper2Lib::Intersect(a, b, Clipper2Lib::FillRule::NonZero, precisionFor(a, b));
}

inline Paths difference(const Paths& a, const Paths& b) {
	if(a.empty() || b.empty()) return a;

	return Clipper2Lib::Difference(a, b, Clipper2Lib::FillRule::NonZero, precisionFor(a, b));
}

// Signed area sum (holes negative once normalized).
inline double area(const Paths& paths) {
	double a = 0.0;

	for(const auto& p : paths) a += Clipper2Lib::Area(p);

	return a;
}

// The connected components of a normalized region: each an outer ring followed by its holes
// (islands inside holes are components of their own).
inline std::vector<Paths> components(const Paths& region) {
	std::vector<Paths> out;

	if(region.empty()) return out;

	Clipper2Lib::ClipperD c(precisionFor(region));

	c.AddSubject(region);

	Clipper2Lib::PolyTreeD tree;

	c.Execute(Clipper2Lib::ClipType::Union, Clipper2Lib::FillRule::NonZero, tree);

	std::vector<const Clipper2Lib::PolyPathD*> todo;

	for(const auto& outer : tree) todo.push_back(outer.get());

	while(!todo.empty()) {
		const Clipper2Lib::PolyPathD* outer = todo.back();

		todo.pop_back();

		Paths comp{outer->Polygon()};

		for(const auto& hole : *outer) {
			comp.push_back(hole->Polygon());

			for(const auto& island : *hole) todo.push_back(island.get());
		}

		out.push_back(std::move(comp));
	}

	return out;
}

inline double perimeter(const Path& p) {
	double l = 0.0;

	for(size_t i = 0; i < p.size(); i++) {
		const auto& a = p[i];
		const auto& b = p[(i + 1) % p.size()];

		l += std::hypot(b.x - a.x, b.y - a.y);
	}

	return l;
}

// Area centroid of one ring (its vertex mean when degenerate).
inline void centroid(const Path& p, double& cx, double& cy) {
	double a = 0.0, x = 0.0, y = 0.0;

	for(size_t i = 0; i < p.size(); i++) {
		const auto& u = p[i];
		const auto& v = p[(i + 1) % p.size()];
		const double w = u.x * v.y - v.x * u.y;

		a += w;
		x += (u.x + v.x) * w;
		y += (u.y + v.y) * w;
	}

	if(std::abs(a) > 1e-12) { cx = x / (3.0 * a); cy = y / (3.0 * a); return; }

	cx = cy = 0.0;

	for(const auto& u : p) { cx += u.x; cy += u.y; }

	if(!p.empty()) { cx /= double(p.size()); cy /= double(p.size()); }
}

// Nonzero containment of a point in a normalized region (outer rings positive, holes negative).
inline bool contains(const Paths& region, double x, double y) {
	int winding = 0;
	const Clipper2Lib::PointD pt(x, y);

	for(const auto& p : region) {
		if(Clipper2Lib::PointInPolygon(pt, p) == Clipper2Lib::PointInPolygonResult::IsOutside) continue;

		winding += Clipper2Lib::Area(p) > 0 ? 1 : -1;
	}

	return winding > 0;
}

inline size_t vertexCount(const Paths& paths) {
	size_t n = 0;

	for(const auto& p : paths) n += p.size();

	return n;
}

// Converts polylines back into slughorn line curves (closed loops), mapping each vertex through
// @p xf. Writes subpath starts (index 0 explicit) to @p starts when non-null. Orientation is
// kept as-is: normalized output (outer positive, holes negative) renders correctly under the
// nonzero Slug shader.
inline Atlas::Curves toCurves(const Paths& paths, std::vector<size_t>* starts=nullptr, const Matrix& xf=Matrix::identity()) {
	Atlas::Curves curves;
	CurveDecomposer dec(curves);

	for(const auto& p : paths) {
		if(p.size() < 3) continue;

		if(starts) starts->push_back(curves.size());

		slug_t x, y;

		xf.apply(static_cast<slug_t>(p[0].x), static_cast<slug_t>(p[0].y), x, y);
		dec.moveTo(x, y);

		for(size_t i = 1; i < p.size(); i++) {
			xf.apply(static_cast<slug_t>(p[i].x), static_cast<slug_t>(p[i].y), x, y);

			// Clipper2 never emits consecutive duplicates at its own precision, but float
			// narrowing can collapse two very close vertices; skip those zero-length segments.
			if(std::abs(x - dec._x) < 1e-9_cv && std::abs(y - dec._y) < 1e-9_cv) continue;

			dec.lineTo(x, y);
		}

		dec.close();
	}

	return curves;
}

}
}
