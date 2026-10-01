"""
Tests for slughorn/thorvg.hpp — slughorn.thorvg submodule.

Mirrors test_nanosvg.py (same inline SVGs, same expectations) and adds:

- parity: the same SVGs loaded with slughorn.nanosvg and slughorn.thorvg, rendered through the
  CPU Slug decoder (slughorn.render.render_composite), must agree within a tolerance; a shifted
  shape must NOT (negative control, so the comparison can actually fail);
- ThorVG-only features (strokes, clip paths, layer provenance);
- fill-rule-robust tessellation of a mis-wound glyph-like contour;
- slughorn.bake merge / planar mesh / cost.

All tests are skipped if slughorn.thorvg is not compiled in.
"""

import pathlib
import pytest
import slughorn
from conftest import requires_thorvg, requires_nanosvg, requires_bake, requires_tessellate

pytestmark = requires_thorvg()

_SVG_DIR = pathlib.Path(__file__).resolve().parent.parent

# ---------------------------------------------------------------------------
# Minimal inline SVGs (identical to test_nanosvg.py)
# ---------------------------------------------------------------------------

_SVG_ONE_SOLID = """\
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
  <rect x="10" y="10" width="80" height="80" fill="#ff0000"/>
</svg>
"""

_SVG_TWO_SOLIDS = """\
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 100">
  <rect x="0"   y="0" width="100" height="100" fill="#00ff00"/>
  <rect x="100" y="0" width="100" height="100" fill="#0000ff"/>
</svg>
"""

_SVG_EMPTY = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100"/>'

_SVG_WITH_IDS = """\
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 300 100">
  <rect id="keep"    x="0"   y="0" width="100" height="100" fill="#ff0000"/>
  <rect id="exclude" x="100" y="0" width="100" height="100" fill="#00ff00"/>
  <rect id="geo"     x="200" y="0" width="100" height="100" fill="#0000ff"/>
</svg>
"""

# Canvas-recording subset: rect, circle, ellipse, cubic path, fill-opacity, transform matrix,
# userSpaceOnUse linear + radial gradients, even-odd path.
_SVG_PARITY = """\
<svg xmlns="http://www.w3.org/2000/svg" width="256" height="128" viewBox="0 0 256 128">
  <defs>
    <linearGradient id="lin" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="256" y2="0">
      <stop offset="0" stop-color="#102030"/>
      <stop offset="1" stop-color="#405060"/>
    </linearGradient>
    <radialGradient id="rad" gradientUnits="userSpaceOnUse" cx="200" cy="90" r="30">
      <stop offset="0" stop-color="#ffffff"/>
      <stop offset="1" stop-color="#ff00ff"/>
    </radialGradient>
  </defs>
  <rect x="0" y="0" width="256" height="128" fill="url(#lin)"/>
  <rect x="8" y="8" width="60" height="40" fill="#e0c020"/>
  <circle cx="120" cy="40" r="30" fill="#30a0e0" fill-opacity="0.6"/>
  <ellipse cx="200" cy="90" rx="40" ry="30" fill="url(#rad)"/>
  <g transform="matrix(0.8,0.3,-0.3,0.8,60,70)">
    <rect x="0" y="0" width="50" height="30" fill="#80ff80"/>
  </g>
  <path fill-rule="evenodd" fill="#ff8000" d="M 100 80 h 40 v 40 h -40 Z M 110 90 h 20 v 20 h -20 Z"/>
</svg>
"""

_SVG_STROKE_CLIP = """\
<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200" viewBox="0 0 200 200">
  <defs>
    <clipPath id="c"><circle cx="100" cy="100" r="50"/></clipPath>
  </defs>
  <rect id="clipped" x="0" y="0" width="200" height="200" fill="#ff0000" clip-path="url(#c)"/>
  <line id="ln" x1="10" y1="190" x2="190" y2="190" stroke="#000000" stroke-width="8" stroke-dasharray="10 10"/>
</svg>
"""


