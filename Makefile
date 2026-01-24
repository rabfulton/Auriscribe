CC = gcc
PKG_CONFIG = pkg-config

CFLAGS = -Wall -Wextra -O2 -g \
         $(shell $(PKG_CONFIG) --cflags gtk+-3.0 ayatana-appindicator3-0.1 libpulse-simple json-c x11)

LDFLAGS = $(shell $(PKG_CONFIG) --libs gtk+-3.0 ayatana-appindicator3-0.1 libpulse-simple json-c x11) \
          -lcurl -lm -lpthread

# whisper.cpp (after building)
WHISPER_DIR = libs/whisper.cpp
CFLAGS += -I$(WHISPER_DIR)/include -I$(WHISPER_DIR)/ggml/include
LDFLAGS += -L$(WHISPER_DIR) -lwhisper -lstdc++ -lgomp

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

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

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
