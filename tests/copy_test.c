#include <stdio.h>
#include "file_utils.h"
int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s src dst\n", argv[0]); return 1; }
    fcz_status s = copy_file(argv[1], argv[2]);
    if (s != FCZ_OK) { fprintf(stderr, "copy failed: %s\n", fcz_strerror(s)); return 1; }
    printf("compare: %d (1 = identical)\n", compare_files(argv[1], argv[2]));
    return 0;
}
