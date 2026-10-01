#include "slughorn-python.hpp"

#include "slughorn/bake.hpp"

namespace slughorn_python {

void bind_bake(py::module_& bake) {
	using namespace slughorn::bake;

	py::class_<LayerSource>(bake, "LayerSource")
		.def(py::init([](slughorn::FillRule fillRule, bool stroke, uint8_t spread) {
			return LayerSource{fillRule, stroke, spread};
		}), "fill_rule"_a=slughorn::FillRule::NonZero, "stroke"_a=false, "spread"_a=uint8_t(0))
		.def_readwrite("fill_rule", &LayerSource::fillRule)
		.def_readwrite("stroke", &LayerSource::stroke)
		.def_readwrite("spread", &LayerSource::spread, "0 pad, 1 reflect, 2 repeat")
	;

	py::class_<MergeResult>(bake, "MergeResult")
		.def_readonly("composite", &MergeResult::composite)
		.def_readonly("layers", &MergeResult::layers)
		.def_readonly("layers_before", &MergeResult::layersBefore)
		.def_readonly("layers_after", &MergeResult::layersAfter)
		.def_readonly("curves_before", &MergeResult::curvesBefore)
		.def_readonly("curves_after", &MergeResult::curvesAfter)
	;

	py::enum_<Paint::Type>(bake, "PaintType")
		.value("Solid", Paint::Type::Solid)
		.value("Linear", Paint::Type::Linear)
		.value("Radial", Paint::Type::Radial)
	;

	py::class_<Paint>(bake, "Paint")
		.def_readonly("type", &Paint::type)
		.def_readonly("color", &Paint::color)
		.def_readonly("stops", &Paint::stops)
		.def_readonly("spread", &Paint::spread)
		.def_readonly("inner_radius", &Paint::innerRadius)
		.def_readonly("opaque", &Paint::opaque)
	;

	py::class_<BakeConfig>(bake, "BakeConfig")
		.def(py::init([](slug_t width, slug_t height, slug_t tolerancePx, bool planarize, bool vUp) {
			BakeConfig c;

			c.width = width;
			c.height = height;
			c.tolerancePx = tolerancePx;
			c.planarize = planarize;
			c.vUp = vUp;

			return c;
		}), "width"_a, "height"_a, "tolerance_px"_a=0.25_cv, "planarize"_a=true, "v_up"_a=false)
		.def_readwrite("v_up", &BakeConfig::vUp, "False: v = y/height (down); True: v = 1 - y/height (up).")
		.def_readwrite("width", &BakeConfig::width)
		.def_readwrite("height", &BakeConfig::height)
		.def_readwrite("tolerance_px", &BakeConfig::tolerancePx)
		.def_readwrite("opaque_alpha", &BakeConfig::opaqueAlpha)
		.def_readwrite("planarize", &BakeConfig::planarize)
		.def_readwrite("alpha_test", &BakeConfig::alphaTest, "> 0: cutout (alpha-tested) bake, see bake.hpp bakeCutout().")
	;

	py::class_<BakedMesh>(bake, "BakedMesh")
		.def_property_readonly("positions", [](const BakedMesh& m) {
			return flatView2D(m.positions, 2);
		}, "Zero-copy (N, 2) float32 view: UV in [0,1], v = 0 at the top edge (SVG Y-down).")
		.def_property_readonly("paint_ids", [](const BakedMesh& m) {
			return flatView2D(m.paintIds, 1);
		}, "Zero-copy (N, 1) uint16 view: per-vertex paint index.")
		.def_property_readonly("params", [](const BakedMesh& m) {
			return flatView2D(m.params, 2);
		}, "Zero-copy (N, 2) float32 view: linear (t, 0), radial (gx, gy), solid (0, 0).")
		.def_property_readonly("indices", [](const BakedMesh& m) {
			return flatView2D(m.indices, 3);
		}, "Zero-copy (T, 3) uint32 view; opaque triangles first, then the overlay range.")
		.def_readonly("opaque_index_count", &BakedMesh::opaqueIndexCount)
		.def_readonly("overlay_index_count", &BakedMesh::overlayIndexCount)
		.def_readonly("paints", &BakedMesh::paints)
		.def_readonly("triangles_before", &BakedMesh::trianglesBefore)
		.def_readonly("triangles_after", &BakedMesh::trianglesAfter)
		.def_readonly("stroke_layers", &BakedMesh::strokeLayers)
		.def_readonly("tolerance_px", &BakedMesh::tolerancePx)
		.def("__repr__", [](const BakedMesh& m) {
			return "BakedMesh(" + std::to_string(m.positions.size() / 2) + " vertices, " +
				std::to_string(m.trianglesAfter) + " triangles, " + std::to_string(m.paints.size()) + " paints)";
		})
	;

	py::class_<Cost>(bake, "Cost")
		.def_readonly("curves", &Cost::curves)
		.def_readonly("max_band_curves_h", &Cost::maxBandCurvesH)
		.def_readonly("max_band_curves_v", &Cost::maxBandCurvesV)
		.def_readonly("slug_work", &Cost::slugWork)
		.def_readonly("slug_work_mean", &Cost::slugWorkMean)
		.def_readonly("layers_before", &Cost::layersBefore)
		.def_readonly("layers_after", &Cost::layersAfter)
		.def_readonly("stroke_layers", &Cost::strokeLayers)
		.def_readonly("gradient_layers", &Cost::gradientLayers)
		.def_readonly("triangles_before", &Cost::trianglesBefore)
		.def_readonly("triangles_after", &Cost::trianglesAfter)
		.def_readonly("mode", &Cost::mode)
	;

	bake.def("merge_layers",
		[](
			const slughorn::Atlas& src,
			const slughorn::CompositeShape& composite,
			const std::vector<LayerSource>& meta,
			slughorn::Atlas& dst,
			slughorn::KeyIterator& keys,
			bool merge,
			size_t maxMergedCurves
		) {
			return mergeLayers(src, composite, meta, dst, keys, merge, maxMergedCurves);
		},
		"src"_a, "composite"_a, "meta"_a, "dst"_a, "keys"_a, "merge"_a=true,
		"max_merged_curves"_a=MAX_MERGED_CURVES,
		"Copy composite (shapes in src) into dst with keys from `keys`, merging same-solid-paint\n"
		"layers where painter's order allows. Returns a MergeResult."
	);

	bake.def("bake_mesh",
		[](const slughorn::Atlas& atlas, const slughorn::CompositeShape& composite, const std::vector<LayerSource>& meta, const BakeConfig& config) {
			return bakeMesh(atlas, composite, meta, config);
		},
		"atlas"_a, "composite"_a, "meta"_a, "config"_a,
		"Tessellate every layer's contours (fill-rule normalized) and planarize painter's order\n"
		"into one opaque triangle set plus an overlay range. Returns a BakedMesh."
	);

	bake.def("cost",
		[](const slughorn::Atlas& atlas, const slughorn::CompositeShape& composite, const BakedMesh& mesh, size_t layersBefore, slug_t w, slug_t h) {
			return cost(atlas, composite, mesh, layersBefore, w, h);
		},
		"atlas"_a, "composite"_a, "mesh"_a, "layers_before"_a, "canvas_em_w"_a=0_cv, "canvas_em_h"_a=0_cv,
		"Curve/band statistics (atlas must be built) + triangle counts + recommended mode."
	);

	bake.attr("MAX_MERGED_CURVES") = MAX_MERGED_CURVES;
	bake.attr("MESH_TRIANGLES_MAX") = MESH_TRIANGLES_MAX;
	bake.attr("SLUG_WORK_MEAN_MAX") = SLUG_WORK_MEAN_MAX;
	bake.attr("SLUG_LAYERS_MAX") = SLUG_LAYERS_MAX;
	bake.attr("MESH_TRIANGLES_HARD_MAX") = MESH_TRIANGLES_HARD_MAX;
}

}
