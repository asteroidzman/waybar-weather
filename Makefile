PLUGIN  := libweather.so
PKGS    := gtk+-3.0 gio-2.0 gio-unix-2.0 json-glib-1.0
CFLAGS  ?= -O2 -Wall -Wextra
CFLAGS  += -fPIC $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lm
PREFIX  ?= $(HOME)/.local/lib/waybar

$(PLUGIN): src/weather.c
	$(CC) $(CFLAGS) -shared -o $@ $< $(LDLIBS)

install: $(PLUGIN)
	install -Dm755 $(PLUGIN) $(PREFIX)/$(PLUGIN)
	@echo "installed to $(PREFIX)/$(PLUGIN)"

clean:
	rm -f $(PLUGIN)
.PHONY: install clean
