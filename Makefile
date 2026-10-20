# cdin-kernel build. Portable: CC/AR default to the platform toolchain.
# Override on the command line, e.g. `make CC=/path/to/gcc.exe AR=/path/to/ar.exe`.

CC      ?= cc
AR      ?= ar
RANLIB  ?= ranlib

# `OS=Windows_NT` is set by native Windows environments, but MSYS2/UCRT64
# intentionally runs a POSIX shell on Windows. Detect the host and shell
# separately so Windows DLLs can be built without routing Bash commands through
# cmd.exe.
HOST_OS := $(shell uname -s 2>/dev/null)
WINDOWS_HOST := $(if $(filter Windows_NT,$(OS)),1,$(if $(filter MINGW% MSYS% CYGWIN%,$(HOST_OS)),1,0))
WINDOWS_CMD_SHELL := $(if $(filter Windows_NT,$(OS)),$(if $(filter MINGW% MSYS% CYGWIN%,$(HOST_OS)),0,1),0)

# The public header is the single source of truth for the ABI version.
# Avoid capture-group parentheses inside $(shell ...): GNU make parses those
# parentheses before invoking the shell, which can break the function call.
ABI_MAJOR := $(shell awk '$$2 == "CDIN_KERNEL_ABI_MAJOR" { value = $$3; sub(/u$$/, "", value); print value; exit }' include/cdin/kernel.h)
ABI_MINOR := $(shell awk '$$2 == "CDIN_KERNEL_ABI_MINOR" { value = $$3; sub(/u$$/, "", value); print value; exit }' include/cdin/kernel.h)
ABI_PATCH := $(shell awk '$$2 == "CDIN_KERNEL_ABI_PATCH" { value = $$3; sub(/u$$/, "", value); print value; exit }' include/cdin/kernel.h)
ABI_VERSION := $(ABI_MAJOR).$(ABI_MINOR).$(ABI_PATCH)
# The default package version tracks the stable public C ABI version. Override only for a deliberate package-only release.
PACKAGE_VERSION ?= $(ABI_VERSION)

WARN    = -Wall -Wextra -Wshadow -Wconversion -Wstrict-prototypes -Wformat=2 -Wpedantic -Werror
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -Iinclude -Isrc $(WARN) -MMD -MP
LDFLAGS ?=
LDLIBS  ?=
ifneq ($(WINDOWS_CMD_SHELL),1)
  LDLIBS += -pthread
endif

BUILD  = build
OBJ    = $(BUILD)/obj
LIB    = $(BUILD)/libcdin_kernel.a
RUNNER = $(BUILD)/conformance-runner
TESTS  = $(BUILD)/test_p1 $(BUILD)/test_p3_conc
BENCH  = $(BUILD)/bench
BENCH_SCALE = $(BUILD)/bench-scale
FUZZ   = $(BUILD)/fuzz_ops
SH_OBJ = $(BUILD)/shared_obj
FFI_OBJ = $(BUILD)/ffi_obj

PREFIX       ?= /usr/local
includedir   ?= $(PREFIX)/include
libdir       ?= $(PREFIX)/lib
bindir       ?= $(PREFIX)/bin
pkgconfigdir ?= $(libdir)/pkgconfig

ifeq ($(WINDOWS_HOST),1)
  SHARED = $(BUILD)/cdin_kernel.dll
  FFI_SHARED = $(BUILD)/cdin_ffi.dll
  SHARED_LINK_FLAGS = -shared
  SHARED_LINK_OPTIONS =
  FFI_LINK_OPTIONS =
  SHARED_EXTRA_LIBS = -Wl,--out-implib,$(BUILD)/libcdin_kernel.dll.a
  FFI_EXTRA_LIBS = -Wl,--out-implib,$(BUILD)/libcdin_ffi.dll.a
  SHARED_CFLAGS = -DCDIN_KERNEL_BUILD_DLL
  SHARED_ALIAS =
  FFI_ALIAS =
