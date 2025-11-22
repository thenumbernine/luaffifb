#
# Use luarocks to install LuaFFI:
# > git clone https://github.com/facebook/luaffifb
# > cd luaffifb && luarocks make
#
# To rebuild the call_* headers:
# > rm call_*.h && make headers
#

CC=clang

.PHONY: all
LUA=lua

CFLAGS+= -I/usr/local/include/lua-5.4.8
LDFLAGS+= -L/usr/local/lib/lua-5.4.8
LDFLAGS+= -llua-5.4.8
CFLAGS+= -fPIC

# use LibFFI calls
CFLAGS+= -DCALL_WITH_LIBFFI

HOST_SYS:= $(shell uname -s)

# if it's Linux ...
ifeq (Linux,$(HOST_SYS))
	LDFLAGS+= -shared
# if OSX ...
else
	CFLAGS+= -Wall
	LDFLAGS+= -dynamiclib -undefined dynamic_lookup -flat_namespace
	LIBFFI_DIR=/usr/local/Cellar/libffi/3.4.8
	CFLAGS+= -I$(LIBFFI_DIR)/include/
	LDFLAGS+= -L$(LIBFFI_DIR)/lib/ -lffi
endif

# our cwd:
CFLAGS+= -I`pwd`

DEBUG=1
ifeq ($(DEBUG),1)
	# debug
	CFLAGS+= -DDEBUG -O0 -gdwarf-2 -mfix-and-continue
	TEST_CMD=        echo 'bt' > lldb.batch && lldb --batch -K lldb.batch -o run -f lua -- test.lua
	SIMPLE_TEST_CMD= echo 'bt' > lldb.batch && lldb --batch -K lldb.batch -o run -f lua -- simple_test.lua
else
	# release
	CFLAGS+= -DNDEBUG -O2
	TEST_CMD= lua test.lua
	SIMPLE_TEST_CMD= lua simple_test.lua
endif

all: ffi.so libtest.so libsimple_test.so

SRCS= call.c ctype.c ffi.c parser.c ffi_complex.c lua.c test.c simple_test.c
OBJS= $(patsubst %.c, %.o, $(SRCS))

ffi.so: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^
# OSX only:
#	install_name_tool -change liblua.5.4.8.so /usr/local/lib/lua-5.4.8/liblua.5.4.8.so $@

libtest.so: test.o
	$(CC) $(LDFLAGS) -o $@ $^
# OSX only:
#	install_name_tool -change liblua.5.4.8.so /usr/local/lib/lua-5.4.8/liblua.5.4.8.so $@

libsimple_test.so: simple_test.o
	$(CC) $(LDFLAGS) -o $@ $^
# OSX only:
#	install_name_tool -change liblua.5.4.8.so /usr/local/lib/lua-5.4.8/liblua.5.4.8.so $@

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $^

.PHONY: clean
clean:
	rm -f *.o *.so *.dylib

.PHONY: test
test: ffi.so libtest.so
	$(TEST_CMD)

.PHONY: simple_test
simple_test: ffi.so libsimple_test.so
	$(SIMPLE_TEST_CMD)


.PHONY: headers
headers:
	$(MAKE) call_x86.h call_x64.h call_x64win.h call_arm.h

call_x86.h: call_x86.dasc dynasm/*.lua
	$(LUA) dynasm/dynasm.lua -LN -o $@ $<

call_x64.h: call_x86.dasc dynasm/*.lua
	$(LUA) dynasm/dynasm.lua -D X64 -LN -o $@ $<

call_x64win.h: call_x86.dasc dynasm/*.lua
	$(LUA) dynasm/dynasm.lua -D X64 -D X64WIN -LN -o $@ $<

call_arm.h: call_arm.dasc dynasm/*.lua
	$(LUA) dynasm/dynasm.lua -LNE -o $@ $<