def _np():
	return pytest.importorskip("numpy")


def _render(atlas, composite, height_em, w=128):
	np = _np()
	h = int(round(w * height_em))
	img = slughorn.render.render_composite(atlas, composite, w, h, 0.0, 0.0, 1.0, height_em)
	return np.asarray(img).copy()


def _quiet():
	cfg = slughorn.thorvg.LoadConfig()
	cfg.log = lambda level, msg: None
	return cfg


# ---------------------------------------------------------------------------
# Module / class availability
# ---------------------------------------------------------------------------

def test_thorvg_submodule_exists():
	assert hasattr(slughorn, "thorvg")

def test_shape_policy_enum_exists():
	assert hasattr(slughorn.thorvg, "ShapePolicy")

def test_shape_rule_class_exists():
	assert hasattr(slughorn.thorvg, "ShapeRule")

def test_load_string_function_exists():
	assert hasattr(slughorn.thorvg, "load_string")

def test_load_file_function_exists():
	assert hasattr(slughorn.thorvg, "load_file")


# ---------------------------------------------------------------------------
# ShapePolicy / ShapeRule
# ---------------------------------------------------------------------------

def test_shape_policy_values():
	sp = slughorn.thorvg.ShapePolicy
	assert hasattr(sp, "Default")
	assert hasattr(sp, "ForceInclude")
	assert hasattr(sp, "ForceExclude")
	assert hasattr(sp, "GeometryOnly")

def test_shape_policy_bitwise_or():
	sp = slughorn.thorvg.ShapePolicy
	assert (sp.ForceInclude | sp.GeometryOnly) is not None
	assert (sp.GeometryOnly | sp.ForceInclude) is not None

def test_shape_rule_default_policy():
	assert slughorn.thorvg.ShapeRule(".*") is not None


# ---------------------------------------------------------------------------
# load_string / load_file — basic
# ---------------------------------------------------------------------------

def test_load_string_single_solid(atlas):
	assert len(slughorn.thorvg.load_string(_SVG_ONE_SOLID, atlas)) == 1

def test_load_string_two_solids(atlas):
	assert len(slughorn.thorvg.load_string(_SVG_TWO_SOLIDS, atlas)) == 2

def test_load_string_empty_svg(atlas):
	assert len(slughorn.thorvg.load_string(_SVG_EMPTY, atlas)) == 0

def test_load_string_invalid_xml(atlas):
	composite = slughorn.thorvg.load_string("this is not xml", atlas, config=_quiet())
	assert len(composite) == 0

def test_load_file_bad_path(atlas):
	messages = []
	cfg = slughorn.thorvg.LoadConfig()
	cfg.log = lambda level, msg: messages.append((level, msg))
	composite = slughorn.thorvg.load_file("does/not/exist.svg", atlas, config=cfg)
	assert len(composite) == 0
	assert messages and messages[0][0] == 2

def test_load_file_gradient(atlas):
	svg_path = _SVG_DIR / "gradient-test.svg"
	if not svg_path.exists():
		pytest.skip(f"test SVG not found: {svg_path}")
	assert len(slughorn.thorvg.load_file(str(svg_path), atlas)) == 3

def test_load_file_output_metrics(atlas, tmp_path):
	svg_path = tmp_path / "square.svg"
	svg_path.write_text(_SVG_ONE_SOLID)
	config = slughorn.thorvg.LoadConfig()
	slughorn.thorvg.load_file(str(svg_path), atlas, config=config)
	assert config.width == pytest.approx(100.0, abs=1e-3)
	assert config.height == pytest.approx(100.0, abs=1e-3)
	assert config.height_em == pytest.approx(1.0, abs=1e-3)
	assert len(config.layers) == 1


# ---------------------------------------------------------------------------
# Keys / colors / rules
# ---------------------------------------------------------------------------

def test_load_string_chained_keys_unique(atlas):
	keys = slughorn.KeyIterator("svg")
	a = slughorn.thorvg.load_string(_SVG_TWO_SOLIDS, atlas, keys)
	b = slughorn.thorvg.load_string(_SVG_TWO_SOLIDS, atlas, keys)
	all_keys = [l.key for l in a.layers] + [l.key for l in b.layers]
	assert len(set(all_keys)) == 4

def test_layer_color_green(atlas):
	composite = slughorn.thorvg.load_string(_SVG_TWO_SOLIDS, atlas)
	c = composite.layers[0].color
	assert (c.r, c.g, c.b, c.a) == pytest.approx((0.0, 1.0, 0.0, 1.0), abs=1e-3)

def test_ids_become_keys(atlas):
	composite = slughorn.thorvg.load_string(_SVG_WITH_IDS, atlas)
	assert [l.key for l in composite.layers] == [slughorn.Key("keep"), slughorn.Key("exclude"), slughorn.Key("geo")]

def test_shape_rule_force_exclude(atlas):
	sp = slughorn.thorvg.ShapePolicy
	cfg = slughorn.thorvg.LoadConfig(rules=[slughorn.thorvg.ShapeRule("exclude", sp.ForceExclude)])
	composite = slughorn.thorvg.load_string(_SVG_WITH_IDS, atlas, config=cfg)
	assert len(composite) == 2

def test_shape_rule_geometry_only(atlas):
	sp = slughorn.thorvg.ShapePolicy
	cfg = slughorn.thorvg.LoadConfig(rules=[slughorn.thorvg.ShapeRule("geo", sp.GeometryOnly)])
	composite = slughorn.thorvg.load_string(_SVG_WITH_IDS, atlas, config=cfg)
	geo = next(l for l in composite.layers if l.key == slughorn.Key("geo"))
	assert geo.drawMode == slughorn.DrawMode.Geometry

def test_shape_rule_origin_centered(atlas):
	sp = slughorn.thorvg.ShapePolicy
	origin = slughorn.ShapeInfo.Origin(slughorn.ShapeInfo.Origin.Type.Centered)
	cfg = slughorn.thorvg.LoadConfig(rules=[slughorn.thorvg.ShapeRule("keep", sp.Default, origin)])
	composite = slughorn.thorvg.load_string(_SVG_WITH_IDS, atlas, config=cfg)
	keep = next(l for l in composite.layers if l.key == slughorn.Key("keep"))
	assert keep.transform.x == pytest.approx(50.0 / 300.0, abs=1e-3)
	assert keep.transform.y == pytest.approx(50.0 / 300.0, abs=1e-3)

def test_auto_metrics_false_transform_zero(atlas):
	cfg = slughorn.thorvg.LoadConfig(auto_metrics=False)
	composite = slughorn.thorvg.load_string(_SVG_ONE_SOLID, atlas, config=cfg)
	assert composite.layers[0].transform.x == pytest.approx(0.0)
	assert composite.layers[0].transform.y == pytest.approx(0.0)


# ---------------------------------------------------------------------------
# ThorVG-only features: strokes, dashes, clip paths, provenance
# ---------------------------------------------------------------------------

def test_stroke_and_clip_layers(atlas):
	cfg = _quiet()
	composite = slughorn.thorvg.load_string(_SVG_STROKE_CLIP, atlas, config=cfg)
	assert len(composite) == len(cfg.layers) == 2
	clipped, line = cfg.layers
	assert clipped.id == "clipped"
	assert line.stroke and line.id == "ln"
	assert composite.layers[1].key == slughorn.Key("ln.stroke")
	if hasattr(slughorn, "bake"):  # Clipper2 compiled in -> clip applied geometrically
		assert clipped.clipped
		atlas.build()
		shape = atlas.get_shape(slughorn.Key("clipped"))
		# The 200x200 rect is clipped to the r=50 circle: 100/200 em wide.
		assert shape.width == pytest.approx(0.5, abs=0.01)

