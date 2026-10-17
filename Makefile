# CarLifePort top-level build
#
#   make all     build every component into dist/
#   make sdk     build only the native CarLife SDK -- dist/libcarlifevehicle.dylib
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

# Assigned rather than defaulted: make predefines CC and CXX, so `?=` would never apply.
# Override on the command line (make CXX=...) if needed.
CC  := clang
CXX := clang++

.PHONY: all sdk link server web clean

# ------------------------------------------------------------------------- everything
all: sdk link server web
	@printf '\n\033[1;32mbuild complete\033[0m\n'
	@printf '  dist/carlife-link              + libcarlifevehicle.dylib\n'
	@printf '  dist/carlifed                  (browser client embedded)\n'
	@printf '\nrun dist/carlifed to start everything\n'

# --------------------------------------------------------------------------- native SDK
# The vendored Baidu CarLife head-unit library (CarLife-Vehicle-Lib) and the protobuf 2.5.0
# static library it serializes with. Both are built from vendored sources and stay
# unmodified, apart from the 3-line arm64 patch applied by scripts/patch-protobuf.py.

LIB_SRC  := $(ROOT)/vendor/carlife/CarLife-Vehicle-Lib/LibSource
LIB_DIRS := core libwrapper modules utility protobuf/cc/src

PB_TARBALL := $(ROOT)/vendor/carlife/CarLife-Vehicle-Lib/protobuf-2.5.0.tar.gz
PB_SRC     := $(BUILD)/protobuf-2.5.0
PB_UNPACK  := $(BUILD)/.protobuf-unpacked
PB_PREFIX  := $(BUILD)/pb
PB_LIB     := $(PB_PREFIX)/lib/libprotobuf.a
PB_PATCHED := $(BUILD)/.protobuf-patched
PB_JOBS    := $(shell sysctl -n hw.ncpu 2>/dev/null || echo 4)

LIB_DYLIB := $(DIST)/libcarlifevehicle.dylib
LIB_OBJ   := $(BUILD)/obj
LIB_SRCS  := $(sort $(foreach d,$(LIB_DIRS),$(shell find $(LIB_SRC)/$(d) \( -name '*.cpp' -o -name '*.cc' \))))
LIB_OBJS  := $(addprefix $(LIB_OBJ)/,$(addsuffix .o,$(patsubst %.cpp,%,$(patsubst %.cc,%,$(LIB_SRCS:$(LIB_SRC)/%=%)))))

LIB_INCLUDES := -I$(LIB_SRC)/include -I$(LIB_SRC)/protobuf/cc/include \
                -I$(PB_PREFIX)/include -I$(PB_PREFIX)/include/google/protobuf \
                -I$(PB_PREFIX)/include/google/protobuf/io
LIB_CXXFLAGS := -std=gnu++98 -O2 -w -fPIC $(LIB_INCLUDES)

sdk: $(LIB_DYLIB)

$(LIB_DYLIB): $(LIB_OBJS) $(PB_LIB)
	@printf '\n\033[1;36m==> native SDK (%s sources)\033[0m\n' '$(words $(LIB_SRCS))'
	@mkdir -p $(DIST)
	$(CXX) -dynamiclib -install_name @rpath/libcarlifevehicle.dylib -o $@ $(LIB_OBJS) \
	    -L$(PB_PREFIX)/lib -lprotobuf -lpthread
	@ls -lh $@

# Objects mirror the source tree under .build/obj. -MMD -MP records the headers each one
# includes, so editing a header rebuilds exactly the objects that use it. protobuf is an
# order-only prerequisite: the objects include its installed headers, but touching its
# static library must not rebuild all of them.
$(LIB_OBJ)/%.o: $(LIB_SRC)/%.cpp | $(PB_LIB)
	@mkdir -p $(@D)
	$(CXX) $(LIB_CXXFLAGS) -MMD -MP -c $< -o $@

$(LIB_OBJ)/%.o: $(LIB_SRC)/%.cc | $(PB_LIB)
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
# The frontend is not a run-time directory. make web builds it unmodified from
# vendor/playport/web and stages the result in server/web, which is what the //go:embed
# directive in server/webassets.go compiles into carlifed. That staging directory is
# generated (and ignored by git), so the vendored copy stays the only one in the tree.
server: web
	@printf '\n\033[1;36m==> backend (browser client embedded)\033[0m\n'
	@mkdir -p $(DIST)
	@cd $(ROOT)/server && go build -o $(DIST)/carlifed .

WEB_DIR   := $(ROOT)/vendor/playport/web
WEB_STAGE := $(ROOT)/server/web

web:
	@printf '\n\033[1;36m==> frontend (vendor/playport, built unmodified)\033[0m\n'
	@if [ ! -d $(WEB_DIR)/node_modules ]; then \
	  echo "== first build, installing web dependencies"; \
	  cd $(WEB_DIR) && npm install --no-package-lock; \
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
