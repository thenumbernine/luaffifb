#pragma once

// this file defines DASM_M_ROW and DASM_M_FREE
// which are used by the dynasm/dasm_*.h files

/*
This was here but weren't these always defined beforehand to something different, so why were they here?

#ifndef Dst_DECL
#define Dst_DECL	DASMState **Dst
#endif

#ifndef Dst_REF
#define Dst_REF		(*Dst)
#endif
*/

#define DASM_IDENT	"DynASM 1.3.0"
#define DASM_VERSION	10300	/* 1.3.0 */

#ifndef DASM_M_GROW
#define DASM_M_GROW(ctx, t, p, sz, need) \
  do { \
    size_t _sz = (sz), _need = (need); \
    if (_sz < _need) { \
      if (_sz < 16) _sz = 16; \
      while (_sz < _need) _sz += _sz; \
      (p) = (t *)realloc((p), _sz); \
      if ((p) == NULL) exit(1); \
      (sz) = _sz; \
    } \
  } while(0)
#endif

#ifndef DASM_M_FREE
#define DASM_M_FREE(ctx, p, sz)	free(p)
#endif
