#include <lz4.h>
#include <lz4frame.h>
/* nct__open_mmap */
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
/* end nct__open_mmap */

void* nct__lz4_decompress(const void* compressed, size_t size_compressed, size_t* size_uncompressed_out) {
    void* uncompressed = NULL;
    LZ4F_frameInfo_t frameinfo;
    size_t used = size_compressed,
	   size_uncompressed;
    int result;
    LZ4F_dctx* dctx;

    result = LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION);
    if (LZ4F_isError(result)) {
	LZ4F_freeDecompressionContext(dctx);
	nct_puterror("LZ4F_createDecompressionContext failed: %s\n", LZ4F_getErrorName(result));
	nct_return_error(NULL);}

    result = LZ4F_getFrameInfo(dctx, &frameinfo, compressed, &used);
    if (LZ4F_isError(result)) {
	nct_puterror("LZ4F_getFrameInfo failed: %s\n", LZ4F_getErrorName(result));
	nct_return_error(NULL);}

    if (!(size_uncompressed = frameinfo.contentSize)) {
	nct_puterror("Unknown uncompressed size. File must be compressed with 'lz4 --content-size'\n");
	nct_return_error(NULL);}

    if (!(uncompressed = malloc(size_uncompressed))) {
	nct_puterror("Malloc failed (%zu)\n", size_uncompressed);
	nct_return_error(NULL);}

    LZ4F_decompressOptions_t valinnat = {.skipChecksums = 1};
    size_t howmuch_thistime_in = size_compressed - used;
    size_t howmuch_thistime_out = size_uncompressed;

    size_t sizeout = 0;
    do {
	while ((result = LZ4F_decompress(dctx,
			uncompressed+sizeout, &howmuch_thistime_out,
			compressed+used, &howmuch_thistime_in, &valinnat)))
	{
	    if (LZ4F_isError(result))
		goto decompress_error;
	    sizeout += howmuch_thistime_out;
	    howmuch_thistime_out = size_uncompressed - sizeout;
	    used += howmuch_thistime_in;
	    howmuch_thistime_in = result; // Result is a hint about the optimal size to pass next.
	}
	sizeout += howmuch_thistime_out;
	howmuch_thistime_out = size_uncompressed - sizeout;
	used += howmuch_thistime_in;
	howmuch_thistime_in = size_compressed - used;
    } while (sizeout < size_uncompressed);

    if (size_uncompressed_out)
	*size_uncompressed_out = size_uncompressed;
    LZ4F_freeDecompressionContext(dctx);
    return uncompressed;

decompress_error: __attribute__((cold));
    nct_puterror("LZ4F_decompress failed: %s\n", LZ4F_getErrorName(result));
    LZ4F_freeDecompressionContext(dctx);
    free(uncompressed);
    nct_return_error(NULL);
}

void* nct__lz4_getcontent(const char* filename, size_t* size_uncompressed) {
    int fd;
    struct stat st;
    if ((fd = open(filename, O_RDONLY)) < 0) {
	nct_puterror("open %s: %s\n", filename, strerror(errno));
	nct_return_error(NULL);}
    if (fstat(fd, &st)) {
	nct_puterror("fstat %s, fd = %i: %s\n", filename, fd, strerror(errno));
	nct_return_error(NULL);}
    void* compressed = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (!compressed) {
	nct_puterror("mmap %s, fd = %i: %s\n", filename, fd, strerror(errno));
	nct_return_error(NULL);}

    void* result = nct__lz4_decompress(compressed, st.st_size, size_uncompressed);
    munmap(compressed, st.st_size);
    return result;
}

nct_set* nct_read_ncf_lz4_gd(nct_set* dest, const char* filename, int flags) {
    struct nct_fileinfo_mem_t arg = {
	.fileinfo.name = filename,
	.getcontent = nct__lz4_getcontent,
    };
    return nct_read_ncf_gd(dest, &arg, flags|nct_rmem);
}

nct_set* nct_read_ncf_lz4(const char* filename, int flags) {
    return nct_read_ncf_lz4_gd(calloc(1, sizeof(nct_set)), filename, flags);
}
