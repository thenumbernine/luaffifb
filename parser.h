#pragma once

typedef struct {
	int line;
	char const * next;
	char const * prev;
	unsigned align_mask;
} Parser;

Parser newParser(char const * str);
void parse_type(lua_State* L, Parser* P, CType* type);
void parse_argument(lua_State* L, Parser* P, int ct_usr, CType* type, struct token* name, Parser* asmname);
