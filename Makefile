# standard cpp makefile
# assumes llvm is installed to /usr/local, like a custom LLVM build recommends

CPPC=g++
CFLAGS=-Wall -Wextra -Wpedantic -O3 -std=c++2b -fno-rtti -lz -lzstd -lcurses -Ifrvdec -Lfrvdec/builddir -lfrvdec -I/usr/local/include -L/usr/local/lib

# linked against llvm commit a403d75be7, but both LLVM 16 and 17 should work

LLVM_CONFIG=/usr/local/bin/llvm-config

SOURCES=main.cc
OUT=main

LLVM_CFLAGS=$(shell $(LLVM_CONFIG) --cppflags --ldflags --libs) -DLLVM_DISABLE_ABI_BREAKING_CHECKS_ENFORCING=1 
.PHONY: all debug clean frvdec-meson forPerf
all: frvdec-meson $(SOURCES)
		$(CPPC) $(SOURCES) $(CFLAGS) $(LLVM_CFLAGS) -DNDEBUG -o $(OUT)

debug: frvdec-meson $(SOURCES)
		$(CPPC) $(SOURCES) $(CFLAGS) $(LLVM_CFLAGS) -O0 -g -o $(OUT)

forPerf: frvdec-meson $(SOURCES)
		$(CPPC) $(SOURCES) $(CFLAGS) $(LLVM_CFLAGS) -DNDEBUG -O3 -g -o $(OUT)

frvdec-meson:
		cd frvdec && [ -d builddir ] || meson setup builddir
		cd frvdec/builddir && meson compile

clean:
		rm $(OUT)
		rm -rf frvdec/builddir
