LUAJIT_OS=$(shell luajit -e "print(require('ffi').os)")
LUAJIT_ARCH=$(shell luajit -e "print(require('ffi').arch)")
TARGET_DIR=libs/$(LUAJIT_OS)-$(LUAJIT_ARCH)

CFLAGS  =-I/usr/local/libuv-1.4.2/include -I/usr/include/luajit-2.0 -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64 -Wall -fPIC
#-Werror -DLUA_STACK_CHECK
LIBS    =-shared -lm

ifeq ($(LUAJIT_OS), Linux)
BASE_LIB=crazysnail
endif

all: build/$(BASE_LIB).so

build/%.so: src/sds.c src/hiredis-light.c src/cb.c src/%.c
	mkdir -p build
	$(CC) ${CFLAGS} -Isrc -o $@ $^ ${LIBS}
	rm -f $(TARGET_DIR)/$(BASE_LIB).so
	cp build/$(BASE_LIB).so $(TARGET_DIR)

clean:
	rm -fr build
