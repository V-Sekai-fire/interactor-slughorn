cmake_minimum_required(VERSION 3.15)

project(slughorn LANGUAGES C CXX VERSION 0.0.3)

# =============================================================================
# Compile options
# =============================================================================

# MSVC (not WIN32): a mingw/clang toolchain on Windows takes the GNU-style flags below.
if(MSVC)
	set(SLUGHORN_COMPILE_OPTIONS
		/W4
		/WX
		/wd4100
		/analyze
		/analyze:external-
		# The MSVC STL/SDK headers are reached via the INCLUDE env var, not -I,
		# so they aren't automatically treated as external; without this,
		# /analyze produces false positives (e.g. C6031) deep in <xutility>
		# et al. when heavily-templated code (pybind11) instantiates them.
		/external:env:INCLUDE
	)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
	set(SLUGHORN_COMPILE_OPTIONS
		-W
		-Wall
		-Wshadow
		-Wconversion
		-Wsign-conversion
		-Wfloat-conversion
		-Wpedantic
		-Wextra
		-Werror
		-Wno-unused-parameter
	)
else()
	set(SLUGHORN_COMPILE_OPTIONS)
endif()

# =============================================================================
# Core library target
# =============================================================================

option(SLUGHORN_SERIAL "Include serialization support (requires nlohmann)" OFF)
option(SLUGHORN_NANOSVG "Include NanoSVG support (requires nanosvg)" OFF)
option(SLUGHORN_THORVG "Include ThorVG SVG support (builds vendored ext/thorvg: SVG loader + sw engine)" OFF)
option(SLUGHORN_THORVG_THREADS "Build ThorVG with its task-scheduler threads (THORVG_THREAD_SUPPORT)" OFF)
option(SLUGHORN_CLIPPER2 "Include Clipper2 polygon booleans (clip paths, fill-rule normalization, planarization)" OFF)
option(SLUGHORN_MESHOPT "Include meshoptimizer (bake LOD: simplify baked meshes to a triangle budget)" OFF)
set(SLUGHORN_MESHOPTIMIZER_DIR "" CACHE PATH "A meshoptimizer checkout (with src/meshoptimizer.h); default ext/meshoptimizer")
option(SLUGHORN_FREETYPE "Include FreeType support (requires freetype2)" OFF)
option(SLUGHORN_SDF "Include SDF/MSDF tile baking (requires msdfgen)" OFF)
option(SLUGHORN_TESSELLATE "Include polygon triangulation/extrusion support (requires earcut.hpp)" OFF)
option(SLUGHORN_BLEND2D "Include Blend2D backend (requires blend2d)" OFF)
option(SLUGHORN_RENDER_PARALLEL "Enable std::execution parallel pixel rendering (requires TBB on Linux)" OFF)
# option(SLUGHORN_GLFW "Include GLFW support (requires glfw3)" OFF)

add_library(slughorn STATIC "slughorn/slughorn.cpp")

target_include_directories(slughorn
	PUBLIC
		# When used as a submodule, consumers get our headers via this path
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
		$<INSTALL_INTERFACE:include>
)

