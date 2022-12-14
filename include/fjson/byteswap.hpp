#pragma once

#ifdef _MSC_VER
# include <stdlib.h>
# define bswap_64(x) _byteswap_uint64(x)
#elif defined(__APPLE__)
# include <libkern/OSByteOrder.h>
# define bswap_64(x) OSSwapInt64(x)
#else
# include <compat/byteswap.h>
#endif
