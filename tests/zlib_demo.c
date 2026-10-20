#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: %s file\n", argv[0]); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);

    unsigned char *src = malloc(n ? n : 1);
    if (!src || fread(src, 1, n, f) != (size_t)n) { fprintf(stderr, "read failed\n"); return 1; }
    fclose(f);

    uLongf clen = compressBound(n);
    unsigned char *comp = malloc(clen), *back = malloc(n ? n : 1);
    uLongf blen = n;
    int rc = compress2(comp, &clen, src, n, 6);
    if (rc != Z_OK) { fprintf(stderr, "compress2 failed: %d\n", rc); return 1; }
    rc = uncompress(back, &blen, comp, clen);
    if (rc != Z_OK) { fprintf(stderr, "uncompress failed: %d\n", rc); return 1; }

    printf("%ld -> %lu bytes, round trip %s\n", n, (unsigned long)clen,
           (blen == (uLongf)n && memcmp(src, back, n) == 0) ? "OK" : "MISMATCH");
    free(src); free(comp); free(back);
    return 0;
}
