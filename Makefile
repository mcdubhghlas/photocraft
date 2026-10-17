# PhotoCraft C17 build (REWRITE_PLAN.md D3, D20). Requires GNU make >= 3.81 (`gmake` on the BSDs).
#
#   make            build the library (and programs, once they exist)
#   make test       build and run every tests/*/test_*.c
#   make check      layer check + strict build (warnings are errors) + tests -> build/check
#   make corpus     fetch the real-file test corpora into corpus/ (tools/corpus.sh)
#   make sanitize   ASan + UBSan build + tests (when the compiler has them) -> build/san
#   make tsan       ThreadSanitizer build + tests                     -> build/tsan
#   make analyze    clang static analyzer, or cppcheck, if installed
#   make valgrind   run the tests under valgrind, if installed
#   make install    install programs into $(DESTDIR)$(PREFIX)/bin
#   make clean      remove build/
#
# Standard variables are respected: CC CPPFLAGS CFLAGS LDFLAGS LDLIBS PREFIX DESTDIR PKG_CONFIG.

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
PKG_CONFIG ?= pkg-config
BUILD ?= build/default
CFLAGS ?= -O2 -g
ARFLAGS = rcs

# Modules in layer order (REWRITE_PLAN.md §3). Each chunk appends its module here.
MODULES = base geom cms color raster psd codecs raw affinity doc ops paint algo vector text compose format io plugins engine automation ui
# pkg-config packages the current modules need.
PKGS = zlib libzstd libblake3 libpng16 libjpeg libwebp libwebpmux libwebpdemux freetype2 harfbuzz fribidi libunibreak
# Vendored third-party C (REWRITE_PLAN.md D17), compiled without our strict warnings: it isn't
# our code to fix, and upstream builds it warning-free with its own flags.
THIRD_PARTY = yyjson nuklear pocketfft
# Executables (src/cli, src/app) once they exist.
PROGRAMS = photocraft-cli
# Libraries only the UI layer may use (REWRITE_PLAN.md D4). Their flags are applied to src/ui
# objects only, so nothing lower can include SDL by accident.
GUI_PKGS = sdl3 freetype2

, := ,

# ---- compiler feature probes (flags a compiler rejects are simply not used) ----
cc_accepts = $(shell printf 'int main(void){return 0;}\n' | $(CC) $(1) -Werror -x c -o /dev/null - >/dev/null 2>&1 && echo y)

WARN_CANDIDATES = -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes \
                  -Wmissing-prototypes -Wvla -Wformat=2 -Wundef -Wcast-qual -Wpointer-arith
WARN := $(foreach f,$(WARN_CANDIDATES),$(if $(call cc_accepts,$(f)),$(f)))
DEPFLAGS := $(if $(call cc_accepts,-MMD -MP -MF /dev/null),-MMD -MP)
PTHREAD := $(if $(call cc_accepts,-pthread),-pthread,-lpthread)
# Never fuse a*b+c into FMA (clang's default allows it): Rust doesn't, and pixel results must match
# the reference bit for bit (REWRITE_PLAN.md D22).
FP_STRICT := $(if $(call cc_accepts,-ffp-contract=off),-ffp-contract=off)

STRICT ?=
WERROR = $(if $(STRICT),-Werror)

PKG_CFLAGS := $(if $(PKGS),$(shell $(PKG_CONFIG) --cflags $(PKGS)))
PKG_LIBS := $(if $(PKGS),$(shell $(PKG_CONFIG) --libs $(PKGS)))

