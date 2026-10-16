CC ?= cc
PYTHON ?= python3
ROM ?= baserom.gb
CPPFLAGS += -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror
LDLIBS += -lX11 -lasound
SOURCES := main.c game.c screens.c level.c audio.c audio_driver.c audio_sfx.c sprites.c actors.c bosses.c contacts.c stage.c world.c savefile.c fireballs.c demo.c
HEADERS := game.h screens.h level.h audio.h audio_driver.h audio_sfx.h sprites.h actors.h bosses.h contacts.h stage.h world.h savefile.h fireballs.h demo.h

.DEFAULT_GOAL := all
.PHONY: all run
all: build/sml2-native

build:
	mkdir -p build

build/rom_data.c: $(ROM) tools/embed_rom.py | build
	$(PYTHON) tools/embed_rom.py $< $@

build/sml2-native: $(SOURCES) $(HEADERS) build/rom_data.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) build/rom_data.c $(LDFLAGS) $(LDLIBS) -o $@

run: build/sml2-native
	./build/sml2-native
