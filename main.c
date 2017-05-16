#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include "lz4reader.h"

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
    // Pretend we're peeking at the magic.
    char zbuf[16];
    ssize_t ret = read(0, zbuf, sizeof zbuf);
    assert(ret >= 0);

    const char *err[2];
    struct lz4reader *zr;
    int zret = lz4reader_fdopen(&zr, 0, zbuf, ret, err);
    if (zret < 0)
	return error("lz4reader_fdopen", err), 1;
    if (zret == 0)
	return fprintf(stderr, PROG ": empty input\n"), 0;

    char buf[256<<10];
    while ((ret = lz4reader_read(zr, buf, sizeof buf, err)) > 0)
	continue;

    if (!lz4reader_rewind(zr, err))
	return error("lz4reader_rewind", err), 1;

    while ((ret = lz4reader_read(zr, buf, sizeof buf, err)) > 0)
	fwrite(buf, 1, ret, stdout);
    lz4reader_close(zr);
    if (ret < 0)
	return error("lz4reader_read", err), 1;

    return 0;
}

// ex:set ts=8 sts=4 sw=4 noet:
