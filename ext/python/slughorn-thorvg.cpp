#include "slughorn-python.hpp"

#include "slughorn/thorvg.hpp"

namespace slughorn_python {

void bind_thorvg(py::module_& thorvg) {
	py::enum_<slughorn::thorvg::ShapePolicy>(thorvg, "ShapePolicy")
		.value("Default", slughorn::thorvg::ShapePolicy::Default)
		.value("ForceInclude", slughorn::thorvg::ShapePolicy::ForceInclude)
		.value("ForceExclude", slughorn::thorvg::ShapePolicy::ForceExclude)
		.value("GeometryOnly", slughorn::thorvg::ShapePolicy::GeometryOnly)
		.def("__or__", [](
			slughorn::thorvg::ShapePolicy a,
			slughorn::thorvg::ShapePolicy b
		) { return a | b; })
		.def("__ror__", [](
			slughorn::thorvg::ShapePolicy a,
			slughorn::thorvg::ShapePolicy b
		) { return a | b; })
	;

	py::enum_<slughorn::thorvg::Spread>(thorvg, "Spread")
		.value("Pad", slughorn::thorvg::Spread::Pad)
		.value("Reflect", slughorn::thorvg::Spread::Reflect)
		.value("Repeat", slughorn::thorvg::Spread::Repeat)
	;

	py::class_<slughorn::thorvg::ShapeRule>(thorvg, "ShapeRule")
		.def(py::init([](
			const std::string& pattern,
			slughorn::thorvg::ShapePolicy policy,
			std::optional<slughorn::Atlas::ShapeInfo::Origin> origin
		) {
			return slughorn::thorvg::ShapeRule{std::regex(pattern), policy, origin};
		}),
		"id"_a,
		"policy"_a=slughorn::thorvg::ShapePolicy::Default,
		"origin"_a=py::none(),
		"id is a regex matched against each SVG element's id attribute (\"\" when it has none).\n"
		"policy controls whether matched shapes are force-included, excluded,\n"
		"or stored as geometry-only (curves in atlas, layer drawn as DrawMode.Geometry).\n"
		"origin overrides LoadConfig.origin for matched shapes (None = inherit).");

	py::class_<slughorn::thorvg::LayerInfo>(thorvg, "LayerInfo")
		.def(py::init<>())
		.def_readwrite("id", &slughorn::thorvg::LayerInfo::id,
			"SVG id of the source element (\"\" if none)."
		)
		.def_readwrite("fill_rule", &slughorn::thorvg::LayerInfo::fillRule,
			"Authored fill rule (stroke layers: NonZero). Atlas curves are already nonzero."
		)
		.def_readwrite("stroke", &slughorn::thorvg::LayerInfo::stroke,
			"True when this layer is a stroke expanded to fill contours."
		)
		.def_readwrite("clipped", &slughorn::thorvg::LayerInfo::clipped,
			"True when a clip path / mask was intersected into the geometry."
		)
		.def_readwrite("spread", &slughorn::thorvg::LayerInfo::spread,
			"Gradient spread as authored (Slug renders pad)."
		)
		.def_readwrite("opacity", &slughorn::thorvg::LayerInfo::opacity,
			"Accumulated paint opacity, already folded into the color / stop alphas."
		)
		.def("__repr__", [](const slughorn::thorvg::LayerInfo& l) {
			return "LayerInfo(id='" + l.id + "', stroke=" + (l.stroke ? "True" : "False") +
				", clipped=" + (l.clipped ? "True" : "False") +
				", fill_rule=" + (l.fillRule == slughorn::FillRule::EvenOdd ? "EvenOdd" : "NonZero") + ")";
		})
	;

	py::class_<slughorn::thorvg::LoadConfig>(thorvg, "LoadConfig")
		.def(py::init([](py::kwargs kwargs) {
			slughorn::thorvg::LoadConfig config;

			for(auto item : kwargs) {
				auto key = item.first.cast<std::string>();

				if(key == "log") config.log = item.second.cast<slughorn::thorvg::LogCallback>();
				else if(key == "rules") {
					config.rules = item.second.cast<std::vector<slughorn::thorvg::ShapeRule>>();
				}
				else if(key == "auto_metrics") config.autoMetrics = item.second.cast<bool>();
				else if(key == "origin") {
					config.origin = item.second.cast<slughorn::Atlas::ShapeInfo::Origin>();
				}
				else if(key == "strokes") config.strokes = item.second.cast<bool>();
				else if(key == "tolerance") config.tolerance = item.second.cast<slug_t>();
				else if(key == "stroke_tolerance_px") config.strokeTolerancePx = item.second.cast<slug_t>();
				else if(key == "clip_tolerance_px") config.clipTolerancePx = item.second.cast<slug_t>();
				else if(key == "width") config.width = item.second.cast<slug_t>();
				else if(key == "height") config.height = item.second.cast<slug_t>();
				else if(key == "height_em") config.heightEm = item.second.cast<slug_t>();
				else throw py::type_error("LoadConfig got an unexpected keyword argument '" + key + "'");
			}

			return config;
		}),
		"Construct with optional field=value kwargs, e.g. LoadConfig(auto_metrics=False)."
		)
		.def_readwrite("log", &slughorn::thorvg::LoadConfig::log,
			"Optional callable(level: int, msg: str) for load-time diagnostics; "
			"omit (None) to print warnings/errors to stderr."
		)
		.def_readwrite("rules", &slughorn::thorvg::LoadConfig::rules,
			"List of ShapeRule objects applied in order; first match wins."
		)
		.def_readwrite("auto_metrics", &slughorn::thorvg::LoadConfig::autoMetrics,
			"If True (default), curves are shifted to local origin and shape metrics\n"
			"are derived from the curve bbox; layer.transform.x/y carries the offset\n"
			"(multiply by picture width to recover authoring coords). If False,\n"
			"curves are stored as-is in SVG canvas space and layer.transform is zero."
		)
		.def_readwrite("origin", &slughorn::thorvg::LoadConfig::origin,
			"Global origin for all shapes (overridden per-shape by ShapeRule.origin)."
		)
		.def_readwrite("strokes", &slughorn::thorvg::LoadConfig::strokes,
			"Expand strokes into fill layers (default True)."
		)
		.def_readwrite("tolerance", &slughorn::thorvg::LoadConfig::tolerance,
			"CurveDecomposer tolerance (em) for path cubics; the default TOLERANCE_EXACT emits\n"
			"two quadratics per cubic, matching the NanoSVG backend."
		)
		.def_readwrite("stroke_tolerance_px", &slughorn::thorvg::LoadConfig::strokeTolerancePx,
			"Centerline flattening tolerance for stroke expansion, in authoring pixels."
		)
		.def_readwrite("clip_tolerance_px", &slughorn::thorvg::LoadConfig::clipTolerancePx,
			"Flattening tolerance for clip-path / mask booleans, in authoring pixels."
		)
		.def_readwrite("width", &slughorn::thorvg::LoadConfig::width,
			"Output: picture width (SVG pixels), populated by load_file/load_string."
		)
		.def_readwrite("height", &slughorn::thorvg::LoadConfig::height,
			"Output: picture height (SVG pixels), populated by load_file/load_string."
		)
		.def_readwrite("height_em", &slughorn::thorvg::LoadConfig::heightEm,
			"Output: SVG viewport height in em-space, populated by load_file/load_string."
		)
		.def_readwrite("layers", &slughorn::thorvg::LoadConfig::layers,
			"Output: list of LayerInfo, parallel to the returned composite's layers."
		)
		.def("__repr__", [](const slughorn::thorvg::LoadConfig& c) { return streamRepr(c); })
	;

	thorvg.def("load_file",
		&slughorn::thorvg::loadFile,
		"path"_a,
		"atlas"_a,
		"keys"_a=slughorn::KeyIterator(),
		"dpi"_a=96_cv,
		"config"_a=nullptr,
		"Parse an SVG file with ThorVG and pack every painted shape into atlas.\n"
		"keys is advanced in-place; pass the same KeyIterator to subsequent calls\n"
		"to pack multiple SVGs into the same atlas without key collisions.\n"
		"dpi is accepted for NanoSVG API parity and ignored (ThorVG resolves units).\n"
		"config: optional LoadConfig; its output fields (width, height, height_em, layers)\n"
		"    are populated in place on return."
	);

	thorvg.def("load_string",
		&slughorn::thorvg::loadString,
		"svg"_a,
		"atlas"_a,
		"keys"_a=slughorn::KeyIterator(),
		"dpi"_a=96_cv,
		"config"_a=nullptr,
		"Parse an SVG string with ThorVG and pack every painted shape into atlas.\n"
		"keys is advanced in-place; pass the same KeyIterator to subsequent calls\n"
		"to pack multiple SVGs into the same atlas without key collisions.\n"
		"dpi is accepted for NanoSVG API parity and ignored (ThorVG resolves units).\n"
		"config: optional LoadConfig; its output fields (width, height, height_em, layers)\n"
		"    are populated in place on return."
	);
}

}
