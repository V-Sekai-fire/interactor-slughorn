// Included by slughorn-test-thorvg.cpp (uses its check / compare / slugRender / thorvgRaster).
//
// Stamp layers (slughorn/stamp.hpp)
//
// A stamped SVG rendered two ways must agree: (a) stamp::loadString - runs lifted into stamp
// layers, evaluated by stamp::renderComposite; (b) thorvg::loadString of the SAME file - ThorVG
// expands every <use> into ordinary layers. Tolerance: the raster one (mean |d| < 0.02, < 3%
// pixels off by > 0.25) plus a discriminating one, flips (pixels off by > 0.5) <= max(8, 0.2%).
// The negative control shifts ONE instance and must break it.

struct StampCase {
	slughorn::Atlas atlas;
	slughorn::stamp::Set set;
	slughorn::CompositeShape comp;
	slughorn::thorvg::LoadConfig cfg;
	std::vector<std::string> notes;
	uint32_t depth = 0;
};

// Stamp parity isolates the stamps: the canvas's own (non-stamp) layers and the <use>-expanded
// reference both split cubics adaptively at STAMP_PARITY_TOLERANCE_PX instead of the default two
// quadratics per cubic, whose error (~0.003 of a circle's radius, 0.4 px coverage at r = 120 px)
// would otherwise be the reference's, not the stamps'. The production pipeline keeps the default
// (the contact sheets measure it against ThorVG).
static constexpr slug_t STAMP_PARITY_TOLERANCE_PX = 0.02_cv;

static void loadStamped(const std::string& svg, StampCase& sc, bool split=true, slughorn::stamp::ProtoTolerance tol=slughorn::stamp::ProtoTolerance::Device) {
	slughorn::KeyIterator keys("k", true);

	sc.cfg.log = [](int, std::string_view) {};
	sc.cfg.tolerancePx = STAMP_PARITY_TOLERANCE_PX;
	sc.comp = slughorn::stamp::loadString(svg, sc.atlas, keys, &sc.cfg, sc.set, "proto/", &sc.notes, tol);

	if(split && sc.cfg.width > 0_cv) sc.depth = slughorn::stamp::splitDeep(sc.set, sc.comp, 1_cv, sc.cfg.heightEm, sc.cfg.width);

	sc.atlas.build();
}

static std::vector<slug_t> expandedRender(const std::string& svg, uint32_t W, uint32_t H) {
	slughorn::Atlas atlas;
	slughorn::KeyIterator keys("e", true);
	auto cfg = quietConfig();

	cfg.tolerancePx = STAMP_PARITY_TOLERANCE_PX;
	auto comp = slughorn::thorvg::loadString(svg, atlas, keys, 96_cv, &cfg);

	atlas.build();

	return slugRender(atlas, comp, cfg.heightEm, W, H);
}

// Negative control: shift the highest-contrast instance (alpha x |instance color - what the
// reference shows under its center|, at least 2 x 2 px) by max(its width, 4 px). Returns the
// contrast, 0 when no instance is usable.
static double shiftOneInstance(slughorn::stamp::Set& set, const std::vector<slug_t>& ref, uint32_t W, uint32_t H, slug_t heightEm) {
	slughorn::stamp::Instance* best = nullptr;
	double bestC = 0.0;
	slug_t bestW = 0_cv;

	const slug_t ppe = cv(W);

	for(auto& l : set.layers) for(auto& in : l.instances) {
		const auto b = slughorn::stamp::bounds(in, set);

		if((b.x1 - b.x0) * ppe < 2_cv || (b.y1 - b.y0) * ppe < 2_cv || (b.x1 - b.x0) * (b.y1 - b.y0) > 0.05_cv * heightEm) continue;

		const auto px = static_cast<int64_t>(((b.x0 + b.x1) * 0.5_cv) * ppe);
		const auto py = static_cast<int64_t>(((b.y0 + b.y1) * 0.5_cv) * ppe);

		if(px < 0 || py < 0 || px >= int64_t(W) || py >= int64_t(H)) continue;

		const size_t o = (size_t(py) * W + size_t(px)) * 4;
		const double c = in.color.a * std::max({std::abs(in.color.r - ref[o]), std::abs(in.color.g - ref[o + 1]), std::abs(in.color.b - ref[o + 2])});

		if(c > bestC) { bestC = c; best = &in; bestW = b.x1 - b.x0; }
	}

	if(!best) return 0.0;

	best->m.dx += std::max(bestW, 4_cv / ppe);

	return bestC;
}

// Parity bound: the raster tolerance plus a per-pixel cap (the worst real key is 0.19).
static bool stampOk(const Diff& d, size_t pixels) {
	return d.mean < 0.02 && d.bad < 0.03 && double(d.flips) <= std::max(8.0, 0.002 * double(pixels)) && d.maxd <= 0.3;
}

