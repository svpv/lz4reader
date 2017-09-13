#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include "lz4reader.h"
#include "reada.h"

#define PROG "lz4reader"

static void error(const char *func, const char *err[2])
{
    if (strcmp(func, err[0]) == 0)
	fprintf(stderr, PROG ": %s: %s\n", err[0], err[1]);
    else
	fprintf(stderr, PROG ": %s: %s: %s\n", func, err[0], err[1]);
}

int main()
{
    char fdabuf[NREADA];
    struct fda fda = { 0, fdabuf };

    const char *err[2];
    struct lz4reader *z;
    int zret = lz4reader_fdopen(&z, &fda, err);
    if (zret < 0)
	return error("lz4reader_fdopen", err), 1;
    if (zret == 0)
	return fprintf(stderr, PROG ": empty input\n"), 0;

    char buf[256<<10];
    ssize_t ret;
#if 0
    while ((ret = lz4reader_read(z, buf, sizeof buf, err)) > 0)
	continue;

    if (!lz4reader_rewind(z, err))
	return error("lz4reader_rewind", err), 1;
#endif
    while ((ret = lz4reader_read(z, buf, sizeof buf, err)) > 0)
	fwrite(buf, 1, ret, stdout);
    lz4reader_close(z);
    if (ret < 0)
	return error("lz4reader_read", err), 1;

    return 0;
}

// ex:set ts=8 sts=4 sw=4 noet:
