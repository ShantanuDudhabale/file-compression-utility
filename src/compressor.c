#define _POSIX_C_SOURCE 200809L
#include "compressor.h"
#include "file_utils.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* One job = one chunk. Each worker touches ONLY its own job, so no locks are needed. */
typedef struct {
    const uint8_t *in;  uint32_t in_len;
    uint8_t       *out; uint32_t out_cap, out_len;
    uint32_t       crc;
    fcz_status     status;      /* how the worker reports success/failure to main */
} cjob;

static void *compress_worker(void *arg)
{
    cjob *j = arg;
    uLongf dest = j->out_cap;
    j->crc = (uint32_t)crc32(crc32(0L, Z_NULL, 0), j->in, j->in_len);
    int rc = compress2(j->out, &dest, j->in, j->in_len, FCZ_LEVEL);
    if (rc == Z_OK) { j->out_len = (uint32_t)dest; j->status = FCZ_OK; }
    else            { j->status = (rc == Z_MEM_ERROR) ? FCZ_ERR_MEM : FCZ_ERR_ZLIB; }
    return NULL;
}

fcz_status fcz_compress(const char *in_path, const char *out_path,
                        unsigned nthreads, uint32_t chunk_size, fcz_stats *st)
{
    fcz_status result = FCZ_OK;
    FILE *fin = NULL, *fout = NULL;
    uint8_t *inbuf = NULL, *outbuf = NULL;
    cjob *jobs = NULL;
    pthread_t *tids = NULL;
    uint64_t total = 0, nchunks = 0, remaining = 0, next = 0, out_bytes = FCZ_HEADER_SIZE;
    size_t nslots = 0, cap = 0;
    uint8_t hdr[FCZ_HEADER_SIZE];
    double t0 = now_seconds();

    if (nthreads < 1 || nthreads > FCZ_MAX_THREADS ||
        chunk_size < FCZ_MIN_CHUNK || chunk_size > FCZ_MAX_CHUNK)
        return FCZ_ERR_ARG;
    if (same_file(in_path, out_path)) {            /* must happen BEFORE fopen(out,"wb") truncates it */
        fprintf(stderr, "error: input and output are the same file\n");
        return FCZ_ERR_ARG;
    }

    fin = fopen(in_path, "rb");
    if (!fin) { perror(in_path); return FCZ_ERR_IO; }
    result = file_size_of(fin, &total);
    if (result != FCZ_OK) {
        fprintf(stderr, "error: %s is not a readable regular file\n", in_path);
        fclose(fin);
        return result;
    }
    nchunks = total / chunk_size + (total % chunk_size != 0);
    if (nchunks > UINT32_MAX) { fprintf(stderr, "error: file too large for this chunk size\n"); fclose(fin); return FCZ_ERR_ARG; }

    fout = fopen(out_path, "wb");
    if (!fout) { perror(out_path); fclose(fin); return FCZ_ERR_IO; }

    /* Bounded memory: one input and one output buffer per slot, reused every batch. */
    nslots = nchunks < nthreads ? (size_t)nchunks : nthreads;
    cap = compressBound(chunk_size);
    if (nslots > 0) {
        inbuf  = malloc(nslots * chunk_size);
        outbuf = malloc(nslots * cap);
        jobs   = calloc(nslots, sizeof *jobs);
        tids   = malloc(nslots * sizeof *tids);
        if (!inbuf || !outbuf || !jobs || !tids) { fprintf(stderr, "error: out of memory\n"); result = FCZ_ERR_MEM; goto cleanup; }
    }

    memcpy(hdr, FCZ_MAGIC, 4);
    put_u16(hdr + 4, FCZ_VERSION);
    put_u16(hdr + 6, 0);
    put_u32(hdr + 8, chunk_size);
    put_u64(hdr + 12, total);
    put_u32(hdr + 20, (uint32_t)nchunks);
    if (fwrite(hdr, 1, sizeof hdr, fout) != sizeof hdr) { perror("write"); result = FCZ_ERR_IO; goto cleanup; }

    remaining = total;
    while (next < nchunks) {
        size_t batch = (nchunks - next < nslots) ? (size_t)(nchunks - next) : nslots;

        /* 1. Main thread reads the batch (sequential disk access). */
        for (size_t i = 0; i < batch; i++) {
            uint32_t len = remaining < chunk_size ? (uint32_t)remaining : chunk_size;
            uint8_t *src = inbuf + i * chunk_size;
            if (fread(src, 1, len, fin) != len) {
                fprintf(stderr, "error: failed reading input (I/O error or file changed)\n");
                result = FCZ_ERR_IO; goto cleanup;
            }
            remaining -= len;
            jobs[i] = (cjob){ .in = src, .in_len = len, .out = outbuf + i * cap,
                              .out_cap = (uint32_t)cap, .status = FCZ_ERR_THREAD };
        }

        /* 2. One worker per chunk, all running concurrently. */
        size_t started = 0;
        for (; started < batch; started++) {
            int rc = pthread_create(&tids[started], NULL, compress_worker, &jobs[started]);
            if (rc != 0) { fprintf(stderr, "error: pthread_create: %s\n", strerror(rc)); result = FCZ_ERR_THREAD; break; }
        }
        /* 3. Always join what we started (never leave threads running), then check. */
        for (size_t i = 0; i < started; i++) pthread_join(tids[i], NULL);
        if (result != FCZ_OK) goto cleanup;
        for (size_t i = 0; i < batch; i++)
            if (jobs[i].status != FCZ_OK) { fprintf(stderr, "error: compression of a chunk failed\n"); result = jobs[i].status; goto cleanup; }

        /* 4. Main thread writes results strictly in chunk order => order preserved. */
        for (size_t i = 0; i < batch; i++) {
            uint8_t ch[FCZ_CHUNK_HDR_SIZE];
            put_u32(ch, jobs[i].in_len);
            put_u32(ch + 4, jobs[i].out_len);
            put_u32(ch + 8, jobs[i].crc);
            if (fwrite(ch, 1, sizeof ch, fout) != sizeof ch ||
                fwrite(jobs[i].out, 1, jobs[i].out_len, fout) != jobs[i].out_len) {
                perror("write"); result = FCZ_ERR_IO; goto cleanup;
            }
            out_bytes += FCZ_CHUNK_HDR_SIZE + jobs[i].out_len;
        }
        next += batch;
    }
    if (fflush(fout) != 0) { perror("flush"); result = FCZ_ERR_IO; }

cleanup:
    free(inbuf); free(outbuf); free(jobs); free(tids);
    fclose(fin);
    int created = (fout != NULL);
    if (fout && fclose(fout) != 0 && result == FCZ_OK) { perror("close"); result = FCZ_ERR_IO; }
    if (created && result != FCZ_OK) remove(out_path);      /* never leave a half-written file */
    if (result == FCZ_OK && st) { st->seconds = now_seconds() - t0; st->in_bytes = total; st->out_bytes = out_bytes; }
    return result;
}