static const std::string SVG_STAMPED = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" width="128" height="96" viewBox="0 0 128 96">
	<defs>
		<symbol id="r" data-stamp-kind="rect" overflow="visible"><path d="M0 0L1 0L1 1L0 1Z"/></symbol>
		<symbol id="e" data-stamp-kind="ellipse" overflow="visible"><path d="M1 0C1 0.55228 0.55228 1 0 1C-0.55228 1 -1 0.55228 -1 0C-1 -0.55228 -0.55228 -1 0 -1C0.55228 -1 1 -0.55228 1 0Z"/></symbol>
		<symbol id="s" data-stamp-kind="path" overflow="visible"><path d="M0 1L0.5 0L1 1Z"/></symbol>
		<symbol id="k" data-stamp-kind="path" overflow="visible"><path d="M0 0.5L1 0.5M0.5 0L0.5 1" fill="none" stroke-width="0.2"/></symbol>
		<linearGradient id="lg" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="1" y2="0">
			<stop offset="0" stop-color="#ff0000"/><stop offset="1" stop-color="#0000ff"/>
		</linearGradient>
		<radialGradient id="rg" gradientUnits="userSpaceOnUse" cx="0" cy="0" r="1">
			<stop offset="0" stop-color="#ffffff"/><stop offset="1" stop-color="#00a000"/>
		</radialGradient>
	</defs>
	<path d="M0 0H128V96H0Z" fill="#203040"/>
	<g data-stamp-run="0">
		<use href="#r" transform="matrix(20 0 0 12 4 4)" fill="#ff8000"/>
		<use href="#r" transform="matrix(10 4 -3 8 40 10)" fill="#00ff80" fill-opacity="0.6"/>
		<use href="#e" transform="matrix(9 0 0 6 80 20)" fill="#ffffff"/>
		<use href="#e" transform="matrix(6 3 -2 4 104 18)" fill="#e0e000" fill-opacity="0.8"/>
		<use href="#s" transform="matrix(16 0 0 14 10 40)" fill="#ff00ff"/>
		<use href="#s" transform="matrix(12 -6 6 12 50 44)" fill="#80c0ff"/>
		<use href="#r" transform="matrix(24 0 0 10 4 60)" fill="url(#lg)"/>
		<use href="#e" transform="matrix(10 0 0 10 104 50)" fill="url(#rg)" fill-opacity="0.9"/>
		<use href="#k" transform="matrix(14 0 0 14 30 22)" stroke="#ffffff"/>
	</g>
	<path d="M60 60H120V90H60Z" fill="#a0a0a0"/>
	<g data-stamp-run="1">
		<use href="#e" transform="matrix(12 0 0 12 70 70)" fill="#000000" fill-opacity="0.5"/>
		<use href="#r" transform="matrix(2 0 0 2 100 66)" fill="#ff0000"/>
		<use href="#r" transform="matrix(1 0 0 1 104 66.5)" fill="#ff0000"/>
		<use href="#s" transform="matrix(8 0 0 8 30 75)" fill="#ffff00"/>
	</g>
	<g clip-path="url(#nope)"><g data-stamp-run="2"><use href="#r" transform="matrix(4 0 0 4 2 88)" fill="#fff"/></g></g>
</svg>
)SVG";

void test_StampThorvgCaveats() {
	std::cout << "\n=== test_StampThorvgCaveats ===" << std::endl;

	// What the recorder relies on ThorVG 1.0.3 doing with stamp markup, checked on ThorVG's own raster.
	auto pixel = [](const std::string& svg, uint32_t x, uint32_t y) {
		const auto img = thorvgRaster(svg, 32, 32);
		const size_t o = (size_t(y) * 32 + x) * 4;

		return std::array<slug_t, 4>{img[o], img[o + 1], img[o + 2], img[o + 3]};
	};

	const std::string head = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32" viewBox="0 0 32 32"><defs>)SVG";

	// 1. overflow="visible": a unit circle at the symbol origin, used at (16,16) radius 10, must
	//    cover the pixel at (10,10) (the quadrant SVG would clip without overflow=visible).
	const std::string vis = head + R"SVG(<symbol id="e" overflow="visible"><path d="M1 0C1 0.55 0.55 1 0 1C-0.55 1 -1 0.55 -1 0C-1 -0.55 -0.55 -1 0 -1C0.55 -1 1 -0.55 1 0Z"/></symbol></defs><use href="#e" transform="matrix(10 0 0 10 16 16)" fill="#ff0000"/></svg>)SVG";
	const std::string hid = head + R"SVG(<symbol id="e"><path d="M1 0C1 0.55 0.55 1 0 1C-0.55 1 -1 0.55 -1 0C-1 -0.55 -0.55 -1 0 -1C0.55 -1 1 -0.55 1 0Z"/></symbol></defs><use href="#e" transform="matrix(10 0 0 10 16 16)" fill="#ff0000"/></svg>)SVG";

	check("ThorVG honors <symbol overflow=\"visible\"> (negative quadrant drawn)", pixel(vis, 10, 10)[3] > 0.9_cv);
	check("ThorVG clips a symbol without overflow=visible (spec behaviour)", pixel(hid, 10, 10)[3] < 0.1_cv && pixel(hid, 20, 20)[3] > 0.9_cv);

	// 2. href without xlink:, 3. fill / fill-opacity inherited from <use> into the symbol's path.
	const std::string inh = head + R"SVG(<symbol id="r" overflow="visible"><path d="M0 0L1 0L1 1L0 1Z"/></symbol></defs><use href="#r" transform="matrix(20 0 0 20 6 6)" fill="#0000ff" fill-opacity="0.5"/></svg>)SVG";
	const auto p = pixel(inh, 16, 16);

	check("ThorVG resolves <use href> without xlink:", p[3] > 0.1_cv);
	checkNear("fill inherited from <use> (blue)", p[2], 0.5_cv, 0.02_cv);
	checkNear("fill-opacity inherited from <use> (0.5)", p[3], 0.5_cv, 0.02_cv);
	check("no red/green from a default fill", p[0] < 0.01_cv && p[1] < 0.01_cv);

	// 4. A <use> carrying BOTH href and xlink:href is drawn TWICE by ThorVG 1.0.3 (each attribute
	//    clones the target), so a 0.5-opaque instance lands at 0.75. Recorded, not hidden: the
	//    recorder should emit one of them; stamp.hpp's runs are unaffected (one use = one instance).
	const std::string both = head + R"SVG(<symbol id="r" overflow="visible"><path d="M0 0L1 0L1 1L0 1Z"/></symbol></defs><use href="#r" xlink:href="#r" transform="matrix(20 0 0 20 6 6)" fill="#0000ff" fill-opacity="0.5"/></svg>)SVG";
	const auto pb = pixel(both, 16, 16);

	std::cout << "  <use href + xlink:href> alpha " << pb[3] << " (once: 0.5)" << std::endl;
	check("ThorVG draws a <use> with both href and xlink:href twice (known caveat)", std::abs(pb[3] - 0.75_cv) < 0.02_cv);

	// 5. ThorVG's software stroker keeps HALF the stroke width in LOCAL units as 26.6 fixed point
	//    (tvgSwStroke.cpp strokeReset: HALF_STROKE = TO_SWCOORD(width * 0.5)) and scales it after,
	//    so a unit-frame stroke is quantized to 1/32 of a unit before the transform: width 0.05 under
	//    x120 (6 px) is drawn 0.03125 x 120 = 3.75 px wide. A ground-truth defect, not slughorn's:
	//    slughorn's own expansion of the same file is 6 px.
	{
		const std::string ring = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128" viewBox="0 0 128 128"><g transform="matrix(120 0 0 120 4 4)"><path d="M1 0.5C1 0.776142 0.776142 1 0.5 1C0.223858 1 0 0.776142 0 0.5C0 0.223858 0.223858 0 0.5 0C0.776142 0 1 0.223858 1 0.5" fill="none" stroke="#000" stroke-width="0.05"/></g></svg>)SVG";
		const uint32_t RW = 512;

		// Ring width (px at 512) along the centre row's left edge, as summed coverage.
		auto width = [&](const std::vector<slug_t>& img) {
			double w = 0;

			for(uint32_t x = 0; x < 64; x++) w += double(img[(size_t(RW / 2) * RW + x) * 4 + 3]);

			return w / 4.0; // canvas px
		};

		const double tvg = width(thorvgRaster(ring, RW, RW));
		const double precise = width(thorvgRaster(ring, RW, RW, true));
		const double own = width(expandedRender(ring, RW, RW));

		std::cout << "  stroke-width 0.05 under x120 (6 px): ThorVG raster " << tvg << " px, with thorvg-01 (swStrokePrecise) "
			<< precise << " px, slughorn expansion " << own << " px" << std::endl;
		check("slughorn expands a unit-frame stroke at its scaled width (6 px)", std::abs(own - 6.0) < 0.05);
		check("ThorVG caveat: local half stroke width quantized to 1/64 (3.75 px drawn)", std::abs(tvg - 3.75) < 0.1);
		check("thorvg-01 patch: swStrokePrecise draws the exact width (6 px)", std::abs(precise - 6.0) < 0.05);

		// Below 1/32 of a unit the stock stroker draws nothing at all.
		std::string thin = ring;

		thin.replace(thin.find("stroke-width=\"0.05\""), std::string("stroke-width=\"0.05\"").size(), "stroke-width=\"0.025\"");

		const double tvgThin = width(thorvgRaster(thin, RW, RW)), preciseThin = width(thorvgRaster(thin, RW, RW, true));

		std::cout << "  stroke-width 0.025 under x120 (3 px): ThorVG raster " << tvgThin << " px, with thorvg-01 " << preciseThin << " px" << std::endl;
		check("ThorVG caveat: a stroke under 1/32 of a local unit vanishes", tvgThin < 0.05);
		check("thorvg-01 patch: ... and is drawn at 3 px with swStrokePrecise", std::abs(preciseThin - 3.0) < 0.05);
	}
}

// A stroke prototype's outline leaves the unit square by half its width: instance bounds (CPU
// decode boxes, shader cell binning, wrap copies, depth splits) must come from the prototype's own
// extent. A ring x120: the stamp decode must match the <use>-expanded file; the control resets the
// prototype's extent to the unit square (the old bounds) and must fail.
void test_StampProtoExtent() {
	std::cout << "\n=== test_StampProtoExtent ===" << std::endl;

	const std::string svg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128" viewBox="0 0 128 128"><defs><symbol id="p1" data-stamp-kind="stroke" overflow="visible"><path d="M1 0.5C1 0.776142 0.776142 1 0.5 1C0.223858 1 0 0.776142 0 0.5C0 0.223858 0.223858 0 0.5 0C0.776142 0 1 0.223858 1 0.5" fill="none" stroke-width="0.05" stroke-miterlimit="10"/></symbol></defs><g data-stamp-run="0"><use href="#p1" transform="matrix(120 0 0 120 4 4)" stroke="#eb9db6"/></g></svg>)SVG";
	const uint32_t W = 512, H = 512;

	StampCase sc;

	loadStamped(svg, sc);

	check("one curve prototype, one instance", sc.set.protos.size() == 1 && sc.set.layers.size() == 1 && sc.set.layers[0].instances.size() == 1);

	if(sc.set.protos.empty()) return;

	const auto& pr = sc.set.protos[0];

	std::cout << "  prototype extent [" << pr.x0 << "," << pr.y0 << " - " << pr.x1 << "," << pr.y1 << "]" << std::endl;
	check("prototype extent includes the stroke overhang (-0.025 .. 1.025)", std::abs(pr.x0 + 0.025_cv) < 1e-3_cv && std::abs(pr.x1 - 1.025_cv) < 1e-3_cv);

	const auto expanded = expandedRender(svg, W, H);
	const Diff d = compare(slughorn::stamp::renderComposite(sc.atlas, sc.comp, sc.set, W, H, 0_cv, 0_cv, 1_cv, sc.cfg.heightEm).data, expanded);

	std::cout << "  stamp vs expanded: mean|d|=" << d.mean << " bad=" << d.bad * 100 << "% flips=" << d.flips << " max|d|=" << d.maxd << std::endl;
	check("ring stamp renders its full width", stampOk(d, size_t(W) * H) && d.flips == 0);

	// Control: the old unit-square bounds.
	slughorn::stamp::Set old = sc.set;

	old.protos[0].x0 = old.protos[0].y0 = 0_cv;
	old.protos[0].x1 = old.protos[0].y1 = 1_cv;

	const Diff n = compare(slughorn::stamp::renderComposite(sc.atlas, sc.comp, old, W, H, 0_cv, 0_cv, 1_cv, sc.cfg.heightEm).data, expanded);

	std::cout << "  control (unit-square bounds): mean|d|=" << n.mean << " bad=" << n.bad * 100 << "% flips=" << n.flips << " max|d|=" << n.maxd << std::endl;
	check("negative control: unit-square bounds cut the overhang", !stampOk(n, size_t(W) * H));

	// Every binning path (CPU boxes, shader cells, wrap copies, depth splits) uses bounds(): it must
	// reach the ring's outer edge, x = 4 - 0.025 * 120 = 1 canvas px of 128.
	const auto b = slughorn::stamp::bounds(sc.set.layers[0].instances[0], sc.set);

	std::cout << "  instance bounds x0=" << b.x0 * 128_cv << " px (outer edge 1 px)" << std::endl;
	check("instance bounds reach the overhang", b.x0 * 128_cv <= 1.001_cv);
}

