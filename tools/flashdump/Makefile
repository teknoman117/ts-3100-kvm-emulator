ASM=nasm
BIN=dump.com

all: flashdump

clean:
	rm -f $(BIN)

flashdump: flashdump.asm
	$(ASM) -f bin -o $(BIN) flashdump.asm
