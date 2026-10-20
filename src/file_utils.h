#ifndef FILE_UTILS_H
#define FILE_UTILS_H
#include <stdbool.h>
#include <stdio.h>
#include "fcz.h"

double     now_seconds(void);
bool       same_file(const char *a, const char *b);
fcz_status file_size_of(FILE *f, uint64_t *size);   /* ERR_ARG if not a regular file */
fcz_status copy_file(const char *src, const char *dst);
int        compare_files(const char *a, const char *b); /* 1 same, 0 differ, -1 error */
#endif