void test_Stamp() {
	std::cout << "\n=== test_Stamp ===" << std::endl;

	StampCase sc;

	loadStamped(SVG_STAMPED, sc);

	size_t stampLayers = 0, instances = 0, gradientInstances = 0, curveProtos = 0;

	for(const auto& l : sc.comp.layers) if(slughorn::stamp::isStampLayer(l)) stampLayers++;
	for(const auto& l : sc.set.layers) for(const auto& in : l.instances) { instances++; if(in.gradient) gradientInstances++; }
	for(const auto& p : sc.set.protos) if(p.kind == slughorn::stamp::Kind::Curve) curveProtos++;

	std::cout << "  stampLayers=" << stampLayers << " instances=" << instances << " protos=" << sc.set.protos.size()
		<< " gradients=" << sc.set.gradients.size() << " notes=" << sc.notes.size() << std::endl;

	check("two runs lifted, the clipped one left to ThorVG", stampLayers == 2 && sc.set.layers.size() == 2);
	check("fallback reported", sc.notes.size() == 1);
	check("13 instances (mixed prototypes in one run)", instances == 13);
	check("rect + ellipse analytic, triangle + stroke as curve prototypes", curveProtos == 2 && sc.set.protos.size() == 4);
	check("two gradient instances, two prototype-frame gradients", gradientInstances == 2 && sc.set.gradients.size() == 2);
	check("paint order: background, run 0, grey rect, run 1, fallback", sc.comp.layers.size() == 5 &&
		slughorn::stamp::isStampLayer(sc.comp.layers[1]) && slughorn::stamp::isStampLayer(sc.comp.layers[3]));

	const uint32_t W = 256, H = 192;
	const auto expanded = expandedRender(SVG_STAMPED, W, H);
	const Diff d = compare(slughorn::stamp::renderComposite(sc.atlas, sc.comp, sc.set, W, H, 0_cv, 0_cv, 1_cv, sc.cfg.heightEm).data, expanded);

	std::cout << "  stamp vs expanded: mean|d|=" << d.mean << " bad=" << d.bad * 100 << "% flips=" << d.flips << " max|d|=" << d.maxd << std::endl;
	check("stamp layers (solid, gradient, stroke) render like the <use>-expanded file", stampOk(d, W * H));

	check("negative control: an instance can be shifted", shiftOneInstance(sc.set, expanded, W, H, sc.cfg.heightEm) > 0.35);

	const Diff n = compare(slughorn::stamp::renderComposite(sc.atlas, sc.comp, sc.set, W, H, 0_cv, 0_cv, 1_cv, sc.cfg.heightEm).data, expanded);

	std::cout << "  negative control: mean|d|=" << n.mean << " bad=" << n.bad * 100 << "% flips=" << n.flips << " max|d|=" << n.maxd << std::endl;
	check("negative control (one instance shifted) fails the tolerance", !stampOk(n, W * H));

	// Grid / depth split / bake / cost.
	StampCase c2;

	loadStamped(SVG_STAMPED, c2);

	const auto& sl = c2.set.layers[0];
	const auto g = slughorn::stamp::buildGrid(c2.atlas, c2.set, sl, 1_cv, c2.cfg.heightEm, c2.cfg.width);

	std::cout << "  grid G=" << g.G << " maxPerCell=" << g.maxPerCell << std::endl;
	check("grid: at most 32 per cell", g.maxPerCell <= 32 && g.cells.size() == size_t(g.G) * g.G);

	bool sorted = true;
	size_t listed = 0;

	for(const auto& cell : g.cells) { listed += cell.size(); sorted = sorted && std::is_sorted(cell.begin(), cell.end()); }

	check("cell lists in paint order", sorted);
	check("every instance listed somewhere", listed >= sl.instances.size());

	// 40 rects piled on one point: depth 40 > 32 must split into 2 consecutive layers.
	{
		std::string pile = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 64 64"><defs><symbol id="r" data-stamp-kind="rect" overflow="visible"><path d="M0 0L1 0L1 1L0 1Z"/></symbol></defs><g data-stamp-run="0">)SVG";

		for(int i = 0; i < 40; i++) pile += "<use href=\"#r\" transform=\"matrix(8 0 0 8 " + std::to_string(20 + i % 3) + " 20)\" fill=\"#" + (i % 2 ? std::string("ff0000") : std::string("00ff00")) + "\" fill-opacity=\"0.3\"/>";

		pile += "</g></svg>";

		StampCase sp;

		loadStamped(pile, sp);

		std::cout << "  pile: depth=" << sp.depth << " layers after split=" << sp.set.layers.size() << std::endl;

		bool fits = true;

		for(const auto& l : sp.set.layers) fits = fits && slughorn::stamp::buildGrid(sp.atlas, sp.set, l, 1_cv, sp.cfg.heightEm, sp.cfg.width).maxPerCell <= 32;

		check("depth 40 pile split into consecutive layers that each fit 32", sp.depth == 40 && sp.set.layers.size() == 2 && fits);

		const Diff pd = compare(slughorn::stamp::renderComposite(sp.atlas, sp.comp, sp.set, 64, 64).data, expandedRender(pile, 64, 64));

		check("split keeps the image", stampOk(pd, 64 * 64));
	}

	// Repeat wrapping: an instance crossing the left edge gets a copy at +W; one whose copy the
	// recorder already drew is not doubled; one well inside gets none.
	{
		slughorn::stamp::Set ws;
		slughorn::stamp::Proto pr;

		pr.kind = slughorn::stamp::Kind::Rect;
		ws.protos.push_back(pr);

		slughorn::stamp::Layer wl;
		auto inst = [](slug_t x, slug_t y) {
			slughorn::stamp::Instance in;

			in.m = slughorn::Matrix{.xx = 0.1_cv, .yy = 0.1_cv, .dx = x, .dy = y};
			in.color = {1_cv, 0_cv, 0_cv, 0.5_cv};

			return in;
		};

		wl.instances = {inst(-0.05_cv, 0.4_cv), inst(0.95_cv, 0.6_cv), inst(-0.05_cv, 0.6_cv), inst(0.5_cv, 0.5_cv)};
		ws.layers.push_back(wl);

		const size_t added = slughorn::stamp::wrapDuplicates(ws, 1_cv, 1_cv, 64_cv);

		std::cout << "  wrap: added=" << added << " instances=" << ws.layers[0].instances.size() << std::endl;
		check("wrap: only the unmatched edge-crossing instance is copied", added == 1 && ws.layers[0].instances.size() == 5 &&
			std::abs(ws.layers[0].instances[1].m.dx - 0.95_cv) < 1e-6_cv && std::abs(ws.layers[0].instances[1].m.dy - 0.4_cv) < 1e-6_cv);
	}

	std::vector<slughorn::bake::LayerSource> meta(c2.comp.layers.size());
	slughorn::bake::BakeConfig bc;

	bc.width = c2.cfg.width;
	bc.height = c2.cfg.height;

	const auto mesh = slughorn::bake::bakeMesh(c2.atlas, c2.comp, meta, bc, &c2.set);

	double opaque = 0.0;

	for(size_t i = 0; i + 2 < mesh.opaqueIndexCount; i += 3) {
		const auto P = [&](uint32_t k) { return std::pair<double, double>{mesh.positions[k * 2], mesh.positions[k * 2 + 1]}; };
		const auto [ax, ay] = P(mesh.indices[i]);
		const auto [bx, by] = P(mesh.indices[i + 1]);
		const auto [cx, cy] = P(mesh.indices[i + 2]);

		opaque += std::abs((bx - ax) * (cy - ay) - (cx - ax) * (by - ay)) * 0.5;
	}

	checkNear("stamps expand into the planar bake (opaque area 1)", cv(opaque), 1_cv, 1e-3_cv);

	const auto c = slughorn::bake::cost(c2.atlas, c2.comp, mesh, c2.comp.layers.size(), 1_cv, c2.cfg.heightEm, &c2.set, c2.cfg.width);

	std::cout << "  cost: stampLayers=" << c.stampLayers << " instances=" << c.stampInstances << " work=" << c.stampWork << " mode=" << c.mode << std::endl;
	check("cost sees the stamp layers", c.stampLayers == 2 && c.stampInstances == 13);
}

