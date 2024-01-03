.PHONY: all clean

SRCS = $(wildcard *.c)

PROGS = $(patsubst %.c,%,$(SRCS))

all: $(PROGS)

# the binaries this produces are included in the git repo, because they're just for testing purposes and not part of the actual project. Makes it easier to try out the translator if you don't have an rv32 toolchain installed
%: %.c
	riscv32-unknown-elf-gcc -nostdlib --sysroot=/usr/riscv32-unknown-elf -march=rv32imd $^ -o $(basename $^);\

clean:
	for i in $(SRCS); do \
		i=$$(basename $$i);\
		rm -f $${i%.*};\
	done
