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
    bool eof;
    bool error;
    LZ4F_decompressionContext_t dctx;
    size_t nextSize;
    size_t zfill;
    size_t zpos;
    char zbuf[];
};

// Reads the stream towards real data, resets the reader handle.
static int lz4reader_init(struct lz4reader *zr, LZ4F_decompressionContext_t dctx,
			  int fd, const void *peekBuf, size_t peekSize, const char *err[2])
{
    memset(zr, 0, sizeof *zr);
    zr->fd = fd;
    zr->dctx = dctx;

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
		return ERRNO("read"), -1;
	    if (ret == 0)
		return 0;
	    peekBuf = zr->zbuf, peekSize = ret;
	}

	// Start the decoding with a NULL output buffer.
	size_t nullSize = 0;
	size_t pokenSize = peekSize;
	size_t nextSize = LZ4F_decompress(zr->dctx, NULL, &nullSize, peekBuf, &pokenSize, NULL);
	if (LZ4F_isError(nextSize))
	    return ERRLZ4("LZ4F_decompress", nextSize), -1;

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
		    return ERRNO("read"), -1;
		// The difference is that EOF is not acceptable here.
		if (ret == 0)
		    return ERRSTR("unexpected EOF"), -1;
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
		return ERRSTR("peekSize too big"), -1;
	    memcpy(zr->zbuf, peekBuf + pokenSize, peekSize - pokenSize);
	    zr->zfill = peekSize - pokenSize;
	}
	break;
    }

    // There is something in zbuf, so don't bother setting zr->nextSize:
    // it will be obtained when zbuf is fed into the decompressor.
    assert(zr->zpos < zr->zfill);

    return 1;
}

int lz4reader_fdopen(struct lz4reader **zrp, int fd, const void *peekBuf, size_t peekSize, const char *err[2])
{
    off_t pos0 = lseek(fd, 0, SEEK_CUR);
    if (pos0 != (off_t) -1 && pos0 != peekSize)
	return ERRSTR("file offset does not match peekSize"), -1;

    struct lz4reader *zr = malloc(sizeof *zr + ZBUFSIZE);
    if (!zr)
	return ERRNO("malloc"), -1;

    LZ4F_decompressionContext_t dctx;
    size_t zret = LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION);
    if (LZ4F_isError(zret))
	return ERRLZ4("LZ4F_createCompressionContext", zret),
	       free(zr), -1;

    int ret = lz4reader_init(zr, dctx, fd, peekBuf, peekSize, err);
    if (ret > 0)
	return *zrp = zr, 1;
    return LZ4F_freeDecompressionContext(dctx), free(zr), ret;
}

void lz4reader_close(struct lz4reader *zr)
{
    close(zr->fd);
    LZ4F_freeDecompressionContext(zr->dctx);
    free(zr);
}

ssize_t lz4reader_read(struct lz4reader *zr, void *buf, size_t size, const char *err[2])
{
    if (zr->eof)
	return 0;
    if (zr->error)
	return -1;

    // How many bytes have been read into the output buffer.
    size_t fill = 0;

    while (size) {
	// There must be something in zbuf.
	if (zr->zpos == zr->zfill) {
	    size_t n = zr->nextSize ? zr->nextSize : BUFSIZ;
	    if (n > ZBUFSIZE)
		n = ZBUFSIZE;
	    ssize_t ret;
	    do
		ret = read(zr->fd, zr->zbuf, n);
	    while (ret < 0 && errno == EINTR);
	    if (ret < 0)
		return ERRNO("read"), -(zr->error = true);
	    if (ret == 0) {
		// The only valid case for EOF.
		if (zr->nextSize == 0) {
		    zr->eof = true;
		    break;
		}
		return ERRSTR("unexpected EOF"), -(zr->error = true);
	    }
	    zr->zfill = ret, zr->zpos = 0;
	}

	// Feed zbuf to the decompressor.
	size_t raedenSize = zr->zfill - zr->zpos;
	size_t wrotenSize = size;
	zr->nextSize = LZ4F_decompress(zr->dctx, buf, &wrotenSize, zr->zbuf + zr->zpos, &raedenSize, NULL);
	if (LZ4F_isError(zr->nextSize))
	    return ERRLZ4("LZ4F_decompress", zr->nextSize), -(zr->error = true);
	fill += wrotenSize, buf += wrotenSize, size -= wrotenSize;
	zr->zpos += raedenSize;
    }

    return fill;
}

bool lz4reader_rewind(struct lz4reader *zr, const char *err[2])
{
    off_t pos0 = lseek(zr->fd, 0, SEEK_SET);
    if (pos0 == (off_t) -1)
	return ERRNO("lseek"), false;
    assert(pos0 == 0);

    // Reallocate the decompression context, unless EOF was reached
    // successfully - in this case, the context can be resued.
    if (!zr->eof) {
	LZ4F_freeDecompressionContext(zr->dctx), zr->dctx = NULL;
	size_t zret = LZ4F_createDecompressionContext(&zr->dctx, LZ4F_VERSION);
	if (LZ4F_isError(zret))
	    return ERRLZ4("LZ4F_createCompressionContext", zret), false;
    }

    zr->error = false;
    zr->eof = false;

    // Simply repositioning the descriptor and relying on the next read() might
    // not be enough, better go through the same routine as with open(), i.e.
    // step over the skippable frames until there's some real data.  This
    // ensures that contentSize() after rewind() will be consistent.
    int ret = lz4reader_init(zr, zr->dctx, zr->fd, NULL, 0, err);
    if (ret == 0)
	ERRSTR("unexpected EOF");
    return ret > 0;
}

// ex:set ts=8 sts=4 sw=4 noet:
