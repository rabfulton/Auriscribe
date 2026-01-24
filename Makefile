CC = gcc
PKG_CONFIG = pkg-config

CFLAGS = -Wall -Wextra -O2 -g \
         $(shell $(PKG_CONFIG) --cflags gtk+-3.0 ayatana-appindicator3-0.1 libpulse-simple json-c x11)

LDFLAGS = $(shell $(PKG_CONFIG) --libs gtk+-3.0 ayatana-appindicator3-0.1 libpulse-simple json-c x11) \
          -lcurl -lm -lpthread

# whisper.cpp (for auriscribe-worker)
WHISPER_DIR = libs/whisper.cpp
WHISPER_LIB = $(WHISPER_DIR)/libwhisper.a
# App only needs ggml header for model magic checking.
CFLAGS += -I$(WHISPER_DIR)/ggml/include

WORKER_CFLAGS = -Wall -Wextra -O2 -g -I$(WHISPER_DIR)/include -I$(WHISPER_DIR)/ggml/include
WORKER_LDFLAGS = -lm -lpthread -lstdc++ -lgomp -ldl
ifneq ($(shell $(PKG_CONFIG) --exists vulkan && echo yes),)
    WORKER_LDFLAGS += $(shell $(PKG_CONFIG) --libs vulkan)
endif

# ONNX Runtime (optional, for Parakeet)
ONNX_DIR = libs/onnxruntime
ifneq ($(wildcard $(ONNX_DIR)/include),)
    CFLAGS += -I$(ONNX_DIR)/include -DHAVE_ONNX
    LDFLAGS += -L$(ONNX_DIR)/lib -lonnxruntime -Wl,-rpath,$(ONNX_DIR)/lib
endif

SRCDIR = src
OBJDIR = obj
APP_SRCS = $(filter-out $(SRCDIR)/worker.c,$(wildcard $(SRCDIR)/*.c))
APP_OBJS = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(APP_SRCS))
WORKER_OBJ = $(OBJDIR)/worker.o

TARGET = auriscribe
WORKER = auriscribe-worker

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
SYSCONFDIR ?= /etc

.PHONY: all clean install uninstall deps

all: $(TARGET) $(WORKER)

REQUIRE_VULKAN := $(if $(AURISCRIBE_REQUIRE_VULKAN),$(AURISCRIBE_REQUIRE_VULKAN),$(if $(XFCE_WHISPER_REQUIRE_VULKAN),$(XFCE_WHISPER_REQUIRE_VULKAN),1))

$(TARGET): $(APP_OBJS)
	$(CC) -o $@ $(APP_OBJS) $(LDFLAGS)

$(WORKER): $(WORKER_OBJ) $(WHISPER_LIB)
	$(CC) -o $@ $(WORKER_OBJ) $(WHISPER_LIB) $(WORKER_LDFLAGS)

$(WHISPER_LIB):
ifeq ($(shell $(PKG_CONFIG) --exists vulkan && command -v glslc >/dev/null 2>&1 && echo yes),yes)
	$(MAKE) -C $(WHISPER_DIR) GGML_VULKAN=1 libwhisper.a
else
ifneq ($(REQUIRE_VULKAN),0)
	@echo "Error: Vulkan build required but not detected (need pkg-config vulkan + glslc)." >&2
	@echo "Install Vulkan dev packages + glslc, or run: make AURISCRIBE_REQUIRE_VULKAN=0" >&2
	@exit 1
endif
	$(MAKE) -C $(WHISPER_DIR) libwhisper.a
endif

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(WORKER_OBJ): $(SRCDIR)/worker.c | $(OBJDIR)
	$(CC) $(WORKER_CFLAGS) -c -o $@ $<

$(OBJDIR):
	mkdir -p $(OBJDIR)

clean:
	rm -rf $(OBJDIR) $(TARGET) $(WORKER)

install: $(TARGET) $(WORKER)
	install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -Dm755 $(WORKER) $(DESTDIR)$(BINDIR)/$(WORKER)
	install -Dm644 resources/auriscribe.desktop $(DESTDIR)$(DATADIR)/applications/auriscribe.desktop
	install -Dm644 resources/icons/auriscribe.svg $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/auriscribe.svg

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -f $(DESTDIR)$(BINDIR)/$(WORKER)
	rm -f $(DESTDIR)$(DATADIR)/applications/auriscribe.desktop
	rm -f $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/auriscribe.svg

deps:
	@echo "Installing build dependencies..."
	sudo apt install -y libgtk-3-dev libayatana-appindicator3-dev \
	                    libpulse-dev libjson-c-dev libcurl4-openssl-dev \
	                    build-essential cmake
