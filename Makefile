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
MODULES = base geom cms color raster psd codecs raw affinity doc ops paint algo vector text
# pkg-config packages the current modules need.
PKGS = zlib libzstd libblake3 libpng16 libjpeg libwebp libwebpmux libwebpdemux freetype2 harfbuzz fribidi
# Vendored third-party C (REWRITE_PLAN.md D17), compiled without our strict warnings: it isn't
# our code to fix, and upstream builds it warning-free with its own flags.
THIRD_PARTY = yyjson nuklear pocketfft
# Executables (src/cli, src/app) once they exist.
PROGRAMS =
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

HAVE_GUI := $(shell $(PKG_CONFIG) --exists $(GUI_PKGS) 2>/dev/null && echo y)
GUI_CFLAGS := $(if $(HAVE_GUI),$(shell $(PKG_CONFIG) --cflags $(GUI_PKGS)))
GUI_LIBS := $(if $(HAVE_GUI),$(shell $(PKG_CONFIG) --libs $(GUI_PKGS)))

ALL_CPPFLAGS = -D_POSIX_C_SOURCE=200809L -Isrc $(THIRD_PARTY:%=-Ithird_party/%) $(PKG_CFLAGS) $(EXTRA_CPPFLAGS) $(CPPFLAGS)
ALL_CFLAGS = -std=c17 $(FP_STRICT) $(WARN) $(WERROR) $(PTHREAD) $(CFLAGS) $(EXTRA_CFLAGS)
ALL_LDFLAGS = $(PTHREAD) $(LDFLAGS) $(EXTRA_LDFLAGS)
ALL_LDLIBS = $(PKG_LIBS) $(EXR_LIBS) $(HEIF_LIBS) $(LDLIBS) -lm

