/* Just enough of zlib for minipro's database.c to compile.
 *
 * minipro only inflates the gzip'd FPGA bitstreams in algorithm.xml, which the
 * T56 and T76 need and the T48 does not. Every call fails, so a T56/T76
 * algorithm lookup reports an error instead of silently doing nothing.
 */
#pragma once

typedef unsigned char Bytef;
typedef unsigned int uInt;
typedef unsigned long uLong;

#define MAX_WBITS 15
#define Z_OK 0
#define Z_STREAM_END 1
#define Z_FINISH 4
#define Z_DATA_ERROR (-3)

typedef struct z_stream_s {
	Bytef *next_in;
	uInt avail_in;
	uLong total_in;
	Bytef *next_out;
	uInt avail_out;
	uLong total_out;
} z_stream;

static inline int inflateInit2(z_stream *s, int w) { (void)s; (void)w; return Z_DATA_ERROR; }
static inline int inflate(z_stream *s, int f) { (void)s; (void)f; return Z_DATA_ERROR; }
static inline int inflateEnd(z_stream *s) { (void)s; return Z_DATA_ERROR; }
