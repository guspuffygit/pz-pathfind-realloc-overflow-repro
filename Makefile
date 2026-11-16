CFLAGS ?= -O2 -Wall

libpfprobe.so: probe.c
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $< -ldl

clean:
	rm -f libpfprobe.so

.PHONY: clean
