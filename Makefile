PLUGIN  := libweather.so
PKGS    := gtk+-3.0 gio-2.0 gio-unix-2.0 json-glib-1.0 gtk-layer-shell-0
CFLAGS  ?= -O2 -Wall -Wextra
CFLAGS  += -fPIC $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lm
PREFIX  ?= $(HOME)/.local/lib/waybar
DATADIR ?= $(HOME)/.local/share/waybar-weather

$(PLUGIN): src/weather.c
	$(CC) $(CFLAGS) -shared -o $@ $< $(LDLIBS)

install: $(PLUGIN)
	install -Dm755 $(PLUGIN) $(PREFIX)/$(PLUGIN)
	install -Dm644 -t $(DATADIR) assets/sunny.svg assets/night.svg assets/pcloudy.svg assets/npcloudy.svg assets/cloud.svg assets/fog.svg assets/rain.svg assets/pour.svg assets/snow.svg assets/heavy-snow.svg assets/tstorm.svg
	@echo "installed to $(PREFIX)/$(PLUGIN) + icons in $(DATADIR)"

clean:
	rm -f $(PLUGIN)
.PHONY: install clean
