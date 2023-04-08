bin:=$(notdir $(shell pwd))
src:=$(shell ls *.c)
obj:=$(src:.c=.o)

obj+=\
	toolbox/perf.o

libs:=\
	egl \
	gbm \
	glesv2 \
	libavcodec \
	libavutil \
	libdrm \
	libva

res:=\
	vertex.glsl \
	luma.glsl \
	chroma.glsl

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

%.o: %.c *.h $(res)
	$(CC) -c $< $(CFLAGS) -o $@

clean:
	-rm $(bin) $(obj)

.PHONY: all clean