# Optional codec libraries (REWRITE_PLAN.md D3, D27, D18): on when found, or force WITH_EXR=0 /
# WITH_HEIF=0. Only OpenEXRCore (the C API) is linked from OpenEXR.
WITH_EXR ?= $(shell $(PKG_CONFIG) --exists OpenEXR 2>/dev/null && echo 1)
WITH_HEIF ?= $(shell $(PKG_CONFIG) --exists libheif 2>/dev/null && echo 1)
EXR_CFLAGS := $(if $(filter 1,$(WITH_EXR)),$(shell $(PKG_CONFIG) --cflags OpenEXR),-DPC_NO_EXR)
EXR_LIBS := $(if $(filter 1,$(WITH_EXR)),$(filter -L% -lOpenEXRCore%,$(shell $(PKG_CONFIG) --libs OpenEXR)))
HEIF_CFLAGS := $(if $(filter 1,$(WITH_HEIF)),$(shell $(PKG_CONFIG) --cflags libheif),-DPC_NO_HEIF)
HEIF_LIBS := $(if $(filter 1,$(WITH_HEIF)),$(shell $(PKG_CONFIG) --libs libheif))
CODEC_CFLAGS = $(EXR_CFLAGS) $(HEIF_CFLAGS)
# JPEG XL-compressed DNG (REWRITE_PLAN.md D30): libjxl, on when found, or force WITH_JXL=0 (those
# files then report "unsupported"). Its flags reach src/raw and tests/raw only.
WITH_JXL ?= $(shell $(PKG_CONFIG) --exists libjxl 2>/dev/null && echo 1)
JXL_CFLAGS := $(if $(filter 1,$(WITH_JXL)),$(shell $(PKG_CONFIG) --cflags libjxl),-DPC_NO_JXL)
JXL_LIBS := $(if $(filter 1,$(WITH_JXL)),$(shell $(PKG_CONFIG) --libs libjxl))

# WebAssembly plug-ins (REWRITE_PLAN.md D13): vendored WAMR (third_party/wamr), fast interpreter
# only: no AOT/JIT, no WASI or libc imports, no SIMD/shared memory; software bounds checks (no
# SIGSEGV handlers); instruction metering (the plug-in fuel budget) and the thread manager, whose
# suspend flags let wasm_runtime_terminate stop a running plug-in (wall-clock budget, sibling
# failure). On by default where WAMR has a port (Linux, macOS, FreeBSD on x86-64/AArch64/x86/
# RISC-V 64); WITH_PLUGINS=0 builds src/plugins without it (loading reports "unavailable").
# Override WAMR_OS / WAMR_TARGET for cross builds (e.g. WAMR_TARGET=X86_32 with -m32).
WAMR_OS ?= $(patsubst Linux,LINUX,$(patsubst Darwin,DARWIN,$(patsubst FreeBSD,FREEBSD,$(filter Linux Darwin FreeBSD,$(shell uname -s)))))
CC_MACHINE := $(shell $(CC) -dumpmachine 2>/dev/null)
WAMR_TARGET ?= $(if $(filter x86_64-% amd64-%,$(CC_MACHINE)),X86_64,$(if $(filter aarch64-% arm64-%,$(CC_MACHINE)),AARCH64,$(if $(filter i386-% i486-% i586-% i686-%,$(CC_MACHINE)),X86_32,$(if $(filter riscv64-%,$(CC_MACHINE)),RISCV64_LP64D))))
WITH_PLUGINS ?= $(if $(and $(WAMR_OS),$(WAMR_TARGET)),1,0)
WAMR_DIR = third_party/wamr/core
WAMR_PLATFORM = $(if $(filter DARWIN,$(WAMR_OS)),darwin,$(if $(filter FREEBSD,$(WAMR_OS)),freebsd,linux))
# Exactly the sources WAMR's CMake (build-scripts/runtime_lib.cmake) compiles for this
# configuration: common, interpreter (fast), thread manager, utils, mem-alloc, POSIX platform.
WAMR_SRCS = $(addprefix $(WAMR_DIR)/, \
    iwasm/common/wasm_runtime_common.c iwasm/common/wasm_exec_env.c iwasm/common/wasm_memory.c \
    iwasm/common/wasm_native.c iwasm/common/wasm_loader_common.c iwasm/common/wasm_blocking_op.c \
    iwasm/common/wasm_c_api.c \
    iwasm/common/arch/invokeNative_general.c \
    iwasm/interpreter/wasm_loader.c iwasm/interpreter/wasm_runtime.c iwasm/interpreter/wasm_interp_fast.c \
    iwasm/libraries/thread-mgr/thread_manager.c \
    shared/utils/bh_assert.c shared/utils/bh_bitmap.c shared/utils/bh_common.c shared/utils/bh_hashmap.c \
    shared/utils/bh_leb128.c shared/utils/bh_list.c shared/utils/bh_log.c shared/utils/bh_queue.c \
    shared/utils/bh_vector.c shared/utils/runtime_timer.c \
    shared/mem-alloc/mem_alloc.c shared/mem-alloc/ems/ems_alloc.c shared/mem-alloc/ems/ems_gc.c \
    shared/mem-alloc/ems/ems_hmu.c shared/mem-alloc/ems/ems_kfc.c \
    shared/platform/$(WAMR_PLATFORM)/platform_init.c \
    shared/platform/common/posix/posix_malloc.c shared/platform/common/posix/posix_memmap.c \
    shared/platform/common/posix/posix_sleep.c shared/platform/common/posix/posix_thread.c \
    shared/platform/common/posix/posix_time.c shared/platform/common/posix/posix_blocking_op.c \
    $(if $(filter LINUX,$(WAMR_OS)),,shared/platform/common/memory/mremap.c))
