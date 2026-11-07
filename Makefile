# NFS5 static-recompilation port - build
#
# Desktop:   make -j$(nproc) build/nfs5-desktop
# Handheld:  make -j$(nproc) build/nfs5.aarch64 CROSS_COMPILE=aarch64-linux-gnu-
#
# Prerequisites: generated translated code (`make translate`) and host dispatch
# glue (`make modules`) must exist first; `make game` does both.
#
# The translated code under build/translated is generated from the user's own
# original Porsche.exe / dx7z.dll and must never be distributed.

CROSS_COMPILE ?=
CC      := $(CROSS_COMPILE)gcc
OUT     ?= build/nfs5-desktop
JOBS    ?= $(shell nproc 2>/dev/null || echo 4)

HERE    := $(CURDIR)
# Local host-deps (SDL2_ttf headers/lib fetched without root) need an rpath so
# the built binary runs without LD_LIBRARY_PATH. Cross builds pass RUNTIME_RPATH=.
RUNTIME_RPATH ?= -Wl,-rpath,$(HERE)/build/host-deps/root/usr/lib/x86_64-linux-gnu
PKG_CONFIG ?= pkg-config

SDL2_CFLAGS   ?= $(shell $(PKG_CONFIG) --cflags sdl2 2>/dev/null)
SDL2_LIBS     ?= $(shell $(PKG_CONFIG) --libs sdl2 2>/dev/null)
SDL2TTF_CFLAGS?= $(shell $(PKG_CONFIG) --cflags SDL2_ttf 2>/dev/null || echo -I$(HERE)/build/host-deps/root/usr/include)
SDL2TTF_LIBS  ?= $(shell $(PKG_CONFIG) --libs SDL2_ttf 2>/dev/null || echo -L$(HERE)/build/host-deps/root/usr/lib/x86_64-linux-gnu -lSDL2_ttf)
GLES_LIBS     ?= -lGLESv2 -lEGL

# TRACE=1 records every lifted function entry in a 1024-entry ring and dumps it
# on a fault (bring-up only; costs a store per call).
TRACE   ?= 0
# RECOMP_RETADDR=0: the lift's dummy return address (0xDEAD0000) has been seen
# leaking into game data as an object pointer on the movie-teardown path
# (0x4CFB10, esi=0xdead0000 -> call [obj+0x144]). pcrecomp documents overriding
# it to 0 so such a leak becomes a NULL/game-handled value while the leak is
# tracked down; set RECOMP_RETADDR=0xDEAD0000 to restore the diagnostic marker.
RETADDR ?= 0
# STACKCHECK=1 validates every lifted call/return against a shadow stack and
# reports the function that returned with the wrong guest esp (bring-up only).
STACKCHECK ?= 0
DEFS    := -D_GNU_SOURCE -DDS_HEAP_START=0x20000000u -DRECOMP_RETADDR=$(RETADDR) $(if $(filter 1,$(TRACE)),-DRECOMP_TRACE) \
           $(if $(filter aarch64%,$(CROSS_COMPILE)),-DRECOMP_MMX_NEON) \
           $(if $(filter 1,$(STACKCHECK)),-DRECOMP_STACKCHECK) \
           $(EXTRA_DEFS)
# EXTRA_DEFS: build-wide experiments, e.g. EXTRA_DEFS=-DRECOMP_MEM_BASE=0x100000000ull
# (guest memory at a fixed host address: ADDR() adds a constant instead of
# reloading g_mem_base after every guest store). Requires a clean object tree.
# Embedded minimal FFmpeg (demuxer ea, decoders eamad + adpcm_ea_r1) for the
# in-process EA-MAD movie player. Built by scripts/build_ffmpeg_mini.sh for the
# host and for aarch64; the game links it statically (no host ffmpeg binary).
FFMPEG_ARCH := $(if $(filter aarch64%,$(CROSS_COMPILE)),aarch64,host)
FFMPEG_ROOT := build/ffmpeg/$(FFMPEG_ARCH)
FFMPEG_LIBS := $(wildcard $(FFMPEG_ROOT)/lib/libavformat.a) \
               $(wildcard $(FFMPEG_ROOT)/lib/libavcodec.a) \
               $(wildcard $(FFMPEG_ROOT)/lib/libavutil.a)