else ifeq ($(HOST_OS),Darwin)
  SHARED = $(BUILD)/libcdin_kernel.dylib
  FFI_SHARED = $(BUILD)/libcdin_ffi.dylib
  SHARED_LINK_FLAGS = -dynamiclib
  SHARED_LINK_OPTIONS = -Wl,-install_name,@rpath/libcdin_kernel.$(ABI_MAJOR).dylib
  FFI_LINK_OPTIONS = -Wl,-install_name,@rpath/libcdin_ffi.dylib
  SHARED_EXTRA_LIBS =
  FFI_EXTRA_LIBS =
  SHARED_CFLAGS = -fPIC -fvisibility=hidden
  SHARED_ALIAS = $(BUILD)/libcdin_kernel.$(ABI_MAJOR).dylib
  FFI_ALIAS = $(BUILD)/libcdin_ffi.$(ABI_MAJOR).dylib
else
  SHARED = $(BUILD)/libcdin_kernel.so
  FFI_SHARED = $(BUILD)/libcdin_ffi.so
  SHARED_LINK_FLAGS = -shared
  SHARED_LINK_OPTIONS = -Wl,-soname,libcdin_kernel.so.$(ABI_MAJOR)
  FFI_LINK_OPTIONS = -Wl,-soname,libcdin_ffi.so.$(ABI_MAJOR)
  SHARED_EXTRA_LIBS =
  FFI_EXTRA_LIBS =
  SHARED_CFLAGS = -fPIC -fvisibility=hidden
  SHARED_ALIAS = $(BUILD)/libcdin_kernel.so.$(ABI_MAJOR)
  FFI_ALIAS = $(BUILD)/libcdin_ffi.so.$(ABI_MAJOR)
endif

# Recursive: src/*.c plus every module directory under src/.
SRC = $(sort $(wildcard src/*.c) $(wildcard src/*/*.c))
# src/new_kernel_code is the integration reference, kept for comparison only.
SRC := $(filter-out src/new_kernel_code/%,$(SRC))
RUN_SRC = $(sort $(wildcard conformance/runner/*.c))
# Objects are flattened into build/obj with '/' replaced by '_', so no
# per-directory mkdir is needed.
K_REL   = $(patsubst src/%.c,%,$(SRC))
K_FLAT  = $(subst /,_,$(K_REL))
K_OBJ   = $(addprefix $(OBJ)/,$(addsuffix .o,$(K_FLAT)))
R_OBJ   = $(patsubst conformance/runner/%.c,$(OBJ)/runner_%.o,$(RUN_SRC))
VECS    = $(sort $(wildcard conformance/vec/*/*.vec))

ifeq ($(WINDOWS_CMD_SHELL),1)
  # Native cmd.exe build. MSYS2/UCRT64 uses the POSIX branch even though its
  # target binaries are Windows DLLs.
  DEL = cmd /c del /q $(subst /,\\,$(1))
  MKDIR = powershell -NoProfile -Command "New-Item -ItemType Directory -Force -Path build/obj,build/shared_obj,build/ffi_obj | Out-Null"
else
  DEL = rm -f $(1)
  MKDIR = mkdir -p build/obj build/shared_obj build/ffi_obj
endif

$(shell $(MKDIR))

.PHONY: all shared ffi-shared quickstart check check-vectors check-tests vectors asan tsan fuzz-build bench bench-scale clean trace tests install uninstall

all: $(LIB)

# Shared builds use separately compiled position-independent objects. `ffi-shared`
# adds the optional examples/ffi bridge, not part of the stable public ABI.
shared: $(SHARED) $(SHARED_ALIAS)
ffi-shared: $(FFI_SHARED) $(FFI_ALIAS)
quickstart: $(BUILD)/quickstart
	$(BUILD)/quickstart

$(SHARED): $(addprefix $(SH_OBJ)/,$(addsuffix .o,$(K_FLAT)))
	$(CC) $(SHARED_LINK_FLAGS) $(LDFLAGS) $(SHARED_LINK_OPTIONS) $(SHARED_EXTRA_LIBS) -o $@ $^ $(LDLIBS)