# The definitions WAMR's CMake would pass (config_common.cmake, iwasm_common.cmake, the platform
# file); everything not listed keeps core/config.h's default (off). invokeNative_general.c stands
# in for the per-architecture assembly: plug-ins have no host imports, so it is never called.
WAMR_CPPFLAGS = -DBH_PLATFORM_$(WAMR_OS) -DBUILD_TARGET_$(WAMR_TARGET) -DBUILD_TARGET=\"$(WAMR_TARGET)\" \
    -DWASM_ENABLE_INTERP=1 -DWASM_ENABLE_FAST_INTERP=1 -DWASM_ENABLE_AOT=0 -DWASM_ENABLE_JIT=0 \
    -DWASM_ENABLE_FAST_JIT=0 -DWASM_ENABLE_BULK_MEMORY=1 -DWASM_ENABLE_REF_TYPES=1 \
    -DWASM_ENABLE_TAIL_CALL=1 -DWASM_ENABLE_EXTENDED_CONST_EXPR=1 -DWASM_ENABLE_SIMD=0 \
    -DWASM_ENABLE_SHARED_MEMORY=0 -DWASM_ENABLE_MULTI_MODULE=0 -DWASM_ENABLE_MINI_LOADER=0 \
    -DWASM_ENABLE_THREAD_MGR=1 -DWASM_ENABLE_INSTRUCTION_METERING=1 -DWASM_ENABLE_LIBC_BUILTIN=0 \
    -DWASM_ENABLE_LIBC_WASI=0 -DWASM_DISABLE_HW_BOUND_CHECK=1 -DWASM_DISABLE_STACK_HW_BOUND_CHECK=1 \
    -DWASM_DISABLE_WAKEUP_BLOCKING_OP=1 -DWASM_DISABLE_WRITE_GS_BASE=1 \
    -DBH_MALLOC=wasm_runtime_malloc -DBH_FREE=wasm_runtime_free \
    $(if $(filter LINUX,$(WAMR_OS)),-DWASM_HAVE_MREMAP=1 -D_GNU_SOURCE,-DWASM_HAVE_MREMAP=0) \
    -I$(WAMR_DIR) -I$(WAMR_DIR)/iwasm/include -I$(WAMR_DIR)/iwasm/common -I$(WAMR_DIR)/iwasm/interpreter \
    -I$(WAMR_DIR)/iwasm/libraries/thread-mgr -I$(WAMR_DIR)/shared/utils -I$(WAMR_DIR)/shared/mem-alloc \
    -I$(WAMR_DIR)/shared/platform/include -I$(WAMR_DIR)/shared/platform/$(WAMR_PLATFORM)
PLUGINS_CFLAGS := $(if $(filter 1,$(WITH_PLUGINS)),-DPC_HAVE_WAMR -I$(WAMR_DIR)/iwasm/include)
WAMR_OBJS := $(if $(filter 1,$(WITH_PLUGINS)),$(WAMR_SRCS:%.c=$(BUILD)/obj/%.o))

HAVE_GUI := $(shell $(PKG_CONFIG) --exists $(GUI_PKGS) 2>/dev/null && echo y)
GUI_CFLAGS := $(if $(HAVE_GUI),$(shell $(PKG_CONFIG) --cflags $(GUI_PKGS)))
GUI_LIBS := $(if $(HAVE_GUI),$(shell $(PKG_CONFIG) --libs $(GUI_PKGS)))

