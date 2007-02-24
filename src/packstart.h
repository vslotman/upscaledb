/**
 * Copyright (C) 2005-2007 Christoph Rupp (chris@crupp.de).
 * All rights reserved. See file LICENSE for licence and copyright
 * information.
 *
 * macros for packing structures; should work with most compilers.
 *
 */

/**
 *  example usage:
 *
 *  #include "packstart.h"
 *
 *  typedef HAM_PACK_0 HAM_PACK_1 struct example_struct
 *  {
 *    oef_u16_t var1;
 *    oef_u8_t var2;
 *    oef_u32_t var3;
 *  } HAM_PACK_2 example_struct_t;
 *
 *  #include "packstop.h"
 *
 */


#ifdef __GNUC__
#  if (((__GNUC__==2) && (__GNUC_MINOR__>=7)) || (__GNUC__>2))
#  define HAM_PACK_2 __attribute__ ((packed))
#  define _NEWGNUC_
#  endif
#endif

#ifdef __WATCOMC__
#  define HAM_PACK_0 _Packed
#endif

#if (defined(_MSC_VER) && (_MSC_VER >= 900)) || defined(__BORLANDC__)
#  define _NEWMSC_
#endif
#if !defined(_NEWGNUC_) && !defined(__WATCOMC__) && !defined(_NEWMSC_)
#  pragma pack(1)
#endif
#ifdef _NEWMSC_
#  pragma pack(push, 1)
#  define HAM_PACK_2 __declspec(align(1))
#endif

#ifndef HAM_PACK_0
#  define HAM_PACK_0
#endif

#ifndef HAM_PACK_1
#  define HAM_PACK_1
#endif

#ifndef HAM_PACK_2
#  define HAM_PACK_2
#endif