$(FFI_SHARED): $(addprefix $(SH_OBJ)/,$(addsuffix .o,$(K_FLAT))) $(FFI_OBJ)/cdin_ffi.o
	$(CC) $(SHARED_LINK_FLAGS) $(LDFLAGS) $(FFI_LINK_OPTIONS) $(FFI_EXTRA_LIBS) -o $@ $^ $(LDLIBS)

ifneq ($(strip $(SHARED_ALIAS)),)
$(SHARED_ALIAS): $(SHARED)
	ln -sfn $(notdir $(SHARED)) $@
endif

ifneq ($(strip $(FFI_ALIAS)),)
$(FFI_ALIAS): $(FFI_SHARED)
	ln -sfn $(notdir $(FFI_SHARED)) $@
endif

$(BUILD)/quickstart: examples/c/quickstart.c $(LIB) include/cdin/kernel.h
	$(CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude $< $(LIB) $(LDLIBS) -o $@


$(LIB): $(K_OBJ)
	$(AR) rcs $@ $^
	$(RANLIB) $@

# A pattern rule cannot flatten '/' in the target, so one explicit rule per
# source file is generated instead. -MF redirects the dependency file to the
# flattened name so no per-directory mkdir is needed.
define K_RULE
$(OBJ)/$(subst /,_,$(subst .c,.o,$(1))).o: src/$(1).c
	$$(CC) $$(CFLAGS) -MF $$(@:.o=.d) -c $$< -o $$@
endef
$(foreach f,$(SRC),$(eval $(call K_RULE,$(patsubst src/%.c,%,$(f)))))

define SH_K_RULE
$(SH_OBJ)/$(subst /,_,$(subst .c,.o,$(1))).o: src/$(1).c
	$$(CC) $$(CFLAGS) $$(SHARED_CFLAGS) -MF $$(@:.o=.d) -c $$< -o $$@
endef
$(foreach f,$(SRC),$(eval $(call SH_K_RULE,$(patsubst src/%.c,%,$(f)))))

$(FFI_OBJ)/cdin_ffi.o: examples/ffi/cdin_ffi.c examples/ffi/cdin_ffi.h include/cdin/kernel.h
	$(CC) $(CFLAGS) $(SHARED_CFLAGS) -Iexamples/ffi -MF $(@:.o=.d) -c $< -o $@

$(OBJ)/runner_%.o: conformance/runner/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(RUNNER): $(R_OBJ) $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) $(R_OBJ) $(LIB) -o $@

# Conformance gate: every vector and every C regression test must pass.
# `make check` stops at the first failing gate; run the two halves separately to
# get both results in one go (check-vectors, check-tests).
check: check-vectors check-tests

# One process per vector file. The kernel keeps some state at process scope
# (shutdown flag, subscriber table), so a defect in one vector would otherwise
# contaminate every file that runs after it and hide the real result.
ifeq ($(WINDOWS_CMD_SHELL),1)
  # cmd treats the forward slashes in the binary path as switches.
  RUNNER_EXE = $(subst /,\\,$(RUNNER))
  VEC_LOOP = cmd /c "for %f in ($(VECS)) do @$(RUNNER_EXE) %f || exit /b 1"
  VEC_LOOP_ALL = cmd /c "for %f in ($(VECS)) do @$(RUNNER_EXE) %f"
else
  RUNNER_EXE = $(RUNNER)
  VEC_LOOP = for f in $(VECS); do $(RUNNER) $$f || exit 1; done
  VEC_LOOP_ALL = for f in $(VECS); do $(RUNNER) $$f; done; exit 0
endif

check-vectors: $(RUNNER)
	@echo "vectors: $(words $(VECS)), one process per file"
	@$(VEC_LOOP)

# Every vector, one process per file, failures ignored: this is the reporting
# target. The gate is check-vectors, which stops at the first failing file.
vectors: $(RUNNER)
	-@$(VEC_LOOP_ALL)

check-tests: $(TESTS)
	$(BUILD)/test_p1
	$(BUILD)/test_p3_conc