ALL_CPPFLAGS = -D_POSIX_C_SOURCE=200809L -Isrc $(THIRD_PARTY:%=-Ithird_party/%) $(PKG_CFLAGS) $(EXTRA_CPPFLAGS) $(CPPFLAGS)
ALL_CFLAGS = -std=c17 $(FP_STRICT) $(WARN) $(WERROR) $(PTHREAD) $(CFLAGS) $(EXTRA_CFLAGS)
ALL_LDFLAGS = $(PTHREAD) $(LDFLAGS) $(EXTRA_LDFLAGS)
ALL_LDLIBS = $(PKG_LIBS) $(EXR_LIBS) $(HEIF_LIBS) $(JXL_LIBS) $(LDLIBS) -lm

SRCS := $(foreach m,$(MODULES),$(wildcard src/$(m)/*.c))
THIRD_SRCS := $(foreach t,$(THIRD_PARTY),$(wildcard third_party/$(t)/*.c))
# Binary data compiled in (POSIX od + sed; no xxd): name = file under data/ (see GEN rules below).
# Bundled fonts (src/text/bundled.h), embedded the same way (assets/fonts/<name>.ttf).
FONT_GEN := Inter-Regular Inter-Medium Inter-SemiBold JetBrainsMono-Regular
# UI translation catalogs (src/ui/i18n.c: pc_i18n_cat_<code with - as _>), embedded uncompressed.
I18N_CATS := ja zh-hans zh-hant es ru uk sr cs fr id ko pl de pt-br el nl it hu tr vi
GEN_SRCS := $(BUILD)/gen/coated_cmyk_icc.c $(FONT_GEN:%=$(BUILD)/gen/font_%.c) $(BUILD)/gen/dict_scowl.c \
            $(I18N_CATS:%=$(BUILD)/gen/i18n_%.c) $(BUILD)/gen/app_icon.c
OBJS := $(SRCS:%.c=$(BUILD)/obj/%.o) $(THIRD_SRCS:%.c=$(BUILD)/obj/%.o) $(GEN_SRCS:%.c=%.o) $(WAMR_OBJS)
LIB := $(BUILD)/libphotocraft.a
# B3 GUI stack spike (src/ui/spike): built when SDL3 and FreeType are installed.
SPIKE_SRCS := $(wildcard src/ui/spike/*.c)
SPIKE_OBJS := $(SPIKE_SRCS:%.c=$(BUILD)/obj/%.o)
SPIKE := $(BUILD)/bin/photocraft-spike
# U1 offscreen snapshot tool (src/ui/snapshot): reuses the spike's GL/font/PNG/renderer helpers
# but drives Nuklear through the real theme tokens (D23).
SNAP_SRCS := $(wildcard src/ui/snapshot/*.c)
SNAP_OBJS := $(SNAP_SRCS:%.c=$(BUILD)/obj/%.o)
SNAP_HELPERS := $(BUILD)/obj/src/ui/spike/gl33.o $(BUILD)/obj/src/ui/spike/font.o \
                $(BUILD)/obj/src/ui/spike/png.o $(BUILD)/obj/src/ui/spike/render.o
SNAP := $(BUILD)/bin/photocraft-ui-snapshot
# PhotocraftApp shell + frame loop + control channel (src/ui/shell): the GUI-coupled app root the
# desktop app (Phase 9 P1) wraps. Built like the snapshot tool (reuses the spike GL/font helpers).
UISHELL_SRCS := $(wildcard src/ui/shell/*.c)
UISHELL_OBJS := $(UISHELL_SRCS:%.c=$(BUILD)/obj/%.o)
UISHELL_TEST_OBJS := $(filter-out $(BUILD)/obj/src/ui/shell/main.o,$(UISHELL_OBJS))
UISHELL := $(BUILD)/bin/photocraft-ui-shell
TEST_SRCS := $(wildcard tests/*/test_*.c)
# The shell control test needs the GUI packages (it links SDL, never inits video); drop it when they
# are absent so a headless build host still runs the rest of the suite.
TEST_SRCS := $(if $(HAVE_GUI),$(TEST_SRCS),$(filter-out tests/ui/test_shell_control.c,$(TEST_SRCS)))
TEST_BINS := $(TEST_SRCS:tests/%.c=$(BUILD)/tests/%)
# Executable apps: each src/<app>/ with a main.c links against the library (src/cli = photocraft-cli).
CLI_SRCS := $(wildcard src/cli/*.c)
CLI_OBJS := $(CLI_SRCS:%.c=$(BUILD)/obj/%.o)
APP_SRCS := $(CLI_SRCS)
APP_OBJS := $(CLI_OBJS)

.PHONY: all spike corpus test check sanitize tsan analyze valgrind install clean
.DEFAULT_GOAL := all

all: $(LIB) $(PROGRAMS:%=$(BUILD)/bin/%) $(if $(HAVE_GUI),$(SPIKE) $(SNAP) $(UISHELL))

# Listed first: GNU make 3.81 takes the first matching pattern rule, not the most specific.
$(BUILD)/obj/third_party/%.o: third_party/%.c
	@mkdir -p $(@D)
	$(CC) $(THIRD_CPPFLAGS) -std=c17 $(PTHREAD) $(CFLAGS) $(EXTRA_CFLAGS) $(THIRD_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(ALL_CPPFLAGS) $(ALL_CFLAGS) $(DEPFLAGS) -c $< -o $@

# The shipped synthetic CMYK profile (src/cms/builtin.h: pc_coated_cmyk_icc).
$(BUILD)/gen/coated_cmyk_icc.c: data/profiles/photocraft-coated-cmyk.icc
	@mkdir -p $(@D)
	@{ echo '/* Generated from data/profiles/photocraft-coated-cmyk.icc by the Makefile. */'; \
	  echo '#include <stddef.h>'; \
	  echo 'const unsigned char pc_coated_cmyk_icc[] = {'; \
	  od -An -v -tu1 $< | sed -e 's/[[:space:]][[:space:]]*/,/g' -e 's/^,//' -e 's/,$$//' -e 's/$$/,/' -e '/^,$$/d'; \
	  echo '};'; \
	  echo "const size_t pc_coated_cmyk_icc_len = sizeof pc_coated_cmyk_icc;"; } > $@.tmp && mv $@.tmp $@

# The bundled fonts (src/text/bundled.h: pc_font_<name with - as _>).
$(BUILD)/gen/font_%.c: assets/fonts/%.ttf
	@mkdir -p $(@D)
	@{ echo '/* Generated from $< by the Makefile. */'; \
	  echo '#include <stddef.h>'; \
	  echo 'const unsigned char pc_font_$(subst -,_,$*)[] = {'; \
	  od -An -v -tu1 $< | sed -e 's/[[:space:]][[:space:]]*/,/g' -e 's/^,//' -e 's/,$$//' -e 's/$$/,/' -e '/^,$$/d'; \
	  echo '};'; \
	  echo "const size_t pc_font_$(subst -,_,$*)_len = sizeof pc_font_$(subst -,_,$*);"; } > $@.tmp && mv $@.tmp $@

# The app-icon PNG (src/ui/brand.c: pc_ui_app_icon_png), embedded as shipped.
$(BUILD)/gen/app_icon.c: assets/app-icon/hicolor/128x128/apps/ai.storyteller.photocraft.png
	@mkdir -p $(@D)
	@{ echo '/* Generated from $< by the Makefile. */'; \
	  echo '#include <stddef.h>'; \
	  echo 'const unsigned char pc_ui_app_icon_png[] = {'; \
	  od -An -v -tu1 $< | sed -e 's/[[:space:]][[:space:]]*/,/g' -e 's/^,//' -e 's/,$$//' -e 's/$$/,/' -e '/^,$$/d'; \
	  echo '};'; \
	  echo "const size_t pc_ui_app_icon_png_len = sizeof pc_ui_app_icon_png;"; } > $@.tmp && mv $@.tmp $@

# UI translation catalogs (src/ui/i18n.c: pc_i18n_cat_<name with - as _>), raw TSV bytes.
$(BUILD)/gen/i18n_%.c: src/ui/i18n/%.tsv
	@mkdir -p $(@D)
	@{ echo '/* Generated from $< by the Makefile. */'; \
	  echo '#include <stddef.h>'; \
	  echo 'const unsigned char pc_i18n_cat_$(subst -,_,$*)[] = {'; \
	  od -An -v -tu1 $< | sed -e 's/[[:space:]][[:space:]]*/,/g' -e 's/^,//' -e 's/,$$//' -e 's/$$/,/' -e '/^,$$/d'; \
	  echo '};'; \
	  echo "const size_t pc_i18n_cat_$(subst -,_,$*)_len = sizeof pc_i18n_cat_$(subst -,_,$*);"; } > $@.tmp && mv $@.tmp $@

# The spelling dictionary (src/text/spell.c: pc_dict_scowl_gz), gzip-compressed as shipped.
$(BUILD)/gen/dict_scowl.c: assets/dict/en_US-scowl-50.txt.gz
	@mkdir -p $(@D)
	@{ echo '/* Generated from $< by the Makefile. */'; \
	  echo '#include <stddef.h>'; \
	  echo 'const unsigned char pc_dict_scowl_gz[] = {'; \
	  od -An -v -tu1 $< | sed -e 's/[[:space:]][[:space:]]*/,/g' -e 's/^,//' -e 's/,$$//' -e 's/$$/,/' -e '/^,$$/d'; \
	  echo '};'; \
	  echo "const size_t pc_dict_scowl_gz_len = sizeof pc_dict_scowl_gz;"; } > $@.tmp && mv $@.tmp $@

$(BUILD)/gen/%.o: $(BUILD)/gen/%.c
	$(CC) -std=c17 $(CFLAGS) $(EXTRA_CFLAGS) -c $< -o $@

$(LIB): $(OBJS)
	@mkdir -p $(@D)
	rm -f $@
	$(AR) $(ARFLAGS) $@ $(OBJS)

$(SPIKE_OBJS): EXTRA_CPPFLAGS = $(GUI_CFLAGS)
$(SNAP_OBJS): EXTRA_CPPFLAGS = $(GUI_CFLAGS) -Isrc/ui/spike
$(UISHELL_OBJS): EXTRA_CPPFLAGS = $(GUI_CFLAGS) -Isrc/ui/spike
# Only the codecs and their tests see the optional libraries' headers (and PC_NO_EXR / PC_NO_HEIF).
# Only the codecs see the optional libraries' headers.
$(filter $(BUILD)/obj/src/codecs/%,$(OBJS)): EXTRA_CPPFLAGS = $(CODEC_CFLAGS)
$(BUILD)/tests/codecs/%: EXTRA_CPPFLAGS = $(CODEC_CFLAGS)
$(filter $(BUILD)/obj/src/raw/%,$(OBJS)): EXTRA_CPPFLAGS = $(JXL_CFLAGS)
$(BUILD)/tests/raw/%: EXTRA_CPPFLAGS = $(JXL_CFLAGS)
# Only src/plugins sees WAMR's API header (and PC_HAVE_WAMR); WAMR itself gets its configuration
# and no FMA contraction (wasm float operations are IEEE single operations).
$(filter $(BUILD)/obj/src/plugins/%,$(OBJS)): EXTRA_CPPFLAGS = $(PLUGINS_CFLAGS)
$(WAMR_OBJS): THIRD_CPPFLAGS = $(WAMR_CPPFLAGS)
# Sanitizer builds, WAMR objects only: its fast interpreter stores operands and code pointers
# unaligned on purpose (x86-64/AArch64), so UBSan's alignment check is off; and it detects native
# stack overflow by comparing a local's address with the thread's stack bounds, which ASan's
# "fake stack" (detect_stack_use_after_return, on by default in recent compilers) breaks.
SAN_FLAGS_SEEN = $(CFLAGS) $(EXTRA_CFLAGS)
WAMR_ASAN_STACK := $(if $(call cc_accepts,-fsanitize=address -fsanitize-address-use-after-return=never),-fsanitize-address-use-after-return=never,$(if $(call cc_accepts,-fsanitize=address --param=asan-use-after-return=0),--param=asan-use-after-return=0))
WAMR_SAN = $(if $(findstring undefined,$(SAN_FLAGS_SEEN)),-fno-sanitize=alignment) $(if $(findstring address,$(SAN_FLAGS_SEEN)),$(WAMR_ASAN_STACK))
$(WAMR_OBJS): THIRD_CFLAGS = $(FP_STRICT) $(WAMR_SAN)

$(SPIKE): $(SPIKE_OBJS) $(LIB)
	@mkdir -p $(@D)
	$(CC) $(SPIKE_OBJS) $(LIB) $(ALL_LDFLAGS) $(GUI_LIBS) $(ALL_LDLIBS) -o $@

$(SNAP): $(SNAP_OBJS) $(SNAP_HELPERS) $(LIB)
	@mkdir -p $(@D)
	$(CC) $(SNAP_OBJS) $(SNAP_HELPERS) $(LIB) $(ALL_LDFLAGS) $(GUI_LIBS) $(ALL_LDLIBS) -o $@

$(UISHELL): $(UISHELL_OBJS) $(SNAP_HELPERS) $(LIB)
	@mkdir -p $(@D)
	$(CC) $(UISHELL_OBJS) $(SNAP_HELPERS) $(LIB) $(ALL_LDFLAGS) $(GUI_LIBS) $(ALL_LDLIBS) -o $@

# photocraft-cli (src/cli): the headless command line, linked against the library.
$(BUILD)/bin/photocraft-cli: $(CLI_OBJS) $(LIB)
	@mkdir -p $(@D)
	$(CC) $(CLI_OBJS) $(LIB) $(ALL_LDFLAGS) $(ALL_LDLIBS) -o $@

spike:
ifeq ($(HAVE_GUI),)
	@echo "spike: needs pkg-config packages: $(GUI_PKGS)"; exit 1
else
	@$(MAKE) $(SPIKE)
endif

# photocraft-cli tests link against the app objects (minus its main) plus the library. Listed
# before the generic rule: GNU make 3.81 takes the first matching pattern rule.
CLI_TEST_OBJS := $(filter-out $(BUILD)/obj/src/cli/main.o,$(CLI_OBJS))
$(BUILD)/tests/cli/%: tests/cli/%.c $(CLI_TEST_OBJS) $(LIB)
	@mkdir -p $(@D)
	$(CC) $(ALL_CPPFLAGS) -Itests $(ALL_CFLAGS) $(DEPFLAGS) $< $(CLI_TEST_OBJS) $(LIB) $(ALL_LDFLAGS) $(ALL_LDLIBS) -o $@

# The PhotocraftApp control test links the shell objects (app + control, no main) and the GUI
# helpers; it never initialises SDL video (no GL context, no Mesa leak under the sanitizers), so it
# runs in the ordinary test loop. Explicit target: it wins over the generic pattern rule.
ifneq ($(HAVE_GUI),)
$(BUILD)/tests/ui/test_shell_control: tests/ui/test_shell_control.c $(UISHELL_TEST_OBJS) $(SNAP_HELPERS) $(LIB)
	@mkdir -p $(@D)
	$(CC) $(ALL_CPPFLAGS) $(GUI_CFLAGS) -Isrc/ui/spike -Itests $(ALL_CFLAGS) $(DEPFLAGS) $< $(UISHELL_TEST_OBJS) $(SNAP_HELPERS) $(LIB) $(ALL_LDFLAGS) $(GUI_LIBS) $(ALL_LDLIBS) -o $@
endif

$(BUILD)/tests/%: tests/%.c $(LIB)
	@mkdir -p $(@D)
	$(CC) $(ALL_CPPFLAGS) -Itests $(ALL_CFLAGS) $(DEPFLAGS) $< $(LIB) $(ALL_LDFLAGS) $(ALL_LDLIBS) -o $@

test: $(TEST_BINS)
	@fail=0; for t in $(TEST_BINS); do \
		printf '%s: ' "$$t"; $(TEST_ENV) $(TEST_WRAP) ./$$t || { fail=1; echo "*** $$t FAILED"; }; \
	done; exit $$fail

check:
	@sh tools/check-layers.sh
	$(MAKE) BUILD=build/check STRICT=1 all test

# Real-file test corpora (gitignored), sha256-verified: tools/corpus.sh list|fetch|verify.
corpus:
	@sh tools/corpus.sh fetch all

SAN := $(if $(call cc_accepts,-fsanitize=address$(,)undefined),-fsanitize=address$(,)undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
sanitize:
ifeq ($(SAN),)
	@echo "sanitize: $(CC) has no -fsanitize=address,undefined; skipped"
else
	$(MAKE) BUILD=build/san STRICT=1 CFLAGS="-O1 -g" EXTRA_CFLAGS="$(SAN)" EXTRA_LDFLAGS="$(SAN)" \
		TEST_ENV="ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1" all test
endif

analyze:
	@if command -v clang >/dev/null 2>&1; then \
		for f in $(SRCS); do \
			clang --analyze -Xclang -analyzer-werror -std=c17 -D_POSIX_C_SOURCE=200809L -Isrc $(THIRD_PARTY:%=-Ithird_party/%) $(PKG_CFLAGS) \
				$(CODEC_CFLAGS) $(JXL_CFLAGS) $(PLUGINS_CFLAGS) -o /dev/null $$f || exit 1; \
		done; \
		for f in $(APP_SRCS); do \
			clang --analyze -Xclang -analyzer-werror -std=c17 -D_POSIX_C_SOURCE=200809L -Isrc $(THIRD_PARTY:%=-Ithird_party/%) $(PKG_CFLAGS) \
				-o /dev/null $$f || exit 1; \
		done; \
		for f in $(if $(HAVE_GUI),$(SPIKE_SRCS)); do \
			clang --analyze -Xclang -analyzer-werror -std=c17 -D_POSIX_C_SOURCE=200809L -Isrc $(THIRD_PARTY:%=-Ithird_party/%) $(PKG_CFLAGS) \
				$(GUI_CFLAGS) -o /dev/null $$f || exit 1; \
		done; \
		for f in $(if $(HAVE_GUI),$(SNAP_SRCS)); do \
			clang --analyze -Xclang -analyzer-werror -std=c17 -D_POSIX_C_SOURCE=200809L -Isrc -Isrc/ui/spike $(THIRD_PARTY:%=-Ithird_party/%) $(PKG_CFLAGS) \
				$(GUI_CFLAGS) -o /dev/null $$f || exit 1; \
		done; \
		for f in $(if $(HAVE_GUI),$(UISHELL_SRCS)); do \
			clang --analyze -Xclang -analyzer-werror -std=c17 -D_POSIX_C_SOURCE=200809L -Isrc -Isrc/ui/spike $(THIRD_PARTY:%=-Ithird_party/%) $(PKG_CFLAGS) \
				$(GUI_CFLAGS) -o /dev/null $$f || exit 1; \
		done; echo "analyze: clang static analyzer clean ($(words $(SRCS) $(APP_SRCS) $(if $(HAVE_GUI),$(SPIKE_SRCS) $(SNAP_SRCS) $(UISHELL_SRCS))) files)"; \
	elif command -v cppcheck >/dev/null 2>&1; then \
		cppcheck --error-exitcode=1 --std=c11 --enable=warning,portability -Isrc $(THIRD_PARTY:%=-Ithird_party/%) $(SRCS); \
	else echo "analyze: neither clang nor cppcheck found; skipped"; fi

TSAN := $(if $(call cc_accepts,-fsanitize=thread),-fsanitize=thread)
tsan:
ifeq ($(TSAN),)
	@echo "tsan: $(CC) has no -fsanitize=thread; skipped"
else
	$(MAKE) BUILD=build/tsan STRICT=1 CFLAGS="-O1 -g" EXTRA_CFLAGS="$(TSAN)" EXTRA_LDFLAGS="$(TSAN)" \
		TEST_ENV="TSAN_OPTIONS=halt_on_error=1" all test
endif

valgrind:
	@if ! command -v valgrind >/dev/null 2>&1; then echo "valgrind: not found; skipped"; \
	elif ! valgrind -q true >/dev/null 2>&1; then \
		echo "valgrind: cannot run even 'true' on this system (unsupported CPU instructions in libc?); skipped"; \
	else \
		$(MAKE) BUILD=build/valgrind CFLAGS="-O1 -g" \
			TEST_WRAP="valgrind -q --error-exitcode=1 --leak-check=full --errors-for-leak-kinds=definite" all test; \
	fi

install: all
	@mkdir -p $(DESTDIR)$(BINDIR)
	@for p in $(PROGRAMS); do cp $(BUILD)/bin/$$p $(DESTDIR)$(BINDIR)/$$p && chmod 755 $(DESTDIR)$(BINDIR)/$$p; done
	@echo "installed: $(if $(PROGRAMS),$(PROGRAMS),nothing yet (no programs ported))"

clean:
	rm -rf build

-include $(OBJS:.o=.d) $(SPIKE_OBJS:.o=.d) $(SNAP_OBJS:.o=.d) $(UISHELL_OBJS:.o=.d) $(APP_OBJS:.o=.d) $(TEST_BINS:=.d)
