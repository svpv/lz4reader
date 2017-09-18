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
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
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
			       int64_t *contentSizep, const char *err[2])
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

    int64_t contentSize = -1;
    if (nextSize == 0)
	contentSize = 0;
    else {
	// The frame header has been decoded by now, so LZ4F_getFrameInfo()
	// will fetch us its internal copy of frameInfo.
	LZ4F_frameInfo_t frameInfo;
	pokenSize = 0;
	zret = LZ4F_getFrameInfo(dctx, &frameInfo, NULL, &pokenSize);
	assert(zret == nextSize);

	if (frameInfo.contentSize) {
	    if (frameInfo.contentSize > INT64_MAX)
		return ERRSTR("invalid contentSize"), -1;
	    contentSize = frameInfo.contentSize;
	}
	else {
	    // Check the size of the first block.
	    unsigned w;
	    memcpy(&w, buf + peekSize - 4, 4);
	    if (w == 0) {
		// Just requesting checksum?
		assert(nextSize == 4);
		contentSize = 0;
	    }
	}
    }

    *contentSizep = contentSize;
    return nextSize + 1;
}

// When the internal block size is LZ4F_max256KB, and the caller does
// 256K bulk reads, the input buffer this big can reduce intermediate
// copying in the guts of the LZ4F library.
#define ZBUFSIZE ((256 << 10) + 4)

struct lz4reader {
    struct fda *fda;
    LZ4F_decompressionContext_t dctx;
    bool eof, err;
    size_t nextSize;
    int64_t contentSize;
    size_t zfill;
    size_t zpos;
    char zbuf[ZBUFSIZE];
};

int lz4reader_open(struct lz4reader **zp, struct fda *fda, const char *err[2])
{
    LZ4F_decompressionContext_t dctx;
    size_t zet = LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION);
    if (LZ4F_isError(zet))
	return ERRLZ4("LZ4F_createCompressionContext", zet), -1;

    int64_t contentSize;
    ssize_t nextSize = lz4reader_begin(fda, dctx, &contentSize, err);
    if (nextSize < 0)
	return LZ4F_freeDecompressionContext(dctx), -1;
    if (nextSize == 0)
	return LZ4F_freeDecompressionContext(dctx), 0;
    nextSize--;

    struct lz4reader *z = malloc(sizeof *z);
    if (!z)
	return ERRNO("malloc"), LZ4F_freeDecompressionContext(dctx), -1;

    z->fda = fda;
    z->dctx = dctx;
    // It is tempting to check for contentSize == 0 here.  However, this
    // would leave out the case where the checksum still must be verified.
    z->eof = nextSize == 0;
    z->err = false;
    z->nextSize = nextSize;
    z->contentSize = contentSize;
    z->zfill = z->zpos = 0;

    *zp = z;
    return 1;
}

int lz4reader_reopen(struct lz4reader *z, struct fda *fda, const char *err[2])
{
    if (fda)
	z->fda = fda;

    z->contentSize = -1;

    // Reallocate the decompression context, unless EOF was reached
    // successfully - in this case, the context can be reused.
    if (z->eof)
	z->eof = false;
    else {
#if LZ4_VERSION_NUMBER >= 10800
	LZ4F_resetDecompressionContext(z->dctx);
#else
	LZ4F_freeDecompressionContext(z->dctx), z->dctx = NULL;
	size_t zret = LZ4F_createDecompressionContext(&z->dctx, LZ4F_VERSION);
	// It is unlikely that malloc after free with the same size will fail.
	// However, what if it does?  The object will be left in a somewhat
	// inconsistent state with z->dctx == NULL.  However, note that
	// LZ4F_freeDecompressionContext accepts NULL.  Moreover, since z->eof
	// has been cleared, the next call to reopen will execute exactly this
	// very same path.  And because z->err will be set, any use of z->dctx
	// will be precluded until it is successfully allocated.
	if (LZ4F_isError(zret))
	    return ERRLZ4("LZ4F_createCompressionContext", zret), -(z->err = true);
#endif
    }

    ssize_t nextSize = lz4reader_begin(z->fda, z->dctx, &z->contentSize, err);
    if (nextSize < 0)
	return -(z->err = true);
    if (nextSize == 0)
	return z->eof = true, +(z->err = false);
    nextSize--;

    z->eof = nextSize == 0;
    z->err = false;

    z->nextSize = nextSize;
    z->zfill = z->zpos = 0;

    return 1;
}

void lz4reader_free(struct lz4reader *z)
{
    if (!z)
	return;
    LZ4F_freeDecompressionContext(z->dctx);
    free(z);
}

ssize_t lz4reader_read(struct lz4reader *z, void *buf, size_t size, const char *err[2])
{
    if (z->err)
	return ERRSTR("pending error"), -1;
    if (z->eof)
	return 0;
    assert(size > 0);

    size_t total = 0;

    do {
	// There must be something in zbuf.
	if (z->zpos == z->zfill) {
	    size_t n = z->nextSize;
	    if (n > ZBUFSIZE)
		n = ZBUFSIZE;
	    ssize_t ret = reada(z->fda, z->zbuf, n);
	    if (ret < 0)
		return ERRNO("read"), -(z->err = true);
	    if (ret < n)
		return ERRSTR("unexpected EOF"), -(z->err = true);
	    z->zfill = ret, z->zpos = 0;
	}

	// Feed zbuf to the decompressor.
	size_t raedenSize = z->zfill - z->zpos;
	size_t wrotenSize = size;
	z->nextSize = LZ4F_decompress(z->dctx, buf, &wrotenSize, z->zbuf + z->zpos, &raedenSize, NULL);
	if (LZ4F_isError(z->nextSize))
	    return ERRLZ4("LZ4F_decompress", z->nextSize), -(z->err = true);
	total += wrotenSize, buf += wrotenSize, size -= wrotenSize;
	z->zpos += raedenSize;

	if (z->nextSize == 0) {
	    z->eof = true;
	    // There shouldn't be anything left in the buffer.
	    assert(z->zpos == z->zfill);
	    break;
	}
    } while (size);

    return total;
}

int64_t lz4reader_contentSize(struct lz4reader *z)
{
    return z->contentSize;
}

// ex:set ts=8 sts=4 sw=4 noet:
