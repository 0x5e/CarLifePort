# CarLifePort top-level build
#
#   make all     build every component into dist/
#   make sdk     build only the native CarLife SDK -- the library inside
#                dist/CarLifeLink.app/Contents/Frameworks
#   make clean   remove dist/ and every intermediate under .build/
#
# All of them are incremental: an artifact is rebuilt only when one of its inputs is newer
# than the artifact itself, so a run after a small edit compiles a few files instead of all
# of them. Nothing is discarded between runs, so make clean is the only way to force a full
# rebuild.
#
# Run-time artifacts go to dist/ and nowhere else: carlifed looks for the link process it
# starts next to itself (see server/main.go). The browser client is not a run-time
# directory -- it is compiled into carlifed. Intermediates stay in .build/ and nothing is
# ever run from there.
#
# macOS ships GNU make 3.81, so nothing here uses .ONESHELL or .RECIPEPREFIX.

ROOT  := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
DIST  := $(ROOT)/dist
BUILD := $(ROOT)/.build
# The bundle the link half produces. The native library goes inside it as well, so that
# dist/ holds one self-contained app and the library is covered by the bundle's signature.
BUNDLE := $(DIST)/CarLifeLink.app

# Assigned rather than defaulted: make predefines CC and CXX, so `?=` would never apply.
# Override on the command line (make CXX=...) if needed.
CC  := clang
CXX := clang++

.PHONY: all sdk link server web clean

# ------------------------------------------------------------------------- everything
all: sdk link server web
	@printf '\n\033[1;32mbuild complete\033[0m\n'
	@printf '  dist/CarLifeLink.app           (link + libcarlifevehicle.dylib, signed for CoreLocation)\n'
	@printf '  dist/carlifed                  (browser client embedded)\n'
	@printf '\nrun dist/carlifed to start everything\n'

# --------------------------------------------------------------------------- native SDK
# The vendored Baidu CarLife head-unit library (CarLife-Vehicle-Lib) and the protobuf 2.5.0
# static library it serializes with, both built from vendored sources. The library needs a
# few code changes for the vehicle data subscription (see 「车载数据」 in README); they live
# in patches/ rather than in the submodule, so that vendor/carlife stays at the upstream
# commit. protobuf is unmodified, apart from the 3-line arm64 patch applied by
# scripts/patch-protobuf.py.

LIB_SRC  := $(ROOT)/vendor/carlife/CarLife-Vehicle-Lib/LibSource
LIB_DIRS := core libwrapper modules utility protobuf/cc/src

# The patch is applied to the submodule in place: that is where the sources are compiled
# from, and where the headers the link half includes come from. The stamp makes the step
# run once per change of the patch, and the checkout in it makes the step repeatable --
# applying a patch on top of itself either fails or applies the hunks twice.
CARLIFE_PATCH := $(ROOT)/patches/carlife-vehicle-lib.patch
CARLIFE_STAMP := $(BUILD)/.carlife-patched

PB_TARBALL := $(ROOT)/vendor/carlife/CarLife-Vehicle-Lib/protobuf-2.5.0.tar.gz
PB_SRC     := $(BUILD)/protobuf-2.5.0
PB_UNPACK  := $(BUILD)/.protobuf-unpacked
PB_PREFIX  := $(BUILD)/pb
PB_LIB     := $(PB_PREFIX)/lib/libprotobuf.a
PB_PATCHED := $(BUILD)/.protobuf-patched
PB_JOBS    := $(shell sysctl -n hw.ncpu 2>/dev/null || echo 4)

LIB_DYLIB := $(BUNDLE)/Contents/Frameworks/libcarlifevehicle.dylib
LIB_OBJ   := $(BUILD)/obj
LIB_SRCS  := $(sort $(foreach d,$(LIB_DIRS),$(shell find $(LIB_SRC)/$(d) \( -name '*.cpp' -o -name '*.cc' \))))
LIB_OBJS  := $(addprefix $(LIB_OBJ)/,$(addsuffix .o,$(patsubst %.cpp,%,$(patsubst %.cc,%,$(LIB_SRCS:$(LIB_SRC)/%=%)))))

LIB_INCLUDES := -I$(LIB_SRC)/include -I$(LIB_SRC)/protobuf/cc/include \
                -I$(PB_PREFIX)/include -I$(PB_PREFIX)/include/google/protobuf \
                -I$(PB_PREFIX)/include/google/protobuf/io
LIB_CXXFLAGS := -std=gnu++98 -O2 -w -fPIC $(LIB_INCLUDES)

sdk: $(LIB_DYLIB)

