PREFIX ?= /usr
UNITDIR ?= $(PREFIX)/lib/systemd/system
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -std=gnu11

aurora-kbd-light: aurora-kbd-light.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< -lm

install: aurora-kbd-light
	install -Dm0755 aurora-kbd-light $(DESTDIR)$(PREFIX)/bin/aurora-kbd-light
	install -Dm0644 aurora-kbd-light.service $(DESTDIR)$(UNITDIR)/aurora-kbd-light.service

clean:
	rm -f aurora-kbd-light

.PHONY: install clean
