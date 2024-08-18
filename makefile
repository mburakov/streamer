bin:=$(notdir $(shell pwd))
src:=$(wildcard *.c)
obj:=$(src:.c=.o)

libs:=\
	egl \
	glesv2 \
	libdrm \
	libpipewire-0.3 \
	wayland-client

protocols_dir:=\
	wlr-protocols/unstable

protocols:=\
	wlr-export-dmabuf-unstable-v1

res:=\
	vertex.glsl \
	luma.glsl \
	chroma.glsl

obj:=$(patsubst %,%.o,$(protocols)) $(obj)
headers:=$(patsubst %,%.h,$(protocols))

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
	-rm $(bin) $(obj) $(headers)

.PHONY: all clean

.PRECIOUS: $(headers)