target_compile_features(slughorn PUBLIC cxx_std_20)
target_compile_options(slughorn PRIVATE ${SLUGHORN_COMPILE_OPTIONS})
set_target_properties(slughorn PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(SLUGHORN_SERIAL)
	if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ext/json")
		message(FATAL_ERROR
			"ext/json not found. Run:\n"
			"  git submodule update --init --recursive"
		)
	endif()

	add_subdirectory("ext/json")

	target_compile_definitions(slughorn PRIVATE SLUGHORN_SERIAL_IMPLEMENTATION)
	target_compile_definitions(slughorn PUBLIC SLUGHORN_HAS_SERIAL)
	target_link_libraries(slughorn PRIVATE nlohmann_json::nlohmann_json)
endif()

if(SLUGHORN_NANOSVG)
	if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ext/nanosvg")
		message(FATAL_ERROR
			"ext/nanosvg not found. Run:\n"
			"  git submodule update --init --recursive"
		)
	endif()

	find_package(Git REQUIRED)

	# NanoSVG upstream is missing a few fixes we depend on (NSVGgradient::units,
	# nsvg__pushAttr id inheritance, OBB linear gradient coordinate handling).
	# Apply them here so a fresh submodule checkout builds without manual steps.
	# --reverse --check detects an already-patched tree so reconfiguring never
	# double-applies or errors out.
	set(SLUGHORN_NANOSVG_PATCHES
		"${CMAKE_CURRENT_SOURCE_DIR}/ext/nanosvg-00-gradient-units-field.diff"
		"${CMAKE_CURRENT_SOURCE_DIR}/ext/nanosvg-01-push-attr-id-inherit.diff"
		"${CMAKE_CURRENT_SOURCE_DIR}/ext/nanosvg-02-obb-linear-gradient-coords.diff"
	)

	# --ignore-whitespace guards against CRLF-checked-out submodules (e.g. Windows
	# with core.autocrlf=true) not matching these LF patch files context-for-context.
	foreach(_slughorn_nanosvg_patch ${SLUGHORN_NANOSVG_PATCHES})
		execute_process(
			COMMAND "${GIT_EXECUTABLE}" apply --reverse --check --ignore-whitespace "${_slughorn_nanosvg_patch}"
			WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/ext/nanosvg"
			RESULT_VARIABLE _slughorn_nanosvg_patch_applied
			OUTPUT_QUIET
			ERROR_QUIET
		)

		if(NOT _slughorn_nanosvg_patch_applied EQUAL 0)
			message(STATUS "Applying nanosvg patch: ${_slughorn_nanosvg_patch}")

			execute_process(
				COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace "${_slughorn_nanosvg_patch}"
				WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/ext/nanosvg"
				RESULT_VARIABLE _slughorn_nanosvg_patch_result
			)

			if(NOT _slughorn_nanosvg_patch_result EQUAL 0)
				message(FATAL_ERROR "Failed to apply nanosvg patch: ${_slughorn_nanosvg_patch}")
			endif()
		endif()
	endforeach()

	target_compile_definitions(slughorn PRIVATE SLUGHORN_NANOSVG_IMPLEMENTATION)
	target_compile_definitions(slughorn PUBLIC SLUGHORN_HAS_NANOSVG)
	target_include_directories(slughorn
		SYSTEM PUBLIC
			$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/ext/nanosvg/src>
	)
endif()

# https://github.com/AngusJohnson/Clipper2 (vendored as ext/clipper2, pinned Clipper2_1.5.4).
# Compiled as its own static library so slughorn's strict warning flags don't apply to it.
if(SLUGHORN_CLIPPER2)
	if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ext/clipper2/CPP/Clipper2Lib")
		message(FATAL_ERROR
			"ext/clipper2 not found. Run:
"
			"  git submodule update --init --recursive"
		)
	endif()

	set(_slughorn_clipper2_dir "${CMAKE_CURRENT_SOURCE_DIR}/ext/clipper2/CPP/Clipper2Lib")

	add_library(slughorn_clipper2 STATIC
		"${_slughorn_clipper2_dir}/src/clipper.engine.cpp"
		"${_slughorn_clipper2_dir}/src/clipper.offset.cpp"
		"${_slughorn_clipper2_dir}/src/clipper.rectclip.cpp"
	)
	target_include_directories(slughorn_clipper2 SYSTEM PUBLIC "${_slughorn_clipper2_dir}/include")
	target_compile_features(slughorn_clipper2 PUBLIC cxx_std_17)
	# clipper.core.h uses std::back_inserter without including <iterator> (MSVC's STL hides it).
	target_compile_options(slughorn_clipper2 PRIVATE
		$<IF:$<CXX_COMPILER_ID:MSVC>,/FIiterator,SHELL:-include iterator>
	)
	set_target_properties(slughorn_clipper2 PROPERTIES POSITION_INDEPENDENT_CODE ON)

	target_compile_definitions(slughorn PUBLIC SLUGHORN_HAS_CLIPPER2)
	target_link_libraries(slughorn PUBLIC slughorn_clipper2)
endif()

# https://github.com/zeux/meshoptimizer - the bake's LOD simplification (bake.hpp simplifyBaked). Not
# vendored: point SLUGHORN_MESHOPTIMIZER_DIR at a checkout (the workspace keeps one at
# 2-contract/meshoptimizer), or clone it to ext/meshoptimizer. Only the simplifier and the index
# generator are compiled.
if(SLUGHORN_MESHOPT)
	set(_slughorn_meshopt_dir "${SLUGHORN_MESHOPTIMIZER_DIR}")

	if(_slughorn_meshopt_dir STREQUAL "")
		set(_slughorn_meshopt_dir "${CMAKE_CURRENT_SOURCE_DIR}/ext/meshoptimizer")
	endif()

	if(NOT EXISTS "${_slughorn_meshopt_dir}/src/meshoptimizer.h")
		message(FATAL_ERROR "SLUGHORN_MESHOPT: no meshoptimizer at '${_slughorn_meshopt_dir}' (set SLUGHORN_MESHOPTIMIZER_DIR)")
	endif()

	add_library(slughorn_meshopt STATIC
		"${_slughorn_meshopt_dir}/src/simplifier.cpp"
		"${_slughorn_meshopt_dir}/src/indexgenerator.cpp"
		"${_slughorn_meshopt_dir}/src/allocator.cpp"
	)
	target_include_directories(slughorn_meshopt SYSTEM PUBLIC "${_slughorn_meshopt_dir}/src")
	set_target_properties(slughorn_meshopt PROPERTIES POSITION_INDEPENDENT_CODE ON)

	target_compile_definitions(slughorn PUBLIC SLUGHORN_HAS_MESHOPT)
	target_link_libraries(slughorn PUBLIC slughorn_meshopt)
endif()

# https://github.com/thorvg/thorvg (vendored as ext/thorvg, pinned v1.0.3 - the version Godot's
# thirdparty/thorvg ships). ThorVG's own build is meson; like Godot (modules/svg/SCsub) we compile
# the subset we need directly: common + renderer + sw_engine + the SVG and raw loaders. No PNG/JPG/
# WebP loaders (embedded raster <image>s are skipped by slughorn/thorvg.hpp anyway), no Lottie,
# no TTF, no GL/WG engines. config.h is generated into the build tree.
if(SLUGHORN_THORVG)
	if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ext/thorvg/inc/thorvg.h")
		message(FATAL_ERROR
			"ext/thorvg not found. Run:
"
			"  git submodule update --init --recursive"
		)
	endif()

	set(_slughorn_tvg "${CMAKE_CURRENT_SOURCE_DIR}/ext/thorvg")

	# Godot's own ThorVG patch (thirdparty/thorvg/patches/0001-let-delete-be-delete.patch,
	# GH-116024): ThorVG 1.0.x replaces the GLOBAL operator new/delete with tvg::malloc/free,
	# which collides with any host that defines them too (godot-sandbox's guest runtime does) and
	# silently reroutes every allocation of the embedding program otherwise. Applied the same
	# idempotent way as the NanoSVG patches above.
	find_package(Git REQUIRED)

	# Our own: thorvg-01 gives the software stroker an opt-in exact stroke width
	# (tvg::swStrokePrecise; stock ThorVG quantizes a stroke to 1/64 of a LOCAL unit, so unit-frame
	# strokes under large transforms are drawn too thin or not at all). Only the test rasterizer sets
	# it; the loader never rasterizes.
	set(_slughorn_tvg_patches
		"${CMAKE_CURRENT_SOURCE_DIR}/ext/thorvg-00-let-delete-be-delete.diff"
		"${CMAKE_CURRENT_SOURCE_DIR}/ext/thorvg-01-stroke-width-precision.diff"
	)

	foreach(_slughorn_tvg_patch ${_slughorn_tvg_patches})
		execute_process(
			COMMAND "${GIT_EXECUTABLE}" apply --reverse --check --ignore-whitespace "${_slughorn_tvg_patch}"
			WORKING_DIRECTORY "${_slughorn_tvg}"
			RESULT_VARIABLE _slughorn_tvg_patch_applied
			OUTPUT_QUIET
			ERROR_QUIET
		)

		if(NOT _slughorn_tvg_patch_applied EQUAL 0)
			message(STATUS "Applying thorvg patch: ${_slughorn_tvg_patch}")

			execute_process(
				COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace "${_slughorn_tvg_patch}"
				WORKING_DIRECTORY "${_slughorn_tvg}"
				RESULT_VARIABLE _slughorn_tvg_patch_result
			)

			if(NOT _slughorn_tvg_patch_result EQUAL 0)
				message(FATAL_ERROR "Failed to apply thorvg patch: ${_slughorn_tvg_patch}")
			endif()
		endif()
	endforeach()
	set(_slughorn_tvg_config "${CMAKE_CURRENT_BINARY_DIR}/thorvg-config")

	set(_slughorn_tvg_threads "")
	if(SLUGHORN_THORVG_THREADS)
		set(_slughorn_tvg_threads "#define THORVG_THREAD_SUPPORT")
	endif()

	file(WRITE "${_slughorn_tvg_config}/config.h.in"
		"#ifndef THORVG_CONFIG_H
"
		"#define THORVG_CONFIG_H
"
		"#define THORVG_SW_RASTER_SUPPORT
"
		"#define THORVG_SVG_LOADER_SUPPORT
"
		"#define THORVG_FILE_IO_SUPPORT
"
		"${_slughorn_tvg_threads}
"
		"#define THORVG_VERSION_STRING \"1.0.3\"
"
		"#endif
"
	)
	configure_file("${_slughorn_tvg_config}/config.h.in" "${_slughorn_tvg_config}/config.h" COPYONLY)

	add_library(slughorn_thorvg STATIC
		# common
		"${_slughorn_tvg}/src/common/tvgColor.cpp"
		"${_slughorn_tvg}/src/common/tvgCompressor.cpp"
		"${_slughorn_tvg}/src/common/tvgMath.cpp"
		"${_slughorn_tvg}/src/common/tvgStr.cpp"
		# SVG parser
		"${_slughorn_tvg}/src/loaders/svg/tvgSvgCssStyle.cpp"
		"${_slughorn_tvg}/src/loaders/svg/tvgSvgLoader.cpp"
		"${_slughorn_tvg}/src/loaders/svg/tvgSvgPath.cpp"
		"${_slughorn_tvg}/src/loaders/svg/tvgSvgSceneBuilder.cpp"
		"${_slughorn_tvg}/src/loaders/svg/tvgSvgUtil.cpp"
		"${_slughorn_tvg}/src/loaders/svg/tvgXmlParser.cpp"
		"${_slughorn_tvg}/src/loaders/raw/tvgRawLoader.cpp"
		# renderer common
		"${_slughorn_tvg}/src/renderer/tvgAccessor.cpp"
		"${_slughorn_tvg}/src/renderer/tvgCanvas.cpp"
		"${_slughorn_tvg}/src/renderer/tvgFill.cpp"
		"${_slughorn_tvg}/src/renderer/tvgInitializer.cpp"
		"${_slughorn_tvg}/src/renderer/tvgLoader.cpp"
		"${_slughorn_tvg}/src/renderer/tvgPaint.cpp"
		"${_slughorn_tvg}/src/renderer/tvgPicture.cpp"
		"${_slughorn_tvg}/src/renderer/tvgRender.cpp"
		"${_slughorn_tvg}/src/renderer/tvgScene.cpp"
		"${_slughorn_tvg}/src/renderer/tvgShape.cpp"
		"${_slughorn_tvg}/src/renderer/tvgTaskScheduler.cpp"
		"${_slughorn_tvg}/src/renderer/tvgText.cpp"
		# renderer sw_engine
		"${_slughorn_tvg}/src/renderer/sw_engine/tvgSwFill.cpp"
		"${_slughorn_tvg}/src/renderer/sw_engine/tvgSwImage.cpp"
		"${_slughorn_tvg}/src/renderer/sw_engine/tvgSwMath.cpp"
		"${_slughorn_tvg}/src/renderer/sw_engine/tvgSwMemPool.cpp"
		"${_slughorn_tvg}/src/renderer/sw_engine/tvgSwPostEffect.cpp"
		"${_slughorn_tvg}/src/renderer/sw_engine/tvgSwRaster.cpp"
		"${_slughorn_tvg}/src/renderer/sw_engine/tvgSwRenderer.cpp"
		"${_slughorn_tvg}/src/renderer/sw_engine/tvgSwRle.cpp"
		"${_slughorn_tvg}/src/renderer/sw_engine/tvgSwShape.cpp"
		"${_slughorn_tvg}/src/renderer/sw_engine/tvgSwStroke.cpp"
	)
	target_include_directories(slughorn_thorvg
		SYSTEM PUBLIC
			"${_slughorn_tvg}/inc"
		PRIVATE
			"${_slughorn_tvg_config}"
			"${_slughorn_tvg}/src/common"
			"${_slughorn_tvg}/src/renderer"
			"${_slughorn_tvg}/src/renderer/sw_engine"
			"${_slughorn_tvg}/src/loaders/svg"
			"${_slughorn_tvg}/src/loaders/raw"
	)
	target_compile_definitions(slughorn_thorvg PUBLIC TVG_STATIC)
	target_compile_features(slughorn_thorvg PUBLIC cxx_std_14)
	set_target_properties(slughorn_thorvg PROPERTIES POSITION_INDEPENDENT_CODE ON)

	if(MSVC)
		target_compile_options(slughorn_thorvg PRIVATE /W0 /wd4996 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX)
	else()
		target_compile_options(slughorn_thorvg PRIVATE -w)
	endif()

	if(SLUGHORN_THORVG_THREADS)
		find_package(Threads REQUIRED)
		target_link_libraries(slughorn_thorvg PUBLIC Threads::Threads)
	endif()

	target_compile_definitions(slughorn PRIVATE SLUGHORN_THORVG_IMPLEMENTATION)
	target_compile_definitions(slughorn PUBLIC SLUGHORN_HAS_THORVG)
	target_link_libraries(slughorn PUBLIC slughorn_thorvg)
endif()

if(SLUGHORN_FREETYPE)
	# if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ext/nanosvg")
	# 	message(FATAL_ERROR
	# 		"ext/nanosvg not found. Run:\n"
	# 		"  git submodule update --init --recursive"
	# 	)
	# endif()

	find_package(Freetype REQUIRED)

	target_compile_definitions(slughorn PRIVATE SLUGHORN_FREETYPE_IMPLEMENTATION)
	target_compile_definitions(slughorn PUBLIC SLUGHORN_HAS_FREETYPE)
	# target_include_directories(slughorn
	# 	PUBLIC
	# 		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
	# 		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/ext/nanosvg/src>
	# 		$<INSTALL_INTERFACE:include>
	# )
	target_link_libraries(slughorn PUBLIC Freetype::Freetype)
endif()

# https://github.com/Chlumsky/msdfgen.git
if(SLUGHORN_SDF)
	if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ext/msdfgen")
		message(FATAL_ERROR
			"ext/msdfgen not found. Run:\n"
			"  git submodule update --init --recursive"
		)
	endif()

	set(MSDFGEN_CORE_ONLY ON CACHE BOOL "" FORCE)
	set(MSDFGEN_BUILD_STANDALONE OFF CACHE BOOL "" FORCE)
	set(MSDFGEN_USE_VCPKG OFF CACHE BOOL "" FORCE)
	set(MSDFGEN_USE_OPENMP ${SLUGHORN_RENDER_PARALLEL} CACHE BOOL "" FORCE)
	set(MSDFGEN_USE_SKIA OFF CACHE BOOL "" FORCE)
	set(MSDFGEN_DISABLE_SVG OFF CACHE BOOL "" FORCE)
	set(MSDFGEN_DISABLE_PNG OFF CACHE BOOL "" FORCE)
	# The rest of the project (and the vcpkg triplet's VCPKG_CRT_LINKAGE) links
	# the dynamic CRT; msdfgen's own default of static breaks the MSVC link
	# with a RuntimeLibrary mismatch (MDd vs MTd). This property is a no-op
	# outside MSVC, so only override it there.
	if(MSVC)
		set(MSDFGEN_DYNAMIC_RUNTIME ON CACHE BOOL "" FORCE)
	else()
		set(MSDFGEN_DYNAMIC_RUNTIME OFF CACHE BOOL "" FORCE)
	endif()
	set(MSDFGEN_INSTALL OFF CACHE BOOL "" FORCE)

	add_subdirectory("ext/msdfgen")
	set_target_properties(msdfgen-core PROPERTIES POSITION_INDEPENDENT_CODE ON)

	target_compile_definitions(slughorn PUBLIC SLUGHORN_HAS_SDF)
	target_include_directories(slughorn
		SYSTEM PUBLIC
			$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/ext/msdfgen>
	)
	target_link_libraries(slughorn PUBLIC msdfgen-core)
endif()

if(SLUGHORN_TESSELLATE)
	if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ext/earcut")
		message(FATAL_ERROR
			"ext/earcut not found. Run:\n"
			"  git submodule update --init --recursive"
		)
	endif()

	target_compile_definitions(slughorn PUBLIC SLUGHORN_HAS_TESSELLATE)
	target_include_directories(slughorn
		SYSTEM PUBLIC
			$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/ext/earcut/include>
	)
endif()

if(SLUGHORN_BLEND2D)
	find_package(blend2d REQUIRED)
	target_compile_definitions(slughorn PUBLIC SLUGHORN_HAS_BLEND2D)
	target_link_libraries(slughorn PUBLIC blend2d::blend2d)
endif()

if(SLUGHORN_RENDER_PARALLEL)
	find_package(OpenMP QUIET)

	if(OpenMP_CXX_FOUND)
		target_compile_definitions(slughorn PUBLIC SLUGHORN_HAS_PARALLEL)
		target_link_libraries(slughorn PUBLIC OpenMP::OpenMP_CXX)
		message(STATUS "SLUGHORN_RENDER_PARALLEL: using OpenMP")
	else()
		message(WARNING "SLUGHORN_RENDER_PARALLEL=ON but OpenMP not found; parallel rendering disabled")
	endif()
endif()

# =============================================================================
# Python bindings (optional - only when building slughorn standalone)
# =============================================================================

# When slughorn is included as a submodule, the parent project controls
# whether Python bindings are built. We only auto-configure them when
# slughorn is the top-level project.

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
	# An extension module needs Python headers but must not link libpython. manylinux
	# deliberately omits the embeddable Development.Embed library, so request the
	# module-development component rather than the aggregate "Development" one (same fix as
	# OpenSceneGraph.py's CMakeLists.txt:10-11).
	find_package(Python3 COMPONENTS Interpreter Development.Module REQUIRED)

	if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ext/pybind11/CMakeLists.txt")
		message(FATAL_ERROR
			"pybind11 submodule is missing. Run:\n"
			"  git submodule update --init --recursive"
		)
	endif()

	add_subdirectory(ext/pybind11)

	# The bindings are split across ext/python/slughorn-*.cpp -- one translation unit per
	# slughorn:: submodule (see ext/python/slughorn-python.hpp for the bind_*() declarations) --
	# instead of one ~2900-line monolithic ext/slughorn-python.cpp. slughorn_python_bindings is a
	# plain STATIC lib, not itself a Python module: touching one submodule's bindings only
	# recompiles that one file plus a relink, a from-scratch build parallelizes across `make -j#`,
	# and pybind11_add_module below only ever has to compile the thin ext/python/slughorn.cpp
	# PYBIND11_MODULE entry point.
	add_library(slughorn_python_bindings STATIC
		"ext/python/slughorn-core.cpp"
		"ext/python/slughorn-render.cpp"
		"ext/python/slughorn-canvas.cpp"
		"ext/python/slughorn-emoji.cpp"
	)

	# PUBLIC, not PRIVATE: ext/python/slughorn.cpp (compiled into the separate slughorn_python
	# target below, which links this one) also includes slughorn-python.hpp and therefore needs
	# every one of these same requirements -- PRIVATE here would resolve fine for this target's
	# own sources but leave slughorn.cpp's own #ifdef SLUGHORN_HAS_FREETYPE/etc. guards (and
	# #include "slughorn-python.hpp") unable to see the same defines/link libraries.
	target_link_libraries(slughorn_python_bindings PUBLIC
		slughorn
		pybind11::module
	)
	target_compile_features(slughorn_python_bindings PUBLIC cxx_std_20)
	target_compile_options(slughorn_python_bindings PRIVATE ${SLUGHORN_COMPILE_OPTIONS})
	# pybind11's per-function registration code pushes these heavily-templated
	# binding TUs past MSVC's default object-file section limit (C1128).
	target_compile_options(slughorn_python_bindings PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)
	set_target_properties(slughorn_python_bindings PROPERTIES POSITION_INDEPENDENT_CODE ON)

	if(SLUGHORN_SERIAL)
		target_link_libraries(slughorn_python_bindings PRIVATE nlohmann_json::nlohmann_json)
	endif()

	if(SLUGHORN_FREETYPE)
		target_sources(slughorn_python_bindings PRIVATE "ext/python/slughorn-freetype.cpp")
	endif()

	if(SLUGHORN_NANOSVG)
		target_sources(slughorn_python_bindings PRIVATE "ext/python/slughorn-nanosvg.cpp")
	endif()

	if(SLUGHORN_TESSELLATE)
		target_sources(slughorn_python_bindings PRIVATE "ext/python/slughorn-tessellate.cpp")
	endif()

	if(SLUGHORN_THORVG)
		target_sources(slughorn_python_bindings PRIVATE "ext/python/slughorn-thorvg.cpp")
	endif()

	if(SLUGHORN_TESSELLATE AND SLUGHORN_CLIPPER2)
		target_sources(slughorn_python_bindings PRIVATE "ext/python/slughorn-bake.cpp")
	endif()

	pybind11_add_module(slughorn_python "ext/python/slughorn.cpp")

	set_target_properties(slughorn_python PROPERTIES
		OUTPUT_NAME "slughorn"
		# On Windows, a MODULE target's companion import library defaults to the same
		# OUTPUT_NAME - colliding with the unrelated `slughorn` STATIC library target's own
		# slughorn.lib in the same output directory under the Visual Studio generator. Worse,
		# slughorn_python links slughorn (via slughorn_python_bindings) PUBLIC, so slughorn.lib
		# is a link INPUT to this exact step, and the linker refuses to overwrite a file it's
		# simultaneously reading (LNK1114, "cannot overwrite the original file", error code 5).
		# Give the import lib its own name; OUTPUT_NAME above still governs the actual .pyd
		# Python imports.
		ARCHIVE_OUTPUT_NAME "slughorn_python"
		POSITION_INDEPENDENT_CODE ON
	)
	target_link_libraries(slughorn_python PRIVATE slughorn_python_bindings)
	target_compile_features(slughorn_python PUBLIC cxx_std_20)
	target_compile_options(slughorn_python PRIVATE ${SLUGHORN_COMPILE_OPTIONS})

	# No separate Python package directory -- the compiled module IS the top-level `slughorn`
	# import. DESTINATION "." puts it at the wheel root (scikit-build-core installs
	# CMAKE_INSTALL_PREFIX as the wheel's site-packages root), so `import slughorn` resolves
	# straight to this .so/.pyd with no wrapper package.
	install(TARGETS slughorn_python DESTINATION .)

	add_custom_target(pytest
		COMMAND ${CMAKE_COMMAND} -E env PYTHONPATH=${CMAKE_BINARY_DIR}
			${Python3_EXECUTABLE} -m pytest
		WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
		DEPENDS slughorn_python
	)
endif()

# =============================================================================
# Tests (optional, enable with -DSLUGHORN_BUILD_TESTS=ON)
# =============================================================================
option(SLUGHORN_BUILD_TESTS "Compile testing files" OFF)

if(SLUGHORN_BUILD_TESTS)
	enable_testing()

	# pkg-config is only used to locate Cairo (optional); Windows/MSVC hosts usually lack it.
	find_package(PkgConfig QUIET)

	# Cairo (optional)
	if(PKG_CONFIG_FOUND)
		pkg_check_modules(CAIRO IMPORTED_TARGET cairo)
	endif()

	if(CAIRO_FOUND)
		add_executable(slughorn-test-cairo "test/slughorn-test-cairo.cpp")
		target_link_libraries(slughorn-test-cairo PRIVATE slughorn PkgConfig::CAIRO)
		add_test(NAME cairo COMMAND slughorn-test-cairo)
		message(STATUS "slughorn tests: Cairo found, building slughorn-test-cairo")
	else()
		message(STATUS "slughorn tests: Cairo not found, skipping slughorn-test-cairo")
	endif()

	# Blend2D (optional - requires SLUGHORN_BLEND2D=ON)
	if(SLUGHORN_BLEND2D)
		add_executable(slughorn-test-blend2d "test/slughorn-test-blend2d.cpp")
		target_link_libraries(slughorn-test-blend2d PRIVATE slughorn)
		add_test(NAME blend2d COMMAND slughorn-test-blend2d)
		message(STATUS "slughorn tests: Blend2D enabled, building slughorn-test-blend2d")
	else()
		message(STATUS "slughorn tests: SLUGHORN_BLEND2D=OFF, skipping slughorn-test-blend2d")
	endif()

	# NanoSVG (optional - requires SLUGHORN_NANOSVG=ON)
	if(SLUGHORN_NANOSVG)
		add_executable(slughorn-test-nanosvg "test/slughorn-test-nanosvg.cpp")

		target_include_directories(slughorn-test-nanosvg PRIVATE
			"${CMAKE_CURRENT_SOURCE_DIR}/ext/nanosvg/src"
		)

		target_link_libraries(slughorn-test-nanosvg PRIVATE slughorn)
		add_test(NAME nanosvg COMMAND slughorn-test-nanosvg)
		message(STATUS "slughorn tests: NanoSVG enabled, building slughorn-test-nanosvg")
	else()
		message(STATUS "slughorn tests: SLUGHORN_NANOSVG=OFF, skipping slughorn-test-nanosvg")
	endif()

	# ThorVG (optional - requires SLUGHORN_THORVG=ON and SLUGHORN_SERIAL=ON; uses NanoSVG,
	# Clipper2 and Tessellate for the parity / mis-winding / bake checks when they are enabled)
	if(SLUGHORN_THORVG AND SLUGHORN_SERIAL)
		add_executable(slughorn-test-thorvg "test/slughorn-test-thorvg.cpp")
		target_link_libraries(slughorn-test-thorvg PRIVATE slughorn)
		target_compile_options(slughorn-test-thorvg PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)
		# stb_image_write (contact sheets) ships with the NanoSVG submodule's examples.
		target_include_directories(slughorn-test-thorvg PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ext/nanosvg/example")
		add_test(NAME thorvg COMMAND slughorn-test-thorvg)
		message(STATUS "slughorn tests: ThorVG enabled, building slughorn-test-thorvg")
	else()
		message(STATUS "slughorn tests: SLUGHORN_THORVG=OFF (or SERIAL=OFF), skipping slughorn-test-thorvg")
	endif()

	# Tessellate (optional - requires SLUGHORN_TESSELLATE=ON)
	if(SLUGHORN_TESSELLATE)
		add_executable(slughorn-test-tessellate "test/slughorn-test-tessellate.cpp")
		target_link_libraries(slughorn-test-tessellate PRIVATE slughorn)
		add_test(NAME tessellate COMMAND slughorn-test-tessellate)
		message(STATUS "slughorn tests: Tessellate enabled, building slughorn-test-tessellate")
	else()
		message(STATUS "slughorn tests: SLUGHORN_TESSELLATE=OFF, skipping slughorn-test-tessellate")
	endif()

	if(SLUGHORN_SERIAL)
		add_executable(slughorn-test-canvas "test/slughorn-test-canvas.cpp")
		target_link_libraries(slughorn-test-canvas PRIVATE slughorn)
		add_test(NAME canvas COMMAND slughorn-test-canvas)
		message(STATUS "slughorn tests: serial found, building slughorn-test-canvas")

		# Convenience targets: regenerate canvas.slug when the binary changes,
		# then inspect it via the CLI without needing to type paths manually.
		#
		# make canvas-list - list all shape + composite keys
		# make canvas-info - atlas summary (dimensions, packing stats)
		set(_CANVAS_SLUG "${CMAKE_CURRENT_BINARY_DIR}/canvas.slug")

		add_custom_command(
			OUTPUT ${_CANVAS_SLUG}
			COMMAND $<TARGET_FILE:slughorn-test-canvas> ${_CANVAS_SLUG}
			DEPENDS slughorn-test-canvas
			COMMENT "Generating canvas.slug"
		)

		add_custom_target(canvas-list
			COMMAND ${CMAKE_SOURCE_DIR}/bin/slughorn list ${_CANVAS_SLUG}
			DEPENDS ${_CANVAS_SLUG}
			USES_TERMINAL
		)

		add_custom_target(canvas-info
			COMMAND ${CMAKE_SOURCE_DIR}/bin/slughorn info ${_CANVAS_SLUG}
			DEPENDS ${_CANVAS_SLUG}
			USES_TERMINAL
		)
		add_executable(slughorn-test-render "test/slughorn-test-render.cpp")
		target_link_libraries(slughorn-test-render PRIVATE slughorn)
		add_test(NAME render COMMAND slughorn-test-render)
		message(STATUS "slughorn tests: building slughorn-test-render")
	else()
		message(STATUS "slughorn tests: SLUGHORN_SERIAL=OFF, skipping slughorn-test-canvas and slughorn-test-render")
	endif()

	# if(SLUGHORN_GLFW)
	# 	find_package(glfw3 REQUIRED)
	# 	add_executable(slughorn-test-glfw "test/slughorn-test-glfw.cpp")
	# 	target_link_libraries(slughorn-test-glfw PRIVATE slughorn glfw glad)
	# 	message(STATUS "slughorn tests: GLFW found, building slughorn-test-glfw")
	# else()
	# 	message(STATUS "slughorn tests: SLUGHORN_GLFW=OFF, skipping slughorn-test-glfw")
	# endif()

	# add_executable(slughorn-test-mvt "test/slughorn-test-mvt.cpp")
	# target_link_libraries(slughorn-test-mvt PRIVATE slughorn)
	# target_compile_options(slughorn-test-mvt PRIVATE ${SLUGHORN_COMPILE_OPTIONS})
	# target_include_directories(slughorn-test-mvt PRIVATE
	# 	"${CMAKE_CURRENT_SOURCE_DIR}/../protozero/include"
	# 	"${CMAKE_CURRENT_SOURCE_DIR}/../vtzero/include"
	# )

	# GDAL (optional)
	# pkg_check_modules(GDAL IMPORTED_TARGET gdal)
	#
	# if(GDAL_FOUND AND SLUGHORN_SERIAL)
	# 	add_executable(slughorn-test-gdal "test/slughorn-test-gdal.cpp")
	# 	target_link_libraries(slughorn-test-gdal PRIVATE slughorn PkgConfig::GDAL)
	# 	target_compile_options(slughorn-test-gdal PRIVATE ${SLUGHORN_COMPILE_OPTIONS})
	# 	message(STATUS "slughorn tests: GDAL found, building slughorn-test-gdal")
	# elseif(NOT GDAL_FOUND)
	# 	message(STATUS "slughorn tests: GDAL not found, skipping slughorn-test-gdal")
	# else()
	# 	message(STATUS "slughorn tests: SLUGHORN_SERIAL=OFF, skipping slughorn-test-gdal")
	# endif()

	# find_package(SQLite3 REQUIRED)
	# find_package(ZLIB REQUIRED)

	# add_executable(slughorn-test-mapbox "test/slughorn-test-mapbox.cpp")
	# target_link_libraries(slughorn-test-mapbox PRIVATE slughorn SQLite::SQLite3 ZLIB::ZLIB)
	# target_compile_options(slughorn-test-mapbox PRIVATE ${SLUGHORN_COMPILE_OPTIONS})
	# target_include_directories(slughorn-test-mapbox PRIVATE
	# 	"${CMAKE_CURRENT_SOURCE_DIR}/../protozero/include"
	# 	"${CMAKE_CURRENT_SOURCE_DIR}/../vtzero/include"
	# )

	if(SLUGHORN_SERIAL)
		add_executable(slughorn-test-bands "test/slughorn-test-bands.cpp")
		target_link_libraries(slughorn-test-bands PRIVATE slughorn)
		target_compile_options(slughorn-test-bands PRIVATE ${SLUGHORN_COMPILE_OPTIONS})
		message(STATUS "slughorn tests: serial found, building slughorn-test-bands")
	else()
		message(STATUS "slughorn tests: SLUGHORN_SERIAL=OFF, skipping slughorn-test-bands")
	endif()

	add_executable(slughorn-test-scanlinesweep "test/slughorn-test-scanlinesweep.cpp")
	target_link_libraries(slughorn-test-scanlinesweep PRIVATE slughorn)
	target_compile_options(slughorn-test-scanlinesweep PRIVATE ${SLUGHORN_COMPILE_OPTIONS})
	add_test(NAME scanlinesweep COMMAND slughorn-test-scanlinesweep)
endif()

# Let's be honest... I mostly care about the frontends I care about! So, these
# will all likely be AI-generated things (for now).
option(SLUGHORN_BUILD_EXAMPLES "Build example programs" OFF)

if(SLUGHORN_BUILD_EXAMPLES)
	add_subdirectory("example")
endif()
