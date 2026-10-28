/* dmc_unrar includes <intrin.h> under _MSC_VER for _byteswap_ulong and
   _byteswap_uint64. The Xbox 360 SDK has no intrin.h; its stdlib.h declares
   both. Only src/RarUnpack.cpp has this folder on its include path. */
#include <stdlib.h>
