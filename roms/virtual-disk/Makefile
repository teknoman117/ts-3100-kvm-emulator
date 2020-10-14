ASM=nasm
BIN=option.rom
PYTHON=python3
CHECKSUM=checksum.py

all: option

clean:
	rm -f $(BIN) $(BIN).tmp

option: option.asm
	$(ASM) -f bin -o $(BIN).tmp option.asm
	$(PYTHON) $(CHECKSUM) $(BIN).tmp $(BIN)