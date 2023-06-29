CC=gcc
CFLAGS=-lconfuse -lX11 -lXext -lavcodec -lavutil -lswscale -lswresample -lavformat -lpulse -lpulse-simple -lm
OPT_LEVEL=-O3

INSTALL_DIR=/usr/local/bin

.PHONY: build buildir clean all install debug


all: build


builddir:
	mkdir -p build

clean:
	rm -rf build/*

main.o: builddir
	$(CC) $(CFLAGS) $(OPT_LEVEL) -c src/main.c -o build/main.o

audio.o: builddir
	$(CC) $(CFLAGS) $(OPT_LEVEL) -c src/audio.c -o build/audio.o

video.o: builddir
	$(CC) $(CFLAGS) $(OPT_LEVEL) -c src/video.c -o build/video.o

spotlight.o: builddir
	$(CC) $(CFLAGS) $(OPT_LEVEL) -c src/spotlight.c -o build/spotlight.o

build: main.o spotlight.o audio.o video.o
	$(CC) $(CFLAGS) $(OPT_LEVEL) build/main.o build/spotlight.o build/video.o build/audio.o -o build/spotlight

install: build
	echo "Installing into ${INSTALL_DIR}"
	install -m 755 build/spotlight ${INSTALL_DIR}

debug: OPT_LEVEL=-O0 -g -fsanitize=address
debug: build
