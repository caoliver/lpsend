#include "lpsend.h"

// Base64 encoder from Luiz Henrique de Figueiredo
#define uint unsigned int

static const char code[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void encode(luaL_Buffer * b, uint c1, uint c2, uint c3, int n)
{
  unsigned long tuple = c3 + 256UL * (c2 + 256UL * c1);
  int i;
  char s[4];
  for (i = 0; i < 4; i++)
    {
      s[3 - i] = code[tuple % 64];
      tuple /= 64;
    }
  for (i = n + 1; i < 4; i++)
    s[i] = '=';
  luaL_addlstring(b, s, 4);
}

static int base64_encode(lua_State *L)
{
  size_t l;
  const unsigned char *s =
    (const unsigned char *) luaL_checklstring(L, 1, &l);
  int break_after = luaL_optinteger(L, 2, 0), quad_count = 0;
  luaL_Buffer b;
  int n;
  luaL_buffinit(L, &b);
  for (n = l / 3; n--; s += 3)
    {
      encode(&b, s[0], s[1], s[2], 3);
      if (break_after > 0 && ++quad_count == break_after)
	{
	  quad_count = 0;
	  luaL_addlstring(&b, "\n", 1);
	}
    }
  switch (l % 3)
    {
    case 1:
      encode(&b, s[0], 0, 0, 1);
      if (break_after > 0)
	luaL_addlstring(&b, "\n", 1);
      break;
    case 2:
      encode(&b, s[0], s[1], 0, 2);
      if (break_after > 0)
	luaL_addlstring(&b, "\n", 1);
      break;
    }
  luaL_pushresult(&b);
  return 1;
}

const luaL_Reg base64funcs[] = {
  {"base64_encode", base64_encode},
  {NULL, NULL}
};
