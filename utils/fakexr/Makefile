# fakexr: a fake OpenXR runtime for testing without a headset (see fakexr.c).
# make            -> build/fakexr32/ (i386, for the 32-bit engine) and build/fakexr64/
# Use: XR_RUNTIME_JSON=$PWD/build/fakexr32/fakexr.json FAKEXR_CONTROL=/path/fifo ./xash3d ...
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
SRC = fakexr.c

all: build/fakexr32/libfakexr.so build/fakexr64/libfakexr.so

build/fakexr%/libfakexr.so: $(SRC)
	mkdir -p $(dir $@)
	$(CC) -m$* -shared -fPIC -fvisibility=hidden $(CFLAGS) -o $@ $(SRC) -lGL -lm
	printf '{\n  "file_format_version": "1.0.0",\n  "runtime": {\n    "name": "fakexr",\n    "library_path": "./libfakexr.so"\n  }\n}\n' > $(dir $@)fakexr.json

clean:
	rm -rf build

.PHONY: all clean
