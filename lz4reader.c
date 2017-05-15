// Copyright (c) 2017 Alexey Tourbin
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <stdio.h> // sys_errlist
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <lz4frame.h>
#include "lz4reader.h"

// A thread-safe strerror(3) replacement.
static const char *xstrerror(int errnum)
{
    // Some of the great minds say that sys_errlist is deprecated.
    // Well, at least it's thread-safe, and it does not deadlock.
    if (errnum > 0 && errnum < sys_nerr)
	return sys_errlist[errnum];
    return "Unknown error";
}

// Helpers to fill err[2] arg.
#define ERRNO(func) err[0] = func, err[1] = xstrerror(errno)
#define ERRLZ4(func, ret) err[0] = func, err[1] = LZ4F_getErrorName(ret)
#define ERRSTR(str) err[0] = __func__, err[1] = str

// When the internal block size is LZ4F_max256KB, and the caller does
// 256K bulk reads, the input buffer this big can reduce intermediate
// copying in the guts of the LZ4F library.
#define ZBUFSIZE ((256 << 10) + 4)

struct lz4reader {
    int fd;
    LZ4F_decompressionContext_t dctx;
    size_t zfill;
    size_t zpos;
    char zbuf[ZBUFSIZE];
};

int lz4reader_fdopen(struct lz4reader **zrp, int fd, const void *peekBuf, size_t peekSize, const char *err[2])
{
    off_t pos0 = lseek(fd, 0, SEEK_CUR);
    if (pos0 != (off_t) -1 && pos0 != peekSize)
	return ERRSTR("file offset does not match peekSize"), -1;

    struct lz4reader *zr = malloc(sizeof *zr);
    if (!zr)
	return ERRNO("malloc"), -1;

    zr->fd = fd;
    zr->zfill = 0;
    zr->zpos = 0;

    size_t zret = LZ4F_createDecompressionContext(&zr->dctx, LZ4F_VERSION);
    if (LZ4F_isError(zret))
	return ERRLZ4("LZ4F_createCompressionContext", zret),
	       free(zr), -1;

    while (1) {
	// Ensure we've got something to peek upon.
	if (!peekSize) {
	    // Don't waste a system call to read just a few bytes,
	    // but also don't read too much.  Perhaps BUFSIZ will do.
	    ssize_t ret;
	    do
		ret = read(fd, zr->zbuf, BUFSIZ);
	    while (ret < 0 && errno == EINTR);
	    if (ret < 0)
		return ERRNO("read"),
		       LZ4F_freeDecompressionContext(zr->dctx), free(zr), -1;
	    if (ret == 0)
		return 0;
	    peekBuf = zr->zbuf, peekSize = ret;
	}

	// Start the decoding with a NULL output buffer.
	size_t nullSize = 0;
	size_t pokenSize = peekSize;
	size_t nextSize = LZ4F_decompress(zr->dctx, NULL, &nullSize, peekBuf, &pokenSize, NULL);
	if (LZ4F_isError(nextSize))
	    return ERRLZ4("LZ4F_decompress", nextSize),
		   LZ4F_freeDecompressionContext(zr->dctx), free(zr), -1;

	if (pokenSize == peekSize) {
	    // Compressed data has been consumed without producing any output.
	    // Will peek on.  If peekSize is zero, it's the next frame,
	    // or possibly EOF (handled at the beginning of the loop).
	    // Otherwise, need to try and fetch some data right here (why?).
	    peekSize = nextSize;
	    if (peekSize) {
		// Disregard the advice and fetch BUFSIZ again.
		ssize_t ret;
		do
		    ret = read(fd, zr->zbuf, BUFSIZ);
		while (ret < 0 && errno == EINTR);
		if (ret < 0)
		    return ERRNO("read"),
			   LZ4F_freeDecompressionContext(zr->dctx), free(zr), -1;
		// The difference is that EOF is not acceptable here.
		if (ret == 0)
		    return ERRSTR("unexpected EOF"),
			   LZ4F_freeDecompressionContext(zr->dctx), free(zr), -1;
		peekBuf = zr->zbuf, peekSize = ret;
	    }
	    continue;
	}

	if (nextSize == 0) {
	    // Compressed data has not been consumed, but the result of
	    // LZ4F_decompress is 0.  This means that the current frame ends
	    // somewhere in the middle of the data.  Need to reload the loop
	    // with the rest of the data.
	    peekBuf += pokenSize, peekSize -= pokenSize;
	    continue;
	}

	// Compressed data not consumed, output buffer is too small!
	if (peekBuf == zr->zbuf)
	    zr->zfill = peekSize, zr->zpos = pokenSize;
	else {
	    if (peekSize - pokenSize > ZBUFSIZE)
		return ERRSTR("peekSize too big"),
		       LZ4F_freeDecompressionContext(zr->dctx), free(zr), -1;
	    memcpy(zr->zbuf, peekBuf + pokenSize, peekSize - pokenSize);
	    zr->zfill = peekSize - pokenSize;
	}
	break;
    }
    *zrp = zr;
    return 1;
}

void lz4reader_close(struct lz4reader *zr)
{
    close(zr->fd);
    LZ4F_freeDecompressionContext(zr->dctx);
    free(zr);
}

// ex:set ts=8 sts=4 sw=4 noet:
