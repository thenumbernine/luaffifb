#
# Use luarocks to install LuaFFI:
# > git clone https://github.com/facebook/luaffifb
# > cd luaffifb && luarocks make
#
# To rebuild the call_* headers:
# > rm call_*.h && make headers
#

.PHONY: build clean headers
LUA=lua

CFLAGS=-I/usr/local/include/lua-5.4.7
LDFLAGS=-L/usr/local/lib/lua-5.4.7 -llua.5.4.7
build: ffi.so
	#luarocks make
	# luarocks disagreed so...

ffi.so: call.o ctype.o ffi.o parser.o
	$(CC) $(LDFLAGS) -shared -o ffi.so
	install_name_tool -change liblua.5.4.7.so /usr/local/lib/lua-5.4.7/liblua.5.4.7.so ffi.so

call.o: call.c
	$(CC) $(CFLAGS) -c -o call.o call.c

ctype.o: ctype.c
	$(CC) $(CFLAGS) -c -o ctype.o ctype.c

ffi.o: ffi.c
	$(CC) $(CFLAGS) -c -o ffi.o ffi.c

parser.o: parser.c
	$(CC) $(CFLAGS) -c -o parser.o parser.c

clean:
	rm -f *.o *.so *.dylib

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
