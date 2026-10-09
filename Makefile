# PhotoCraft C17 build (REWRITE_PLAN.md D3, D20). Requires GNU make >= 3.81 (`gmake` on the BSDs).
#
#   make            build the library (and programs, once they exist)
#   make test       build and run every tests/*/test_*.c
#   make check      strict build (warnings are errors) + tests       -> build/check
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
MODULES = base
# pkg-config packages the current modules need.
PKGS = zlib libzstd libblake3
# Vendored third-party C (REWRITE_PLAN.md D17), compiled without our strict warnings: it isn't
# our code to fix, and upstream builds it warning-free with its own flags.
THIRD_PARTY = yyjson
# Executables (src/cli, src/app) once they exist.
PROGRAMS =

, := ,

# ---- compiler feature probes (flags a compiler rejects are simply not used) ----
cc_accepts = $(shell printf 'int main(void){return 0;}\n' | $(CC) $(1) -Werror -x c -o /dev/null - >/dev/null 2>&1 && echo y)

WARN_CANDIDATES = -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes \
                  -Wmissing-prototypes -Wvla -Wformat=2 -Wundef -Wcast-qual -Wpointer-arith
WARN := $(foreach f,$(WARN_CANDIDATES),$(if $(call cc_accepts,$(f)),$(f)))
DEPFLAGS := $(if $(call cc_accepts,-MMD -MP -MF /dev/null),-MMD -MP)
PTHREAD := $(if $(call cc_accepts,-pthread),-pthread,-lpthread)

STRICT ?=
WERROR = $(if $(STRICT),-Werror)

PKG_CFLAGS := $(if $(PKGS),$(shell $(PKG_CONFIG) --cflags $(PKGS)))
PKG_LIBS := $(if $(PKGS),$(shell $(PKG_CONFIG) --libs $(PKGS)))

ALL_CPPFLAGS = -D_POSIX_C_SOURCE=200809L -Isrc $(THIRD_PARTY:%=-Ithird_party/%) $(PKG_CFLAGS) $(CPPFLAGS)
ALL_CFLAGS = -std=c17 $(WARN) $(WERROR) $(PTHREAD) $(CFLAGS) $(EXTRA_CFLAGS)
ALL_LDFLAGS = $(PTHREAD) $(LDFLAGS) $(EXTRA_LDFLAGS)
ALL_LDLIBS = $(PKG_LIBS) $(LDLIBS) -lm

SRCS := $(foreach m,$(MODULES),$(wildcard src/$(m)/*.c))
THIRD_SRCS := $(foreach t,$(THIRD_PARTY),$(wildcard third_party/$(t)/*.c))
OBJS := $(SRCS:%.c=$(BUILD)/obj/%.o) $(THIRD_SRCS:%.c=$(BUILD)/obj/%.o)
LIB := $(BUILD)/libphotocraft.a
TEST_SRCS := $(wildcard tests/*/test_*.c)
TEST_BINS := $(TEST_SRCS:tests/%.c=$(BUILD)/tests/%)

.PHONY: all test check sanitize tsan analyze valgrind install clean
.DEFAULT_GOAL := all

all: $(LIB) $(PROGRAMS:%=$(BUILD)/bin/%)

# Listed first: GNU make 3.81 takes the first matching pattern rule, not the most specific.
$(BUILD)/obj/third_party/%.o: third_party/%.c
	@mkdir -p $(@D)
	$(CC) -std=c17 $(PTHREAD) $(CFLAGS) $(EXTRA_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(ALL_CPPFLAGS) $(ALL_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(LIB): $(OBJS)
	@mkdir -p $(@D)
	rm -f $@
	$(AR) $(ARFLAGS) $@ $(OBJS)

$(BUILD)/tests/%: tests/%.c $(LIB)
	@mkdir -p $(@D)
	$(CC) $(ALL_CPPFLAGS) -Itests $(ALL_CFLAGS) $(DEPFLAGS) $< $(LIB) $(ALL_LDFLAGS) $(ALL_LDLIBS) -o $@

test: $(TEST_BINS)
	@fail=0; for t in $(TEST_BINS); do \
		printf '%s: ' "$$t"; $(TEST_ENV) $(TEST_WRAP) ./$$t || { fail=1; echo "*** $$t FAILED"; }; \
	done; exit $$fail

check:
	$(MAKE) BUILD=build/check STRICT=1 test

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
				-o /dev/null $$f || exit 1; \
		done; echo "analyze: clang static analyzer clean ($(words $(SRCS)) files)"; \
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

-include $(OBJS:.o=.d) $(TEST_BINS:=.d)
