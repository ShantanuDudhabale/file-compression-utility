#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <zlib.h>        /* zlib compression library: deflate/inflate algorithms and CRC32 */
#include <pthread.h>     /* POSIX Threads: pthread_create, pthread_join for concurrency */

/* Platform-specific headers for CPU core detection */
#ifdef _WIN32
#include <windows.h>     /* Windows API: GetSystemInfo for processor count */
#else
#include <unistd.h>      /* POSIX API: sysconf(_SC_NPROCESSORS_ONLN) */
#include <sys/stat.h>
#endif

/* --- PCMP Archive Format & Buffer Constants --- */
#define MAGIC_HEADER        "PCMP"              /* 4-byte signature identifying PCMP archive files */
#define MAGIC_LEN           4                   /* Length of magic header */
#define DEFAULT_CHUNK_SIZE  (1024 * 1024)       /* Default chunk size: 1 MB */
#define MIN_CHUNK_SIZE      4096                /* Minimum chunk size: 4 KB */
#define MAX_CHUNK_SIZE      (64 * 1024 * 1024)  /* Maximum chunk size: 64 MB */
#define MAX_THREADS         64                  /* Maximum worker threads allowed */
#define DEFAULT_LEVEL       6                   /* Default zlib compression level (1=Fast, 6=Balanced, 9=Max) */
#define EXT_FIELD_LEN       16                  /* Max length for saved file extension (e.g., ".docx", ".pdf") */

/*
 * ArchiveHeader (36 bytes packed)
 * Stored at the very beginning of every .pcmp archive.
 * Contains all metadata required to reconstruct and verify the original file.
 */
#pragma pack(push, 1)
typedef struct {
    char magic[MAGIC_LEN];              /* "PCMP" identifier */
    uint64_t original_size;             /* Total byte size of uncompressed original file */
    uint32_t total_chunks;              /* Total number of chunks divided during compression */
    uint32_t chunk_size;                /* Chunk size in bytes used during compression */
    char original_ext[EXT_FIELD_LEN];   /* Original file extension (e.g. ".pdf", ".docx", ".txt") */
} ArchiveHeader;
#pragma pack(pop)

/*
 * ThreadTask
 * Per-worker data structure passed as argument to pthread_create.
 * Enables lock-free data parallelism: each thread operates exclusively on its own memory buffers.
 */
typedef struct {
    int thread_id;              /* Worker thread index (0, 1, 2, ...) */
    int chunk_index;            /* Global chunk index being processed */
    const uint8_t *in_data;     /* Pointer to raw uncompressed input chunk */
    uint32_t in_len;            /* Actual byte length of input chunk */
    uint8_t *out_data;          /* Destination buffer for compressed output */
    uint32_t out_cap;           /* Maximum capacity of destination buffer */
    uint32_t out_len;           /* Resulting compressed byte length */
    uint32_t crc;               /* CRC32 integrity checksum of chunk data */
    double elapsed_sec;         /* Execution time measured for this chunk */
    int success;                /* 1 if operation succeeded, 0 on error */
    int level;                  /* Compression level (1 to 9) */
} ThreadTask;

/* High-resolution monotonic timer for measuring throughput and latency */
static double get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Query OS kernel to detect total available hardware CPU cores */
static int get_cpu_cores(void)
{
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (si.dwNumberOfProcessors > 0) ? (int)si.dwNumberOfProcessors : 4;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (int)n : 4;
#endif
}

/* Extract file extension from input filename to store in archive header */
static void extract_extension(const char *path, char *out_ext, size_t max_len)
{
    out_ext[0] = '\0';
    const char *dot = strrchr(path, '.');
    if (dot && strlen(dot) < max_len) {
        strncpy(out_ext, dot, max_len - 1);
        out_ext[max_len - 1] = '\0';
    }
}

/*
 * compress_worker (Executed concurrently by POSIX worker threads)
 * 1. Computes CRC32 checksum over the uncompressed input chunk.
 * 2. Compresses the chunk using zlib compress2() at the specified compression level.
 * 3. Records execution time and sets task->success status.
 * Note: Runs completely lock-free because each thread owns isolated in/out buffers.
 */
static void *compress_worker(void *arg)
{
    ThreadTask *task = (ThreadTask *)arg;
    double t0 = get_time();

    /* Compute CRC32 checksum before compression for integrity verification */
    task->crc = (uint32_t)crc32(0L, task->in_data, task->in_len);

    uLongf dest_cap = task->out_cap;
    int level = (task->level >= 1 && task->level <= 9) ? task->level : DEFAULT_LEVEL;
    int ret = compress2(task->out_data, &dest_cap, task->in_data, task->in_len, level);

    if (ret == Z_OK) {
        task->out_len = (uint32_t)dest_cap;
        task->success = 1;
    } else {
        task->success = 0;
    }

    task->elapsed_sec = get_time() - t0;
    return NULL;
}

/*
 * decompress_worker (Executed concurrently by POSIX worker threads)
 * 1. Decompresses chunk using zlib uncompress().
 * 2. Computes CRC32 checksum over restored data.
 * 3. Compares computed checksum against header checksum to guarantee 100% byte integrity.
 */
static void *decompress_worker(void *arg)
{
    ThreadTask *task = (ThreadTask *)arg;
    double t0 = get_time();

    uLongf expected_len = task->out_cap;
    int ret = uncompress(task->out_data, &expected_len, task->in_data, task->in_len);

    if (ret == Z_OK && expected_len == task->out_cap) {
        /* Verify integrity: match recalculated CRC32 with original CRC32 */
        uint32_t computed_crc = (uint32_t)crc32(0L, task->out_data, expected_len);
        if (computed_crc == task->crc) {
            task->out_len = (uint32_t)expected_len;
            task->success = 1;
        } else {
            fprintf(stderr, "[Thread %d] Error: CRC32 mismatch in chunk %d\n",
                    task->thread_id, task->chunk_index);
            task->success = 0;
        }
    } else {
        task->success = 0;
    }

    task->elapsed_sec = get_time() - t0;
    return NULL;
}

/*
 * compress_file (Main Compression Coordinator)
 * 1. Validates thread count, chunk size, and compression level.
 * 2. Writes 36-byte ArchiveHeader (magic, original size, total chunks, extension).
 * 3. Batches chunks across worker threads.
 * 4. Spawns threads with pthread_create() to compress in parallel.
 * 5. Uses pthread_join() as a barrier synchronization point.
 * 6. Sequentially writes compressed chunks and chunk metadata to disk in original order.
 */
int compress_file(const char *in_path, const char *out_path, int num_threads, uint32_t chunk_size, int level)
{
    /* Sanitize parameters */
    if (num_threads < 1) num_threads = get_cpu_cores();
    if (num_threads > MAX_THREADS) num_threads = MAX_THREADS;
    if (chunk_size < MIN_CHUNK_SIZE) chunk_size = DEFAULT_CHUNK_SIZE;
    if (chunk_size > MAX_CHUNK_SIZE) chunk_size = MAX_CHUNK_SIZE;
    if (level < 1 || level > 9) level = DEFAULT_LEVEL;

    FILE *fin = fopen(in_path, "rb");
    if (!fin) {
        perror("Error opening input file");
        return 1;
    }

    /* Determine input file size */
    fseek(fin, 0, SEEK_END);
    int64_t file_size = ftell(fin);
    rewind(fin);

    if (file_size < 0) {
        fclose(fin);
        return 1;
    }

    FILE *fout = fopen(out_path, "wb");
    if (!fout) {
        perror("Error creating output file");
        fclose(fin);
        return 1;
    }

    /* Calculate chunk count */
    uint32_t total_chunks = (uint32_t)(file_size / chunk_size + (file_size % chunk_size != 0));
    if (file_size == 0) total_chunks = 0;

    printf("[OS Coordinator] Starting parallel compression...\n");
    printf("[OS Coordinator] Input: %s (%lld bytes)\n", in_path, (long long)file_size);
    printf("[OS Coordinator] Allocated %d worker threads across %u chunk(s) (chunk: %u KB, level: %d)\n\n",
           num_threads, total_chunks, chunk_size / 1024, level);

    /* Write PCMP Archive Header */
    ArchiveHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, MAGIC_HEADER, MAGIC_LEN);
    hdr.original_size = (uint64_t)file_size;
    hdr.total_chunks = total_chunks;
    hdr.chunk_size = chunk_size;
    extract_extension(in_path, hdr.original_ext, sizeof(hdr.original_ext));

    if (fwrite(&hdr, sizeof(hdr), 1, fout) != 1) {
        perror("Error writing header to output file");
        fclose(fin);
        fclose(fout);
        return 1;
    }

    /* Allocate batch processing buffers for threads */
    int batch_size = num_threads;
    if ((uint32_t)batch_size > total_chunks) batch_size = (total_chunks > 0) ? (int)total_chunks : 1;

    size_t cap = compressBound(chunk_size);
    uint8_t *in_buf = malloc((size_t)batch_size * chunk_size);
    uint8_t *out_buf = malloc((size_t)batch_size * cap);
    ThreadTask *tasks = calloc(batch_size, sizeof(ThreadTask));
    pthread_t *tids = malloc(batch_size * sizeof(pthread_t));

    if (!in_buf || !out_buf || !tasks || !tids) {
        fprintf(stderr, "Memory allocation error\n");
        free(in_buf); free(out_buf); free(tasks); free(tids);
        fclose(fin);
        fclose(fout);
        return 1;
    }

    uint64_t remaining_bytes = (uint64_t)file_size;
    uint32_t chunk_idx = 0;
    uint64_t total_compressed = sizeof(ArchiveHeader);
    double t_start = get_time();
    int error_occurred = 0;

    /* Process chunks in parallel batches equal to thread count */
    while (chunk_idx < total_chunks && !error_occurred) {
        int current_batch = batch_size;
        if (chunk_idx + current_batch > total_chunks) {
            current_batch = (int)(total_chunks - chunk_idx);
        }

        /* Read batch of chunks from input disk */
        int read_ok = 1;
        for (int i = 0; i < current_batch; i++) {
            uint32_t len = (remaining_bytes < chunk_size) ? (uint32_t)remaining_bytes : chunk_size;
            uint8_t *src = in_buf + (size_t)i * chunk_size;

            size_t bytes_read = fread(src, 1, len, fin);
            if (bytes_read != len) {
                fprintf(stderr, "[Error] Disk read failed on chunk %u (expected %u, got %zu bytes)\n",
                        chunk_idx + i, len, bytes_read);
                read_ok = 0;
                error_occurred = 1;
                break;
            }
            remaining_bytes -= len;

            /* Setup per-thread task descriptors */
            tasks[i].thread_id = i;
            tasks[i].chunk_index = chunk_idx + i;
            tasks[i].in_data = src;
            tasks[i].in_len = len;
            tasks[i].out_data = out_buf + (size_t)i * cap;
            tasks[i].out_cap = (uint32_t)cap;
            tasks[i].out_len = 0;
            tasks[i].level = level;
            tasks[i].success = 0;
        }

        if (!read_ok) break;

        /* Fork worker threads to compress chunks concurrently */
        int started = 0;
        for (int i = 0; i < current_batch; i++) {
            int rc = pthread_create(&tids[i], NULL, compress_worker, &tasks[i]);
            if (rc != 0) {
                fprintf(stderr, "[Error] pthread_create failed for worker %d: %s\n", i, strerror(rc));
                error_occurred = 1;
                break;
            }
            started++;
        }

        /* Barrier synchronization: wait for all threads in current batch */
        for (int i = 0; i < started; i++) {
            pthread_join(tids[i], NULL);
        }

        if (error_occurred) break;

        /* Write compressed batch sequentially to output archive to prevent race conditions */
        for (int i = 0; i < current_batch; i++) {
            if (!tasks[i].success) {
                fprintf(stderr, "[Error] Thread %d failed on chunk %d\n", i, tasks[i].chunk_index);
                error_occurred = 1;
                break;
            }

            printf("[Thread %2d] Chunk #%2d: %7u -> %7u bytes (saved %.1f%%) in %.4fs\n",
                   tasks[i].thread_id, tasks[i].chunk_index, tasks[i].in_len, tasks[i].out_len,
                   100.0 - (100.0 * tasks[i].out_len / tasks[i].in_len),
                   tasks[i].elapsed_sec);

            /* Write chunk header: in_len, out_len, CRC32 checksum, followed by compressed payload */
            fwrite(&tasks[i].in_len, sizeof(uint32_t), 1, fout);
            fwrite(&tasks[i].out_len, sizeof(uint32_t), 1, fout);
            fwrite(&tasks[i].crc, sizeof(uint32_t), 1, fout);
            if (fwrite(tasks[i].out_data, 1, tasks[i].out_len, fout) != tasks[i].out_len) {
                perror("Error writing compressed chunk to disk");
                error_occurred = 1;
                break;
            }

            total_compressed += 12 + tasks[i].out_len;
        }

        chunk_idx += current_batch;
    }

    double total_time = get_time() - t_start;
    double throughput = (total_time > 0) ? (file_size / 1e6 / total_time) : 0;

    /* Display summary report */
    if (!error_occurred) {
        double ratio = (file_size > 0) ? (((double)total_compressed / (double)file_size) * 100.0) : 100.0;
        double saved_pct = (file_size > 0) ? ((((double)file_size - (double)total_compressed) / (double)file_size) * 100.0) : 0.0;

        printf("\n--------------------------------------------------\n");
        printf("Compression Complete:\n");
        printf("  Original Size    : %lld bytes\n", (long long)file_size);
        printf("  Compressed Size  : %lld bytes\n", (long long)total_compressed);
        printf("  Compression Ratio: %.2f%%\n", ratio);
        if (total_compressed > (uint64_t)file_size) {
            double expansion_pct = (((double)total_compressed - (double)file_size) / (double)file_size) * 100.0;
            printf("  Space Saved      : File expanded by %.2f%%\n", expansion_pct);
        } else if (total_compressed == (uint64_t)file_size) {
            printf("  Space Saved      : No size reduction\n");
        } else {
            printf("  Space Saved      : %.2f%% space saved\n", saved_pct);
        }
        printf("  Execution Time   : %.3f seconds\n", total_time);
        printf("  Throughput       : %.1f MB/s\n", throughput);
        printf("  Threads Used     : %d\n", num_threads);
        printf("  Compression Lvl  : %d (1=Fast, 6=Balanced, 9=Max)\n", level);
        printf("--------------------------------------------------\n");
    }

    /* Release resources */
    free(in_buf);
    free(out_buf);
    free(tasks);
    free(tids);
    fclose(fin);
    fclose(fout);

    if (error_occurred) {
        remove(out_path);
        return 1;
    }
    return 0;
}

