CC = x86_64-w64-mingw32-gcc
CFLAGS ?= -O2 -Wall -Wextra
BUILD_DIR := build

.PHONY: all clean check install uninstall

all: $(BUILD_DIR)/AppUINativePlugin.dll \
	$(BUILD_DIR)/DnsZeroConfLib.dll \
	$(BUILD_DIR)/WclBlePluginCPP.dll

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/AppUINativePlugin.dll: src/AppUINativePlugin-shim.c src/AppUINativePlugin-shim.def | $(BUILD_DIR)
	$(CC) -std=c11 -Os -Wall -Wextra -Werror -shared -static-libgcc \
		-Wl,--kill-at -o $@ $^ -luser32

$(BUILD_DIR)/DnsZeroConfLib.dll: src/DnsZeroConfLib-shim.c src/DnsZeroConfLib-shim.def | $(BUILD_DIR)
	$(CC) -shared -O2 -s -o $@ $^

$(BUILD_DIR)/WclBlePluginCPP.dll: src/WclBlePluginCPP-bridge.c src/WclBlePluginCPP-bridge.def | $(BUILD_DIR)
	$(CC) $(CFLAGS) -shared -o $@ $^ -lws2_32 -lole32

check: all
	python3 -m py_compile src/rouvy-ble-host.py
	@for dll in $(BUILD_DIR)/*.dll; do file "$$dll"; done

install: all
	./scripts/install.sh

uninstall:
	./scripts/uninstall.sh

clean:
	rm -rf $(BUILD_DIR) src/__pycache__
