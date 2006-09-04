/*
 * Copyright © 2000 Keith Packard
 * Copyright © 2005 Patrick Lam
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of Keith Packard not be used in
 * advertising or publicity pertaining to distribution of the software without
 * specific, written prior permission.  Keith Packard makes no
 * representations about the suitability of this software for any purpose.  It
 * is provided "as is" without express or implied warranty.
 *
 * KEITH PACKARD DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL KEITH PACKARD BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include "fcint.h"
#include "../fc-arch/fcarch.h"
#include <stdio.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <sys/types.h>
#if defined(HAVE_MMAP) || defined(__CYGWIN__)
#  include <unistd.h>
#  include <sys/mman.h>
#elif defined(_WIN32)
#  include <windows.h>
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif

struct MD5Context {
        FcChar32 buf[4];
        FcChar32 bits[2];
        unsigned char in[64];
};

static void MD5Init(struct MD5Context *ctx);
static void MD5Update(struct MD5Context *ctx, unsigned char *buf, unsigned len);
static void MD5Final(unsigned char digest[16], struct MD5Context *ctx);
static void MD5Transform(FcChar32 buf[4], FcChar32 in[16]);

#define CACHEBASE_LEN (1 + 32 + 1 + sizeof (FC_ARCHITECTURE) + sizeof (FC_CACHE_SUFFIX))

static const char bin2hex[] = { '0', '1', '2', '3',
				'4', '5', '6', '7',
				'8', '9', 'a', 'b',
				'c', 'd', 'e', 'f' };

static FcChar8 *
FcDirCacheBasename (const FcChar8 * dir, FcChar8 cache_base[CACHEBASE_LEN])
{
    unsigned char 	hash[16];
    FcChar8		*hex_hash;
    int			cnt;
    struct MD5Context 	ctx;

    MD5Init (&ctx);
    MD5Update (&ctx, (unsigned char *)dir, strlen ((char *) dir));

    MD5Final (hash, &ctx);

    cache_base[0] = '/';
    hex_hash = cache_base + 1;
    for (cnt = 0; cnt < 16; ++cnt)
    {
	hex_hash[2*cnt  ] = bin2hex[hash[cnt] >> 4];
	hex_hash[2*cnt+1] = bin2hex[hash[cnt] & 0xf];
    }
    hex_hash[2*cnt] = 0;
    strcat ((char *) cache_base, "-" FC_ARCHITECTURE FC_CACHE_SUFFIX);

    return cache_base;
}

FcBool
FcDirCacheUnlink (const FcChar8 *dir, FcConfig *config)
{
    FcChar8	*cache_hashed = NULL;
    FcChar8	cache_base[CACHEBASE_LEN];
    FcStrList	*list;
    FcChar8	*cache_dir;

    FcDirCacheBasename (dir, cache_base);

    list = FcStrListCreate (config->cacheDirs);
    if (!list)
        return FcFalse;
	
    while ((cache_dir = FcStrListNext (list)))
    {
        cache_hashed = FcStrPlus (cache_dir, cache_base);
        if (!cache_hashed)
	    break;
	(void) unlink ((char *) cache_hashed);
    }
    FcStrListDone (list);
    /* return FcFalse if something went wrong */
    if (cache_dir)
	return FcFalse;
    return FcTrue;
}

static int
FcDirCacheOpenFile (const FcChar8 *cache_file, struct stat *file_stat)
{
    int	fd;

    fd = open((char *) cache_file, O_RDONLY | O_BINARY);
    if (fd < 0)
	return fd;
    if (fstat (fd, file_stat) < 0)
    {
	close (fd);
	return -1;
    }
    return fd;
}

/* 
 * Look for a cache file for the specified dir. Attempt
 * to use each one we find, stopping when the callback
 * indicates success
 */
