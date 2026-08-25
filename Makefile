CC ?= cc
PKG_CONFIG ?= pkg-config

CPPFLAGS += $(shell $(PKG_CONFIG) --cflags libdrm)
CFLAGS ?= -O2 -pipe
CFLAGS += -std=c17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow
LDLIBS += $(shell $(PKG_CONFIG) --libs libdrm) -lm

TARGET := niri-edid-color
SOURCE := niri-edid-color.c

.PHONY: all check clean

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

check: $(TARGET)
	./$(TARGET) self-test
	./$(TARGET) profile >/dev/null
	./$(TARGET) --help >/dev/null
	sh -n session-autostart
	sh -n packaging/niri-edid-color-apply
	sh -n packaging/niri-edid-color-reset

clean:
	rm -f $(TARGET)
