CC      ?= cc
CFLAGS  ?= -std=c99 -Os -Wall -Wextra -Isrc
CROSS   ?= arm-linux-gnueabi-
ARMCC   ?= $(CROSS)gcc

# Displayed splash name (build-time override): make NAME='my/name'.
# Empty NAME keeps the SPLASH_NAME default baked into render.h.
NAME    ?=
ifneq ($(NAME),)
CFLAGS  += -DSPLASH_NAME='"$(NAME)"'
endif

CORE    := src/fb.c src/render.c src/text.c src/version.c
SPLASH  := $(CORE) src/splash.c
DEPS    := src/fb.h src/render.h src/text.h src/metric9.h src/apple7.h src/version.h

.PHONY: all check test-fb test-text test-version test-render test-splash splash arm clean

all: splash

build:
	mkdir -p build

splash: build $(SPLASH) $(DEPS)
	$(CC) $(CFLAGS) -o build/splash $(SPLASH)

test-fb: build test/test_fb.c src/fb.c src/fb.h
	$(CC) $(CFLAGS) -o build/test_fb test/test_fb.c src/fb.c
	./build/test_fb

test-text: build test/test_text.c src/text.c src/fb.c src/text.h src/metric9.h src/apple7.h src/fb.h
	$(CC) $(CFLAGS) -o build/test_text test/test_text.c src/text.c src/fb.c
	./build/test_text

test-version: build test/test_version.c src/version.c src/version.h
	$(CC) $(CFLAGS) -o build/test_version test/test_version.c src/version.c
	./build/test_version

test-render: build test/test_render.c $(CORE) $(DEPS)
	$(CC) $(CFLAGS) -o build/test_render test/test_render.c $(CORE)
	./build/test_render

test-splash: splash
	sh test/test_splash.sh

check: test-fb test-text test-version test-render test-splash

# Static binary for the initramfs. Buildroot passes CROSS=$(TARGET_CROSS).
arm: $(SPLASH) $(DEPS)
	mkdir -p overlay/usr/bin
	$(ARMCC) $(CFLAGS) -static -o overlay/usr/bin/splash $(SPLASH)

clean:
	rm -rf build overlay/usr/bin/splash