/*
 * decompress_file (Main Decompression Coordinator)
 * 1. Reads and validates the 36-byte ArchiveHeader ("PCMP" magic check).
 * 2. Allocates decompression batch buffers for worker threads.
 * 3. Reads chunk headers (in_len, out_len, CRC32) and compressed payloads.
 * 4. Spawns threads to execute zlib uncompress() and CRC32 verification concurrently.
 * 5. Joins threads and sequentially writes restored chunks to the output file.
 * 6. Guarantees byte-for-byte fidelity with the original file.
 */
int decompress_file(const char *in_path, const char *out_path, int num_threads)
{
    /* Sanitize thread count */
    if (num_threads < 1) num_threads = get_cpu_cores();
    if (num_threads > MAX_THREADS) num_threads = MAX_THREADS;

    FILE *fin = fopen(in_path, "rb");
    if (!fin) {
        perror("Error opening compressed file");
        return 1;
    }

    /* Read and validate archive header */
    ArchiveHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, fin) != 1) {
        fprintf(stderr, "Error: Failed to read archive header\n");
        fclose(fin);
        return 1;
    }

    if (memcmp(hdr.magic, MAGIC_HEADER, MAGIC_LEN) != 0) {
        fprintf(stderr, "Error: Not a valid PCMP compressed file\n");
        fclose(fin);
        return 1;
    }

    uint64_t original_size = hdr.original_size;
    uint32_t total_chunks = hdr.total_chunks;
    uint32_t chunk_size = hdr.chunk_size;

    /* Validate header sanity */
    if (chunk_size < MIN_CHUNK_SIZE || chunk_size > MAX_CHUNK_SIZE) {
        fprintf(stderr, "Error: Corrupted archive header (invalid chunk size: %u bytes)\n", chunk_size);
        fclose(fin);
        return 1;
    }
    if (total_chunks > 10000000) {
        fprintf(stderr, "Error: Corrupted archive header (unrealistic chunk count: %u)\n", total_chunks);
        fclose(fin);
        return 1;
    }

    FILE *fout = fopen(out_path, "wb");
    if (!fout) {
        perror("Error creating restored file");
        fclose(fin);
        return 1;
    }

    printf("[OS Coordinator] Starting parallel decompression...\n");
    printf("[OS Coordinator] Archive: %s\n", in_path);
    printf("[OS Coordinator] Target : %lld bytes across %u chunk(s) (%d threads)\n",
           (long long)original_size, total_chunks, num_threads);
    if (hdr.original_ext[0] != '\0') {
        printf("[OS Coordinator] Original Extension: %s\n\n", hdr.original_ext);
    } else {
        printf("\n");
    }

    /* Allocate batch processing buffers */
    int batch_size = num_threads;
    if ((uint32_t)batch_size > total_chunks) batch_size = (total_chunks > 0) ? (int)total_chunks : 1;

    size_t cap = compressBound(chunk_size);
    uint8_t *in_buf = malloc((size_t)batch_size * cap);
    uint8_t *out_buf = malloc((size_t)batch_size * chunk_size);
    ThreadTask *tasks = calloc(batch_size, sizeof(ThreadTask));
    pthread_t *tids = malloc(batch_size * sizeof(pthread_t));

    if (!in_buf || !out_buf || !tasks || !tids) {
        fprintf(stderr, "Memory allocation error\n");
        free(in_buf); free(out_buf); free(tasks); free(tids);
        fclose(fin);
        fclose(fout);
        return 1;
    }

    uint32_t chunk_idx = 0;
    uint64_t total_restored = 0;
    double t_start = get_time();
    int error_occurred = 0;

    /* Decompress chunks in concurrent batches */
    while (chunk_idx < total_chunks && !error_occurred) {
        int current_batch = batch_size;
        if (chunk_idx + current_batch > total_chunks) {
            current_batch = (int)(total_chunks - chunk_idx);
        }

        /* Read batch of chunk metadata and compressed payloads from archive */
        int read_ok = 1;
        for (int i = 0; i < current_batch; i++) {
            uint32_t orig_len = 0, comp_len = 0, crc = 0;
            if (fread(&orig_len, sizeof(uint32_t), 1, fin) != 1 ||
                fread(&comp_len, sizeof(uint32_t), 1, fin) != 1 ||
                fread(&crc, sizeof(uint32_t), 1, fin) != 1) {
                fprintf(stderr, "[Error] Failed reading chunk %u metadata\n", chunk_idx + i);
                read_ok = 0;
                error_occurred = 1;
                break;
            }

            if (orig_len == 0 || orig_len > chunk_size) {
                fprintf(stderr, "[Error] Corrupted chunk %u: invalid orig_len\n", chunk_idx + i);
                read_ok = 0;
                error_occurred = 1;
                break;
            }
            if (comp_len == 0 || comp_len > cap) {
                fprintf(stderr, "[Error] Corrupted chunk %u: invalid comp_len\n", chunk_idx + i);
                read_ok = 0;
                error_occurred = 1;
                break;
            }

            uint8_t *src = in_buf + (size_t)i * cap;
            if (fread(src, 1, comp_len, fin) != comp_len) {
                fprintf(stderr, "[Error] Failed reading chunk %u data\n", chunk_idx + i);
                read_ok = 0;
                error_occurred = 1;
                break;
            }

            /* Configure per-thread decompression task */
            tasks[i].thread_id = i;
            tasks[i].chunk_index = chunk_idx + i;
            tasks[i].in_data = src;
            tasks[i].in_len = comp_len;
            tasks[i].out_data = out_buf + (size_t)i * chunk_size;
            tasks[i].out_cap = orig_len;
            tasks[i].crc = crc;
            tasks[i].success = 0;
        }

        if (!read_ok) break;

        /* Spawn worker threads for parallel decompression */
        int started = 0;
        for (int i = 0; i < current_batch; i++) {
            int rc = pthread_create(&tids[i], NULL, decompress_worker, &tasks[i]);
            if (rc != 0) {
                fprintf(stderr, "[Error] pthread_create failed in decompression: %s\n", strerror(rc));
                error_occurred = 1;
                break;
            }
            started++;
        }

        /* Wait for all decompression threads to finish */
        for (int i = 0; i < started; i++) {
            pthread_join(tids[i], NULL);
        }

        if (error_occurred) break;

        /* Write verified uncompressed chunks to destination file in sequential order */
        for (int i = 0; i < current_batch; i++) {
            if (!tasks[i].success) {
                fprintf(stderr, "[Error] Decompression failed on chunk %d\n", tasks[i].chunk_index);
                error_occurred = 1;
                break;
            }

            printf("[Thread %2d] Restored Chunk #%2d (%u bytes, CRC32 Verified OK) in %.4fs\n",
                   tasks[i].thread_id, tasks[i].chunk_index, tasks[i].out_len, tasks[i].elapsed_sec);

            if (fwrite(tasks[i].out_data, 1, tasks[i].out_len, fout) != tasks[i].out_len) {
                perror("Error writing restored data to disk");
                error_occurred = 1;
                break;
            }
            total_restored += tasks[i].out_len;
        }

        chunk_idx += current_batch;
    }

    double total_time = get_time() - t_start;
    double throughput = (total_time > 0) ? (total_restored / 1e6 / total_time) : 0;

    /* Display decompression summary */
    if (!error_occurred && total_restored == original_size) {
        printf("\n--------------------------------------------------\n");
        printf("Decompression Complete:\n");
        printf("  Restored Size  : %lld bytes\n", (long long)total_restored);
        printf("  Integrity Check: ALL CHUNKS MATCHED CRC32 CHECKSUM\n");
        printf("  Execution Time : %.3f seconds\n", total_time);
        printf("  Throughput     : %.1f MB/s\n", throughput);
        printf("--------------------------------------------------\n");
    }

    /* Clean up allocated resources */
    free(in_buf);
    free(out_buf);
    free(tasks);
    free(tids);
    fclose(fin);
    fclose(fout);

    if (error_occurred || total_restored != original_size) {
        remove(out_path);
        return 1;
    }
    return 0;
}

