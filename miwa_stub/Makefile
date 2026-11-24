SOURCES_DIR=./src
BUILD_DIR=./build
RSC_DIR=./rsc

CC=m68k-atari-mint-gcc
CFLAGS=-c -O3
LINKFLAGS=-s -Wl,--gc-sections -fomit-frame-pointer

# VASM PARAMETERS
ASM=vasmm68k_mot
ASMFLAGS=-Faout -quiet -x -m68000 -spaces -showopt

all:
	mkdir -p $(BUILD_DIR)
	${CC} -nostdlib lib/crt0.o src/loader.c -o build/miwaldr.prg -s -Llib -lcmini -lgcc

clean:
	rm -rf $(BUILD_DIR)
