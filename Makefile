CFLAGS+=-fPIC -I /usr/local/include/luajit-2.0/
CFLAGS+=-Wall -Wno-parentheses -O2 -mtune=generic -fomit-frame-pointer
LDFLAGS+=-lluajit-5.1 -lm -ldl -lrt -lpthread
OBJS=main.o base64.o util.o io.o mock.o bytecode.o
CPARTS=main.c base64.c util.c io.c lpsend.h
LUAPARTS=util.lua options.lua sendmail.lua sendjob.lua status.lua main.lua

.PHONY: all clean loc

all: lpsend

lpsend.lua: ${LUAPARTS}
	lua combine.lua ${LUAPARTS} >lpsend.lua

bytecode.o: ${LUAPARTS}
	lua combine.lua ${LUAPARTS} | lua -b -n bytecode - $@

dfa.h: dfa.lua
	lua dfa.lua generate DFA_BYTE_CLASS DFA_TRANSITIONS '' >dfa.h

io.o: io.c dfa.h

lpsend: ${OBJS}
	gcc ${LDFLAGS} -s -Wl,-E -o $@ $^

lpchat: lpchat.c
	gcc -O2 -s -olpchat lpchat.c -lpthread

clean:
	rm -f lpchat lpsend lpsend.lua dfa.h ${OBJS} `find -name \*~`

loc:
	wc -l ${LUAPARTS}
	wc -l ${CPARTS}
