.PHONY: all clean

OUT=mini

all: mini.c
	riscv32-unknown-elf-gcc -nostdlib --sysroot=/usr/riscv32-unknown-elf -march=rv32id $^ -o $(OUT)

clean:
	rm mini
