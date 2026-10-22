# rgds-terminal. Build with scripts/build.sh (runs this Makefile in an arm64
# Debian container), then install with scripts/deploy.sh.

VTERM := third_party/libvterm-0.3.3
PKGS := sdl2 SDL2_ttf SDL2_gfx

CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra $(shell pkg-config --cflags $(PKGS)) -I$(VTERM)/include
LDLIBS := $(shell pkg-config --libs $(PKGS)) -lutil

SRC := $(wildcard src/*.c)
OBJ := $(SRC:src/%.c=build/obj/%.o)
VTERM_OBJ := $(patsubst $(VTERM)/src/%.c,build/obj/vterm/%.o,$(wildcard $(VTERM)/src/*.c))

app/terminal/dsterm: $(OBJ) $(VTERM_OBJ)
	$(CC) -o $@ $^ $(LDLIBS)

build/obj/%.o: src/%.c src/dsterm.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

# libvterm is compiled in as-is, without our warning flags.
build/obj/vterm/%.o: $(VTERM)/src/%.c
	@mkdir -p $(@D)
	$(CC) -O2 -std=c99 -I$(VTERM)/include -c -o $@ $<

clean:
	rm -rf build/obj app/terminal/dsterm

.PHONY: clean