$(BUILD)/test_p1: tests/test_p1.c $(OBJ)/runner_alloc_strict.o $(LIB) include/cdin/kernel.h src/internal.h conformance/runner/alloc_strict.h
	$(CC) $(CFLAGS) $(LDFLAGS) -Isrc -Iconformance/runner tests/test_p1.c \
	    $(OBJ)/runner_alloc_strict.o $(LIB) -o $@

$(BUILD)/test_p3_conc: tests/test_p3_conc.c $(OBJ)/runner_alloc_strict.o $(LIB) include/cdin/kernel.h conformance/runner/alloc_strict.h
	$(CC) $(CFLAGS) $(LDFLAGS) -Isrc -Iconformance/runner tests/test_p3_conc.c \
	    $(OBJ)/runner_alloc_strict.o $(LIB) $(LDLIBS) -o $@

tests: check-tests

# Sanitizer targets need a POSIX shell and a toolchain with the sanitizer
# runtime (CI on Ubuntu, or WSL). They run EVERY vector (the vector gate is
# check-vectors) and then the C tests, which decide the sanitizer gate.
asan: CFLAGS  += -fsanitize=address,undefined -g
asan: LDFLAGS += -fsanitize=address,undefined
asan:
	$(MAKE) clean
	$(MAKE) check-vectors
	$(MAKE) check-tests
	@echo "asan gate GREEN: every vector and every C test passed with no sanitizer report"

tsan: CFLAGS  += -fsanitize=thread -g
tsan: LDFLAGS += -fsanitize=thread
tsan:
	$(MAKE) clean
	$(MAKE) check-vectors
	$(MAKE) check-tests
	@echo "tsan gate GREEN: every vector and every C test passed with no sanitizer report"

# libFuzzer targets (clang only).
fuzz-build: CFLAGS  += -fsanitize=fuzzer,address,undefined -g
fuzz-build: LDFLAGS += -fsanitize=fuzzer,address,undefined
fuzz-build: build/fuzz_abi build/fuzz_classifiers build/fuzz_vector_parser $(FUZZ)

ifneq ($(STANDALONE),)
FUZZ_CFLAGS := -DSTANDALONE -Wno-unused-parameter
endif
build/fuzz_ops: fuzz/fuzz_ops.c support/strict_alloc.c $(LIB)
	$(CC) $(CFLAGS) $(FUZZ_CFLAGS) $(LDFLAGS) fuzz/fuzz_ops.c support/strict_alloc.c \
	    -Iinclude -I. $(LIB) -o $@

build/fuzz_abi: tests/fuzz_abi.c $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) tests/fuzz_abi.c -Iinclude $(LIB) -o $@

build/fuzz_classifiers: tests/fuzz_classifiers.c $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) tests/fuzz_classifiers.c -Iinclude $(LIB) -o $@

build/fuzz_vector_parser: tests/fuzz_vector_parser.c conformance/runner/parser.c conformance/runner/parser.h
	$(CC) $(CFLAGS) $(LDFLAGS) tests/fuzz_vector_parser.c conformance/runner/parser.c -Iinclude -Iconformance/runner -o $@

support/strict_alloc.o: support/strict_alloc.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BENCH): bench/bench.c $(LIB) support/strict_alloc.o
	$(CC) $(CFLAGS) $(LDFLAGS) bench/bench.c support/strict_alloc.o -I. \
	    $(LIB) -o $@

$(BENCH_SCALE): bench/bench_scale.c $(LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) bench/bench_scale.c -Iinclude $(LIB) $(LDLIBS) -o $@

# Large-document benchmark; examples: make bench-scale SIZE_MIB=125 (or 500 / 1024)
SIZE_MIB ?= 125
bench-scale: $(BENCH_SCALE)
	$(BENCH_SCALE) $(SIZE_MIB)

# Regenerate the requirement -> vector -> result table from the vectors.
trace: $(RUNNER)
	$(RUNNER) --trace $(VECS)

