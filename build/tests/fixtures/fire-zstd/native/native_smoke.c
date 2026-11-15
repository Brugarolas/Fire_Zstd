/* Runtime-load smoke test for the single-library Napalm/Zstd ABI. */
#include "fire_zstd.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

typedef int32_t (*abi_version_fn)(void);
typedef int32_t (*zstd_version_fn)(void);
typedef const char *(*last_error_fn)(void);
typedef int32_t (*compress_file_fn)(const char *, const char *, int32_t);
typedef int32_t (*decompress_file_fn)(const char *, const char *, uint64_t);

static void *library_open(const char *path) {
#if defined(_WIN32)
    return (void *)LoadLibraryA(path);
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

static void library_close(void *handle) {
#if defined(_WIN32)
    (void)FreeLibrary((HMODULE)handle);
#else
    (void)dlclose(handle);
#endif
}

static void *library_symbol(void *handle, const char *name) {
#if defined(_WIN32)
    return (void *)GetProcAddress((HMODULE)handle, name);
#else
    return dlsym(handle, name);
#endif
}

static int fail(const char *message) {
    fprintf(stderr, "FireZstd native smoke: %s\n", message);
    return EXIT_FAILURE;
}

/* Copy a symbol pointer without nonportable object-to-function-pointer casts. */
#define LOAD_SYMBOL(target, handle, symbol_name) do {                       \
    void *sym_ = library_symbol((handle), (symbol_name));                   \
    if (!sym_) { library_close(handle); return fail("missing symbol: " symbol_name); } \
    memcpy(&(target), &sym_, sizeof(target));                               \
} while (0)

int main(int argc, char **argv) {
    if (argc != 2) return fail("expected exactly one argument: path to libzstd");
    void *handle = library_open(argv[1]);
    if (!handle) return fail("cannot load the shared library");

    abi_version_fn abi_version = NULL;
    zstd_version_fn version = NULL;
    last_error_fn last_error = NULL;
    compress_file_fn compress_file = NULL;
    decompress_file_fn decompress_file = NULL;
    LOAD_SYMBOL(abi_version, handle, "fire_zstd_abi_version");
    LOAD_SYMBOL(version, handle, "fire_zstd_version_number");
    LOAD_SYMBOL(last_error, handle, "fire_zstd_last_error");
    LOAD_SYMBOL(compress_file, handle, "fire_zstd_compress_file");
    LOAD_SYMBOL(decompress_file, handle, "fire_zstd_decompress_file");

    if (abi_version() != 1 || version() < 10000) {
        library_close(handle);
        return fail("unexpected ABI or Zstd version");
    }

    const char *input = "fire_zstd_test_input.tmp";
    const char *compressed = "fire_zstd_test_compressed.tmp";
    const char *output = "fire_zstd_test_output.tmp";
    FILE *stream = fopen(input, "wb");
    if (!stream) { library_close(handle); return fail("cannot create source file"); }
    for (int i = 0; i < 262144; ++i) {
        const unsigned char byte = (unsigned char)((i * 13 + (i >> 5)) & 255);
        if (fwrite(&byte, 1, 1, stream) != 1) {
            fclose(stream);
            library_close(handle);
            return fail("cannot populate source file");
        }
    }
    if (fclose(stream) != 0) { library_close(handle); return fail("cannot close source file"); }

    if (compress_file(input, compressed, 3) != FIRE_ZSTD_OK) {
        fprintf(stderr, "Compression: %s\n", last_error());
        library_close(handle);
        return EXIT_FAILURE;
    }
    if (decompress_file(compressed, output, 262144) != FIRE_ZSTD_OK) {
        fprintf(stderr, "Decompression: %s\n", last_error());
        library_close(handle);
        return EXIT_FAILURE;
    }
    FILE *left = fopen(input, "rb");
    FILE *right = fopen(output, "rb");
    if (!left || !right) {
        if (left) fclose(left);
        if (right) fclose(right);
        library_close(handle);
        return fail("cannot open files for comparison");
    }
    int matched = 1;
    for (;;) {
        unsigned char a[4096];
        unsigned char b[4096];
        const size_t alen = fread(a, 1, sizeof a, left);
        const size_t blen = fread(b, 1, sizeof b, right);
        if (alen != blen || memcmp(a, b, alen) != 0) { matched = 0; break; }
        if (alen == 0) { matched = !ferror(left) && !ferror(right); break; }
    }
    fclose(left);
    fclose(right);
    if (!matched) { library_close(handle); return fail("round-trip content mismatch"); }

    if (last_error()[0] != '\0') {
        library_close(handle);
        return fail("a successful operation left a stale error message");
    }
    if (decompress_file(compressed, output, 42) != FIRE_ZSTD_LIMIT_EXCEEDED) {
        library_close(handle);
        return fail("size limit was not enforced");
    }
    if (last_error()[0] == '\0') {
        library_close(handle);
        return fail("the failed operation did not provide an error message");
    }
    if (decompress_file(input, output, 262144) == FIRE_ZSTD_OK) {
        library_close(handle);
        return fail("non-Zstd data was incorrectly decompressed");
    }
    if (compress_file(input, input, 3) != FIRE_ZSTD_INVALID_ARGUMENT) {
        library_close(handle);
        return fail("identical input and output were accepted");
    }

    if (remove(input) || remove(compressed) || remove(output)) {
        library_close(handle);
        return fail("cannot remove temporary files");
    }
    printf("FireZstd native smoke passed (ABI %d, Zstd %d)\n", abi_version(), version());
    library_close(handle);
    return EXIT_SUCCESS;
}