SRCS := $(foreach m,$(MODULES),$(wildcard src/$(m)/*.c))
THIRD_SRCS := $(foreach t,$(THIRD_PARTY),$(wildcard third_party/$(t)/*.c))
# Binary data compiled in (POSIX od + sed; no xxd): name = file under data/ (see GEN rules below).
# Bundled fonts (src/text/bundled.h), embedded the same way (assets/fonts/<name>.ttf).
FONT_GEN := Inter-Regular Inter-Medium Inter-SemiBold JetBrainsMono-Regular
GEN_SRCS := $(BUILD)/gen/coated_cmyk_icc.c $(FONT_GEN:%=$(BUILD)/gen/font_%.c)
OBJS := $(SRCS:%.c=$(BUILD)/obj/%.o) $(THIRD_SRCS:%.c=$(BUILD)/obj/%.o) $(GEN_SRCS:%.c=%.o)
LIB := $(BUILD)/libphotocraft.a
# B3 GUI stack spike (src/ui/spike): built when SDL3 and FreeType are installed.
SPIKE_SRCS := $(wildcard src/ui/spike/*.c)
SPIKE_OBJS := $(SPIKE_SRCS:%.c=$(BUILD)/obj/%.o)
SPIKE := $(BUILD)/bin/photocraft-spike
TEST_SRCS := $(wildcard tests/*/test_*.c)
TEST_BINS := $(TEST_SRCS:tests/%.c=$(BUILD)/tests/%)

.PHONY: all spike corpus test check sanitize tsan analyze valgrind install clean
.DEFAULT_GOAL := all

all: $(LIB) $(PROGRAMS:%=$(BUILD)/bin/%) $(if $(HAVE_GUI),$(SPIKE))

# Listed first: GNU make 3.81 takes the first matching pattern rule, not the most specific.
$(BUILD)/obj/third_party/%.o: third_party/%.c
	@mkdir -p $(@D)
	$(CC) -std=c17 $(PTHREAD) $(CFLAGS) $(EXTRA_CFLAGS) $(DEPFLAGS) -c $< -o $@

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

$(BUILD)/gen/%.o: $(BUILD)/gen/%.c
	$(CC) -std=c17 $(CFLAGS) $(EXTRA_CFLAGS) -c $< -o $@

$(LIB): $(OBJS)
	@mkdir -p $(@D)
	rm -f $@
	$(AR) $(ARFLAGS) $@ $(OBJS)

$(SPIKE_OBJS): EXTRA_CPPFLAGS = $(GUI_CFLAGS)
# Only the codecs and their tests see the optional libraries' headers (and PC_NO_EXR / PC_NO_HEIF).
# Only the codecs see the optional libraries' headers.
$(filter $(BUILD)/obj/src/codecs/%,$(OBJS)): EXTRA_CPPFLAGS = $(CODEC_CFLAGS)
$(BUILD)/tests/codecs/%: EXTRA_CPPFLAGS = $(CODEC_CFLAGS)

$(SPIKE): $(SPIKE_OBJS) $(LIB)
	@mkdir -p $(@D)
	$(CC) $(SPIKE_OBJS) $(LIB) $(ALL_LDFLAGS) $(GUI_LIBS) $(ALL_LDLIBS) -o $@

spike:
ifeq ($(HAVE_GUI),)
	@echo "spike: needs pkg-config packages: $(GUI_PKGS)"; exit 1
else
	@$(MAKE) $(SPIKE)
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
		TEST_ENV="ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1" test
endif

analyze:
	@if command -v clang >/dev/null 2>&1; then \
		for f in $(SRCS); do \
			clang --analyze -Xclang -analyzer-werror -std=c17 -D_POSIX_C_SOURCE=200809L -Isrc $(THIRD_PARTY:%=-Ithird_party/%) $(PKG_CFLAGS) \
				$(CODEC_CFLAGS) -o /dev/null $$f || exit 1; \
		done; \
		for f in $(if $(HAVE_GUI),$(SPIKE_SRCS)); do \
			clang --analyze -Xclang -analyzer-werror -std=c17 -D_POSIX_C_SOURCE=200809L -Isrc $(THIRD_PARTY:%=-Ithird_party/%) $(PKG_CFLAGS) \
				$(GUI_CFLAGS) -o /dev/null $$f || exit 1; \
		done; echo "analyze: clang static analyzer clean ($(words $(SRCS) $(if $(HAVE_GUI),$(SPIKE_SRCS))) files)"; \
	elif command -v cppcheck >/dev/null 2>&1; then \
		cppcheck --error-exitcode=1 --std=c11 --enable=warning,portability -Isrc $(THIRD_PARTY:%=-Ithird_party/%) $(SRCS); \
	else echo "analyze: neither clang nor cppcheck found; skipped"; fi

TSAN := $(if $(call cc_accepts,-fsanitize=thread),-fsanitize=thread)
tsan:
ifeq ($(TSAN),)
	@echo "tsan: $(CC) has no -fsanitize=thread; skipped"
else
	$(MAKE) BUILD=build/tsan STRICT=1 CFLAGS="-O1 -g" EXTRA_CFLAGS="$(TSAN)" EXTRA_LDFLAGS="$(TSAN)" \
		TEST_ENV="TSAN_OPTIONS=halt_on_error=1" test
endif

valgrind:
	@if ! command -v valgrind >/dev/null 2>&1; then echo "valgrind: not found; skipped"; \
	elif ! valgrind -q true >/dev/null 2>&1; then \
		echo "valgrind: cannot run even 'true' on this system (unsupported CPU instructions in libc?); skipped"; \
	else \
		$(MAKE) BUILD=build/valgrind CFLAGS="-O1 -g" \
			TEST_WRAP="valgrind -q --error-exitcode=1 --leak-check=full --errors-for-leak-kinds=definite" test; \
	fi

install: all
	@mkdir -p $(DESTDIR)$(BINDIR)
	@for p in $(PROGRAMS); do cp $(BUILD)/bin/$$p $(DESTDIR)$(BINDIR)/$$p && chmod 755 $(DESTDIR)$(BINDIR)/$$p; done
	@echo "installed: $(if $(PROGRAMS),$(PROGRAMS),nothing yet (no programs ported))"

clean:
	rm -rf build

-include $(OBJS:.o=.d) $(SPIKE_OBJS:.o=.d) $(TEST_BINS:=.d)
