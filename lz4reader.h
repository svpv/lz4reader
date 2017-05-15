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

#ifndef LZ4READER_H
#define LZ4READER_H

#include <stdint.h>
#include <sys/types.h> // ssize_t

#ifdef __cplusplus
extern "C" {
#else
#include <stdbool.h>
#endif

struct lz4reader;
// Returns 1 on success, 0 on EOF or valid empty input (no uncompressed data,
// the Reader handle is not created), -1 on error.  On success, the Reader
// handle is returned via zrp.  Bytes already read must be presented via
// peekBuf.  Information about an error is returned via the err[2] parameter:
// the first string is typically a function name, and the second is a string
// which describes the error.  Both strings normally come from the read-only
// data section.
int lz4reader_fdopen(struct lz4reader **zrp, int fd, const void *peekBuf, size_t peekSize, const char *err[2])
		     __attribute__((nonnull(1, 5)));

// Returns the number of bytes read, -1 on error.  If the number of bytes read
// is less than the number of bytes requested, this indicates EOF (subsequent
// reads will return 0).
ssize_t lz4reader_read(struct lz4reader *zr, void *buf, size_t size, const char *err[2]) __attribute__((nonnull));
void lz4reader_close(struct lz4reader *zr) __attribute__((nonnull));

// Returns the uncompressed size, or 0 if the uncompressed size is not
// available.  (The uncompressed size cannot be 0, due to fdopen semantics.)
uint64_t lz4reader_contentSize(struct lz4reader *zr) __attribute__((nonnull));
bool lz4reader_rewind(struct lz4reader *zr, const char *err[2]) __attribute__((nonnull));

#ifdef __cplusplus
}
#endif
#endif