def test_strokes_disabled(atlas):
	cfg = _quiet()
	cfg.strokes = False
	composite = slughorn.thorvg.load_string(_SVG_STROKE_CLIP, atlas, config=cfg)
	assert all(not l.stroke for l in cfg.layers)
	assert len(composite) == 1

def test_dashed_stroke_coverage(atlas):
	np = _np()
	cfg = _quiet()
	composite = slughorn.thorvg.load_string(_SVG_STROKE_CLIP, atlas, config=cfg)
	atlas.build()
	img = _render(atlas, slughorn.CompositeShape(), 1.0, 8)  # empty composite renders blank
	assert float(img[..., 3].max()) == 0.0
	line_only = slughorn.CompositeShape()
	line_only.layers.append(composite.layers[1])
	img = _render(atlas, line_only, 1.0, 200)
	row = img[190, 10:190, 3]
	# 10-on / 10-off dashes: about half of the row is covered.
	assert 0.4 < float((row > 0.5).mean()) < 0.6


# ---------------------------------------------------------------------------
# Parity with the NanoSVG backend (the check that can fail)
# ---------------------------------------------------------------------------

def _load_both(svg):
	ta, na = slughorn.Atlas(), slughorn.Atlas()
	tcfg, ncfg = slughorn.thorvg.LoadConfig(), slughorn.nanosvg.LoadConfig()
	tc = slughorn.thorvg.load_string(svg, ta, slughorn.KeyIterator("t"), config=tcfg)
	nc = slughorn.nanosvg.load_string(svg, na, slughorn.KeyIterator("n"), config=ncfg)
	ta.build()
	na.build()
	return (ta, tc, tcfg), (na, nc, ncfg)

def _diff(a, b):
	np = _np()
	d = np.abs(a - b)
	return float(d.mean()), float((d.max(axis=-1) > 0.25).mean())

@requires_nanosvg()
@pytest.mark.parametrize("svg", [_SVG_ONE_SOLID, _SVG_TWO_SOLIDS, _SVG_WITH_IDS, _SVG_PARITY])
def test_parity_with_nanosvg(svg):
	(ta, tc, tcfg), (na, nc, ncfg) = _load_both(svg)
	assert len(tc) == len(nc)
	assert tcfg.height_em == pytest.approx(ncfg.height_em, abs=1e-6)
	mean, bad = _diff(_render(ta, tc, tcfg.height_em), _render(na, nc, ncfg.height_em))
	assert mean < 0.01, f"mean |delta| {mean}"
	assert bad < 0.01, f"{bad * 100:.2f}% of pixels differ by > 0.25"

@requires_nanosvg()
def test_parity_negative_control():
	moved = _SVG_ONE_SOLID.replace('x="10"', 'x="18"')
	ta, na = slughorn.Atlas(), slughorn.Atlas()
	tc = slughorn.thorvg.load_string(_SVG_ONE_SOLID, ta)
	nc = slughorn.nanosvg.load_string(moved, na)
	ta.build()
	na.build()
	mean, bad = _diff(_render(ta, tc, 1.0), _render(na, nc, 1.0))
	assert mean >= 0.01 or bad >= 0.01, "an 8 px shift must fail the parity tolerance"


# ---------------------------------------------------------------------------
# Fill-rule-robust tessellation (mis-wound glyph-like contour)
# ---------------------------------------------------------------------------

def _square(d, x0, y0, x1, y1, ccw):
	d.move_to(x0, y0)
	if ccw:
		d.line_to(x1, y0); d.line_to(x1, y1); d.line_to(x0, y1)
	else:
		d.line_to(x0, y1); d.line_to(x1, y1); d.line_to(x1, y0)
	d.close()

def _mesh_area(mesh):
	np = _np()
	p = np.asarray(mesh.positions, dtype=np.float64)
	t = np.asarray(mesh.indices)
	a, b, c = p[t[:, 0]], p[t[:, 1]], p[t[:, 2]]
	return float(np.abs((b[:, 0] - a[:, 0]) * (c[:, 1] - a[:, 1]) - (c[:, 0] - a[:, 0]) * (b[:, 1] - a[:, 1])).sum() * 0.5)

