#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "benchmark.h"
#include "compressor.h"
#include "decompressor.h"
#include "file_utils.h"

static void usage(FILE *o)
{
    fprintf(o,
        "Usage:\n"
        "  filecompress compress   <input> <output> [threads] [chunk_KiB]\n"
        "  filecompress decompress <input> <output> [threads]\n"
        "  filecompress benchmark  <input> [chunk_KiB]\n"
        "  filecompress help\n\n"
        "  threads    1-%u (default: number of CPUs)\n"
        "  chunk_KiB  %u-%u (default: %u)\n"
        "Exit codes: 0 ok, 1 usage, 2 I/O, 3 corrupt/invalid file, 4 memory/thread\n",
        FCZ_MAX_THREADS, FCZ_MIN_CHUNK / 1024, FCZ_MAX_CHUNK / 1024, FCZ_DEFAULT_CHUNK / 1024);
}

static int exit_code(fcz_status s)
{
    switch (s) {
    case FCZ_OK: return 0;
    case FCZ_ERR_ARG: return 1;
    case FCZ_ERR_IO: return 2;
    case FCZ_ERR_FORMAT: case FCZ_ERR_ZLIB: return 3;
    default: return 4;
    }
}

static int parse_ulong(const char *s, unsigned long lo, unsigned long hi, unsigned long *out)
{
    char *end; errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno || end == s || *end != '\0' || v < lo || v > hi) return -1;
    *out = v; return 0;
}

static unsigned default_threads(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > (long)FCZ_MAX_THREADS) n = FCZ_MAX_THREADS;
    return (unsigned)n;
}

static void print_stats(const char *label_in, const char *label_out, const fcz_stats *s, uint64_t throughput_bytes, int show_ratio)
{
    printf("%-16s: %llu bytes\n", label_in,  (unsigned long long)s->in_bytes);
    printf("%-16s: %llu bytes\n", label_out, (unsigned long long)s->out_bytes);
    if (show_ratio) {
        if (s->in_bytes) printf("%-16s: %.2f%% of original (%.2f%% saved)\n", "Ratio",
                                100.0 * s->out_bytes / s->in_bytes, 100.0 - 100.0 * s->out_bytes / s->in_bytes);
        else             printf("%-16s: n/a (empty input)\n", "Ratio");
    }
    printf("%-16s: %.3f s\n", "Time", s->seconds);
    printf("%-16s: %.1f MB/s\n", "Throughput", s->seconds > 1e-9 ? throughput_bytes / 1e6 / s->seconds : 0.0);
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(stderr); return 1; }
    const char *cmd = argv[1];

    if (!strcmp(cmd, "help") || !strcmp(cmd, "--help") || !strcmp(cmd, "-h")) { usage(stdout); return 0; }

    unsigned long threads = default_threads(), chunk_kib = FCZ_DEFAULT_CHUNK / 1024;
    fcz_stats st;
    fcz_status s;

    if (!strcmp(cmd, "compress")) {
        if (argc < 4 || argc > 6) { usage(stderr); return 1; }
        if (argc >= 5 && parse_ulong(argv[4], 1, FCZ_MAX_THREADS, &threads)) { fprintf(stderr, "error: threads must be 1-%u\n", FCZ_MAX_THREADS); return 1; }
        if (argc == 6 && parse_ulong(argv[5], FCZ_MIN_CHUNK / 1024, FCZ_MAX_CHUNK / 1024, &chunk_kib)) { fprintf(stderr, "error: chunk size must be %u-%u KiB\n", FCZ_MIN_CHUNK / 1024, FCZ_MAX_CHUNK / 1024); return 1; }
        s = fcz_compress(argv[2], argv[3], (unsigned)threads, (uint32_t)(chunk_kib * 1024), &st);
        if (s != FCZ_OK) { fprintf(stderr, "compress failed: %s\n", fcz_strerror(s)); return exit_code(s); }
        printf("Threads         : %lu, chunk %lu KiB\n", threads, chunk_kib);
        print_stats("Original size", "Compressed size", &st, st.in_bytes, 1);
        return 0;
    }
    if (!strcmp(cmd, "decompress")) {
        if (argc < 4 || argc > 5) { usage(stderr); return 1; }
        if (argc == 5 && parse_ulong(argv[4], 1, FCZ_MAX_THREADS, &threads)) { fprintf(stderr, "error: threads must be 1-%u\n", FCZ_MAX_THREADS); return 1; }
        s = fcz_decompress(argv[2], argv[3], (unsigned)threads, &st);
        if (s != FCZ_OK) { fprintf(stderr, "decompress failed: %s\n", fcz_strerror(s)); return exit_code(s); }
        printf("Threads         : %lu\n", threads);
        print_stats("Compressed size", "Restored size", &st, st.out_bytes, 0);
        return 0;
    }
    if (!strcmp(cmd, "benchmark")) {
        if (argc < 3 || argc > 4) { usage(stderr); return 1; }
        if (argc == 4 && parse_ulong(argv[3], FCZ_MIN_CHUNK / 1024, FCZ_MAX_CHUNK / 1024, &chunk_kib)) { fprintf(stderr, "error: bad chunk size\n"); return 1; }
        s = run_benchmark(argv[2], (uint32_t)(chunk_kib * 1024));
        if (s != FCZ_OK) { fprintf(stderr, "benchmark failed: %s\n", fcz_strerror(s)); return exit_code(s); }
        return 0;
    }
    fprintf(stderr, "error: unknown command '%s'\n\n", cmd);
    usage(stderr);
    return 1;
}