static FcBool
FcDirCacheProcess (FcConfig *config, const FcChar8 *dir, 
		   FcBool (*callback) (int fd, off_t size, void *closure),
		   void *closure, FcChar8 **cache_file_ret)
{
    int		fd = -1;
    FcChar8	cache_base[CACHEBASE_LEN];
    FcStrList	*list;
    FcChar8	*cache_dir;
    struct stat file_stat, dir_stat;
    FcBool	ret = FcFalse;

    if (stat ((char *) dir, &dir_stat) < 0)
        return FcFalse;

    FcDirCacheBasename (dir, cache_base);

    list = FcStrListCreate (config->cacheDirs);
    if (!list)
        return FcFalse;
	
    while ((cache_dir = FcStrListNext (list)))
    {
        FcChar8	*cache_hashed = FcStrPlus (cache_dir, cache_base);
        if (!cache_hashed)
	    break;
        fd = FcDirCacheOpenFile (cache_hashed, &file_stat);
        if (fd >= 0) {
	    if (dir_stat.st_mtime <= file_stat.st_mtime)
	    {
		ret = (*callback) (fd, file_stat.st_size, closure);
		if (ret)
		{
		    if (cache_file_ret)
			*cache_file_ret = cache_hashed;
		    else
			FcStrFree (cache_hashed);
		    close (fd);
		    break;
		}
	    }
	    close (fd);
	}
    	FcStrFree (cache_hashed);
    }
    FcStrListDone (list);
    
    return ret;
}

#define FC_CACHE_MIN_MMAP   1024

/*
 * Map a cache file into memory
 */
static FcCache *
FcDirCacheMapFd (int fd, off_t size)
{
    FcCache	*cache = NULL;
    FcBool	allocated = FcFalse;

    if (size < sizeof (FcCache))
	return NULL;
    /*
     * For small cache files, just read them into memory
     */
    if (size >= FC_CACHE_MIN_MMAP)
    {
#if defined(HAVE_MMAP) || defined(__CYGWIN__)
	cache = mmap (0, size, PROT_READ, MAP_SHARED, fd, 0);
#elif defined(_WIN32)
	{
	    HANDLE hFileMap;

	    cache = NULL;
	    hFileMap = CreateFileMapping((HANDLE) _get_osfhandle(fd), NULL,
					 PAGE_READONLY, 0, 0, NULL);
	    if (hFileMap != NULL)
	    {
		cache = MapViewOfFile (hFileMap, FILE_MAP_READ, 0, 0, size);
		CloseHandle (hFileMap);
	    }
	}
#endif
    }
    if (!cache)
    {
	cache = malloc (size);
	if (!cache)
	    return NULL;

	if (read (fd, cache, size) != size)
	{
	    free (cache);
	    return NULL;
	}
	allocated = FcTrue;
    } 
    if (cache->magic != FC_CACHE_MAGIC_MMAP || 
	cache->version < FC_CACHE_CONTENT_VERSION ||
	cache->size != size)
    {
	if (allocated)
	    free (cache);
	else
	{
#if defined(HAVE_MMAP) || defined(__CYGWIN__)
	    munmap (cache, size);
#elif defined(_WIN32)
	    UnmapViewOfFile (cache);
#endif
	}
	return NULL;
    }

    /* Mark allocated caches so they're freed rather than unmapped */
    if (allocated)
	cache->magic = FC_CACHE_MAGIC_ALLOC;
	
    return cache;
}

void
FcDirCacheUnload (FcCache *cache)
{
    switch (cache->magic) {
    case FC_CACHE_MAGIC_ALLOC:
	free (cache);
	break;
    case FC_CACHE_MAGIC_MMAP:
#if defined(HAVE_MMAP) || defined(__CYGWIN__)
	munmap (cache, cache->size);
#elif defined(_WIN32)
	UnmapViewOfFile (cache);
#endif
	break;
    }
}

static FcBool
FcDirCacheMapHelper (int fd, off_t size, void *closure)
{
    FcCache *cache = FcDirCacheMapFd (fd, size);

    if (!cache)
	return FcFalse;
    *((FcCache **) closure) = cache;
    return FcTrue;
}

FcCache *
FcDirCacheLoad (const FcChar8 *dir, FcConfig *config, FcChar8 **cache_file)
{
    FcCache *cache = NULL;

    if (!FcDirCacheProcess (config, dir,
			    FcDirCacheMapHelper,
			    &cache, cache_file))
	return NULL;
    return cache;
}

FcCache *
FcDirCacheLoadFile (const FcChar8 *cache_file, struct stat *file_stat)
{
    int	fd;
    FcCache *cache;

    fd = FcDirCacheOpenFile (cache_file, file_stat);
    if (fd < 0)
	return NULL;
    cache = FcDirCacheMapFd (fd, file_stat->st_size);
    close (fd);
    return cache;
}

/*
 * Validate a cache file by reading the header and checking
 * the magic number and the size field
 */
static FcBool
FcDirCacheValidateHelper (int fd, off_t size, void *closure)
{
    FcBool  ret = FcTrue;
    FcCache	c;
    struct stat	file_stat;
    
    if (read (fd, &c, sizeof (FcCache)) != sizeof (FcCache))
	ret = FcFalse;
    else if (c.magic != FC_CACHE_MAGIC_MMAP)
	ret = FcFalse;
    else if (c.version < FC_CACHE_CONTENT_VERSION)
	ret = FcFalse;
    else if (fstat (fd, &file_stat) < 0)
	ret = FcFalse;
    else if (file_stat.st_size != c.size)
	ret = FcFalse;
    return ret;
}

static FcBool
FcDirCacheValidConfig (const FcChar8 *dir, FcConfig *config)
{
    return FcDirCacheProcess (config, dir, 
			      FcDirCacheValidateHelper,
			      NULL, NULL);
}

FcBool
FcDirCacheValid (const FcChar8 *dir)
{
    FcConfig	*config;
    
    config = FcConfigGetCurrent ();
    if (!config)
        return FcFalse;

    return FcDirCacheValidConfig (dir, config);
}

/*
 * Build a cache structure from the given contents
 */
FcCache *
FcDirCacheBuild (FcFontSet *set, const FcChar8 *dir, FcStrSet *dirs)
{
    FcSerialize	*serialize = FcSerializeCreate ();
    FcCache *cache;
    int i;
    intptr_t	cache_offset;
    intptr_t	dirs_offset;
    FcChar8	*dir_serialize;
    intptr_t	*dirs_serialize;
    FcFontSet	*set_serialize;
    
    if (!serialize)
	return NULL;
    /*
     * Space for cache structure
     */
    cache_offset = FcSerializeReserve (serialize, sizeof (FcCache));
    /*
     * Directory name
     */
    if (!FcStrSerializeAlloc (serialize, dir))
	goto bail1;
    /*
     * Subdirs
     */
    dirs_offset = FcSerializeAlloc (serialize, dirs, dirs->num * sizeof (FcChar8 *));
    for (i = 0; i < dirs->num; i++)
	if (!FcStrSerializeAlloc (serialize, dirs->strs[i]))
	    goto bail1;

    /*
     * Patterns
     */
    if (!FcFontSetSerializeAlloc (serialize, set))
	goto bail1;
    
    /* Serialize layout complete. Now allocate space and fill it */
    cache = malloc (serialize->size);
    if (!cache)
	goto bail1;
    /* shut up valgrind */
    memset (cache, 0, serialize->size);

    serialize->linear = cache;

    cache->magic = FC_CACHE_MAGIC_ALLOC;
    cache->version = FC_CACHE_CONTENT_VERSION;
    cache->size = serialize->size;

    /*
     * Serialize directory name
     */
    dir_serialize = FcStrSerialize (serialize, dir);
    if (!dir_serialize)
	goto bail2;
    cache->dir = FcPtrToOffset (cache, dir_serialize);
    
    /*
     * Serialize sub dirs
     */
    dirs_serialize = FcSerializePtr (serialize, dirs);
    if (!dirs_serialize)
	goto bail2;
    cache->dirs = FcPtrToOffset (cache, dirs_serialize);
    cache->dirs_count = dirs->num;
    for (i = 0; i < dirs->num; i++) 
    {
	FcChar8	*d_serialize = FcStrSerialize (serialize, dirs->strs[i]);
	if (!d_serialize)
	    goto bail2;
	dirs_serialize[i] = FcPtrToOffset (dirs_serialize, d_serialize);
    }
    
    /*
     * Serialize font set
     */
    set_serialize = FcFontSetSerialize (serialize, set);
    if (!set_serialize)
	goto bail2;
    cache->set = FcPtrToOffset (cache, set_serialize);

    FcSerializeDestroy (serialize);
    
    return cache;

bail2:
    free (cache);
bail1:
    FcSerializeDestroy (serialize);
    return NULL;
}

