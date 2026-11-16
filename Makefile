LIB ?= $(HOME)/.steam/steam/steamapps/common/Project Zomboid Dedicated Server/linux64/libPZPathFind64.so
CFLAGS ?= -O2 -Wall

all: repro libpfprobe.so

repro: repro.c
	$(CC) $(CFLAGS) -o $@ $< -ldl

libpfprobe.so: probe.c
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $< -ldl

# Runs the standalone reproduction against LIB.
run: repro
	./repro "$(LIB)"

# Builds a fixed copy of LIB and runs the same reproduction against it.
run-patched: repro
	python3 patch.py "$(LIB)" libPZPathFind64.patched.so
	./repro ./libPZPathFind64.patched.so

clean:
	rm -f repro libpfprobe.so libPZPathFind64.patched.so

.PHONY: all run run-patched clean
