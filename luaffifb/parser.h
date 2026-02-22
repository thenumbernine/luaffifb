#pragma once

typedef struct {
	int line;
	char const * next;
	char const * prev;
	unsigned align_mask;
	int numTypeParams;
	int typeParamStartLoc;		// lua stack loc
	int typeParamIndex;	// 0-based
} Parser;

//forward-declared here for parse_argument's prototype even though its arg is only ever passed NULL so the outside never sees it.
struct Token;

Parser newParser(lua_State *L, char const * str, int argLoc);

void parse_type(
	lua_State * L,
	Parser * P,
	CType * type);

void parse_argument(
	lua_State * L,
	Parser * P,
	int ct_usr,
	CType * type,
	struct Token * name,
	Parser * asmname);

int ffi_cdef(lua_State* L);
void push_type_name(lua_State* L, int usr, const CType* ct);	// LOL WTF kind of API is this that some kind of retarded anonymous userdata's uservalue has to be passed around with every single C pointer?
