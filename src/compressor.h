#ifndef COMPRESSOR_H
#define COMPRESSOR_H
#include "fcz.h"
fcz_status fcz_compress(const char *in_path, const char *out_path,
                        unsigned nthreads, uint32_t chunk_size, fcz_stats *st);
#endif
