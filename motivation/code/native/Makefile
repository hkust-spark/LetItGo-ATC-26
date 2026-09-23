CC ?= cc
PKG_CONFIG ?= pkg-config

CFLAGS ?= -O2 -g -Wall -Wextra -std=c11
LDFLAGS ?=

ifneq ($(strip $(FFMPEG_PREFIX)),)
FFMPEG_PKG_CONFIG = env PKG_CONFIG_PATH="$(FFMPEG_PREFIX)/lib/pkgconfig:$(FFMPEG_PREFIX)/lib64/pkgconfig:$(PKG_CONFIG_PATH)" $(PKG_CONFIG)
FFMPEG_RPATH = -Wl,-rpath,$(FFMPEG_PREFIX)/lib -Wl,-rpath,$(FFMPEG_PREFIX)/lib64
else
FFMPEG_PKG_CONFIG = $(PKG_CONFIG)
FFMPEG_RPATH =
endif

FFMPEG_CFLAGS := $(shell $(FFMPEG_PKG_CONFIG) --cflags libavformat libavcodec libavutil libswscale)
FFMPEG_LIBS := $(shell $(FFMPEG_PKG_CONFIG) --libs libavformat libavcodec libavutil libswscale)

.PHONY: all clean

all: frame_extract

frame_extract: frame_extract.c Makefile
	$(CC) $(CFLAGS) $(FFMPEG_CFLAGS) $(LDFLAGS) $(FFMPEG_RPATH) -o $@ $< $(FFMPEG_LIBS)

clean:
	rm -f frame_extract