# Install the public header and both link-time/runtime forms of the core
# library. DESTDIR is intended for package staging and CI smoke tests.
install: all shared
	install -d "$(DESTDIR)$(includedir)/cdin" "$(DESTDIR)$(libdir)" "$(DESTDIR)$(bindir)" "$(DESTDIR)$(pkgconfigdir)"
	install -m 0644 include/cdin/kernel.h "$(DESTDIR)$(includedir)/cdin/kernel.h"
	install -m 0644 $(LIB) "$(DESTDIR)$(libdir)/libcdin_kernel.a"
	sed -e 's|@VERSION@|$(PACKAGE_VERSION)|g' packaging/cdin-kernel.pc.in > "$(DESTDIR)$(pkgconfigdir)/cdin-kernel.pc"
ifeq ($(WINDOWS_HOST),1)
	install -m 0644 $(BUILD)/libcdin_kernel.dll.a "$(DESTDIR)$(libdir)/libcdin_kernel.dll.a"
	install -m 0755 $(SHARED) "$(DESTDIR)$(bindir)/cdin_kernel.dll"
else ifeq ($(HOST_OS),Darwin)
	install -m 0755 $(SHARED) "$(DESTDIR)$(libdir)/libcdin_kernel.$(ABI_VERSION).dylib"
	ln -sfn "libcdin_kernel.$(ABI_VERSION).dylib" "$(DESTDIR)$(libdir)/libcdin_kernel.$(ABI_MAJOR).dylib"
	ln -sfn "libcdin_kernel.$(ABI_MAJOR).dylib" "$(DESTDIR)$(libdir)/libcdin_kernel.dylib"
else
	install -m 0755 $(SHARED) "$(DESTDIR)$(libdir)/libcdin_kernel.so.$(ABI_VERSION)"
	ln -sfn "libcdin_kernel.so.$(ABI_VERSION)" "$(DESTDIR)$(libdir)/libcdin_kernel.so.$(ABI_MAJOR)"
	ln -sfn "libcdin_kernel.so.$(ABI_MAJOR)" "$(DESTDIR)$(libdir)/libcdin_kernel.so"
endif

uninstall:
	rm -f "$(DESTDIR)$(includedir)/cdin/kernel.h" "$(DESTDIR)$(libdir)/libcdin_kernel.a" "$(DESTDIR)$(pkgconfigdir)/cdin-kernel.pc"
ifeq ($(WINDOWS_HOST),1)
	rm -f "$(DESTDIR)$(libdir)/libcdin_kernel.dll.a" "$(DESTDIR)$(bindir)/cdin_kernel.dll"
else ifeq ($(HOST_OS),Darwin)
	rm -f "$(DESTDIR)$(libdir)/libcdin_kernel.$(ABI_VERSION).dylib" "$(DESTDIR)$(libdir)/libcdin_kernel.$(ABI_MAJOR).dylib" "$(DESTDIR)$(libdir)/libcdin_kernel.dylib"
else
	rm -f "$(DESTDIR)$(libdir)/libcdin_kernel.so.$(ABI_VERSION)" "$(DESTDIR)$(libdir)/libcdin_kernel.so.$(ABI_MAJOR)" "$(DESTDIR)$(libdir)/libcdin_kernel.so"
endif

clean:
	-$(call DEL,$(OBJ)/*.o $(OBJ)/*.d $(SH_OBJ)/*.o $(SH_OBJ)/*.d $(FFI_OBJ)/*.o $(FFI_OBJ)/*.d $(LIB) $(SHARED) $(FFI_SHARED) $(SHARED_ALIAS) $(FFI_ALIAS) $(BUILD)/libcdin_kernel.dll.a $(BUILD)/libcdin_ffi.dll.a $(BUILD)/quickstart $(RUNNER) $(TESTS) $(BENCH) $(FUZZ) build/fuzz_abi build/fuzz_classifiers build/fuzz_vector_parser support/strict_alloc.o)

-include $(wildcard $(OBJ)/*.d $(SH_OBJ)/*.d $(FFI_OBJ)/*.d)
