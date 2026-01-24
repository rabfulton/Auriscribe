CC = gcc
PKG_CONFIG = pkg-config

CFLAGS = -Wall -Wextra -O2 -g \
         $(shell $(PKG_CONFIG) --cflags gtk+-3.0 ayatana-appindicator3-0.1 libpulse-simple json-c x11)

LDFLAGS = $(shell $(PKG_CONFIG) --libs gtk+-3.0 ayatana-appindicator3-0.1 libpulse-simple json-c x11) \
          -lcurl -lm -lpthread

# whisper.cpp (after building)
WHISPER_DIR = libs/whisper.cpp
WHISPER_LIB = $(WHISPER_DIR)/libwhisper.a
CFLAGS += -I$(WHISPER_DIR)/include -I$(WHISPER_DIR)/ggml/include
LDFLAGS += -lstdc++ -lgomp

# Optional: Vulkan runtime linkage (needed when whisper.cpp is built with GGML_VULKAN=1)
ifneq ($(shell $(PKG_CONFIG) --exists vulkan && echo yes),)
    LDFLAGS += $(shell $(PKG_CONFIG) --libs vulkan)
endif

# ONNX Runtime (optional, for Parakeet)
ONNX_DIR = libs/onnxruntime
ifneq ($(wildcard $(ONNX_DIR)/include),)
    CFLAGS += -I$(ONNX_DIR)/include -DHAVE_ONNX
    LDFLAGS += -L$(ONNX_DIR)/lib -lonnxruntime -Wl,-rpath,$(ONNX_DIR)/lib
endif

SRCDIR = src
OBJDIR = obj
SRCS = $(wildcard $(SRCDIR)/*.c)
OBJS = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRCS))

TARGET = auriscribe

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
SYSCONFDIR ?= /etc

.PHONY: all clean install uninstall deps

all: $(TARGET)

$(TARGET): $(OBJS) $(WHISPER_LIB)
	$(CC) -o $@ $(OBJS) $(WHISPER_LIB) $(LDFLAGS)

$(WHISPER_LIB):
ifeq ($(shell $(PKG_CONFIG) --exists vulkan && command -v glslc >/dev/null 2>&1 && echo yes),yes)
	$(MAKE) -C $(WHISPER_DIR) GGML_VULKAN=1 libwhisper.a
else
ifneq ($(XFCE_WHISPER_REQUIRE_VULKAN),0)
	@echo "Error: Vulkan build required but not detected (need pkg-config vulkan + glslc)." >&2
	@echo "Install Vulkan dev packages + glslc, or run: make XFCE_WHISPER_REQUIRE_VULKAN=0" >&2
	@exit 1
endif
	$(MAKE) -C $(WHISPER_DIR) libwhisper.a
endif

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR):
	mkdir -p $(OBJDIR)

clean:
	rm -rf $(OBJDIR) $(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -Dm644 resources/auriscribe.desktop $(DESTDIR)$(DATADIR)/applications/auriscribe.desktop
	install -Dm644 resources/icons/auriscribe.svg $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/auriscribe.svg

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -f $(DESTDIR)$(DATADIR)/applications/auriscribe.desktop
	rm -f $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/auriscribe.svg

deps:
	@echo "Installing build dependencies..."
	sudo apt install -y libgtk-3-dev libayatana-appindicator3-dev \
	                    libpulse-dev libjson-c-dev libcurl4-openssl-dev \
	                    build-essential cmake