/*
 * run_benchmark (Amdahl's Law Scaling Analysis)
 * Runs compression repeatedly on the same file with 1, 2, 4, 8, and N threads.
 * Measures speedup = (T_1 / T_N) to evaluate concurrency efficiency and I/O bottlenecks.
 */
void run_benchmark(const char *in_path)
{
    int max_cores = get_cpu_cores();
    int test_threads[] = {1, 2, 4, 8, max_cores};
    int num_tests = sizeof(test_threads) / sizeof(test_threads[0]);

    printf("==============================================================\n");
    printf("   OS Concurrency Benchmark: Amdahl's Law Scaling Analysis   \n");
    printf("   Test file: %s (Cores detected: %d)\n", in_path, max_cores);
    printf("==============================================================\n");
    printf("%-10s %-15s %-15s %-12s\n", "Threads", "Time (s)", "Throughput", "Speedup");
    printf("--------------------------------------------------------------\n");

    char temp_out[256];
#ifdef _WIN32
    const char *tmp = getenv("TEMP");
    if (!tmp) tmp = ".";
    snprintf(temp_out, sizeof(temp_out), "%s\\bench_test.pcmp", tmp);
#else
    snprintf(temp_out, sizeof(temp_out), "/tmp/bench_test.pcmp");
#endif

    double base_time = 0.0;
    for (int i = 0; i < num_tests; i++) {
        int th = test_threads[i];
        if (i > 0 && th == test_threads[i - 1]) continue;

        double t0 = get_time();
        int rc = compress_file(in_path, temp_out, th, DEFAULT_CHUNK_SIZE, DEFAULT_LEVEL);
        double duration = get_time() - t0;
        remove(temp_out);

        if (rc != 0) {
            printf("%-10d FAILED\n", th);
            continue;
        }

        if (i == 0) base_time = duration;
        double speedup = (duration > 0) ? (base_time / duration) : 1.0;

        FILE *f = fopen(in_path, "rb");
        int64_t fsz = 0;
        if (f) { fseek(f, 0, SEEK_END); fsz = ftell(f); fclose(f); }
        double tput = (duration > 0) ? (fsz / 1e6 / duration) : 0;

        printf("%-10d %-15.4f %-15.1f %.2fx\n", th, duration, tput, speedup);
    }
    printf("==============================================================\n");
}

/*
 * main (CLI Entry Point & Argument Dispatcher)
 * Parses command-line flags:
 *   -c: Compress mode
 *   -d: Decompress mode
 *   -b: Benchmark mode
 */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("Parallel File Compressor (pthreads)\n");
        printf("Usage:\n");
        printf("  %s -c <input> <output.pcmp> [threads] [chunk_kib] [level] (Compress)\n", argv[0]);
        printf("  %s -d <input.pcmp> <output> [threads]                     (Decompress)\n", argv[0]);
        printf("  %s -b <input_file>                                        (Benchmark scaling)\n\n", argv[0]);
        return 1;
    }

    const char *mode = argv[1];

    /* Mode -c: Parallel Compression */
    if (strcmp(mode, "-c") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: %s -c <input> <output.pcmp> [threads] [chunk_kib] [level]\n", argv[0]);
            return 1;
        }
        int threads = (argc >= 5) ? atoi(argv[4]) : get_cpu_cores();
        uint32_t chunk_kib = (argc >= 6) ? (uint32_t)atoi(argv[5]) : (DEFAULT_CHUNK_SIZE / 1024);
        int level = (argc >= 7) ? atoi(argv[6]) : DEFAULT_LEVEL;
        return compress_file(argv[2], argv[3], threads, chunk_kib * 1024, level);
    }
    /* Mode -d: Parallel Decompression */
    else if (strcmp(mode, "-d") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: %s -d <input.pcmp> <output> [threads]\n", argv[0]);
            return 1;
        }
        int threads = (argc >= 5) ? atoi(argv[4]) : get_cpu_cores();
        return decompress_file(argv[2], argv[3], threads);
    }
    /* Mode -b: Amdahl's Law Benchmark */
    else if (strcmp(mode, "-b") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s -b <input_file>\n", argv[0]);
            return 1;
        }
        run_benchmark(argv[2]);
        return 0;
    }

    fprintf(stderr, "Unknown option '%s'. Use -c (compress), -d (decompress), or -b (benchmark).\n", mode);
    return 1;
}