static FcBool
FcMakeDirectory (const FcChar8 *dir)
{
    FcChar8 *parent;
    FcBool  ret;
    
    if (strlen ((char *) dir) == 0)
	return FcFalse;
    
    parent = FcStrDirname (dir);
    if (!parent)
	return FcFalse;
    if (access ((char *) parent, W_OK|X_OK) == 0)
	ret = mkdir ((char *) dir, 0777) == 0;
    else if (access ((char *) parent, F_OK) == -1)
	ret = FcMakeDirectory (parent) && (mkdir ((char *) dir, 0777) == 0);
    else
	ret = FcFalse;
    FcStrFree (parent);
    return ret;
}

/* write serialized state to the cache file */
FcBool
FcDirCacheWrite (FcCache *cache, FcConfig *config)
{
    FcChar8	    *dir = FcCacheDir (cache);
    FcChar8	    cache_base[CACHEBASE_LEN];
    FcChar8	    *cache_hashed;
    int 	    fd;
    FcAtomic 	    *atomic;
    FcStrList	    *list;
    FcChar8	    *cache_dir = NULL;
    FcChar8	    *test_dir;
    int		    magic;
    int		    written;

    /*
     * Write it to the first directory in the list which is writable
     */
    
    list = FcStrListCreate (config->cacheDirs);
    if (!list)
	return FcFalse;
    while ((test_dir = FcStrListNext (list))) {
	if (access ((char *) test_dir, W_OK|X_OK) == 0)
	{
	    cache_dir = test_dir;
	    break;
	}
	else
	{
	    /*
	     * If the directory doesn't exist, try to create it
	     */
	    if (access ((char *) test_dir, F_OK) == -1) {
		if (FcMakeDirectory (test_dir))
		{
		    cache_dir = test_dir;
		    break;
		}
	    }
	}
    }
    FcStrListDone (list);
    if (!cache_dir)
	return FcFalse;

    FcDirCacheBasename (dir, cache_base);
    cache_hashed = FcStrPlus (cache_dir, cache_base);
    if (!cache_hashed)
        return FcFalse;

    if (FcDebug () & FC_DBG_CACHE)
        printf ("FcDirCacheWriteDir dir \"%s\" file \"%s\"\n",
		dir, cache_hashed);

    atomic = FcAtomicCreate ((FcChar8 *)cache_hashed);
    if (!atomic)
	goto bail1;

    if (!FcAtomicLock (atomic))
	goto bail3;

    fd = open((char *)FcAtomicNewFile (atomic), O_RDWR | O_CREAT | O_BINARY, 0666);
    if (fd == -1)
	goto bail4;
    
    /* Temporarily switch magic to MMAP while writing to file */
    magic = cache->magic;
    if (magic != FC_CACHE_MAGIC_MMAP)
	cache->magic = FC_CACHE_MAGIC_MMAP;
    
    /*
     * Write cache contents to file
     */
    written = write (fd, cache, cache->size);
    
    /* Switch magic back */
    if (magic != FC_CACHE_MAGIC_MMAP)
	cache->magic = magic;
    
    if (written != cache->size)
    {
	perror ("write cache");
	goto bail5;
    }

    close(fd);
    if (!FcAtomicReplaceOrig(atomic))
        goto bail4;
    FcStrFree (cache_hashed);
    FcAtomicUnlock (atomic);
    FcAtomicDestroy (atomic);
    return FcTrue;

 bail5:
    close (fd);
 bail4:
    FcAtomicUnlock (atomic);
 bail3:
    FcAtomicDestroy (atomic);
 bail1:
    FcStrFree (cache_hashed);
    return FcFalse;
}

/*
 * Hokey little macro trick to permit the definitions of C functions
 * with the same name as CPP macros
 */
#define args(x...)	    (x)

const FcChar8 *
FcCacheDir args(const FcCache *c)
{
    return FcCacheDir (c);
}

