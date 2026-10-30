# myTV — build for LG webOS (armv7, webOS 6+).
#
#   make deps       download the webOS toolchain and SDL2-webOS (Linux hosts)
#   make            build/launcher
#   make ipk        build/com.viiees.mytvlauncher_<version>_arm.ipk + manifest
#   make assets     regenerate assets/*.c from tools/ (needs Python with Pillow, fontTools, rsvg-convert)
#   make DEBUG=1    build-debug/: adds the /tmp/launcher.log log and the /tmp/launcher_cmd test hooks

TOOLCHAIN ?= $(CURDIR)/deps/arm-webos-linux-gnueabi_sdk-buildroot
SDL       ?= $(CURDIR)/deps/sdl
PYTHON    ?= python3

CROSS   := $(TOOLCHAIN)/bin/arm-webos-linux-gnueabi-
CC      := $(CROSS)gcc
AR      := $(CROSS)ar
SYSROOT := $(TOOLCHAIN)/arm-webos-linux-gnueabi/sysroot

APP_ID  := $(shell $(PYTHON) -c 'import json;print(json.load(open("package/appinfo.json"))["id"])')
VERSION := $(shell $(PYTHON) -c 'import json;print(json.load(open("package/appinfo.json"))["version"])')
REPO    ?= https://github.com/ViieeS/mytvlauncher

DEBUG ?= 0
CPPFLAGS := -I src -I third_party -I . -I $(SDL)/include/SDL2 -DLV_CONF_INCLUDE_SIMPLE
ifeq ($(DEBUG),1)
B := build-debug
CPPFLAGS += -DMYTV_DEBUG
else
B := build
endif
CFLAGS   := --sysroot=$(SYSROOT) -O2 -mtune=cortex-a55 -ffunction-sections -fdata-sections
WARN     := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
LDFLAGS  := --sysroot=$(SYSROOT) -L $(SDL)/lib -Wl,-rpath,'$$ORIGIN/lib' -Wl,--gc-sections -s
LDLIBS   := -lSDL2 -lhelpers -lm -lpthread -ldl -lrt

LVGL_SRC := $(shell find third_party/lvgl/src -name '*.c')
LVGL_OBJ := $(LVGL_SRC:%.c=$(B)/%.o)
APP_SRC  := src/main.c src/screensaver.c assets/fonts.c assets/icons.c assets/cities.c third_party/cJSON.c
APP_OBJ  := $(APP_SRC:%.c=$(B)/%.o) $(B)/src/glass.o
IPK      := $(B)/$(APP_ID)_$(VERSION)_arm.ipk

.PHONY: all ipk homehook deps assets clean
all: $(B)/launcher

$(B)/third_party/lvgl/%.o: third_party/lvgl/%.c src/lv_conf.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(B)/liblvgl.a: $(LVGL_OBJ)
	rm -f $@
	$(AR) rcs $@ $^

# glass.c is per-pixel float work off the UI thread: let the compiler vectorise it
$(B)/src/glass.o: src/glass.c src/glass.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -O3 -ffast-math $(WARN) $(CPPFLAGS) -c $< -o $@

$(B)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(WARN) $(CPPFLAGS) -c $< -o $@

$(B)/src/main.o $(B)/src/screensaver.o: src/*.h src/lv_conf.h assets/*.h

$(B)/launcher: $(APP_OBJ) $(B)/liblvgl.a
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

ipk: $(B)/launcher $(B)/homehook/libmytvhook.so
	$(PYTHON) tools/make_ipk.py --binary $(B)/launcher --sdl $(SDL)/lib/libSDL2-2.0.so.0 \
	  --package package --out $(B) --repo $(REPO) --homehook $(B)/homehook

# Home key hook (root only): ezinject and the injected library, see homehook/build.sh
homehook: $(B)/homehook/libmytvhook.so
$(B)/homehook/libmytvhook.so: homehook/mytvhook.c homehook/CMakeLists.txt homehook/build.sh $(wildcard homehook/*.patch)
	sh homehook/build.sh $(TOOLCHAIN) $(B)/homehook

deps:
	sh tools/fetch_deps.sh deps

assets:
	$(PYTHON) tools/make_fonts.py tools assets/fonts.c assets/fonts.h
	$(PYTHON) tools/make_icons.py assets/icons.c assets/icons.h
	$(PYTHON) tools/make_app_icon.py package

clean:
	rm -rf build build-debug