$(CARLIFE_STAMP): $(CARLIFE_PATCH)
	@printf '\n\033[1;36m==> vendored CarLife library (apply patches/)\033[0m\n'
	@git -C $(ROOT)/vendor/carlife checkout -- .
	@cd $(ROOT)/vendor/carlife && patch -p1 --batch < $(CARLIFE_PATCH)
	@touch $@

# Signed here rather than together with the bundle: codesign refuses a bundle that contains
# unsigned nested code, and signing it from the bundle step would update its timestamp and
# make the link half relink on every run. The linker already leaves an ad-hoc signature on
# arm64, but not on every platform, and this pass is what the bundle relies on.
$(LIB_DYLIB): $(LIB_OBJS) $(PB_LIB)
	@printf '\n\033[1;36m==> native SDK (%s sources)\033[0m\n' '$(words $(LIB_SRCS))'
	@mkdir -p $(dir $@)
	$(CXX) -dynamiclib -install_name @rpath/libcarlifevehicle.dylib -o $@ $(LIB_OBJS) \
	    -L$(PB_PREFIX)/lib -lprotobuf -lpthread
	@codesign --force --sign - $@
	@ls -lh $@

# Objects mirror the source tree under .build/obj. -MMD -MP records the headers each one
# includes, so editing a header rebuilds exactly the objects that use it. protobuf is an
# order-only prerequisite: the objects include its installed headers, but touching its
# static library must not rebuild all of them. The patch stamp is not: it rewrites the
# sources the objects were compiled from, so an object has to be rebuilt after it.
$(LIB_OBJ)/%.o: $(LIB_SRC)/%.cpp $(CARLIFE_STAMP) | $(PB_LIB)
	@mkdir -p $(@D)
	$(CXX) $(LIB_CXXFLAGS) -MMD -MP -c $< -o $@

$(LIB_OBJ)/%.o: $(LIB_SRC)/%.cc $(CARLIFE_STAMP) | $(PB_LIB)
	@mkdir -p $(@D)
	$(CXX) $(LIB_CXXFLAGS) -MMD -MP -c $< -o $@

# protobuf 2.5.0 is a patched upstream tarball: it is unpacked into .build/ and patched
# once. The static library is the cache for that work -- configure and make run only when
# the library is missing or the patch stamp is newer than it.
#
# Unpacking is recorded by a stamp rather than by a file inside the tree: tar restores the
# archive's own timestamps (2013), so the unpacked files always look older than the tarball
# and every run would unpack again -- overwriting the patch and recompiling everything.
$(PB_UNPACK): $(PB_TARBALL)
	@printf '\n\033[1;36m==> protobuf 2.5.0 (extract)\033[0m\n'
	@mkdir -p $(BUILD)
	@tar xzf $< -C $(BUILD)
	@touch $@

$(PB_PATCHED): $(PB_UNPACK)
	@printf '\n\033[1;36m==> protobuf 2.5.0 (arm64 patch)\033[0m\n'
	@python3 $(ROOT)/scripts/patch-protobuf.py $(PB_SRC)/src/google/protobuf/stubs/platform_macros.h
	@touch $@

$(PB_LIB): $(PB_PATCHED)
	@printf '\n\033[1;36m==> protobuf 2.5.0 (build, logs: .build/protobuf-*.log)\033[0m\n'
	@cd $(PB_SRC) && rm -rf conftest conftest.dir conftest.c conftest.cpp conftest.o conftest.err && \
	  ./configure --disable-shared --enable-static CC=clang CXX=clang++ \
	    CFLAGS="-O2 -w" CXXFLAGS="-std=gnu++98 -O2 -w" --prefix=$(PB_PREFIX) \
	    > $(BUILD)/protobuf-configure.log 2>&1
	@grep -q '^#define HAVE_PTHREAD 1' $(PB_SRC)/config.h || \
	  { echo "protobuf pthread probe failed, see .build/protobuf-configure.log" >&2; exit 1; }
	@cd $(PB_SRC) && MAKEFLAGS= make -j$(PB_JOBS) > $(BUILD)/protobuf-make.log 2>&1
	@cd $(PB_SRC) && MAKEFLAGS= make install > $(BUILD)/protobuf-install.log 2>&1
	@echo "== protobuf ready"

# ------------------------------------------------------------------------------- link
link: sdk
	@printf '\n\033[1;36m==> link (USB transport + protocol stack)\033[0m\n'
	@$(MAKE) -C $(ROOT)/link

# ------------------------------------------------------------------- backend + frontend
# Go and vite track their own inputs, so these two are phony on purpose.
#
# The frontend is not a run-time directory. make web builds vendor/playport/web and stages
# the result in server/web, which is what the //go:embed directive in server/webassets.go
# compiles into carlifed. That staging directory is generated (and ignored by git), so the
# vendored copy stays the only one in the tree.
#
# The viewer was written for CarPlay over a wireless connection, so its copy names another
# product. Only strings change -- no markup, no logic -- so they are rewritten in place
# rather than kept as a patch file. The rules are idempotent, so running them on every build
# needs neither a stamp nor a checkout; the grep after them turns an upstream copy change
# into a build failure instead of a silently skipped hunk.
#
# Sentence-level rules come first and match the original text. The words they introduce are
# already the final form, so the keyword pass below cannot touch them twice. Keys, DOM ids,
# CSS classes and the 'siri' message type are left alone: they are not copy, and renaming
# them would drop the saved token, volume and gain values.
#
# Signed after the build. go writes the new Mach-O over the old file in place, which leaves
# the signature embedded in it pointing at bytes that are no longer there, and the kernel
# then kills the result at exec with no output at all -- it reads as a crash in the program.
# Re-signing replaces that signature with one that matches what is there now.
server: web
	@printf '\n\033[1;36m==> backend (browser client embedded)\033[0m\n'
	@mkdir -p $(DIST)
	@cd $(ROOT)/server && go build -o $(DIST)/carlifed .
	@codesign --force --sign - $(DIST)/carlifed

WEB_DIR   := $(ROOT)/vendor/playport/web
WEB_STAGE := $(ROOT)/server/web

# BSD sed takes the backup suffix as a separate argument, hence the empty string, and is
# named outright because whatever sed comes first on PATH is not necessarily that one.
SED := /usr/bin/sed

WEB_SED := \
    -e 's|<span>Talk to Siri</span>|<span>Voice assistant</span>|' \
    -e 's|Talk to Siri|Talk to the voice assistant|g' \
    -e 's|<span>Siri</span>|<span>Voice</span>|' \
    -e 's|Siri and calls use|Voice and calls use|' \
    -e 's|control CarPlay directly|control the phone directly|' \
    -e 's|<b>Pair your iPhone</b>|<b>Plug in your phone</b>|' \
    -e 's|<strong>Pair with PlayPort over Bluetooth</strong><p>On your iPhone, open Settings → Bluetooth and select the server’s device name. If asked for a PIN, enter <b>3939</b>.</p>|<strong>Plug your phone in with a USB cable</strong><p>Connect your Android phone to this machine with a data cable. A charge-only cable is not enough: the phone has to be seen as a device, not as a charger. The CarLife app opens on the phone by itself.</p>|' \
    -e 's|<p>Keep the iPhone and server on the same Wi-Fi network. Accept the CarPlay prompt on your iPhone; the screen appears automatically.</p>|<p>Allow the connection when your phone asks, and pick the CarLife app if it offers a choice. The screen appears here automatically, and the phone charges while it is plugged in.</p>|' \
    -e 's|PlayPort|CarLifePort|g' \
    -e 's|CARPLAY|CARLIFE|g' \
    -e 's|CarPlay|CarLife|g' \
    -e 's|iPhone|Android phone|g'

web:
	@printf '\n\033[1;36m==> frontend (vendor/playport, copy rewritten)\033[0m\n'
	@if [ ! -d $(WEB_DIR)/node_modules ]; then \
	  echo "== first build, installing web dependencies"; \
	  cd $(WEB_DIR) && npm install --no-package-lock; \
	fi
	@cd $(WEB_DIR) && $(SED) -i '' $(WEB_SED) index.html src/main.ts
	@cd $(WEB_DIR) && if /usr/bin/grep -nE 'PlayPort|CarPlay|CARPLAY|iPhone|Siri|3939' index.html src/main.ts; then \
	  echo "viewer copy still names the upstream product, update the sed rules in the top-level Makefile" >&2; \
	  exit 1; \
	fi
	@cd $(WEB_DIR) && npm run build
	@rm -rf $(WEB_STAGE) && cp -R $(WEB_DIR)/dist $(WEB_STAGE)

# ------------------------------------------------------------------------------ clean
# dist/, .build/ and the staged frontend go away entirely. vendor/playport/web/node_modules
# is a dependency cache and is kept, so the next build does not reinstall it.
clean:
	@$(MAKE) -C $(ROOT)/link clean
	@rm -rf $(DIST) $(BUILD) $(WEB_STAGE)
	@echo "== clean: removed dist/, .build/ and server/web"

-include $(LIB_OBJS:.o=.d)
