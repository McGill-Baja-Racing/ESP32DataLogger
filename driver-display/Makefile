CC ?= gcc
OS := $(shell uname -s)
OBJ := build/obj-$(OS)

LVGL_SOURCES := $(shell find third_party/lvgl/src -name '*.c' 2>/dev/null)
LVGL_OBJECTS := $(LVGL_SOURCES:%.c=$(OBJ)/%.o)
UI_SOURCES := $(wildcard src/ui/*.c src/ui/fonts/*.c src/views/*.c)
UI_OBJECTS := $(UI_SOURCES:%.c=$(OBJ)/%.o)
HEADERS := src/lv_conf.h $(wildcard src/*.h src/ui/*.h src/ui/fonts/*.h src/views/*.h)

CPPFLAGS := -DLV_CONF_INCLUDE_SIMPLE -Isrc -Ithird_party/lvgl \
	-Ithird_party/libdrm/include -Ithird_party/libdrm/include/libdrm
# The Pi has the libdrm library but no libdrm.so link, which comes with libdrm-dev.
DRM_LIBS := -l:libdrm.so.2 -lm

SERVICE := /etc/systemd/system/driver-display.service

# On the Pi: the programs that draw on the screen. Elsewhere: the screenshots.
ifeq ($(OS),Linux)
all: build/driver-display build/gallery build/display-poc
else
all: snapshots
endif

build/driver-display: $(OBJ)/src/main.o $(OBJ)/src/drm_app.o $(UI_OBJECTS) $(LVGL_OBJECTS)
	@$(CC) $^ -o $@ $(DRM_LIBS)
	@echo "Built $@"

build/gallery: $(OBJ)/src/gallery.o $(OBJ)/src/drm_app.o $(UI_OBJECTS) $(LVGL_OBJECTS)
	@$(CC) $^ -o $@ $(DRM_LIBS)
	@echo "Built $@"

build/display-poc: $(OBJ)/src/display_poc.o $(LVGL_OBJECTS)
	@$(CC) $^ -o $@ $(DRM_LIBS)
	@echo "Built $@"

build/snapshot: $(OBJ)/src/snapshot.o $(UI_OBJECTS) $(LVGL_OBJECTS)
	@$(CC) $^ -o $@ -lz -lm

# Renders every demo screen to build/snapshots/, without the Pi.
snapshots: build/snapshot
	@mkdir -p build/snapshots
	@./build/snapshot build/snapshots

$(OBJ)/src/%.o: src/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -std=gnu11 -O2 -Wall -Wextra -c $< -o $@

$(OBJ)/third_party/%.o: third_party/%.c src/lv_conf.h
	@mkdir -p $(dir $@)
	@$(CC) $(CPPFLAGS) -std=gnu11 -O2 -w -c $< -o $@

# Start the driver display at every power-up, and restart it if it stops.
autostart-on: build/driver-display
	sed -e 's|@USER@|$(shell id -un)|' -e 's|@PROGRAM@|$(CURDIR)/build/driver-display|' \
		systemd/driver-display.service.in | sudo tee $(SERVICE) > /dev/null
	sudo systemctl daemon-reload
	sudo systemctl enable --now driver-display.service
	@echo "The driver display now starts at power-up."

autostart-off:
	-sudo systemctl disable --now driver-display.service
	sudo rm -f $(SERVICE)
	sudo systemctl daemon-reload
	@echo "The driver display no longer starts at power-up."

clean:
	rm -rf build

.PHONY: all snapshots autostart-on autostart-off clean
