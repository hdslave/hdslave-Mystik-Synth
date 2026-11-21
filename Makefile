# Mystik: a CLAP west-coast hand drum for the Akai MPC Sample (64-bit ARM Linux, Cortex-A55).
# Build inside a Linux arm64 image with ./build.sh, which runs `make all test`.
#   make            build/Mystik.clap
#   make test       the tests against the built plug-in (tests/host.cpp) and the sound's fingerprint
#                   (tests/fingerprint.cpp: the output to the bit, against tests/fingerprint.txt)
#   make bench      build/bench: renders busy patterns; tools/bench.sh counts their instructions
#   make sanitize   the host tests against a build with AddressSanitizer and UBSan (any error stops it)
CXX ?= g++
OUT := build
WARN := -Wall -Wextra -Werror
# The MPC's cores. -ffp-contract=fast lets the compiler fuse multiply-adds; nothing here relies on IEEE errno, traps or
# exceptions.
OPT := -O3 -mcpu=cortex-a55 -ffp-contract=fast -fno-math-errno -fno-trapping-math
CXXFLAGS := -std=c++17 $(OPT) $(WARN) -fno-exceptions -fno-rtti -fvisibility=hidden -fPIC -Ithird_party/clap/include -Isrc
# Only clap_entry is exported; libstdc++ and libgcc are linked in so the MPC needs nothing beyond libc and libm. -s leaves
# out the symbol table.
LDFLAGS := -shared -s -static-libgcc -static-libstdc++ -Wl,--gc-sections -Wl,-z,now -Wl,-O1 -Wl,--version-script=src/exports.map

SOURCES := src/mystik.cpp
HEADERS := $(wildcard src/*.h src/ui/*.h)

.PHONY: all test bench clean fingerprint-update sanitize
all: $(OUT)/Mystik.clap

$(OUT)/Mystik.clap: $(SOURCES) $(HEADERS) src/exports.map Makefile
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) $(SOURCES) $(LDFLAGS) -o $@

$(OUT)/host: tests/host.cpp tests/wav.h tests/png.h $(HEADERS) Makefile
	@mkdir -p $(OUT)
	$(CXX) -std=c++17 -O2 $(WARN) -Ithird_party/clap/include tests/host.cpp -ldl -o $@

$(OUT)/fingerprint: tests/fingerprint.cpp Makefile
	@mkdir -p $(OUT)
	$(CXX) -std=c++17 -O2 $(WARN) -Ithird_party/clap/include tests/fingerprint.cpp -ldl -o $@

test: $(OUT)/Mystik.clap $(OUT)/host $(OUT)/fingerprint
	$(OUT)/host $(OUT)/Mystik.clap $(OUT)
	$(OUT)/fingerprint $(OUT)/Mystik.clap tests/fingerprint.txt > /dev/null

# The sound's hashes after a change meant to change the sound (see tests/fingerprint.cpp).
fingerprint-update: $(OUT)/Mystik.clap $(OUT)/fingerprint
	$(OUT)/fingerprint $(OUT)/Mystik.clap > tests/fingerprint.txt

SAN := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g -O1
sanitize: $(SOURCES) $(HEADERS) tests/host.cpp tests/wav.h tests/png.h src/melodee_screen.h Makefile
	@mkdir -p $(OUT)/san
	$(CXX) -std=c++17 $(SAN) -mcpu=cortex-a55 $(WARN) -fPIC -shared -Ithird_party/clap/include -Isrc $(SOURCES) -o $(OUT)/san/Mystik.clap
	$(CXX) -std=c++17 $(SAN) $(WARN) -Ithird_party/clap/include tests/host.cpp -ldl -o $(OUT)/san/host
	ASAN_OPTIONS=abort_on_error=1:detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:abort_on_error=1:print_stacktrace=1 \
	    $(OUT)/san/host $(OUT)/san/Mystik.clap $(OUT)/san

$(OUT)/bench: tests/bench.cpp $(SOURCES) $(HEADERS) Makefile
	@mkdir -p $(OUT)
	$(CXX) -std=c++17 $(OPT) $(WARN) -g -Ithird_party/clap/include -Isrc tests/bench.cpp -o $@

bench: $(OUT)/bench

clean:
	rm -rf $(OUT)