// --stamp-compare: every file as stamp layers vs its ThorVG <use>-expanded self (256 px wide), plus
// the one-instance-shifted negative control per file. Exit 1 when a file fails parity or a control
// passes.
int stampCompareFiles(int argc, char** argv) {
	int failures = 0;

	for(int i = 2; i < argc; i++) {
		std::ifstream in(argv[i], std::ios::binary);
		std::stringstream ss;

		ss << in.rdbuf();

		const std::string svg = ss.str();
		StampCase sc;

		loadStamped(svg, sc);

		size_t inst = 0;

		for(const auto& l : sc.set.layers) inst += l.instances.size();

		const uint32_t W = 256;
		const auto H = static_cast<uint32_t>(std::max<long>(1, std::lround(W * sc.cfg.heightEm)));

		// ThorVG draws a <use> with both href and xlink:href twice; compare against what the file means.
		const auto expanded = expandedRender(slughorn::stamp::dedupeUseHref(svg), W, H);
		const Diff d = compare(slughorn::stamp::renderComposite(sc.atlas, sc.comp, sc.set, W, H, 0_cv, 0_cv, 1_cv, sc.cfg.heightEm).data, expanded);
		const bool ok = stampOk(d, size_t(W) * H);

		// Control: among the 6 highest-contrast instances, shift the one whose move changes the
		// stamp render the most (an instance hidden under later ones proves nothing), then demand
		// that the parity check rejects it.
		std::string control = "n/a(no instance >= 2 px)";

		{
			const auto base = slughorn::stamp::renderComposite(sc.atlas, sc.comp, sc.set, W, H, 0_cv, 0_cv, 1_cv, sc.cfg.heightEm).data;

			std::vector<std::pair<double, std::pair<size_t, size_t>>> cand;
			const slug_t ppe = cv(W);

			for(size_t li = 0; li < sc.set.layers.size(); li++) for(size_t k = 0; k < sc.set.layers[li].instances.size(); k++) {
				const auto& in = sc.set.layers[li].instances[k];
				const auto b = slughorn::stamp::bounds(in, sc.set);

				if((b.x1 - b.x0) * ppe < 2_cv || (b.y1 - b.y0) * ppe < 2_cv || (b.x1 - b.x0) * (b.y1 - b.y0) > 0.05_cv * sc.cfg.heightEm) continue;

				const auto px = static_cast<int64_t>(((b.x0 + b.x1) * 0.5_cv) * ppe);
				const auto py = static_cast<int64_t>(((b.y0 + b.y1) * 0.5_cv) * ppe);

				if(px < 0 || py < 0 || px >= int64_t(W) || py >= int64_t(H)) continue;

				const size_t o = (size_t(py) * W + size_t(px)) * 4;

				cand.push_back({in.color.a * std::max({std::abs(in.color.r - base[o]), std::abs(in.color.g - base[o + 1]), std::abs(in.color.b - base[o + 2])}), {li, k}});
			}

			std::sort(cand.begin(), cand.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

			double bestVis = -1.0;
			std::vector<slug_t> bestImg;

			for(size_t c = 0; c < std::min<size_t>(6, cand.size()); c++) {
				slughorn::stamp::Set shifted = sc.set;
				auto& in = shifted.layers[cand[c].second.first].instances[cand[c].second.second];
				const auto b = slughorn::stamp::bounds(in, shifted);

				in.m.dx += std::max(b.x1 - b.x0, 4_cv / ppe);

				auto img = slughorn::stamp::renderComposite(sc.atlas, sc.comp, shifted, W, H, 0_cv, 0_cv, 1_cv, sc.cfg.heightEm).data;
				const double vis = compare(img, base).maxd;

				if(vis > bestVis) { bestVis = vis; bestImg = std::move(img); }
			}

			if(!bestImg.empty()) {
				const Diff n = compare(bestImg, expanded);
				const bool caught = !stampOk(n, size_t(W) * H);

				control = std::string(caught ? "caught" : "MISSED") + "(visible change " + std::to_string(bestVis) + ", max|d| " + std::to_string(n.maxd) + ")";

				// A shift too faint to see (max change under the per-pixel bound) cannot be caught.
				if(!caught && bestVis > 0.3) failures++;
			}
		}

		if(!ok) failures++;

		std::cout << (ok ? "ok   " : "FAIL ") << std::filesystem::path(argv[i]).filename().string()
			<< " stampLayers=" << sc.set.layers.size() << " instances=" << inst << " protos=" << sc.set.protos.size()
			<< " gradients=" << sc.set.gradients.size() << " depth=" << sc.depth << " fallbackRuns=" << sc.notes.size()
			<< " mean|d|=" << d.mean << " bad=" << d.bad * 100 << "% flips=" << d.flips << " max|d|=" << d.maxd
			<< " control=" << control << std::endl;
	}

	return failures ? 1 : 0;
}

// A stroke prototype drawn at 40x: its outline (resolved once in the prototype frame) must stay
// within 0.05 px of the same stroke expanded by ThorVG at canvas scale with a much finer tolerance
// (0.002 px). Before prototype tolerances followed the instance scale, the prototype was
// flattened to 0.05 of itself, i.e. ~2 px here.
static double polylineDistance(const std::vector<std::vector<std::pair<double, double>>>& from, const std::vector<std::vector<std::pair<double, double>>>& to) {
	double worst = 0.0;

	for(const auto& poly : from) {
		for(const auto& p : poly) {
			double best = 1e30;

			for(const auto& q : to) {
				for(size_t i = 0; i < q.size(); i++) {
					const auto& a = q[i];
					const auto& b = q[(i + 1) % q.size()];
					const double dx = b.first - a.first, dy = b.second - a.second;
					const double l2 = dx * dx + dy * dy;
					const double t = l2 > 0 ? std::clamp(((p.first - a.first) * dx + (p.second - a.second) * dy) / l2, 0.0, 1.0) : 0.0;
					const double ex = a.first + dx * t - p.first, ey = a.second + dy * t - p.second;

					best = std::min(best, ex * ex + ey * ey);
				}
			}

			worst = std::max(worst, std::sqrt(best));
		}
	}

	return worst;
}

static std::vector<std::vector<std::pair<double, double>>> flattenPx(const slughorn::Atlas::Contours& contours, const slughorn::Matrix& toPx) {
	std::vector<std::vector<std::pair<double, double>>> out;

	for(const auto& p : slughorn::clipper::toPaths(contours, 0.001_cv, toPx)) {
		std::vector<std::pair<double, double>> poly;

		for(const auto& pt : p) poly.push_back({pt.x, pt.y});

		out.push_back(std::move(poly));
	}

	return out;
}

// The prototype of the file's first instance, at that instance's scale, against the same use
// expanded by ThorVG at canvas scale with 0.002 px tolerances: largest outline distance (px) both
// ways, for the device tolerance and for the legacy fixed one.
struct ToleranceProbe {
	double device = -1.0, legacy = -1.0;
	size_t deviceCurves = 0, legacyCurves = 0;
};

static ToleranceProbe probeProtoTolerance(const std::string& svg, slug_t widthPx) {
	ToleranceProbe out;

	slughorn::Atlas ref;
	slughorn::KeyIterator rk("r", true);
	auto rcfg = quietConfig();

	rcfg.strokeTolerancePx = 0.002_cv;
	rcfg.clipTolerancePx = 0.002_cv;
	rcfg.tolerance = 0.002_cv / widthPx; // em

	auto rcomp = slughorn::thorvg::loadString(svg, ref, rk, 96_cv, &rcfg);

	check("reference expands the first use", !rcomp.layers.empty());

	if(rcomp.layers.empty()) return out;

	const auto rshape = ref.getShape(rcomp.layers[0].key);
	const slughorn::Matrix refPx = slughorn::Matrix::scale(rcfg.width, rcfg.width) *
		slughorn::Matrix::translate(rcomp.layers[0].transform.x - rshape->originX, rcomp.layers[0].transform.y - rshape->originY);
	const auto refOutline = flattenPx(ref.getShapeContours(rcomp.layers[0].key), refPx);

	for(auto mode : {slughorn::stamp::ProtoTolerance::Device, slughorn::stamp::ProtoTolerance::Legacy}) {
		StampCase sc;

		loadStamped(svg, sc, true, mode);

		if(sc.set.layers.empty() || sc.set.layers[0].instances.empty()) continue;

		const auto& in = sc.set.layers[0].instances[0];
		const auto& proto = sc.set.protos[in.proto];
		const slughorn::Matrix toPx = slughorn::Matrix::scale(sc.cfg.width, sc.cfg.width) * in.m;
		const auto outline = flattenPx(slughorn::clipper::splitContours(proto.curves, proto.starts), toPx);
		const double d = std::max(polylineDistance(outline, refOutline), polylineDistance(refOutline, outline));

		if(std::getenv("SLUG_PROBE_DUMP") && mode == slughorn::stamp::ProtoTolerance::Legacy) {
			for(const auto& c : proto.curves) std::cout << "    q " << c.x1 << "," << c.y1 << " " << c.x2 << "," << c.y2 << " " << c.x3 << "," << c.y3 << std::endl;
			for(auto st : proto.starts) std::cout << "    start " << st << std::endl;
			std::cout << "    m " << in.m.xx << " " << in.m.yx << " " << in.m.xy << " " << in.m.yy << " " << in.m.dx << " " << in.m.dy << " width " << sc.cfg.width << std::endl;
		}

		if(mode == slughorn::stamp::ProtoTolerance::Device) { out.device = d; out.deviceCurves = proto.curves.size(); }
		else { out.legacy = d; out.legacyCurves = proto.curves.size(); }
	}

	return out;
}

void test_StampStrokeTolerance() {
	std::cout << "\n=== test_StampStrokeTolerance ===" << std::endl;

	// A stroke prototype (round caps / joins, cubic centreline) and a fill prototype (unit circle
	// as four cubics), each drawn at 40x and at 4x.
	const std::string stroke = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128" viewBox="0 0 128 128"><defs><symbol id="w" data-stamp-kind="path" overflow="visible"><path d="M0 0.5C0.3 0 0.7 1 1 0.5" fill="none" stroke-width="0.05" stroke-linecap="round" stroke-linejoin="round"/></symbol></defs><g data-stamp-run="0"><use href="#w" transform="matrix(40 0 0 40 20 30)" stroke="#203040"/><use href="#w" transform="matrix(4 0 0 4 80 100)" stroke="#203040"/></g></svg>)SVG";
	const std::string fill = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128" viewBox="0 0 128 128"><defs><symbol id="c" data-stamp-kind="path" overflow="visible"><path d="M1 0C1 0.55228 0.55228 1 0 1C-0.55228 1 -1 0.55228 -1 0C-1 -0.55228 -0.55228 -1 0 -1C0.55228 -1 1 -0.55228 1 0Z"/></symbol></defs><g data-stamp-run="0"><use href="#c" transform="matrix(40 0 0 40 64 64)" fill="#203040"/><use href="#c" transform="matrix(4 0 0 4 110 110)" fill="#203040"/></g></svg>)SVG";

	{
		StampCase sc;

		loadStamped(stroke, sc);

		check("one stroke prototype, two instances", sc.set.protos.size() == 1 && sc.set.protos[0].kind == slughorn::stamp::Kind::Curve &&
			sc.set.layers.size() == 1 && sc.set.layers[0].instances.size() == 2);
	}

	for(const auto& [name, svg] : {std::pair<const char*, const std::string*>{"stroke", &stroke}, {"cubic fill", &fill}}) {
		const ToleranceProbe p = probeProtoTolerance(*svg, 128_cv);

		std::cout << "  40x " << name << " prototype vs 0.002 px reference: device tolerance " << p.device << " px (" << p.deviceCurves
			<< " curves), legacy fixed tolerance " << p.legacy << " px (" << p.legacyCurves << " curves)" << std::endl;

		check((std::string("40x ") + name + " prototype within 0.05 px of the reference").c_str(), p.device >= 0.0 && p.device <= 0.05);
		check((std::string("negative control: the legacy fixed tolerance misses 0.05 px (") + name + ")").c_str(), p.legacy > 0.05);
	}
}
