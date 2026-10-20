#define _POSIX_C_SOURCE 200809L
#include "file_utils.h"
#include <string.h>
#include <sys/stat.h>
#include <time.h>

const char *fcz_strerror(fcz_status s)
{
    switch (s) {
    case FCZ_OK:         return "success";
    case FCZ_ERR_ARG:    return "invalid argument";
    case FCZ_ERR_IO:     return "I/O error";
    case FCZ_ERR_MEM:    return "out of memory";
    case FCZ_ERR_FORMAT: return "invalid or corrupted file";
    case FCZ_ERR_ZLIB:   return "compression library error";
    case FCZ_ERR_THREAD: return "thread creation failed";
    }
    return "unknown error";
}

double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);       /* monotonic: unaffected by clock changes */
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Same device + inode = same file, even via different paths or symlinks.
   If either path doesn't exist yet, they can't be the same file. */
bool same_file(const char *a, const char *b)
{
    struct stat sa, sb;
    if (stat(a, &sa) != 0 || stat(b, &sb) != 0) return false;
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

fcz_status file_size_of(FILE *f, uint64_t *size)
{
    struct stat st;
    if (fstat(fileno(f), &st) != 0) return FCZ_ERR_IO;
    if (!S_ISREG(st.st_mode)) return FCZ_ERR_ARG;   /* directory, pipe, device... */
    *size = (uint64_t)st.st_size;
    return FCZ_OK;
}

fcz_status copy_file(const char *src, const char *dst)
{
    if (same_file(src, dst)) {
        fprintf(stderr, "error: source and destination are the same file\n");
        return FCZ_ERR_ARG;
    }
    FILE *in = fopen(src, "rb");
    if (!in) { perror(src); return FCZ_ERR_IO; }
    FILE *out = fopen(dst, "wb");
    if (!out) { perror(dst); fclose(in); return FCZ_ERR_IO; }

    uint8_t buf[65536];
    fcz_status rc = FCZ_OK;
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { perror("write"); rc = FCZ_ERR_IO; break; }
    }
    if (rc == FCZ_OK && ferror(in)) { perror("read"); rc = FCZ_ERR_IO; }
    fclose(in);
    if (fclose(out) != 0 && rc == FCZ_OK) { perror("close"); rc = FCZ_ERR_IO; }  /* catches disk-full */
    if (rc != FCZ_OK) remove(dst);                                               /* no partial output */
    return rc;
}

int compare_files(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    int result = -1;
    static uint8_t ba[65536], bb[65536];       /* static: keeps big buffers off the stack */
    if (fa && fb) {
        for (;;) {
            size_t na = fread(ba, 1, sizeof ba, fa);
            size_t nb = fread(bb, 1, sizeof bb, fb);
            if (ferror(fa) || ferror(fb)) break;
            if (na != nb || memcmp(ba, bb, na) != 0) { result = 0; break; }
            if (na == 0) { result = 1; break; }
        }
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return result;
}
