bin:=$(notdir $(shell pwd))
src:=$(wildcard *.c)
obj:=$(src:.c=.o)

obj+=\
	toolbox/buffer.o \
	toolbox/io_muxer.o \
	toolbox/perf.o

libs:=\
	egl \
	gbm \
	glesv2 \
	libdrm \
	libpipewire-0.3 \
	libva \
	libva-drm

protocols_dir:=\
	wlr-protocols/unstable

protocols:=\
	wlr-export-dmabuf-unstable-v1

res:=\
	vertex.glsl \
	luma.glsl \
	chroma.glsl

ifdef USE_WAYLAND
	obj:=$(patsubst %,%.o,$(protocols)) $(obj)
	headers:=$(patsubst %,%.h,$(protocols))
	libs+=wayland-client
	CFLAGS+=-DUSE_WAYLAND
endif

#CFLAGS+=-DUSE_EGL_MESA_PLATFORM_SURFACELESS
CFLAGS+=$(shell pkg-config --cflags $(libs))
LDFLAGS+=$(shell pkg-config --libs $(libs))

comma:=,
LDFLAGS+= \
	-Wl,--format=binary \
	$(patsubst %,-Wl$(comma)%,$(res)) \
	-Wl,--format=default

all: $(bin)

$(bin): $(obj)
	$(CC) $^ $(LDFLAGS) -o $@

%.o: %.c *.h $(res) $(headers)
	$(CC) -c $< $(CFLAGS) -o $@

%.h: $(protocols_dir)/%.xml
	wayland-scanner client-header $< $@

%.c: $(protocols_dir)/%.xml
	wayland-scanner private-code $< $@

clean:
	-rm $(bin) $(obj) $(headers) \
		$(foreach proto,$(protocols),$(proto).h $(proto).o)

.PHONY: all clean

.PRECIOUS: $(headers)
