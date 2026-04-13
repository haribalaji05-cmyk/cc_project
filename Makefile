CC      = gcc
CFLAGS  = -Wall -Wextra -g $(shell pkg-config --cflags fuse3)
LDFLAGS = $(shell pkg-config --libs fuse3)
TARGET  = mini_unionfs
SRC     = mini_unionfs.c

.PHONY: all clean install-deps

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

install-deps:
	sudo apt-get update
	sudo apt-get install -y fuse3 libfuse3-dev pkg-config

clean:
	rm -f $(TARGET)

unmount:
	fusermount3 -u ./mnt 2>/dev/null || umount ./mnt 2>/dev/null || true
