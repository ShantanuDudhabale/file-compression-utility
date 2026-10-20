#define _POSIX_C_SOURCE 200809L
#include "decompressor.h"
#include "file_utils.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

typedef struct {
    const uint8_t *in;  uint32_t in_len;
    uint8_t       *out; uint32_t out_len;   /* EXPECTED size, already validated */
    uint32_t       crc;
    fcz_status     status;
} djob;

static void *decompress_worker(void *arg)
{
    djob *j = arg;
    uLongf dest = j->out_len;                /* capacity == expected size: zlib can never write past it */
    int rc = uncompress(j->out, &dest, j->in, j->in_len);
    if (rc == Z_MEM_ERROR)                    j->status = FCZ_ERR_MEM;
    else if (rc != Z_OK || dest != j->out_len) j->status = FCZ_ERR_FORMAT;   /* corrupt, or wrong size */
    else if ((uint32_t)crc32(crc32(0L, Z_NULL, 0), j->out, j->out_len) != j->crc) j->status = FCZ_ERR_FORMAT;
    else                                      j->status = FCZ_OK;
    return NULL;
}

static fcz_status fail(fcz_status s, const char *msg) { fprintf(stderr, "error: %s\n", msg); return s; }

static fcz_status read_exact(FILE *f, void *buf, size_t n)
{
    if (fread(buf, 1, n, f) == n) return FCZ_OK;
    if (ferror(f)) { perror("read"); return FCZ_ERR_IO; }
    return fail(FCZ_ERR_FORMAT, "file is truncated");
}

fcz_status fcz_decompress(const char *in_path, const char *out_path,
                          unsigned nthreads, fcz_stats *st)
{
    fcz_status result = FCZ_OK;
    FILE *fin = NULL, *fout = NULL;
    uint8_t *cin = NULL, *obuf = NULL;
    djob *jobs = NULL;
    pthread_t *tids = NULL;
    uint64_t in_size = 0, total = 0, nchunks = 0, next = 0, written = 0;
    uint32_t chunk_size = 0;
    size_t nslots = 0, cap = 0;
    uint8_t hdr[FCZ_HEADER_SIZE];
    double t0 = now_seconds();

    if (nthreads < 1 || nthreads > FCZ_MAX_THREADS) return FCZ_ERR_ARG;
    if (same_file(in_path, out_path)) return fail(FCZ_ERR_ARG, "input and output are the same file");

    fin = fopen(in_path, "rb");
    if (!fin) { perror(in_path); return FCZ_ERR_IO; }
    result = file_size_of(fin, &in_size);
    if (result != FCZ_OK) { fail(result, "input is not a regular file"); goto cleanup; }

    /* ---- validate header BEFORE creating the output file ---- */
    if ((result = read_exact(fin, hdr, sizeof hdr)) != FCZ_OK) goto cleanup;
    if (memcmp(hdr, FCZ_MAGIC, 4) != 0)  { result = fail(FCZ_ERR_FORMAT, "not an FCZ file (bad magic)"); goto cleanup; }
    if (get_u16(hdr + 4) != FCZ_VERSION) { result = fail(FCZ_ERR_FORMAT, "unsupported format version"); goto cleanup; }
    if (get_u16(hdr + 6) != 0)           { result = fail(FCZ_ERR_FORMAT, "unsupported flags"); goto cleanup; }
    chunk_size = get_u32(hdr + 8);
    total      = get_u64(hdr + 12);
    nchunks    = get_u32(hdr + 20);
    if (chunk_size < FCZ_MIN_CHUNK || chunk_size > FCZ_MAX_CHUNK) { result = fail(FCZ_ERR_FORMAT, "invalid chunk size in header"); goto cleanup; }
    if (nchunks != total / chunk_size + (total % chunk_size != 0)) { result = fail(FCZ_ERR_FORMAT, "chunk count does not match original size"); goto cleanup; }

    fout = fopen(out_path, "wb");
    if (!fout) { perror(out_path); result = FCZ_ERR_IO; goto cleanup; }

    nslots = nchunks < nthreads ? (size_t)nchunks : nthreads;
    cap = compressBound(chunk_size);
    if (nslots > 0) {
        cin   = malloc(nslots * cap);
        obuf  = malloc(nslots * chunk_size);
        jobs  = calloc(nslots, sizeof *jobs);
        tids  = malloc(nslots * sizeof *tids);
        if (!cin || !obuf || !jobs || !tids) { result = fail(FCZ_ERR_MEM, "out of memory"); goto cleanup; }
    }

    while (next < nchunks) {
        size_t batch = (nchunks - next < nslots) ? (size_t)(nchunks - next) : nslots;

        for (size_t i = 0; i < batch; i++) {
            uint64_t idx = next + i;
            /* Expected size comes from validated header math, NOT from untrusted chunk metadata. */
            uint32_t expect = (idx + 1 == nchunks) ? (uint32_t)(total - idx * (uint64_t)chunk_size) : chunk_size;
            uint8_t ch[FCZ_CHUNK_HDR_SIZE];
            if ((result = read_exact(fin, ch, sizeof ch)) != FCZ_OK) goto cleanup;
            uint32_t olen = get_u32(ch), clen = get_u32(ch + 4), crc = get_u32(ch + 8);
            if (olen != expect)             { result = fail(FCZ_ERR_FORMAT, "chunk original size mismatch"); goto cleanup; }
            if (clen == 0 || clen > cap)    { result = fail(FCZ_ERR_FORMAT, "invalid compressed chunk size"); goto cleanup; }
            if ((result = read_exact(fin, cin + i * cap, clen)) != FCZ_OK) goto cleanup;
            jobs[i] = (djob){ .in = cin + i * cap, .in_len = clen, .out = obuf + i * chunk_size,
                              .out_len = olen, .crc = crc, .status = FCZ_ERR_THREAD };
        }

        size_t started = 0;
        for (; started < batch; started++) {
            int rc = pthread_create(&tids[started], NULL, decompress_worker, &jobs[started]);
            if (rc != 0) { fprintf(stderr, "error: pthread_create: %s\n", strerror(rc)); result = FCZ_ERR_THREAD; break; }
        }
        for (size_t i = 0; i < started; i++) pthread_join(tids[i], NULL);
        if (result != FCZ_OK) goto cleanup;
        for (size_t i = 0; i < batch; i++)
            if (jobs[i].status != FCZ_OK) { result = fail(jobs[i].status, "chunk is corrupted (decompression or checksum failed)"); goto cleanup; }

        for (size_t i = 0; i < batch; i++) {
            if (fwrite(jobs[i].out, 1, jobs[i].out_len, fout) != jobs[i].out_len) { perror("write"); result = FCZ_ERR_IO; goto cleanup; }
            written += jobs[i].out_len;
        }
        next += batch;
    }

    if (fgetc(fin) != EOF) { result = fail(FCZ_ERR_FORMAT, "unexpected data after last chunk"); goto cleanup; }
    if (written != total)  { result = fail(FCZ_ERR_FORMAT, "restored size mismatch"); goto cleanup; }
    if (fflush(fout) != 0) { perror("flush"); result = FCZ_ERR_IO; }

cleanup:
    free(cin); free(obuf); free(jobs); free(tids);
    fclose(fin);
    int created = (fout != NULL);
    if (fout && fclose(fout) != 0 && result == FCZ_OK) { perror("close"); result = FCZ_ERR_IO; }
    if (created && result != FCZ_OK) remove(out_path);
    if (result == FCZ_OK && st) { st->seconds = now_seconds() - t0; st->in_bytes = in_size; st->out_bytes = written; }
    return result;
}