FcFontSet *
FcCacheCopySet args(const FcCache *c)
{
    FcFontSet	*old = FcCacheSet (c);
    FcFontSet	*new = FcFontSetCreate ();
    int		i;
    
    if (!new)
	return NULL;
    for (i = 0; i < old->nfont; i++)
	if (!FcFontSetAdd (new, FcFontSetFont (old, i)))
	{
	    FcFontSetDestroy (new);
	    return NULL;
	}
    return new;
}

const FcChar8 *
FcCacheSubdir args(const FcCache *c, int i)
{
    return FcCacheSubdir (c, i);
}

int
FcCacheNumSubdir args(const FcCache *c)
{
    return c->dirs_count;
}

int
FcCacheNumFont args(const FcCache *c)
{
    return FcCacheSet(c)->nfont;
}

/*
 * This code implements the MD5 message-digest algorithm.
 * The algorithm is due to Ron Rivest.	This code was
 * written by Colin Plumb in 1993, no copyright is claimed.
 * This code is in the public domain; do with it what you wish.
 *
 * Equivalent code is available from RSA Data Security, Inc.
 * This code has been tested against that, and is equivalent,
 * except that you don't need to include two pages of legalese
 * with every copy.
 *
 * To compute the message digest of a chunk of bytes, declare an
 * MD5Context structure, pass it to MD5Init, call MD5Update as
 * needed on buffers full of bytes, and then call MD5Final, which
 * will fill a supplied 16-byte array with the digest.
 */

#ifndef HIGHFIRST
#define byteReverse(buf, len)	/* Nothing */
#else
/*
 * Note: this code is harmless on little-endian machines.
 */
void byteReverse(unsigned char *buf, unsigned longs)
{
    FcChar32 t;
    do {
	t = (FcChar32) ((unsigned) buf[3] << 8 | buf[2]) << 16 |
	    ((unsigned) buf[1] << 8 | buf[0]);
	*(FcChar32 *) buf = t;
	buf += 4;
    } while (--longs);
}
#endif

/*
 * Start MD5 accumulation.  Set bit count to 0 and buffer to mysterious
 * initialization constants.
 */
static void MD5Init(struct MD5Context *ctx)
{
    ctx->buf[0] = 0x67452301;
    ctx->buf[1] = 0xefcdab89;
    ctx->buf[2] = 0x98badcfe;
    ctx->buf[3] = 0x10325476;

    ctx->bits[0] = 0;
    ctx->bits[1] = 0;
}

/*
 * Update context to reflect the concatenation of another buffer full
 * of bytes.
 */
static void MD5Update(struct MD5Context *ctx, unsigned char *buf, unsigned len)
{
    FcChar32 t;

    /* Update bitcount */

    t = ctx->bits[0];
    if ((ctx->bits[0] = t + ((FcChar32) len << 3)) < t)
	ctx->bits[1]++; 	/* Carry from low to high */
    ctx->bits[1] += len >> 29;

    t = (t >> 3) & 0x3f;	/* Bytes already in shsInfo->data */

    /* Handle any leading odd-sized chunks */

    if (t) {
	unsigned char *p = (unsigned char *) ctx->in + t;

	t = 64 - t;
	if (len < t) {
	    memcpy(p, buf, len);
	    return;
	}
	memcpy(p, buf, t);
	byteReverse(ctx->in, 16);
	MD5Transform(ctx->buf, (FcChar32 *) ctx->in);
	buf += t;
	len -= t;
    }
    /* Process data in 64-byte chunks */

    while (len >= 64) {
	memcpy(ctx->in, buf, 64);
	byteReverse(ctx->in, 16);
	MD5Transform(ctx->buf, (FcChar32 *) ctx->in);
	buf += 64;
	len -= 64;
    }

    /* Handle any remaining bytes of data. */

    memcpy(ctx->in, buf, len);
}

/*
 * Final wrapup - pad to 64-byte boundary with the bit pattern 
 * 1 0* (64-bit count of bits processed, MSB-first)
 */
