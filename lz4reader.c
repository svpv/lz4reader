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
#include <lz4.h> // LZ4_VERSION_NUMBER
#include <lz4frame.h>
#include "lz4reader.h"
#include "reada.h"

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

// Start decoding at the beginning of a frame.
static ssize_t lz4reader_begin(struct fda *fda, LZ4F_decompressionContext_t dctx,
			       const char *err[2])
{
    char buf[16];
    const unsigned char magic[4] = { 0x04, 0x22, 0x4d, 0x18 };

    // Read the magic and just enough leading bytes to start decoding
    // the header; minFHSize = 7, see lz4frame.c.
    size_t peekSize = 7;
    ssize_t ret = reada(fda, buf, peekSize);
    if (ret < 0)
	return ERRNO("read"), -1;
    if (ret == 0)
	return 0;
    if (ret < 4)
	return ERRSTR("unexpected EOF"), -1;
    if (memcmp(buf, magic, 4))
	return ERRSTR("bad LZ4 magic"), -1;
    if (ret < peekSize)
	return ERRSTR("unexpected EOF"), -1;

    // Start decoding with a NULL output buffer.
    // All the input must be consumed while no output produced.
    size_t nullSize = 0;
    size_t pokenSize = peekSize;
    size_t zret = LZ4F_decompress(dctx, NULL, &nullSize, buf, &pokenSize, NULL);
    if (LZ4F_isError(zret))
	return ERRLZ4("LZ4F_decompress", zret), -1;
    assert(pokenSize == peekSize);

    // Now, the second call should get us to the first block size;
    // maxFHSize = 19 and the block size is 4 bytes, so buf[19+4-7=16]
    // should be just enough.  Additionally to the 4-byte block size,
    // lz4 can request the content size (8 bytes) and dictID (4 bytes).
    size_t nextSize = zret;
    assert(nextSize == 4 || nextSize == 8 || nextSize == 12 || nextSize == 16);

    peekSize = nextSize;
    ret = reada(fda, buf, peekSize);
    if (ret < 0)
	return ERRNO("read"), -1;
    if (ret < peekSize)
	return ERRSTR("unexpected EOF"), -1;

    pokenSize = peekSize;
    zret = LZ4F_decompress(dctx, NULL, &nullSize, buf, &pokenSize, NULL);
    if (LZ4F_isError(zret))
	return ERRLZ4("LZ4F_decompress", zret), -1;
    assert(pokenSize == peekSize);

    nextSize = zret;
    return nextSize + 1;
}

// When the internal block size is LZ4F_max256KB, and the caller does
// 256K bulk reads, the input buffer this big can reduce intermediate
// copying in the guts of the LZ4F library.
#define ZBUFSIZE ((256 << 10) + 4)

struct lz4reader {
    struct fda *fda;
    LZ4F_decompressionContext_t dctx;
    size_t nextSize;
    bool eof;
    bool error;
    size_t zfill;
    size_t zpos;
    char zbuf[ZBUFSIZE];
};

int lz4reader_fdopen(struct lz4reader **zp, struct fda *fda, const char *err[2])
{
    LZ4F_decompressionContext_t dctx;
    size_t zet = LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION);
    if (LZ4F_isError(zet))
	return ERRLZ4("LZ4F_createCompressionContext", zet), -1;

    ssize_t nextSize = lz4reader_begin(fda, dctx, err);
    if (nextSize < 0)
	return LZ4F_freeDecompressionContext(dctx), -1;
    if (nextSize == 0)
	return LZ4F_freeDecompressionContext(dctx), 0;
    nextSize--;

    struct lz4reader *z = malloc(sizeof *z);
    if (!z)
	return LZ4F_freeDecompressionContext(dctx),
	       ERRNO("malloc"), -1;

    z->fda = fda;
    z->dctx = dctx;
    z->nextSize = nextSize;
    z->eof = z->error = false;
    z->zfill = z->zpos = 0;

    *zp = z;
    return 1;
}

int lz4reader_nextFrame(struct lz4reader *z, const char *err[2])
{
    // Reallocate the decompression context, unless EOF was reached
    // successfully - in this case, the context can be reused.
    if (!z->eof) {
#if LZ4_VERSION_NUMBER >= 10800
	LZ4F_resetDecompressionContext(z->dctx);
#else
	LZ4F_freeDecompressionContext(z->dctx), z->dctx = NULL;
	size_t zret = LZ4F_createDecompressionContext(&z->dctx, LZ4F_VERSION);
	if (LZ4F_isError(zret))
	    return ERRLZ4("LZ4F_createCompressionContext", zret), -1;
#endif
    }

    ssize_t nextSize = lz4reader_begin(z->fda, z->dctx, err);
    if (nextSize < 0)
	return -1;
    if (nextSize == 0)
	return 0;
    nextSize--;

    z->nextSize = nextSize;
    z->eof = z->error = false;
    z->zfill = z->zpos = 0;

    return 1;
}

void lz4reader_close(struct lz4reader *z)
{
    close(z->fda->fd);
    LZ4F_freeDecompressionContext(z->dctx);
    free(z);
}

ssize_t lz4reader_read(struct lz4reader *z, void *buf, size_t size, const char *err[2])
{
    if (z->eof)
	return 0;
    if (z->error)
	return -1;
    assert(size > 0);

    // How many bytes have been read into the output buffer.
    size_t fill = 0;

    do {
	// nextSize is the return value of LZ4F_decompress,
	// 0 indicates EOF.
	if (z->nextSize == 0) {
	    z->eof = true;
	    // There shouldn't be anything left in the buffer.
	    assert(z->zpos == z->zfill);
	    return fill;
	}

	// There must be something in zbuf.
	if (z->zpos == z->zfill) {
	    size_t n = z->nextSize;
	    if (n > ZBUFSIZE)
		n = ZBUFSIZE;
	    ssize_t ret = reada(z->fda, z->zbuf, n);
	    if (ret < 0)
		return ERRNO("read"), -(z->error = true);
	    if (ret < n)
		return ERRSTR("unexpected EOF"), -(z->error = true);
	    z->zfill = ret, z->zpos = 0;
	}

	// Feed zbuf to the decompressor.
	size_t raedenSize = z->zfill - z->zpos;
	size_t wrotenSize = size;
	z->nextSize = LZ4F_decompress(z->dctx, buf, &wrotenSize, z->zbuf + z->zpos, &raedenSize, NULL);
	if (LZ4F_isError(z->nextSize))
	    return ERRLZ4("LZ4F_decompress", z->nextSize), -(z->error = true);
	fill += wrotenSize, buf += wrotenSize, size -= wrotenSize;
	z->zpos += raedenSize;
    } while (size);

    return fill;
}

// ex:set ts=8 sts=4 sw=4 noet:
