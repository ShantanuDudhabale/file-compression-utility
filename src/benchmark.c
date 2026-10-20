
#define _POSIX_C_SOURCE 200809L

#include "benchmark.h"
#include "compressor.h"
#include "file_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

fcz_status run_benchmark(const char *in_path, uint32_t chunk_size)
{
    unsigned max_threads = FCZ_MAX_THREADS;

    printf("Benchmark: %s\n", in_path);
    printf("Chunk size: %u bytes\n\n", chunk_size);
    printf("%-10s %-15s %-15s %-15s\n",
           "Threads", "Input bytes", "Output bytes", "Time (s)");

    for (unsigned threads = 1; threads <= max_threads; threads++) {
        char out_path[128];

        snprintf(out_path, sizeof(out_path),
                 "/tmp/fcz_benchmark_%u.fcz", threads);

        fcz_stats stats;
        fcz_status status = fcz_compress(
            in_path, out_path, threads, chunk_size, &stats
        );

        if (status != FCZ_OK) {
            fprintf(stderr, "Benchmark failed at %u threads: %s\n",
                    threads, fcz_strerror(status));
            remove(out_path);
            return status;
        }

        printf("%-10u %-15llu %-15llu %-15.3f\n",
               threads,
               (unsigned long long)stats.in_bytes,
               (unsigned long long)stats.out_bytes,
               stats.seconds);

        remove(out_path);
    }

    return FCZ_OK;
}

