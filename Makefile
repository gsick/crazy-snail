LUVIT = /usr/local/bin/luvit
CFLAGS  = $(shell $(LUVIT) --cflags | sed s/-Werror//)
LIBS    = $(shell $(LUVIT) --libs)
#-DLUA_STACK_CHECK

all: bin/crazysnail.luvit

bin/%.luvit: src/sds.c src/hiredis-light.c src/cb.c src/%.c
	mkdir -p bin
	$(CC) ${CFLAGS} -Isrc -o $@ $^ ${LIBS}

clean:
	rm -fr bin