static void MD5Final(unsigned char digest[16], struct MD5Context *ctx)
{
    unsigned count;
    unsigned char *p;

    /* Compute number of bytes mod 64 */
    count = (ctx->bits[0] >> 3) & 0x3F;

    /* Set the first char of padding to 0x80.  This is safe since there is
       always at least one byte free */
    p = ctx->in + count;
    *p++ = 0x80;

    /* Bytes of padding needed to make 64 bytes */
    count = 64 - 1 - count;

    /* Pad out to 56 mod 64 */
    if (count < 8) {
	/* Two lots of padding:  Pad the first block to 64 bytes */
	memset(p, 0, count);
	byteReverse(ctx->in, 16);
	MD5Transform(ctx->buf, (FcChar32 *) ctx->in);

	/* Now fill the next block with 56 bytes */
	memset(ctx->in, 0, 56);
    } else {
	/* Pad block to 56 bytes */
	memset(p, 0, count - 8);
    }
    byteReverse(ctx->in, 14);

    /* Append length in bits and transform */
    ((FcChar32 *) ctx->in)[14] = ctx->bits[0];
    ((FcChar32 *) ctx->in)[15] = ctx->bits[1];

    MD5Transform(ctx->buf, (FcChar32 *) ctx->in);
    byteReverse((unsigned char *) ctx->buf, 4);
    memcpy(digest, ctx->buf, 16);
    memset(ctx, 0, sizeof(ctx));        /* In case it's sensitive */
}


/* The four core functions - F1 is optimized somewhat */

/* #define F1(x, y, z) (x & y | ~x & z) */
#define F1(x, y, z) (z ^ (x & (y ^ z)))
#define F2(x, y, z) F1(z, x, y)
#define F3(x, y, z) (x ^ y ^ z)
#define F4(x, y, z) (y ^ (x | ~z))

/* This is the central step in the MD5 algorithm. */
#define MD5STEP(f, w, x, y, z, data, s) \
	( w += f(x, y, z) + data,  w = w<<s | w>>(32-s),  w += x )

/*
 * The core of the MD5 algorithm, this alters an existing MD5 hash to
 * reflect the addition of 16 longwords of new data.  MD5Update blocks
 * the data and converts bytes into longwords for this routine.
 */
