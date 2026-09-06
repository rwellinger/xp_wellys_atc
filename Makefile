SHELL := /bin/bash

XPLANE_ROOT := /Users/robertw/X-Plane 12
PLUGIN_DIR  := $(XPLANE_ROOT)/Resources/available plugins/xp_wellys_vfr_atc

SDK_SENTINEL    := sdk/XPLM/XPLMPlugin.h
IMGUI_SENTINEL  := vendor/imgui/imgui.h
JSON_SENTINEL   := vendor/json.hpp
CATCH2_SENTINEL := vendor/catch2/catch_amalgamated.hpp

# miniaudio: single-header capture backend, Windows-only (issue #21). The
# macOS build never references it (the #elif defined(_WIN32) branch in
# audio_recorder.cpp is dead code on Apple), but `make setup` still fetches
# it so the vendor tree is complete for anyone cross-checking the Windows
# slice locally.
MINIAUDIO_VERSION  := 0.11.25
MINIAUDIO_SENTINEL := vendor/miniaudio.h

# Prebuilt local-inference bundle (xp_wellys_libs). The heavy third-party
# trees (whisper/llama/ggml/Piper/espeak) are compiled ONCE in that repo and
# consumed here as a versioned arm64 binary bundle — no local compile, no
# submodules. Version pinned in PREBUILT_LIBS_VERSION; bump it in lockstep
# with a bundle release. The sentinel is one archive from the extracted tree.
PREBUILT_LIBS_VERSION := $(shell cat PREBUILT_LIBS_VERSION)
PREBUILT_LIBS_REPO    := rwellinger/xp_wellys_libs
PREBUILT_LIBS_DIR     := vendor/prebuilt/xp_wellys_libs
PREBUILT_SENTINEL     := $(PREBUILT_LIBS_DIR)/lib/libwhisper.a

CATCH2_VERSION := 3.15.1

# SkunkCrafts Updater: staging dir for the publishable release tree.
# Lives under build/ so `make clean` removes it. The tree mirrors the
# installed plugin MINUS the in-sim-downloaded models and runtime logs,
# plus the generated skunkcrafts_updater_*.txt control files.
SKUNK_DIR := build/skunkcrafts

.PHONY: all help setup setup-cloud build ci-fast ci-remote win-artifact win-zip install clean distclean format lint sanitize release release-build skunkcrafts verify-install cleanup-tags cleanup-branches cleanup-runs repl run-repl test test-unit test-scenarios

.DEFAULT_GOAL := help

all: clean format build lint test

# ── Help ──────────────────────────────────────────────────────────────────────
help:
	@echo "xp_wellys_vfr_atc - Makefile targets"
	@echo ""
	@echo "  make                   Show this help (default)"
	@echo "  make all               clean + format + build + lint"
	@echo "  make setup             Download prebuilt xp_wellys_libs bundle + X-Plane SDK, Dear ImGui, nlohmann/json, Catch2"
	@echo "  make setup-cloud       Cloud-only deps (SDK, ImGui, json, Catch2) WITHOUT the local-inference bundle — for the fast CI sanity build"
	@echo "  make build             Build universal plugin (arm64 local+cloud, x86_64 cloud-only) -> build/xp_wellys_vfr_atc.xpl"
	@echo "  make ci-fast           Fast cloud-only arm64 sanity build + unit/scenario tests (no submodules, no local backends)"
	@echo "  make ci-remote         Trigger the GitHub CI (fast macOS + Windows slice) on the current branch via gh (builds the PUSHED state)"
	@echo "  make win-artifact      Download the newest Windows CI artifact via gh -> dist-win/ + transfer zips"
	@echo "  make win-zip           Re-pack dist-win/ into transfer zips (no download)"
	@echo "  make repl              Build headless CLI -> build/atc_repl"
	@echo "  make run-repl          Build + run the CLI (stdin transcripts)"
	@echo "  make test              Run unit tests + scenario tests"
	@echo "  make test-unit         Build + run Catch2 unit tests"
	@echo "  make test-scenarios    Build + run all scenario tests in testscripts/"
	@echo "  make install           Code-sign and install plugin to X-Plane"
	@echo "  make format            Run clang-format on src/*.cpp src/*.hpp"
	@echo "  make lint              Run clang-tidy on src/*.cpp"
	@echo "  make sanitize          Build atc_repl + tests with ASan+UBSan and run them"
	@echo "  make release VERSION=X Tag and push release (writes VERSION.txt)"
	@echo "  make release-build     Build plugin with RELEASE=ON (embeds VERSION.txt)"
	@echo "  make skunkcrafts       Stage SkunkCrafts Updater release tree -> $(SKUNK_DIR) (after 'make install')"
	@echo "  make cleanup-tags      Prune local tags no longer on origin"
	@echo "  make cleanup-branches  Prune local branches whose remote is gone"
	@echo "  make cleanup-runs      Delete all GitHub Actions runs except the newest per workflow"
	@echo "  make clean             Remove build/, build-lint/ and build-sanitize/"
	@echo "  make distclean         clean + remove sdk/ and vendor/ (everything 'make setup' installed)"
	@echo "  make help              Show this help"

# ── Setup ─────────────────────────────────────────────────────────────────────
setup: $(PREBUILT_SENTINEL) $(SDK_SENTINEL) $(IMGUI_SENTINEL) $(JSON_SENTINEL) $(CATCH2_SENTINEL) $(MINIAUDIO_SENTINEL)
	@echo "Setup complete. Run 'make build' to compile."

# Cloud-only setup: the four non-bundle vendor deps only. Deliberately skips
# the prebuilt xp_wellys_libs bundle — the cloud-only slice
# (XPWELLYS_USE_LOCAL_INFERENCE=OFF) never references whisper/llama/Piper, so
# the fast CI sanity build (`make ci-fast`) needs neither the bundle download
# nor any local-inference libraries.
setup-cloud: $(SDK_SENTINEL) $(IMGUI_SENTINEL) $(JSON_SENTINEL) $(CATCH2_SENTINEL) $(MINIAUDIO_SENTINEL)
	@echo "Cloud-only setup complete (no submodules). Run 'make ci-fast'."

$(SDK_SENTINEL):
	@echo "Downloading X-Plane SDK..."
	@set -euo pipefail; \
	TMP=$$(mktemp -d); \
	trap "rm -rf $$TMP" EXIT; \
	curl -fsSL "https://developer.x-plane.com/wp-content/plugins/code-sample-generation/sdk_zip_files/XPSDK430.zip" \
	     -o "$$TMP/sdk.zip"; \
	unzip -q "$$TMP/sdk.zip" -d "$$TMP/sdk_extracted"; \
	mkdir -p sdk/XPLM sdk/XPWidgets sdk/Libraries/Win sdk/Libraries/Mac; \
	find "$$TMP/sdk_extracted" -path "*/CHeaders/XPLM/*.h"   -exec cp {} sdk/XPLM/ \;; \
	find "$$TMP/sdk_extracted" -path "*/CHeaders/Widgets/*.h" -exec cp {} sdk/XPWidgets/ \;; \
	find "$$TMP/sdk_extracted" -path "*/Libraries/Win/*.lib"  -exec cp {} sdk/Libraries/Win/ \;; \
	cp -R "$$TMP/sdk_extracted"/*/Libraries/Mac/*.framework sdk/Libraries/Mac/ 2>/dev/null || \
	find "$$TMP/sdk_extracted" -name "*.framework" -exec cp -R {} sdk/Libraries/Mac/ \;
	@echo "SDK headers installed."

$(IMGUI_SENTINEL):
	@echo "Downloading Dear ImGui v1.92.8..."
	@set -euo pipefail; \
	TMP=$$(mktemp -d); \
	trap "rm -rf $$TMP" EXIT; \
	mkdir -p vendor/imgui/backends; \
	curl -fsSL "https://github.com/ocornut/imgui/archive/refs/tags/v1.92.8.zip" -o "$$TMP/imgui.zip"; \
	unzip -q "$$TMP/imgui.zip" -d "$$TMP/"; \
	SRC="$$TMP/imgui-1.92.8"; \
	cp "$$SRC"/imgui.{h,cpp} vendor/imgui/; \
	cp "$$SRC"/imgui_{draw,tables,widgets}.cpp vendor/imgui/; \
	cp "$$SRC"/imgui_internal.h "$$SRC"/imconfig.h vendor/imgui/; \
	cp "$$SRC"/imstb_textedit.h "$$SRC"/imstb_rectpack.h "$$SRC"/imstb_truetype.h vendor/imgui/ 2>/dev/null || true; \
	cp "$$SRC"/backends/imgui_impl_opengl2.{h,cpp} vendor/imgui/backends/
	@echo "Dear ImGui installed."

$(JSON_SENTINEL):
	@echo "Downloading nlohmann/json v3.12.0..."
	@mkdir -p vendor
	@curl -fsSL "https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp" \
	     -o vendor/json.hpp
	@echo "nlohmann/json installed."

$(MINIAUDIO_SENTINEL):
	@echo "Downloading miniaudio v$(MINIAUDIO_VERSION)..."
	@mkdir -p vendor
	@curl -fsSL "https://raw.githubusercontent.com/mackron/miniaudio/$(MINIAUDIO_VERSION)/miniaudio.h" \
	     -o vendor/miniaudio.h
	@echo "miniaudio installed."

# Download + extract + SHA256-verify the prebuilt xp_wellys_libs bundle.
# Public GitHub release, so a plain curl works with no auth (local or CI).
# The manifest's `<sha256>  <name>` lines are checked against the extracted
# lib/ tree; a mismatch fails the build (never link an unverified binary).
$(PREBUILT_SENTINEL):
	@echo "Downloading xp_wellys_libs bundle v$(PREBUILT_LIBS_VERSION)..."
	@mkdir -p $(PREBUILT_LIBS_DIR)
	@curl -fsSL "https://github.com/$(PREBUILT_LIBS_REPO)/releases/download/v$(PREBUILT_LIBS_VERSION)/xp_wellys_libs-arm64-macos-$(PREBUILT_LIBS_VERSION).tar.gz" \
	     -o $(PREBUILT_LIBS_DIR)/bundle.tar.gz
	@tar -xzf $(PREBUILT_LIBS_DIR)/bundle.tar.gz -C $(PREBUILT_LIBS_DIR)
	@rm -f $(PREBUILT_LIBS_DIR)/bundle.tar.gz
	@echo "Verifying bundle SHA256 against manifest.txt..."
	@cd $(PREBUILT_LIBS_DIR)/lib && grep -E '^[0-9a-f]{64}  ' ../manifest.txt | shasum -a 256 -c -
	@echo "xp_wellys_libs bundle v$(PREBUILT_LIBS_VERSION) installed + verified."

$(CATCH2_SENTINEL):
	@echo "Downloading Catch2 v$(CATCH2_VERSION) (amalgamated)..."
	@set -euo pipefail; \
	TMP=$$(mktemp -d); \
	trap "rm -rf $$TMP" EXIT; \
	mkdir -p vendor/catch2; \
	curl -fsSL "https://github.com/catchorg/Catch2/archive/refs/tags/v$(CATCH2_VERSION).tar.gz" \
	     -o "$$TMP/catch2.tar.gz"; \
	tar -xzf "$$TMP/catch2.tar.gz" -C "$$TMP/"; \
	cp "$$TMP/Catch2-$(CATCH2_VERSION)/extras/catch_amalgamated.hpp" vendor/catch2/; \
	cp "$$TMP/Catch2-$(CATCH2_VERSION)/extras/catch_amalgamated.cpp" vendor/catch2/
	@echo "Catch2 installed."

# ── Build ─────────────────────────────────────────────────────────────────────
# Always produces a universal xp_wellys_vfr_atc.xpl that contains both an
# arm64 slice (whisper.cpp + llama.cpp + Piper + OpenAI) and an x86_64
# slice (OpenAI only — Metal + the onnxruntime prebuilt are Apple
# Silicon only). The two slices share src/ and CMakeLists.txt but
# differ via -DXPWELLYS_USE_LOCAL_INFERENCE.
#
# Strategy: two separate CMake configures (build-arm64/, build-x86_64/),
# each producing its own .xpl, then lipo-merged into build/xp_wellys_vfr_atc.xpl.
# The arm64 slice ships libpiper.dylib + libonnxruntime.dylib next to the
# .xpl so they're picked up by @loader_path; the x86_64 slice has no
# such dylibs to ship.
#
# `make install` copies build/xp_wellys_vfr_atc.xpl into the plugin dir
# together with the staged dylibs.
# `RELEASE_FLAG` is passed through to both CMake configure calls so the
# GitHub Actions workflow can flip `-DRELEASE=ON` for tag-driven
# release builds without duplicating the logic. Empty by default
# (regular dev build); `release-build` sets it to `-DRELEASE=ON`.
RELEASE_FLAG ?=

build: $(PREBUILT_SENTINEL) $(SDK_SENTINEL) $(IMGUI_SENTINEL) $(JSON_SENTINEL) $(CATCH2_SENTINEL)
	@echo "=== Building universal xp_wellys_vfr_atc (arm64 local+cloud, x86_64 cloud-only) ==="
	@echo ""
	@echo "--- arm64 slice (local + cloud) ---"
	cmake -B build-arm64 -DCMAKE_BUILD_TYPE=Release $(RELEASE_FLAG) \
	    -DCMAKE_OSX_ARCHITECTURES=arm64 \
	    -DXPWELLYS_USE_LOCAL_INFERENCE=ON \
	    -DBUILD_TESTS=OFF \
	    -Wno-dev
	cmake --build build-arm64 --parallel
	@echo ""
	@echo "--- x86_64 slice (cloud-only) ---"
	@# Local TTS (Piper) is explicitly OFF here until the x86_64-macOS
	@# Piper prebuilt lands (#71/#72); XPWELLYS_USE_LOCAL_TTS defaults ON,
	@# so it must be forced OFF to keep this slice cloud-only.
	cmake -B build-x86_64 -DCMAKE_BUILD_TYPE=Release $(RELEASE_FLAG) \
	    -DCMAKE_OSX_ARCHITECTURES=x86_64 \
	    -DXPWELLYS_USE_LOCAL_INFERENCE=OFF \
	    -DXPWELLYS_USE_LOCAL_TTS=OFF \
	    -DBUILD_TESTS=OFF \
	    -Wno-dev
	cmake --build build-x86_64 --parallel
	@echo ""
	@echo "--- lipo merge ---"
	@mkdir -p build
	lipo -create \
	    build-arm64/xp_wellys_vfr_atc.xpl \
	    build-x86_64/xp_wellys_vfr_atc.xpl \
	    -output build/xp_wellys_vfr_atc.xpl
	@# Stage the arm64 slice's dylibs next to the merged binary so
	@# `make install` finds them where it expects. The x86_64 slice
	@# has no dylib dependencies (cloud-only build).
	@cp build-arm64/libpiper.dylib              build/
	@cp build-arm64/libonnxruntime.1.22.0.dylib build/
	@cp build-arm64/libonnxruntime.dylib        build/
	@cp -R build-arm64/espeak_ng-install        build/ 2>/dev/null || true
	@echo ""
	@file build/xp_wellys_vfr_atc.xpl
	@lipo -info build/xp_wellys_vfr_atc.xpl
	@echo "Done. Run 'make install' to deploy the universal .xpl."

# ── CI fast sanity build ──────────────────────────────────────────────────────
# Single cloud-only arm64 configure that builds the tests, the headless
# REPL and the plugin (all with XPWELLYS_USE_LOCAL_INFERENCE=OFF), then runs
# the unit + scenario suites. No whisper/llama/Piper/espeak-ng/onnxruntime,
# so no submodules are required (see `setup-cloud`). This is the fast macOS
# gate for all non-tag pushes (incl. main merges); the full universal
# *release* `make build` only runs on release tags, while main pushes run the
# deps cache-warm job (`warm-deps`) instead. Uses its own build-ci/ dir so it never
# clobbers the release build/ trees. Depends on the four non-submodule
# vendor sentinels only.
ci-fast: $(SDK_SENTINEL) $(IMGUI_SENTINEL) $(JSON_SENTINEL) $(CATCH2_SENTINEL)
	@echo "=== Fast cloud-only sanity build (arm64, no local backends) ==="
	@# Both local flags OFF: this fast build has no prebuilt bundle
	@# (no PREBUILT_SENTINEL dep), so XPWELLYS_USE_LOCAL_TTS (default ON)
	@# must be forced OFF or bundle discovery would FATAL_ERROR.
	cmake -B build-ci -DCMAKE_BUILD_TYPE=Release \
	    -DCMAKE_OSX_ARCHITECTURES=arm64 \
	    -DXPWELLYS_USE_LOCAL_INFERENCE=OFF \
	    -DXPWELLYS_USE_LOCAL_TTS=OFF \
	    -DBUILD_TESTS=ON \
	    -Wno-dev
	cmake --build build-ci --target xp_wellys_vfr_atc_tests atc_repl xp_wellys_vfr_atc --parallel
	@echo ""
	@echo "=== Running unit tests ==="
	@# Same rand+fixed-seed regime as `make test` (see Issue #3).
	@./build-ci/xp_wellys_vfr_atc_tests --order rand --rng-seed 42
	@echo ""
	@echo "=== Running scenario tests ==="
	./build-ci/atc_repl run testscripts/*.json
	@echo ""
	@file build-ci/xp_wellys_vfr_atc.xpl
	@echo "Fast sanity build clean."

# ── Remote CI trigger (GitHub Actions) ────────────────────────────────────────
# There is NO local Windows toolchain — the Windows .xpl (and a fresh macOS
# sanity build) can only be produced by GitHub Actions. This fires the `Build`
# workflow's `workflow_dispatch` trigger against the CURRENT branch, which runs
# `build-macos-fast` + `build-windows` (NOT the tag-only universal build /
# release). Grab the result afterwards with `make win-artifact`.
#
# IMPORTANT: CI builds the state PUSHED to the branch on GitHub, never your
# local worktree. Commit + push first, or the run compiles stale sources.
# The workflow_dispatch trigger itself must already exist on the branch's
# build.yml on GitHub (i.e. this Makefile+workflow change has to be pushed once
# before `make ci-remote` can find it).
ci-remote:
	@command -v gh >/dev/null 2>&1 || { \
	    echo "gh not found. Install with: brew install gh (then: gh auth login)"; exit 1; }
	@set -euo pipefail; \
	BRANCH=$$(git rev-parse --abbrev-ref HEAD); \
	if [ -n "$$(git status --porcelain)" ]; then \
	    echo "WARNING: uncommitted changes — CI builds the PUSHED state of '$$BRANCH', not your worktree."; \
	fi; \
	if [ -n "$$(git log origin/$$BRANCH..$$BRANCH 2>/dev/null || true)" ]; then \
	    echo "WARNING: local commits not on origin/$$BRANCH — push first so CI sees them."; \
	fi; \
	echo "Triggering the Build workflow (workflow_dispatch) on '$$BRANCH'..."; \
	gh workflow run build.yml --ref "$$BRANCH"; \
	echo ""; \
	echo "Started. Track it with:"; \
	echo "    gh run list --workflow=build.yml"; \
	echo "    gh run watch"; \
	echo "Then fetch the Windows drop-in with: make win-artifact"

# Download the freshest Windows CI artifact into dist-win/ (drop-in tree:
# win_x64/xp_wellys_vfr_atc.xpl + data/). Pulls from the most recent run
# that produced the artifact, then packs two zips for transfer to the test box.
#
# Zipped rather than raw files on purpose: a zip carries a per-entry CRC, so a
# truncated transfer fails loudly at unpack time instead of looking like a
# broken DLL — which is exactly the kind of ghost that costs an afternoon. It
# also cuts ~110 MB to ~30 MB and turns hundreds of files into one, which
# matters over a remote-desktop file channel.
WIN_ZIP_FULL   := dist-win/xp_wellys_vfr_atc_win_full.zip
WIN_ZIP_BINARY := dist-win/xp_wellys_vfr_atc_win_x64.zip

win-artifact:
	@command -v gh >/dev/null 2>&1 || { \
	    echo "gh not found. Install with: brew install gh (then: gh auth login)"; exit 1; }
	@rm -rf dist-win && mkdir -p dist-win
	@echo "Downloading newest xp_wellys_vfr_atc-win artifact -> dist-win/ ..."
	@gh run download -n xp_wellys_vfr_atc-win -D dist-win
	@$(MAKE) --no-print-directory win-zip

# Pack dist-win/ without re-downloading. Split in two because the usual case is
# a code change, where only the .xpl moved: win_x64 alone is the smaller, faster
# transfer. The full tree is for a first install or when data/ changed.
win-zip:
	@test -d dist-win/win_x64 || { \
	    echo "dist-win/win_x64 missing - run 'make win-artifact' first"; exit 1; }
	@rm -f $(WIN_ZIP_FULL) $(WIN_ZIP_BINARY)
	@cd dist-win && zip -q -r $(notdir $(WIN_ZIP_BINARY)) win_x64
	@cd dist-win && zip -q -r $(notdir $(WIN_ZIP_FULL)) . -x '*.zip'
	@echo ""
	@echo "Windows drop-in ready:"
	@find dist-win -name '*.xpl' -print
	@echo ""
	@ls -lh $(WIN_ZIP_BINARY) $(WIN_ZIP_FULL) | awk '{printf "  %-52s %s\n", $$9, $$5}'
	@echo ""
	@echo "  binary zip : win_x64/ only - the usual update (just the .xpl moved)"
	@echo "  full zip   : whole tree - first install, or when data/ changed"
	@echo ""
	@echo "On the target, after unpacking, clear the download mark:"
	@echo "  Get-ChildItem <plugin>\\win_x64 | Unblock-File"

# ── REPL (headless CLI) ───────────────────────────────────────────────────────
repl: $(PREBUILT_SENTINEL) $(SDK_SENTINEL) $(IMGUI_SENTINEL) $(JSON_SENTINEL) $(CATCH2_SENTINEL)
	@echo "=== Building atc_repl ==="
	cmake -B build -DCMAKE_BUILD_TYPE=Release -Wno-dev
	cmake --build build --target atc_repl --parallel
	@echo ""
	@file build/atc_repl
	@echo "Done. Run 'make run-repl' or './build/atc_repl'."

run-repl: repl
	./build/atc_repl

# ── Tests ─────────────────────────────────────────────────────────────────────
test: test-unit test-scenarios

test-unit: $(PREBUILT_SENTINEL) $(SDK_SENTINEL) $(IMGUI_SENTINEL) $(JSON_SENTINEL) $(CATCH2_SENTINEL)
	@echo "=== Building xp_wellys_vfr_atc unit tests ==="
	cmake -B build -DCMAKE_BUILD_TYPE=Release -Wno-dev
	cmake --build build --target xp_wellys_vfr_atc_tests --parallel
	@echo ""
	@echo "=== Running unit tests ==="
	@# --order rand with a FIXED seed: reproducible in CI AND keeps Catch2's
	@# latent-bug detector on (random order surfaces shared-global-state
	@# coupling between cases). Per-test isolation is enforced by the
	@# module-reset listener (tests/module_reset_listener.cpp), which resets
	@# the engine module globals before every case. See Issue #3. Override
	@# the seed locally to hunt new leaks, e.g.:
	@#   ./build/xp_wellys_vfr_atc_tests --order rand --rng-seed 7
	@./build/xp_wellys_vfr_atc_tests --order rand --rng-seed 42

test-scenarios: repl
	@echo "=== Running scenario tests ==="
	./build/atc_repl run testscripts/*.json

# ── Install ───────────────────────────────────────────────────────────────────
install:
	@if [ ! -f "build/xp_wellys_vfr_atc.xpl" ]; then \
	    echo "Plugin not built yet. Run 'make build' first."; exit 1; \
	fi
	@if [ ! -f "build/libpiper.dylib" ] || [ ! -f "build/libonnxruntime.1.22.0.dylib" ]; then \
	    echo "Runtime dylibs missing in build/. Did 'make build' succeed?"; exit 1; \
	fi
	@echo "=== Installing xp_wellys_vfr_atc ==="
	@mkdir -p "$(PLUGIN_DIR)/mac_x64"
	@cp build/xp_wellys_vfr_atc.xpl "$(PLUGIN_DIR)/mac_x64/"
	@cp build/libpiper.dylib "$(PLUGIN_DIR)/mac_x64/"
	@cp build/libonnxruntime.1.22.0.dylib "$(PLUGIN_DIR)/mac_x64/"
	@cp build/libonnxruntime.dylib "$(PLUGIN_DIR)/mac_x64/"
	@xattr -dr com.apple.quarantine "$(PLUGIN_DIR)/mac_x64/xp_wellys_vfr_atc.xpl" 2>/dev/null || true
	@xattr -dr com.apple.quarantine "$(PLUGIN_DIR)/mac_x64/libpiper.dylib" 2>/dev/null || true
	@xattr -dr com.apple.quarantine "$(PLUGIN_DIR)/mac_x64/libonnxruntime.1.22.0.dylib" 2>/dev/null || true
	@# Strip the dev-time rpaths (build/, source-tree onnxruntime path)
	@# baked in by CMake and replace with @loader_path so the .xpl finds
	@# the dylibs we just copied next to it.
	@for rp in $$(otool -l "$(PLUGIN_DIR)/mac_x64/xp_wellys_vfr_atc.xpl" \
	    | awk '/LC_RPATH/{flag=1; next} flag && /path/ {print $$2; flag=0}'); do \
	    install_name_tool -delete_rpath "$$rp" "$(PLUGIN_DIR)/mac_x64/xp_wellys_vfr_atc.xpl" 2>/dev/null || true; \
	done
	@install_name_tool -add_rpath "@loader_path" "$(PLUGIN_DIR)/mac_x64/xp_wellys_vfr_atc.xpl"
	@codesign --force --deep --sign - "$(PLUGIN_DIR)/mac_x64/libonnxruntime.1.22.0.dylib"
	@codesign --force --deep --sign - "$(PLUGIN_DIR)/mac_x64/libpiper.dylib"
	@codesign --force --deep --sign - "$(PLUGIN_DIR)/mac_x64/xp_wellys_vfr_atc.xpl"
	@# Bundle espeak-ng-data (~19 MB) inside the plugin so Piper's
	@# phonemizer finds its dictionary at runtime via the plugin-relative
	@# path resolved by model_paths::espeakng_data_dir(). Models live in
	@# Resources/models/ and are downloaded by the user on first launch
	@# (P5); espeak-ng-data is part of the .xpl bundle, NOT downloaded.
	@if [ -d "build/espeak_ng-install/share/espeak-ng-data" ]; then \
	    mkdir -p "$(PLUGIN_DIR)/Resources/espeak-ng-data"; \
	    rsync -a --delete \
	        "build/espeak_ng-install/share/espeak-ng-data/" \
	        "$(PLUGIN_DIR)/Resources/espeak-ng-data/"; \
	    echo "Installed: $(PLUGIN_DIR)/Resources/espeak-ng-data/"; \
	else \
	    echo "WARNING: build/espeak_ng-install/share/espeak-ng-data missing — run make build first"; \
	fi
	@# Models live under Resources/models/. Created empty here so the
	@# in-plugin downloader has a target dir on first launch even
	@# before the user has downloaded anything.
	@mkdir -p "$(PLUGIN_DIR)/Resources/models"
	@mkdir -p "$(PLUGIN_DIR)/data"
	@if [ ! -f "$(PLUGIN_DIR)/data/settings.json" ]; then \
	    cp data/settings.json "$(PLUGIN_DIR)/data/"; \
	    echo "Installed: $(PLUGIN_DIR)/data/settings.json"; \
	else \
	    echo "Kept existing settings.json"; \
	fi
	@cp data/atc_prompt_templates.json "$(PLUGIN_DIR)/data/"
	@echo "Installed: $(PLUGIN_DIR)/data/atc_prompt_templates.json"
	@# Always overwrite the models catalog — slugs and Piper voice
	@# hashes are baked into the bundled JSON, and a stale user copy
	@# (e.g. carried over from a pre-catalog install) would silently
	@# point the loader at outdated entries. Per-user customization
	@# happens via Settings, not by hand-editing this file.
	@cp data/models_catalog.json "$(PLUGIN_DIR)/data/"
	@echo "Installed: $(PLUGIN_DIR)/data/models_catalog.json"
	@# Install BOTH profile bundles in full — every *.json, including
	@# conformance.json. The active profile is chosen at runtime via
	@# atc_language(); previously only de/ (and not even its
	@# conformance.json) was copied, so EN mode found no ui_strings and
	@# fell back to raw keys ("airport.frequencies_header" etc.). Globbing
	@# *.json keeps this correct if a bundle gains a new file.
	@for lang in de en; do \
	    mkdir -p "$(PLUGIN_DIR)/data/atc_profiles/$$lang"; \
	    cp data/atc_profiles/$$lang/*.json "$(PLUGIN_DIR)/data/atc_profiles/$$lang/"; \
	    echo "Installed: $(PLUGIN_DIR)/data/atc_profiles/$$lang/*.json"; \
	done
	@mkdir -p "$(PLUGIN_DIR)/data/vrps"
	@cp data/vrps/airport_vrps.json "$(PLUGIN_DIR)/data/vrps/"
	@echo "Installed: $(PLUGIN_DIR)/data/vrps/airport_vrps.json"
	@mkdir -p "$(PLUGIN_DIR)/data/airspaces"
	@cp data/airspaces/de_airspace.txt "$(PLUGIN_DIR)/data/airspaces/"
	@echo "Installed: $(PLUGIN_DIR)/data/airspaces/de_airspace.txt"
	@# Cleanup of legacy paths from pre-Phase-B installs (region-scoped
	@# data + top-level JSONs from the era before the regions folder).
	@rm -f "$(PLUGIN_DIR)/data/atc_templates.json" \
	       "$(PLUGIN_DIR)/data/flight_rules.json" \
	       "$(PLUGIN_DIR)/data/airport_vrps.json"
	@rm -rf "$(PLUGIN_DIR)/data/regions"
	@echo "Installed and signed."

# ── Lint ──────────────────────────────────────────────────────────────────────
format:
	@command -v clang-format >/dev/null 2>&1 || { \
	    echo "clang-format not found. Install with: brew install llvm"; \
	    echo "Then add to PATH: export PATH=\"$$(brew --prefix llvm)/bin:$$PATH\""; \
	    exit 1; }
	clang-format -i src/main.cpp src/*/*.cpp src/*/*.hpp

lint: $(PREBUILT_SENTINEL) $(SDK_SENTINEL) $(IMGUI_SENTINEL) $(JSON_SENTINEL) $(CATCH2_SENTINEL)
	@command -v clang-tidy >/dev/null 2>&1 || { \
	    echo "clang-tidy not found. Install with: brew install llvm"; \
	    echo "Then add to PATH: export PATH=\"$$(brew --prefix llvm)/bin:$$PATH\""; \
	    exit 1; }
	cmake -B build-lint -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_OSX_ARCHITECTURES=arm64 -Wno-dev
	# Exclude the Windows-only *_win.cpp bridges: CMake selects the .mm
	# counterparts on macOS, so those TUs are absent from the compile DB
	# and pull <windows.h>, which this (macOS) toolchain cannot resolve.
	clang-tidy -p build-lint --extra-arg="-isysroot" --extra-arg="$(shell xcrun --show-sdk-path)" $(shell find src -maxdepth 2 -name '*.cpp' ! -name '*_win.cpp')

# ── Sanitize ──────────────────────────────────────────────────────────────────
# AddressSanitizer + UBSan on the SDK-free engine OBJECT lib + atc_repl +
# Catch2 tests. The plugin module (`xp_wellys_vfr_atc.xpl`) is NOT instrumented —
# ASan inside the X-Plane process is fragile on macOS ARM64. For runtime
# leaks in the live plugin use Instruments.app (Leaks / Allocations
# templates) attached to the X-Plane process.
#
# Findings abort with a non-zero exit (`-fno-sanitize-recover=all`), so this
# target is CI-friendly. Build dir is `build-sanitize/` — independent of
# `build/` so Release artifacts stay untouched.
sanitize: $(PREBUILT_SENTINEL) $(SDK_SENTINEL) $(IMGUI_SENTINEL) $(JSON_SENTINEL) $(CATCH2_SENTINEL)
	@echo "=== Configuring sanitizer build (ASan + UBSan) ==="
	cmake -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DXP_WELLYS_ATC_SANITIZE=ON -Wno-dev
	@echo "=== Building atc_repl + xp_wellys_vfr_atc_tests with ASan + UBSan ==="
	cmake --build build-sanitize --target atc_repl xp_wellys_vfr_atc_tests --parallel
	@echo ""
	@echo "=== Running unit tests under ASan + UBSan ==="
	@ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:print_stacktrace=1 \
	 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
	     ./build-sanitize/xp_wellys_vfr_atc_tests
	@echo ""
	@echo "=== Running scenario tests under ASan + UBSan ==="
	@ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:print_stacktrace=1 \
	 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
	     ./build-sanitize/atc_repl run testscripts/*.json
	@echo ""
	@echo "Sanitizer run clean."

# ── Release ───────────────────────────────────────────────────────────────────
release:
	@if [ -z "$(VERSION)" ]; then \
	    echo "Usage: make release VERSION=1.2.1"; exit 1; \
	fi
	@if ! git diff --quiet || ! git diff --cached --quiet; then \
	    echo "Uncommitted changes present. Commit or stash first."; exit 1; \
	fi
	@if [ -n "$$(git ls-files --others --exclude-standard)" ]; then \
	    echo "Untracked files present. Commit or clean up first."; exit 1; \
	fi
	@# The tag is what CI builds, and it is cut from HEAD. Releasing from a
	@# feature branch or from a main that is behind origin therefore ships an
	@# older tree than you think — the artifacts are consistent, just not the
	@# code you expected. Both are cheap to rule out, so rule them out.
	@BR="$$(git rev-parse --abbrev-ref HEAD)"; \
	if [ "$$BR" != "main" ]; then \
	    echo "On branch '$$BR', not main. Releases are cut from main."; exit 1; \
	fi
	@git fetch -q origin main
	@if [ -n "$$(git rev-list HEAD..origin/main)" ]; then \
	    echo "main is behind origin/main by $$(git rev-list --count HEAD..origin/main) commit(s)."; \
	    echo "Run 'git pull' first — otherwise the tag would ship an older tree."; exit 1; \
	fi
	@if git rev-parse -q --verify "refs/tags/v$(VERSION)" >/dev/null; then \
	    echo "Tag v$(VERSION) already exists locally."; exit 1; \
	fi
	@echo "$(VERSION)" > VERSION.txt
	@git add VERSION.txt
	@git commit -m "release $(VERSION)"
	@git push origin main
	@git tag -a "v$(VERSION)" -m "Release $(VERSION)"
	@git push origin "v$(VERSION)"
	@echo "Released v$(VERSION) and pushed tag to origin."

release-build:
	@$(MAKE) build RELEASE_FLAG=-DRELEASE=ON
	@echo "Done. Universal release build with version from VERSION.txt."

# ── SkunkCrafts Updater release tree ──────────────────────────────────────────
# Stages a clean, publishable copy of the installed plugin into
# $(SKUNK_DIR) and generates the SkunkCrafts control files against it.
# The in-sim-downloaded models (~2 GB) and runtime flight logs are
# excluded — they are deliberately NOT managed by the updater (untracked
# files survive every update; blacklisting them would DELETE them).
# Version comes from VERSION=x.y.z, falling back to VERSION.txt.
# Publish the contents of $(SKUNK_DIR)/ to the `release` branch (or your
# HTTPS host) that tools/skunkcrafts/skunkcrafts_updater.cfg.template's
# `module` URL points at.
skunkcrafts:
	@if [ ! -d "$(PLUGIN_DIR)" ]; then \
	    echo "Plugin not installed at '$(PLUGIN_DIR)'. Run 'make install' first."; exit 1; \
	fi
	@VER="$(VERSION)"; \
	if [ -z "$$VER" ] && [ -f VERSION.txt ]; then VER="$$(cat VERSION.txt)"; fi; \
	if [ -z "$$VER" ]; then \
	    echo "No version. Set VERSION=x.y.z or populate VERSION.txt."; exit 1; \
	fi; \
	echo "=== Staging SkunkCrafts release tree ($$VER) ==="; \
	rm -rf "$(SKUNK_DIR)"; mkdir -p "$(SKUNK_DIR)"; \
	rsync -a \
	    --exclude 'Resources/models/' \
	    --exclude 'data/flightlog/' \
	    --exclude '.DS_Store' \
	    --exclude 'skunkcrafts_updater*' \
	    "$(PLUGIN_DIR)/" "$(SKUNK_DIR)/"; \
	python3 tools/skunkcrafts/generate.py --tree "$(SKUNK_DIR)" --version "$$VER"; \
	echo "Staged release tree at $(SKUNK_DIR)/ (version $$VER)."; \
	echo "Publish its contents to the 'release' branch / your update host."

# ── Verify an installed tree against the published update ─────────────────────
# Answers "did the update actually land?" by CRC32-comparing every whitelisted
# file against the control files on the update host. The updater's UI shows the
# version from the cfg, which it can record even when a file replacement failed
# (a mapped .xpl while X-Plane is running is the classic case) — so the UI and
# the plugin's own version can disagree with nothing flagging it.
# TREE defaults to the local install; pass TREE=... for another one.
verify-install:
	@python3 tools/skunkcrafts/verify_install.py --tree "$(if $(TREE),$(TREE),$(PLUGIN_DIR))"

# ── Cleanup Tags ──────────────────────────────────────────────────────────────
cleanup-tags:
	git fetch --prune --prune-tags origin
	@echo "Local tags synced with remote."

# ── Cleanup Branches ──────────────────────────────────────────────────────────
cleanup-branches:
	@echo "Pruning remote-tracking references..."
	@git fetch --prune origin
	@echo ""
	@echo "Local branches whose upstream is gone:"
	@STALE=$$(git for-each-ref --format '%(refname:short) %(upstream:track)' refs/heads | awk '$$2 == "[gone]" {print $$1}'); \
	if [ -z "$$STALE" ]; then \
	    echo "  (none)"; \
	else \
	    echo "$$STALE" | sed 's/^/  /'; \
	    echo ""; \
	    echo "$$STALE" | xargs -n1 git branch -d; \
	fi
	@echo "Local branches synced with remote."

# ── Cleanup GitHub Actions Runs ───────────────────────────────────────────────
cleanup-runs:
	@command -v gh >/dev/null 2>&1 || { \
	    echo "gh not found. Install with: brew install gh"; exit 1; }
	@echo "Deleting GitHub Actions runs (keeping newest per workflow)..."
	@for wf in $$(gh workflow list --json id -q '.[].id'); do \
	    gh run list --workflow=$$wf --limit 1000 --json databaseId -q '.[1:] | .[].databaseId' \
	        | xargs -I {} gh run delete {}; \
	done
	@echo "Cleanup complete."

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	rm -rf build/ build-ci/ build-lint/ build-sanitize/ build-arm64/ build-x86_64/

# ── Distclean ─────────────────────────────────────────────────────────────────
# Remove everything 'make setup' downloaded so a full re-bootstrap is forced.
distclean: clean
	rm -rf sdk/ vendor/
	@echo "Removed sdk/ and vendor/. Run 'make setup' to re-download dependencies."
