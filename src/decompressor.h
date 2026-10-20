#ifndef DECOMPRESSOR_H
#define DECOMPRESSOR_H
#include "fcz.h"
fcz_status fcz_decompress(const char *in_path, const char *out_path,
                          unsigned nthreads, fcz_stats *st);
#endif
