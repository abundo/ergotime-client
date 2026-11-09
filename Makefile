# Dynamic build when CMake is not installed. Prefer CMakeLists.txt.
# `make static` links Qt archives from $(QT_STATIC_PREFIX).
QT_PREFIX ?= $(HOME)/Qt/6.12.0/gcc_64
QT_STATIC_PREFIX ?= $(HOME)/Qt/static/6.12.0
QT_WINDOWS_PREFIX ?= $(HOME)/Qt/static/6.12.0-mingw
QT_HOST_PATH ?= $(QT_PREFIX)

USER_CXX := $(HOME)/.local/opt/gcc-user/usr/bin/g++
ifneq ($(wildcard $(USER_CXX)),)
CXX := $(USER_CXX)
else
CXX := g++
endif
MINGW_CXX ?= x86_64-w64-mingw32-g++
MINGW_NM ?= nm

MOC := $(QT_PREFIX)/libexec/moc
STATIC_MOC := $(QT_STATIC_PREFIX)/libexec/moc
ifeq ($(wildcard $(STATIC_MOC)),)
STATIC_MOC := $(MOC)
endif
WIN_MOC := $(QT_HOST_PATH)/libexec/moc

PKG_CONFIG_PATH := $(QT_PREFIX)/lib/pkgconfig
export PKG_CONFIG_PATH

SRC := src/main.cpp src/settings.cpp src/store.cpp src/idle.cpp src/tracker.cpp src/sync.cpp src/dialogs.cpp src/mainwindow.cpp
MOC_SRC := build/moc_mainwindow.cpp build/moc_tracker.cpp build/moc_engine.cpp

ifneq ($(wildcard $(QT_PREFIX)/lib/pkgconfig/Qt6Widgets.pc),)
CXXFLAGS := -std=c++17 -Wall -Wextra -Wno-sfinae-incomplete -fPIC -O2 $(shell pkg-config --cflags Qt6Widgets Qt6Sql Qt6Network Qt6DBus)
LIBS := $(shell pkg-config --libs Qt6Widgets Qt6Sql Qt6Network Qt6DBus)
CXXFLAGS += -DERGOTIME_HAS_DBUS=1
LDFLAGS := -Wl,-rpath,$(QT_PREFIX)/lib
endif

.PHONY: all clean static static-windows install-qt-static static-container static-windows-container

all: build/ergotime

build/ergotime: $(SRC) $(MOC_SRC)
	$(CXX) $(CXXFLAGS) -Isrc -Ibuild -o $@ $(SRC) $(MOC_SRC) $(LIBS) $(LDFLAGS)

build/moc_mainwindow.cpp: src/mainwindow.hpp | build
	$(MOC) $(shell pkg-config --cflags Qt6Widgets) src/mainwindow.hpp -o $@

build/moc_tracker.cpp: src/tracker.hpp | build
	$(MOC) $(shell pkg-config --cflags Qt6Core) src/tracker.hpp -o $@

build/moc_engine.cpp: src/sync.hpp | build
	$(MOC) $(shell pkg-config --cflags Qt6Core) src/sync.hpp -o $@

build/static_plugins.cpp: $(QT_STATIC_PREFIX)/lib/libQt6Core.a scripts/qt-static.py | build
	python3 scripts/qt-static.py plugins $(QT_STATIC_PREFIX) $@

build/ergotime-static: $(SRC) build/static_plugins.cpp | build
	$(STATIC_MOC) $(shell python3 scripts/qt-static.py mocflags $(QT_STATIC_PREFIX)) src/mainwindow.hpp -o build/moc_mainwindow.cpp
	$(STATIC_MOC) $(shell python3 scripts/qt-static.py mocflags $(QT_STATIC_PREFIX)) src/tracker.hpp -o build/moc_tracker.cpp
	$(STATIC_MOC) $(shell python3 scripts/qt-static.py mocflags $(QT_STATIC_PREFIX)) src/sync.hpp -o build/moc_engine.cpp
	$(CXX) $(shell python3 scripts/qt-static.py cflags $(QT_STATIC_PREFIX)) -Isrc -Ibuild -o $@ \
		$(SRC) $(MOC_SRC) build/static_plugins.cpp \
		$(shell python3 scripts/qt-static.py ldflags $(QT_STATIC_PREFIX)) \
		-static-libgcc -static-libstdc++
	@if ldd $@ | grep -E 'libQt6|libicu'; then echo 'error: binary still depends on Qt or ICU shared libraries'; exit 1; fi
	./scripts/pack-upx.sh $@

build/windows_plugins.cpp: $(QT_WINDOWS_PREFIX)/lib/libQt6Core.a scripts/qt-static.py | build
	NM=$(MINGW_NM) python3 scripts/qt-static.py plugins $(QT_WINDOWS_PREFIX) $@

build/ergotime.exe: $(SRC) build/windows_plugins.cpp | build
	$(WIN_MOC) $(shell python3 scripts/qt-static.py mocflags $(QT_WINDOWS_PREFIX)) src/mainwindow.hpp -o build/moc_mainwindow.cpp
	$(WIN_MOC) $(shell python3 scripts/qt-static.py mocflags $(QT_WINDOWS_PREFIX)) src/tracker.hpp -o build/moc_tracker.cpp
	$(WIN_MOC) $(shell python3 scripts/qt-static.py mocflags $(QT_WINDOWS_PREFIX)) src/sync.hpp -o build/moc_engine.cpp
	$(MINGW_CXX) $(shell python3 scripts/qt-static.py cflags $(QT_WINDOWS_PREFIX)) -Isrc -Ibuild -o $@ \
		$(SRC) $(MOC_SRC) build/windows_plugins.cpp \
		$(shell NM=$(MINGW_NM) python3 scripts/qt-static.py ldflags $(QT_WINDOWS_PREFIX)) \
		-mwindows -static-libgcc -static-libstdc++
	MINGW_STRIP=$(MINGW_CXX:%g++=%strip) ./scripts/pack-upx.sh $@

static: build/ergotime-static

static-windows: build/ergotime.exe

install-qt-static:
	./scripts/install-qt-static.sh

static-container:
	./scripts/container-build-static.sh

static-windows-container:
	./scripts/container-build-windows.sh

build:
	mkdir -p build

clean:
	rm -rf build