@requires_tessellate()
@requires_bake()
def test_tessellate_miswound_glyph():
	# "O" with its counter wound the same way as its outline: 10x10 minus 4x4 = 84.
	d = slughorn.CurveDecomposer()
	_square(d, 0, 0, 10, 10, True)
	_square(d, 3, 3, 7, 7, True)
	atlas = slughorn.Atlas()
	info = slughorn.ShapeInfo()
	info.curves = d.get_curves()
	atlas.add_shape(slughorn.Key("o"), info)
	contours = atlas.get_shape_contours(slughorn.Key("o"))
	naive = _mesh_area(slughorn.tessellate.tessellate(contours, 1e-3))
	evenodd = _mesh_area(slughorn.tessellate.tessellate(contours, 1e-3, slughorn.FillRule.EvenOdd))
	nonzero = _mesh_area(slughorn.tessellate.tessellate(contours, 1e-3, slughorn.FillRule.NonZero))
	assert naive != pytest.approx(84.0, abs=0.5)   # signed-area classification is fooled
	assert evenodd == pytest.approx(84.0, abs=1e-2)
	assert nonzero == pytest.approx(100.0, abs=1e-2)


# ---------------------------------------------------------------------------
# bake: merge / planar mesh / cost
# ---------------------------------------------------------------------------

@requires_bake()
def test_bake_merge_planar_cost():
	np = _np()
	parts = ['<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 64 64">',
	         '<rect width="64" height="64" fill="#204060"/>']
	for i in range(100):
		x, y = (i * 13) % 60, (i * 29) % 60
		parts.append(f'<rect x="{x}" y="{y}" width="1" height="1" fill="{"#fff" if i % 2 else "#000"}"/>')
	parts.append('<rect x="0" y="0" width="32" height="64" fill="#ff0000" fill-opacity="0.5"/>')
	parts.append('</svg>')
	svg = "".join(parts)

	staging, cfg = slughorn.Atlas(), _quiet()
	loaded = slughorn.thorvg.load_string(svg, staging, slughorn.KeyIterator("s", True), config=cfg)
	meta = [slughorn.bake.LayerSource(l.fill_rule, l.stroke, int(l.spread)) for l in cfg.layers]
	atlas = slughorn.Atlas()
	merged = slughorn.bake.merge_layers(staging, loaded, meta, atlas, slughorn.KeyIterator("k"))
	assert merged.layers_before == 102
	assert merged.layers_after <= 4
	atlas.build()

	mesh = slughorn.bake.bake_mesh(atlas, merged.composite, merged.layers, slughorn.bake.BakeConfig(cfg.width, cfg.height))
	idx = np.asarray(mesh.indices)
	assert idx.size * 1 == mesh.opaque_index_count + mesh.overlay_index_count
	uv = np.asarray(mesh.positions, dtype=np.float64)
	assert uv.min() >= -1e-6 and uv.max() <= 1 + 1e-6

	def area(tris):
		a, b, c = uv[tris[:, 0]], uv[tris[:, 1]], uv[tris[:, 2]]
		return float(np.abs((b[:, 0] - a[:, 0]) * (c[:, 1] - a[:, 1]) - (c[:, 0] - a[:, 0]) * (b[:, 1] - a[:, 1])).sum() * 0.5)

	opaque = idx[: mesh.opaque_index_count // 3]
	overlay = idx[mesh.opaque_index_count // 3:]
	assert area(opaque) == pytest.approx(1.0, abs=1e-3)    # planar: covers the key exactly once
	assert area(overlay) == pytest.approx(0.5, abs=1e-3)   # translucent half stays an overlay

	cost = slughorn.bake.cost(atlas, merged.composite, mesh, merged.layers_before)
	assert cost.mode in ("mesh", "slug", "mean")
	assert cost.layers_after == merged.layers_after
	assert cost.max_band_curves_h > 0 and cost.max_band_curves_v > 0