ifneq ($(strip $(FFMPEG_LIBS)),)
DEFS    += -DNFS5_HAVE_FFMPEG -I$(FFMPEG_ROOT)/include
endif
INCS    := -Iruntime -Iruntime/render -Ilauncher \
           -Ithird_party/pcrecomp/runtime/recomp32 \
           -Ithird_party/pcrecomp/runtime/win32hle \
           -Ibuild/game $(SDL2_CFLAGS) $(SDL2TTF_CFLAGS)
COMMON  := -std=gnu11 -g -O2 -fno-strict-aliasing -fwrapv -ffp-contract=off \
           -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable \
           -Wno-format -Wno-pointer-sign $(DEFS) $(INCS)

# Runtime glue (our code), the vendored win32hle layer, generated dispatch.
# EXCLUDE: bring-up builds can drop files that are mid-edit, e.g.
#   make build/nfs5-desktop EXCLUDE="runtime/d3d7.c runtime/render"
EXCLUDE     ?=
RUNTIME_SRC := $(filter-out %_selftest.c $(filter runtime/%,$(EXCLUDE)),$(wildcard runtime/*.c) $(wildcard runtime/profiling/*.c))
HLE_SRC     := $(filter-out %_selftest.c $(filter third_party/%,$(EXCLUDE)),$(wildcard third_party/pcrecomp/runtime/win32hle/*.c))
RENDER_SRC  := $(filter-out %_selftest.c,$(wildcard runtime/render/*.c))
RENDER_SRC  := $(if $(filter runtime/render,$(EXCLUDE)),,$(RENDER_SRC))
GAME_SRC    := $(wildcard build/game/*.c)
TRANSLATED  := $(wildcard build/translated/*/functions_*.c) $(wildcard build/translated/*/dispatch.c)

RUNTIME_OBJ := $(patsubst runtime/%.c,obj/runtime/%.o,$(RUNTIME_SRC)) \
               $(patsubst runtime/render/%.c,obj/render/%.o,$(RENDER_SRC))
HLE_OBJ     := $(patsubst third_party/pcrecomp/runtime/win32hle/%.c,obj/hle/%.o,$(HLE_SRC))
GAME_OBJ    := $(patsubst build/game/%.c,obj/game/%.o,$(GAME_SRC))
GEN_OBJ     := $(patsubst build/translated/%.c,obj/gen/%.o,$(TRANSLATED))
OBJ         := $(RUNTIME_OBJ) $(HLE_OBJ) $(GAME_OBJ) $(GEN_OBJ)

.PHONY: all game translate modules dev-env clean desktop cross check-translation
all: $(OUT)

game: translate modules

translate:
	python3 tools/translate_game.py --modules Porsche.exe Drivers/dx7z.dll
	python3 tools/patch_generated_headers.py
	python3 tools/patch_local_regs.py

modules: docs/evidence/translation-summary.json
	python3 tools/generate_modules.py

$(OUT): $(OBJ) $(FFMPEG_LIBS)
	@mkdir -p $(dir $@)
	$(CC) -g -rdynamic $(EXTRA_LDFLAGS) -o $@ $(OBJ) $(FFMPEG_LIBS) \
	  $(SDL2_LIBS) $(SDL2TTF_LIBS) $(GLES_LIBS) $(RUNTIME_RPATH) -lm -lpthread -ldl

obj/runtime/%.o: runtime/%.c
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) -Wall -c $< -o $@

obj/render/%.o: runtime/render/%.c
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) -Wall -c $< -o $@

obj/hle/%.o: third_party/pcrecomp/runtime/win32hle/%.c
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) -c $< -o $@

obj/game/%.o: build/game/%.c
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) -c $< -o $@

obj/gen/%.o: build/translated/%.c
	@mkdir -p $(dir $@)
	$(CC) $(COMMON) -O2 -w -Ibuild/translated/$(firstword $(subst /, ,$(patsubst build/translated/%,%,$<))) -c $< -o $@

# Desktop development install root: build/run with the extracted original files
# mounted read-only under data/ and writable userdata/ + config/.
dev-env:
	@mkdir -p build/run/userdata build/run/config
	@if [ ! -e build/run/data ]; then ln -s $(HERE)/build/game-input build/run/data; fi
	@test -f build/run/data/Porsche.exe && echo "dev root ready: build/run (NFS5_ROOT=$(HERE)/build/run)" || \
	  (echo "no extracted game in build/game-input; see docs/INSTALL.md"; exit 1)

clean:
	rm -rf obj $(OUT)

distclean: clean
	rm -rf build/translated build/game docs/evidence/translation-*.json