static void MD5Transform(FcChar32 buf[4], FcChar32 in[16])
{
    register FcChar32 a, b, c, d;

    a = buf[0];
    b = buf[1];
    c = buf[2];
    d = buf[3];

    MD5STEP(F1, a, b, c, d, in[0] + 0xd76aa478, 7);
    MD5STEP(F1, d, a, b, c, in[1] + 0xe8c7b756, 12);
    MD5STEP(F1, c, d, a, b, in[2] + 0x242070db, 17);
    MD5STEP(F1, b, c, d, a, in[3] + 0xc1bdceee, 22);
    MD5STEP(F1, a, b, c, d, in[4] + 0xf57c0faf, 7);
    MD5STEP(F1, d, a, b, c, in[5] + 0x4787c62a, 12);
    MD5STEP(F1, c, d, a, b, in[6] + 0xa8304613, 17);
    MD5STEP(F1, b, c, d, a, in[7] + 0xfd469501, 22);
    MD5STEP(F1, a, b, c, d, in[8] + 0x698098d8, 7);
    MD5STEP(F1, d, a, b, c, in[9] + 0x8b44f7af, 12);
    MD5STEP(F1, c, d, a, b, in[10] + 0xffff5bb1, 17);
    MD5STEP(F1, b, c, d, a, in[11] + 0x895cd7be, 22);
    MD5STEP(F1, a, b, c, d, in[12] + 0x6b901122, 7);
    MD5STEP(F1, d, a, b, c, in[13] + 0xfd987193, 12);
    MD5STEP(F1, c, d, a, b, in[14] + 0xa679438e, 17);
    MD5STEP(F1, b, c, d, a, in[15] + 0x49b40821, 22);

    MD5STEP(F2, a, b, c, d, in[1] + 0xf61e2562, 5);
    MD5STEP(F2, d, a, b, c, in[6] + 0xc040b340, 9);
    MD5STEP(F2, c, d, a, b, in[11] + 0x265e5a51, 14);
    MD5STEP(F2, b, c, d, a, in[0] + 0xe9b6c7aa, 20);
    MD5STEP(F2, a, b, c, d, in[5] + 0xd62f105d, 5);
    MD5STEP(F2, d, a, b, c, in[10] + 0x02441453, 9);
    MD5STEP(F2, c, d, a, b, in[15] + 0xd8a1e681, 14);
    MD5STEP(F2, b, c, d, a, in[4] + 0xe7d3fbc8, 20);
    MD5STEP(F2, a, b, c, d, in[9] + 0x21e1cde6, 5);
    MD5STEP(F2, d, a, b, c, in[14] + 0xc33707d6, 9);
    MD5STEP(F2, c, d, a, b, in[3] + 0xf4d50d87, 14);
    MD5STEP(F2, b, c, d, a, in[8] + 0x455a14ed, 20);
    MD5STEP(F2, a, b, c, d, in[13] + 0xa9e3e905, 5);
    MD5STEP(F2, d, a, b, c, in[2] + 0xfcefa3f8, 9);
    MD5STEP(F2, c, d, a, b, in[7] + 0x676f02d9, 14);
    MD5STEP(F2, b, c, d, a, in[12] + 0x8d2a4c8a, 20);

    MD5STEP(F3, a, b, c, d, in[5] + 0xfffa3942, 4);
    MD5STEP(F3, d, a, b, c, in[8] + 0x8771f681, 11);
    MD5STEP(F3, c, d, a, b, in[11] + 0x6d9d6122, 16);
    MD5STEP(F3, b, c, d, a, in[14] + 0xfde5380c, 23);
    MD5STEP(F3, a, b, c, d, in[1] + 0xa4beea44, 4);
    MD5STEP(F3, d, a, b, c, in[4] + 0x4bdecfa9, 11);
    MD5STEP(F3, c, d, a, b, in[7] + 0xf6bb4b60, 16);
    MD5STEP(F3, b, c, d, a, in[10] + 0xbebfbc70, 23);
    MD5STEP(F3, a, b, c, d, in[13] + 0x289b7ec6, 4);
    MD5STEP(F3, d, a, b, c, in[0] + 0xeaa127fa, 11);
    MD5STEP(F3, c, d, a, b, in[3] + 0xd4ef3085, 16);
    MD5STEP(F3, b, c, d, a, in[6] + 0x04881d05, 23);
    MD5STEP(F3, a, b, c, d, in[9] + 0xd9d4d039, 4);
    MD5STEP(F3, d, a, b, c, in[12] + 0xe6db99e5, 11);
    MD5STEP(F3, c, d, a, b, in[15] + 0x1fa27cf8, 16);
    MD5STEP(F3, b, c, d, a, in[2] + 0xc4ac5665, 23);

    MD5STEP(F4, a, b, c, d, in[0] + 0xf4292244, 6);
    MD5STEP(F4, d, a, b, c, in[7] + 0x432aff97, 10);
    MD5STEP(F4, c, d, a, b, in[14] + 0xab9423a7, 15);
    MD5STEP(F4, b, c, d, a, in[5] + 0xfc93a039, 21);
    MD5STEP(F4, a, b, c, d, in[12] + 0x655b59c3, 6);
    MD5STEP(F4, d, a, b, c, in[3] + 0x8f0ccc92, 10);
    MD5STEP(F4, c, d, a, b, in[10] + 0xffeff47d, 15);
    MD5STEP(F4, b, c, d, a, in[1] + 0x85845dd1, 21);
    MD5STEP(F4, a, b, c, d, in[8] + 0x6fa87e4f, 6);
    MD5STEP(F4, d, a, b, c, in[15] + 0xfe2ce6e0, 10);
    MD5STEP(F4, c, d, a, b, in[6] + 0xa3014314, 15);
    MD5STEP(F4, b, c, d, a, in[13] + 0x4e0811a1, 21);
    MD5STEP(F4, a, b, c, d, in[4] + 0xf7537e82, 6);
    MD5STEP(F4, d, a, b, c, in[11] + 0xbd3af235, 10);
    MD5STEP(F4, c, d, a, b, in[2] + 0x2ad7d2bb, 15);
    MD5STEP(F4, b, c, d, a, in[9] + 0xeb86d391, 21);

    buf[0] += a;
    buf[1] += b;
    buf[2] += c;
    buf[3] += d;
}
