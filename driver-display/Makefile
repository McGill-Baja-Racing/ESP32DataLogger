CC ?= gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra
# The repository root, so "protocol/app_protocol.h" resolves as in the firmware.
CPPFLAGS += -I..

build/driver-display: src/main.c ../protocol/app_protocol.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/main.c -o $@

# Display test. deploy.sh copies the sources to the Pi and runs this target there.
LVGL_SOURCES := $(shell find third_party/lvgl/src -name '*.c' 2>/dev/null)
LVGL_OBJECTS := $(LVGL_SOURCES:%.c=build/%.o)
DISPLAY_CPPFLAGS := -DLV_CONF_INCLUDE_SIMPLE -Isrc -Ithird_party/lvgl \
	-Ithird_party/libdrm/include -Ithird_party/libdrm/include/libdrm

display-poc: build/display-poc

# The Pi has the libdrm library but no libdrm.so link, which comes with libdrm-dev.
build/display-poc: build/src/display_poc.o $(LVGL_OBJECTS)
	@$(CC) $^ -o $@ -l:libdrm.so.2 -lm

build/src/display_poc.o: src/display_poc.c src/lv_conf.h
	@mkdir -p $(dir $@)
	$(CC) $(DISPLAY_CPPFLAGS) -std=gnu11 -O2 -Wall -Wextra -c $< -o $@

build/third_party/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(DISPLAY_CPPFLAGS) -std=gnu11 -O2 -c $< -o $@

clean:
	rm -rf build

.PHONY: clean display-poc
