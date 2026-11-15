/* Expose POSIX.1-2008 declarations when compiling in strict C11 mode.
 * Feature-test macros must be defined before including any system header. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE 1
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#  define _DARWIN_C_SOURCE 1
#endif
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "fire_zstd.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if !defined(_WIN32)
#  include <fcntl.h>
#endif
#if defined(__linux__)
#  include <sys/syscall.h>
#endif
#if defined(__APPLE__)
#  include <sys/stdio.h>
#endif
#include <zstd.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <fcntl.h>
#  include <io.h>
#  include <wchar.h>
#else
#  include <unistd.h>
#endif

#if defined(_MSC_VER)
#  define FIRE_THREAD_LOCAL __declspec(thread)
#else
#  define FIRE_THREAD_LOCAL _Thread_local
#endif

#define FIRE_IO_CHUNK (128u * 1024u)
static FIRE_THREAD_LOCAL char g_last_error[512];

static int32_t set_error(int32_t code, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(g_last_error, sizeof(g_last_error), format, args);
    va_end(args);
    return code;
}

static void clear_error(void) {
    g_last_error[0] = '\0';
}

int32_t fire_zstd_abi_version(void) { return 3; }
int32_t fire_zstd_version_number(void) { return (int32_t)ZSTD_versionNumber(); }
const char *fire_zstd_last_error(void) { return g_last_error; }

#if defined(_WIN32)
static wchar_t *utf8_to_wide(const char *value) {
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, NULL, 0);
    if (length <= 0) return NULL;
    wchar_t *result = (wchar_t *)malloc((size_t)length * sizeof(wchar_t));
    if (!result) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, result, length) != length) {
        free(result);
        return NULL;
    }
    return result;
}

static FILE *open_input(const char *path) {
    wchar_t *wide = utf8_to_wide(path);
    if (!wide) return NULL;
    FILE *result = NULL;
    (void)_wfopen_s(&result, wide, L"rb");
    free(wide);
    return result;
}

static int remove_temp(const char *path) {
    wchar_t *wide = utf8_to_wide(path);
    if (!wide) return -1;
    BOOL ok = DeleteFileW(wide);
    free(wide);
    return ok ? 0 : -1;
}

static int commit_temp(const char *source, const char *destination) {
    wchar_t *src = utf8_to_wide(source);
    wchar_t *dst = utf8_to_wide(destination);
    if (!src || !dst) {
        free(src);
        free(dst);
        return -1;
    }
    BOOL ok = MoveFileExW(src, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    free(src);
    free(dst);
    return ok ? 0 : -1;
}

static FILE *open_temporary(const char *destination, char **temporary_path) {
    static volatile LONG counter = 0;
    size_t length = strlen(destination);
    if (length > SIZE_MAX - 80) return NULL;
    char *name = (char *)malloc(length + 80);
    if (!name) return NULL;
    for (unsigned int attempt = 0; attempt < 64; attempt++) {
        unsigned long sequence = (unsigned long)InterlockedIncrement(&counter);
        int count = snprintf(name, length + 80, "%s.fire-tmp-%lu-%lu", destination, (unsigned long)GetCurrentProcessId(), sequence);
        if (count < 0 || (size_t)count >= length + 80) break;
        wchar_t *wide = utf8_to_wide(name);
        if (!wide) break;
        HANDLE handle = CreateFileW(wide, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, NULL);
        free(wide);
        if (handle == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS) continue;
            break;
        }
        int fd = _open_osfhandle((intptr_t)handle, _O_WRONLY | _O_BINARY);
        if (fd < 0) {
            CloseHandle(handle);
            remove_temp(name);
            break;
        }
        FILE *file = _fdopen(fd, "wb");
        if (!file) {
            _close(fd);
            remove_temp(name);
            break;
        }
        *temporary_path = name;
        return file;
    }
    free(name);
    return NULL;
}
#else
static FILE *open_input(const char *path) { return fopen(path, "rb"); }
static int remove_temp(const char *path) { return unlink(path); }
static int commit_temp(const char *source, const char *destination) {
    return rename(source, destination);
}

static FILE *open_temporary(const char *destination, char **temporary_path) {
    size_t length = strlen(destination);
    if (length > SIZE_MAX - 20) return NULL;
    char *name = (char *)malloc(length + 20);
    if (!name) return NULL;
    snprintf(name, length + 20, "%s.fire-tmp-XXXXXX", destination);
    int fd = mkstemp(name);
    if (fd < 0) {
        free(name);
        return NULL;
    }
    FILE *file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
        unlink(name);
        free(name);
        return NULL;
    }
    *temporary_path = name;
    return file;
}
#endif

static int32_t validate(const char *source, const char *destination) {
    if (!source || !destination || !source[0] || !destination[0])
        return set_error(FIRE_ZSTD_INVALID_ARGUMENT, "Source and destination paths must be nonempty");
    if (strcmp(source, destination) == 0)
        return set_error(FIRE_ZSTD_INVALID_ARGUMENT, "Source and destination paths cannot be identical");
    return FIRE_ZSTD_OK;
}

static int32_t write_all(FILE *file, const void *data, size_t count) {
    if (count && fwrite(data, 1, count, file) != count)
        return set_error(FIRE_ZSTD_IO_ERROR, "Failed to write output: %s", strerror(errno));
    return FIRE_ZSTD_OK;
}

static int32_t finish_or_discard(FILE *output, char *temporary, const char *destination, int32_t status) {
    if (status == FIRE_ZSTD_OK) {
        /* Only publish complete, successfully synchronized files. */
        if (fflush(output) != 0 || ferror(output)) {
            status = set_error(FIRE_ZSTD_IO_ERROR, "Could not flush output file");
        }
#if defined(_WIN32)
        if (status == FIRE_ZSTD_OK && _commit(_fileno(output)) != 0)
#else
        if (status == FIRE_ZSTD_OK && fsync(fileno(output)) != 0)
#endif
            status = set_error(FIRE_ZSTD_IO_ERROR, "Could not synchronize output file");
    }
    if (fclose(output) != 0 && status == FIRE_ZSTD_OK)
        status = set_error(FIRE_ZSTD_IO_ERROR, "Could not close output file");
    if (status == FIRE_ZSTD_OK && commit_temp(temporary, destination) != 0)
        status = set_error(FIRE_ZSTD_IO_ERROR, "Could not rename temporary output file");
    if (status != FIRE_ZSTD_OK) remove_temp(temporary);
    free(temporary);
    return status;
}

int32_t fire_zstd_compress_file(const char *source, const char *destination, int32_t level) {
    clear_error();
    int32_t status = validate(source, destination);
    if (status) return status;
    if (level < ZSTD_minCLevel() || level > ZSTD_maxCLevel())
        return set_error(FIRE_ZSTD_INVALID_ARGUMENT, "Compression level outside zstd-supported range");

    FILE *input = open_input(source);
    if (!input) return set_error(FIRE_ZSTD_IO_ERROR, "Cannot open source file");

    ZSTD_CCtx *context = ZSTD_createCCtx();
    unsigned char *in_data = (unsigned char *)malloc(FIRE_IO_CHUNK);
    unsigned char *out_data = (unsigned char *)malloc(FIRE_IO_CHUNK);
    char *temporary = NULL;
    FILE *output = NULL;
    if (!context || !in_data || !out_data) {
        status = set_error(FIRE_ZSTD_OUT_OF_MEMORY, "Could not allocate compression buffers");
        goto cleanup;
    }

    size_t code = ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel, level);
    if (!ZSTD_isError(code)) code = ZSTD_CCtx_setParameter(context, ZSTD_c_checksumFlag, 1);
    if (ZSTD_isError(code)) {
        status = set_error(FIRE_ZSTD_CODEC_ERROR, "Zstd configuration: %s", ZSTD_getErrorName(code));
        goto cleanup;
    }

    output = open_temporary(destination, &temporary);
    if (!output) {
        status = set_error(FIRE_ZSTD_IO_ERROR, "Cannot create temporary output file");
        goto cleanup;
    }

    for (;;) {
        size_t read_count = fread(in_data, 1, FIRE_IO_CHUNK, input);
        if (ferror(input)) {
            status = set_error(FIRE_ZSTD_IO_ERROR, "Could not read input file");
            break;
        }
        ZSTD_EndDirective mode = feof(input) ? ZSTD_e_end : ZSTD_e_continue;
        ZSTD_inBuffer in = {in_data, read_count, 0};
        size_t remaining;
        do {
            ZSTD_outBuffer out = {out_data, FIRE_IO_CHUNK, 0};
            remaining = ZSTD_compressStream2(context, &out, &in, mode);
            if (ZSTD_isError(remaining)) {
                status = set_error(FIRE_ZSTD_CODEC_ERROR, "Zstd compression: %s", ZSTD_getErrorName(remaining));
                break;
            }
            status = write_all(output, out_data, out.pos);
            if (status) break;
        } while (in.pos < in.size || (mode == ZSTD_e_end && remaining != 0));
        if (status || mode == ZSTD_e_end) break;
    }

cleanup:
    if (fclose(input) != 0 && status == FIRE_ZSTD_OK)
        status = set_error(FIRE_ZSTD_IO_ERROR, "Could not close source file");
    if (output) status = finish_or_discard(output, temporary, destination, status);
    free(in_data);
    free(out_data);
    ZSTD_freeCCtx(context);
    return status;
}

int32_t fire_zstd_decompress_file(const char *source, const char *destination, uint64_t max_output_bytes) {
    clear_error();
    int32_t status = validate(source, destination);
    if (status) return status;
    if (max_output_bytes == 0)
        return set_error(FIRE_ZSTD_INVALID_ARGUMENT, "Decompression size limit must be positive");

    FILE *input = open_input(source);
    if (!input) return set_error(FIRE_ZSTD_IO_ERROR, "Cannot open compressed input file");

    ZSTD_DCtx *context = ZSTD_createDCtx();
    unsigned char *in_data = (unsigned char *)malloc(FIRE_IO_CHUNK);
    unsigned char *out_data = (unsigned char *)malloc(FIRE_IO_CHUNK);
    char *temporary = NULL;
    FILE *output = NULL;
    uint64_t produced = 0;
    int end_of_frame = 0;
    if (!context || !in_data || !out_data) {
        status = set_error(FIRE_ZSTD_OUT_OF_MEMORY, "Could not allocate decompression buffers");
        goto cleanup;
    }
    output = open_temporary(destination, &temporary);
    if (!output) {
        status = set_error(FIRE_ZSTD_IO_ERROR, "Cannot create temporary output file");
        goto cleanup;
    }

    for (;;) {
        size_t read_count = fread(in_data, 1, FIRE_IO_CHUNK, input);
        if (ferror(input)) {
            status = set_error(FIRE_ZSTD_IO_ERROR, "Could not read compressed input");
            break;
        }
        if (!read_count) {
            if (!end_of_frame)
                status = set_error(FIRE_ZSTD_INVALID_STREAM, "Empty or truncated Zstd stream");
            break;
        }
        ZSTD_inBuffer in = {in_data, read_count, 0};
        while (in.pos < in.size) {
            size_t previous_pos = in.pos;
            ZSTD_outBuffer out = {out_data, FIRE_IO_CHUNK, 0};
            size_t remaining = ZSTD_decompressStream(context, &out, &in);
            if (ZSTD_isError(remaining)) {
                status = set_error(FIRE_ZSTD_CODEC_ERROR, "Zstd decompression: %s", ZSTD_getErrorName(remaining));
                break;
            }
            if ((uint64_t)out.pos > max_output_bytes - produced) {
                status = set_error(FIRE_ZSTD_LIMIT_EXCEEDED, "Decompressed file exceeds configured limit");
                break;
            }
            status = write_all(output, out_data, out.pos);
            if (status) break;
            produced += (uint64_t)out.pos;
            end_of_frame = (remaining == 0);
            if (in.pos == previous_pos && out.pos == 0) {
                status = set_error(FIRE_ZSTD_INVALID_STREAM, "Zstd decoder made no progress");
                break;
            }
        }
        if (status) break;
    }

cleanup:
    if (fclose(input) != 0 && status == FIRE_ZSTD_OK)
        status = set_error(FIRE_ZSTD_IO_ERROR, "Could not close compressed input file");
    if (output) status = finish_or_discard(output, temporary, destination, status);
    free(in_data);
    free(out_data);
    ZSTD_freeDCtx(context);
    return status;
}

/*
 * A streaming POSIX ustar writer that feeds the Zstandard compressor directly.
 * File names are deliberately restricted to flat UTF-8/ASCII basenames: the
 * public package is one directory, without links or nested paths.  Only the
 * selected files enter the archive.  No temporary uncompressed TAR is created.
 */

typedef struct FireTarWriter {
    ZSTD_CCtx *context;
    FILE *output;
    unsigned char *scratch;
} FireTarWriter;

static int32_t tar_push(FireTarWriter *writer, const void *data, size_t length) {
    ZSTD_inBuffer input = {data, length, 0};
    while (input.pos < input.size) {
        ZSTD_outBuffer output = {writer->scratch, FIRE_IO_CHUNK, 0};
        size_t result = ZSTD_compressStream2(writer->context, &output, &input, ZSTD_e_continue);
        if (ZSTD_isError(result)) {
            return set_error(FIRE_ZSTD_CODEC_ERROR, "Zstd TAR compression: %s", ZSTD_getErrorName(result));
        }
        int32_t status = write_all(writer->output, writer->scratch, output.pos);
        if (status != FIRE_ZSTD_OK) return status;
        if (input.pos == 0 && output.pos == 0) {
            return set_error(FIRE_ZSTD_CODEC_ERROR, "Zstd TAR compressor made no progress");
        }
    }
    return FIRE_ZSTD_OK;
}

static int32_t tar_finish(FireTarWriter *writer) {
    size_t remaining;
    do {
        ZSTD_inBuffer input = {NULL, 0, 0};
        ZSTD_outBuffer output = {writer->scratch, FIRE_IO_CHUNK, 0};
        remaining = ZSTD_compressStream2(writer->context, &output, &input, ZSTD_e_end);
        if (ZSTD_isError(remaining)) {
            return set_error(FIRE_ZSTD_CODEC_ERROR, "Zstd TAR finalization: %s", ZSTD_getErrorName(remaining));
        }
        int32_t status = write_all(writer->output, writer->scratch, output.pos);
        if (status != FIRE_ZSTD_OK) return status;
        if (remaining != 0 && output.pos == 0) {
            return set_error(FIRE_ZSTD_CODEC_ERROR, "Zstd TAR finalization made no progress");
        }
    } while (remaining != 0);
    return FIRE_ZSTD_OK;
}

/* Encode an unsigned octal TAR field, leaving the last byte NUL-terminated. */
static int tar_octal(unsigned char *field, size_t width, uint64_t value) {
    size_t i = width - 1;
    field[i] = '\0';
    while (i > 0) {
        --i;
        field[i] = (unsigned char)('0' + (value & 7u));
        value >>= 3;
    }
    return value == 0;
}

static int32_t tar_header(FireTarWriter *writer, const char *name, uint64_t size) {
    unsigned char header[512] = {0};
    const size_t name_length = strlen(name);
    if (name_length == 0 || name_length >= 100u) {
        return set_error(FIRE_ZSTD_INVALID_ARGUMENT, "Invalid TAR filename length");
    }
    memcpy(header, name, name_length);
    if (!tar_octal(header + 100, 8, 0644u) ||
        !tar_octal(header + 108, 8, 0u) ||
        !tar_octal(header + 116, 8, 0u) ||
        !tar_octal(header + 124, 12, size) ||
        !tar_octal(header + 136, 12, 0u)) {
        return set_error(FIRE_ZSTD_INVALID_ARGUMENT, "TAR file is too large for ustar format");
    }
    memset(header + 148, ' ', 8);
    header[156] = '0';
    memcpy(header + 257, "ustar", 5);
    memcpy(header + 263, "00", 2);
    unsigned int checksum = 0;
    for (size_t i = 0; i < sizeof(header); ++i) checksum += header[i];
    if (!tar_octal(header + 148, 7, checksum)) {
        return set_error(FIRE_ZSTD_INVALID_ARGUMENT, "TAR header checksum overflow");
    }
    header[155] = ' ';
    return tar_push(writer, header, sizeof(header));
}

static int32_t tar_one_file(FireTarWriter *writer, const char *directory, const char *name,
                            const char *destination) {
    size_t base_length = strlen(directory);
    size_t name_length = strlen(name);
    if (base_length > SIZE_MAX - name_length - 2) {
        return set_error(FIRE_ZSTD_INVALID_ARGUMENT, "Package path is too long");
    }
    char *path = (char *)malloc(base_length + name_length + 2);
    if (!path) return set_error(FIRE_ZSTD_OUT_OF_MEMORY, "Cannot allocate package path");
    memcpy(path, directory, base_length);
    path[base_length] = '/';
    memcpy(path + base_length + 1, name, name_length + 1);
    if (strcmp(path, destination) == 0) {
        free(path);
        return set_error(FIRE_ZSTD_INVALID_ARGUMENT, "Archive cannot include itself");
    }

#if !defined(_WIN32)
    struct stat link_info;
    if (lstat(path, &link_info) != 0 || !S_ISREG(link_info.st_mode)) {
        int32_t status = set_error(FIRE_ZSTD_IO_ERROR, "Package entry is missing or not a regular file: %s", name);
        free(path);
        return status;
    }
#endif
    FILE *input = open_input(path);
    free(path);
    if (!input) return set_error(FIRE_ZSTD_IO_ERROR, "Cannot open package entry: %s", name);

    uint64_t length;
#if defined(_WIN32)
    struct _stat64 info;
    if (_fstat64(_fileno(input), &info) != 0 || (info.st_mode & _S_IFMT) != _S_IFREG || info.st_size < 0) {
#else
    struct stat info;
    if (fstat(fileno(input), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0) {
#endif
        fclose(input);
        return set_error(FIRE_ZSTD_IO_ERROR, "Cannot stat regular package file: %s", name);
    }
    length = (uint64_t)info.st_size;
    int32_t status = tar_header(writer, name, length);
    unsigned char *buffer = NULL;
    if (status == FIRE_ZSTD_OK) {
        buffer = (unsigned char *)malloc(FIRE_IO_CHUNK);
        if (!buffer) status = set_error(FIRE_ZSTD_OUT_OF_MEMORY, "Cannot allocate TAR input buffer");
    }
    uint64_t remaining = length;
    while (status == FIRE_ZSTD_OK && remaining != 0) {
        size_t amount = remaining > FIRE_IO_CHUNK ? FIRE_IO_CHUNK : (size_t)remaining;
        size_t read_count = fread(buffer, 1, amount, input);
        if (read_count != amount) {
            status = set_error(FIRE_ZSTD_IO_ERROR, "Package entry changed or could not be read: %s", name);
            break;
        }
        status = tar_push(writer, buffer, amount);
        remaining -= amount;
    }
    if (status == FIRE_ZSTD_OK && fgetc(input) != EOF) {
        status = set_error(FIRE_ZSTD_IO_ERROR, "Package entry grew while archiving: %s", name);
    }
    if (status == FIRE_ZSTD_OK && ferror(input)) {
        status = set_error(FIRE_ZSTD_IO_ERROR, "Cannot verify package entry: %s", name);
    }
    if (fclose(input) != 0 && status == FIRE_ZSTD_OK) {
        status = set_error(FIRE_ZSTD_IO_ERROR, "Cannot close package entry: %s", name);
    }
    free(buffer);
    if (status != FIRE_ZSTD_OK) return status;

    static const unsigned char padding[512] = {0};
    const size_t padding_size = (size_t)((512u - (length % 512u)) % 512u);
    return padding_size ? tar_push(writer, padding, padding_size) : FIRE_ZSTD_OK;
}

static int tar_valid_name(const char *name, size_t length) {
    if (length == 0 || length >= 100 || (length == 1 && name[0] == '.') ||
        (length == 2 && name[0] == '.' && name[1] == '.')) return 0;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = (unsigned char)name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return 0;
    }
    return 1;
}

int32_t fire_zstd_pack_files(const char *directory_utf8, const char *filenames_utf8,
                             const char *destination_utf8, int32_t level) {
    clear_error();
    if (!directory_utf8 || !directory_utf8[0] || !filenames_utf8 || !filenames_utf8[0] ||
        !destination_utf8 || !destination_utf8[0]) {
        return set_error(FIRE_ZSTD_INVALID_ARGUMENT, "Package directory, filenames and destination are required");
    }
    if (level < ZSTD_minCLevel() || level > ZSTD_maxCLevel()) {
        return set_error(FIRE_ZSTD_INVALID_ARGUMENT, "Compression level outside Zstd-supported range");
    }
    /* Keep the ABI simple enough for FFI while preventing huge manifests. */
    if (strlen(filenames_utf8) > 8192) {
        return set_error(FIRE_ZSTD_INVALID_ARGUMENT, "Package file list exceeds 8192 bytes");
    }

    char filenames[64][100];
    size_t file_count = 0;
    const char *cursor = filenames_utf8;
    while (*cursor) {
        const char *end = strchr(cursor, '\n');
        const size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        if (!tar_valid_name(cursor, length) || file_count == 64) {
            return set_error(FIRE_ZSTD_INVALID_ARGUMENT, "Invalid package filename or more than 64 files");
        }
        memcpy(filenames[file_count], cursor, length);
        filenames[file_count][length] = '\0';
        for (size_t i = 0; i < file_count; ++i) {
            if (strcmp(filenames[i], filenames[file_count]) == 0) {
                return set_error(FIRE_ZSTD_INVALID_ARGUMENT, "Duplicate package filename: %s", filenames[i]);
            }
        }
        ++file_count;
        if (!end) break;
        cursor = end + 1;
    }

    int32_t status = FIRE_ZSTD_OK;
    FireTarWriter writer = {0};
    writer.context = ZSTD_createCCtx();
    writer.scratch = (unsigned char *)malloc(FIRE_IO_CHUNK);
    if (!writer.context || !writer.scratch) {
        status = set_error(FIRE_ZSTD_OUT_OF_MEMORY, "Could not allocate TAR compressor");
        goto cleanup;
    }
    size_t code = ZSTD_CCtx_setParameter(writer.context, ZSTD_c_compressionLevel, level);
    if (!ZSTD_isError(code)) code = ZSTD_CCtx_setParameter(writer.context, ZSTD_c_checksumFlag, 1);
    if (ZSTD_isError(code)) {
        status = set_error(FIRE_ZSTD_CODEC_ERROR, "Zstd TAR configuration: %s", ZSTD_getErrorName(code));
        goto cleanup;
    }
    char *temporary = NULL;
    writer.output = open_temporary(destination_utf8, &temporary);
    if (!writer.output) {
        status = set_error(FIRE_ZSTD_IO_ERROR, "Cannot create temporary TAR.ZST output file");
        goto cleanup;
    }
    for (size_t i = 0; i < file_count && status == FIRE_ZSTD_OK; ++i) {
        status = tar_one_file(&writer, directory_utf8, filenames[i], destination_utf8);
    }
    if (status == FIRE_ZSTD_OK) {
        static const unsigned char tar_terminator[1024] = {0};
        status = tar_push(&writer, tar_terminator, sizeof(tar_terminator));
    }
    if (status == FIRE_ZSTD_OK) status = tar_finish(&writer);
    status = finish_or_discard(writer.output, temporary, destination_utf8, status);
    writer.output = NULL;

cleanup:
    if (writer.output) { /* The output is normally handled by finish_or_discard. */
        fclose(writer.output);
    }
    free(writer.scratch);
    ZSTD_freeCCtx(writer.context);
    return status;
}

/*
 * Streaming extractor for the flat POSIX ustar format written above.
 * All entries are staged in a private sibling directory. Publication uses
 * no-replace renaming; a malformed archive never replaces an installation.
 */
typedef enum FireTarState {
    FIRE_TAR_HEADER,
    FIRE_TAR_DATA,
    FIRE_TAR_PADDING,
    FIRE_TAR_END
} FireTarState;

typedef struct FireTarExtractor {
    char *staging;
    FILE *current;
    char filenames[64][100];
    size_t file_count;
    uint64_t total_bytes;
    uint64_t expanded_bytes;
    uint64_t max_bytes;
    uint64_t remaining;
    size_t padding;
    unsigned char header[512];
    size_t header_bytes;
    unsigned int zero_headers;
    FireTarState state;
} FireTarExtractor;

static char *fire_join_path(const char *directory, const char *name) {
    const size_t a = strlen(directory);
    const size_t b = strlen(name);
    if (a > SIZE_MAX - b - 2) return NULL;
    char *path = (char *)malloc(a + b + 2);
    if (!path) return NULL;
    memcpy(path, directory, a);
    path[a] = '/';
    memcpy(path + a + 1, name, b + 1);
    return path;
}

static int fire_ascii_case_equal(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb) return 0;
    }
    return *a == *b;
}

static int fire_directory_exists(const char *path) {
#if defined(_WIN32)
    wchar_t *wide = utf8_to_wide(path);
    if (!wide) return 1;
    const DWORD flags = GetFileAttributesW(wide);
    free(wide);
    return flags != INVALID_FILE_ATTRIBUTES;
#else
    struct stat info;
    return lstat(path, &info) == 0 || errno != ENOENT;
#endif
}

static char *fire_make_staging_directory(const char *destination) {
    const size_t length = strlen(destination);
    if (length > SIZE_MAX - 64) return NULL;
    char *staging = (char *)malloc(length + 64);
    if (!staging) return NULL;
#if defined(_WIN32)
    static volatile LONG serial = 0;
    for (unsigned int attempt = 0; attempt < 128; ++attempt) {
        const unsigned long n = (unsigned long)InterlockedIncrement(&serial);
        const int count = snprintf(staging, length + 64, "%s.fire-tmp-%lu-%lu",
                                   destination, (unsigned long)GetCurrentProcessId(), n);
        if (count < 0 || (size_t)count >= length + 64) break;
        wchar_t *wide = utf8_to_wide(staging);
        if (!wide) break;
        const BOOL created = CreateDirectoryW(wide, NULL);
        const DWORD error = created ? ERROR_SUCCESS : GetLastError();
        free(wide);
        if (created) return staging;
        if (error != ERROR_ALREADY_EXISTS) break;
    }
#else
    const int count = snprintf(staging, length + 64, "%s.fire-tmp-XXXXXX", destination);
    if (count >= 0 && (size_t)count < length + 64 && mkdtemp(staging)) return staging;
#endif
    free(staging);
    return NULL;
}

static void fire_discard_staging(FireTarExtractor *tar) {
    if (tar->current) {
        fclose(tar->current);
        tar->current = NULL;
    }
    if (!tar->staging) return;
    for (size_t i = 0; i < tar->file_count; ++i) {
        char *path = fire_join_path(tar->staging, tar->filenames[i]);
        if (path) {
            (void)remove_temp(path);
            free(path);
        }
    }
#if defined(_WIN32)
    wchar_t *wide = utf8_to_wide(tar->staging);
    if (wide) {
        (void)RemoveDirectoryW(wide);
        free(wide);
    }
#else
    (void)rmdir(tar->staging);
#endif
    free(tar->staging);
    tar->staging = NULL;
}

/* The destination must not exist. Never use POSIX rename(), which can
 * overwrite an empty directory in a race against another installer. */
static int fire_publish_staging(const char *source, const char *destination) {
#if defined(_WIN32)
    wchar_t *src = utf8_to_wide(source);
    wchar_t *dst = utf8_to_wide(destination);
    if (!src || !dst) {
        free(src);
        free(dst);
        return -1;
    }
    const BOOL ok = MoveFileExW(src, dst, 0);
    free(src);
    free(dst);
    return ok ? 0 : -1;
#elif defined(__linux__)
#if defined(SYS_renameat2)
    return (int)syscall(SYS_renameat2, AT_FDCWD, source, AT_FDCWD,
                        destination, (unsigned int)1 /* RENAME_NOREPLACE */);
#else
    (void)source;
    (void)destination;
    errno = ENOTSUP;
    return -1;
#endif
#elif defined(__APPLE__)
    return renamex_np(source, destination, RENAME_EXCL);
#else
    (void)source;
    (void)destination;
    errno = ENOTSUP;
    return -1;
#endif
}

static FILE *fire_create_regular_file(const char *path) {
#if defined(_WIN32)
    wchar_t *wide = utf8_to_wide(path);
    if (!wide) return NULL;
    HANDLE handle = CreateFileW(wide, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, NULL);
    free(wide);
    if (handle == INVALID_HANDLE_VALUE) return NULL;
    const int fd = _open_osfhandle((intptr_t)handle, _O_WRONLY | _O_BINARY);
    if (fd < 0) {
        CloseHandle(handle);
        return NULL;
    }
    FILE *file = _fdopen(fd, "wb");
    if (!file) {
        _close(fd);
        (void)remove_temp(path);
    }
    return file;
#else
    int flags = O_CREAT | O_EXCL | O_WRONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(path, flags, 0600);
    if (fd < 0) return NULL;
    FILE *file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
        (void)unlink(path);
    }
    return file;
#endif
}

static int32_t fire_close_extracted_file(FireTarExtractor *tar) {
    if (!tar->current) return FIRE_ZSTD_OK;
    FILE *file = tar->current;
    tar->current = NULL;
    int32_t status = FIRE_ZSTD_OK;
    if (fflush(file) != 0) {
        status = set_error(FIRE_ZSTD_IO_ERROR, "Could not flush an extracted file");
    }
#if defined(_WIN32)
    if (status == FIRE_ZSTD_OK && _commit(_fileno(file)) != 0)
#else
    if (status == FIRE_ZSTD_OK && fsync(fileno(file)) != 0)
#endif
        status = set_error(FIRE_ZSTD_IO_ERROR, "Could not synchronize an extracted file");
    if (fclose(file) != 0 && status == FIRE_ZSTD_OK)
        status = set_error(FIRE_ZSTD_IO_ERROR, "Could not close an extracted file");
    return status;
}

/* Strict octal decoder: no base-256 or PAX GNU extensions. */
static int fire_parse_octal(const unsigned char *field, size_t length, uint64_t *out) {
    uint64_t number = 0;
    size_t index = 0;
    while (index < length && field[index] == ' ') ++index;
    size_t digits = 0;
    for (; index < length; ++index) {
        const unsigned char c = field[index];
        if (c == '\0' || c == ' ') break;
        if (c < '0' || c > '7' || number > (UINT64_MAX - (c - '0')) / 8) return 0;
        number = number * 8 + (c - '0');
        ++digits;
    }
    if (!digits) return 0;
    for (; index < length; ++index) {
        if (field[index] != '\0' && field[index] != ' ') return 0;
    }
    *out = number;
    return 1;
}

static int fire_is_zero_block(const unsigned char *bytes, size_t size) {
    for (size_t i = 0; i < size; ++i) if (bytes[i]) return 0;
    return 1;
}

static int32_t fire_process_tar_header(FireTarExtractor *tar) {
    const unsigned char *h = tar->header;
    if (fire_is_zero_block(h, sizeof(tar->header))) {
        if (++tar->zero_headers == 2) tar->state = FIRE_TAR_END;
        return FIRE_ZSTD_OK;
    }
    if (tar->zero_headers != 0) {
        return set_error(FIRE_ZSTD_INVALID_STREAM, "TAR contains data after an end marker");
    }
    if (tar->file_count == 64) {
        return set_error(FIRE_ZSTD_LIMIT_EXCEEDED, "TAR contains more than 64 files");
    }
    uint64_t expected_checksum = 0;
    if (!fire_parse_octal(h + 148, 8, &expected_checksum))
        return set_error(FIRE_ZSTD_INVALID_STREAM, "Invalid TAR checksum field");
    uint64_t checksum = 0;
    for (size_t i = 0; i < sizeof(tar->header); ++i) {
        checksum += (i >= 148 && i < 156) ? (unsigned char)' ' : h[i];
    }
    if (checksum != expected_checksum) {
        return set_error(FIRE_ZSTD_INVALID_STREAM, "TAR header checksum mismatch");
    }
    if (memcmp(h + 257, "ustar", 5) != 0 || h[262] != '\0' ||
        memcmp(h + 263, "00", 2) != 0 || (h[156] != '0' && h[156] != '\0') ||
        !fire_is_zero_block(h + 345, 155)) {
        return set_error(FIRE_ZSTD_INVALID_STREAM, "Unsupported TAR header or entry type");
    }
    size_t name_length = 0;
    while (name_length < 100 && h[name_length] != '\0') ++name_length;
    if (!tar_valid_name((const char *)h, name_length)) {
        return set_error(FIRE_ZSTD_INVALID_STREAM, "Unsafe or unsupported TAR filename");
    }
    for (size_t i = name_length + 1; i < 100; ++i) {
        if (h[i] != '\0') return set_error(FIRE_ZSTD_INVALID_STREAM, "Malformed TAR filename field");
    }
    char name[100];
    memcpy(name, h, name_length);
    name[name_length] = '\0';
    for (size_t i = 0; i < tar->file_count; ++i) {
        if (fire_ascii_case_equal(name, tar->filenames[i])) {
            return set_error(FIRE_ZSTD_INVALID_STREAM, "Duplicate TAR entry: %s", name);
        }
    }
    uint64_t size = 0;
    if (!fire_parse_octal(h + 124, 12, &size)) {
        return set_error(FIRE_ZSTD_INVALID_STREAM, "Invalid TAR entry size");
    }
    if (size > tar->max_bytes - tar->total_bytes) {
        return set_error(FIRE_ZSTD_LIMIT_EXCEEDED, "TAR output exceeds the extraction limit");
    }
    char *path = fire_join_path(tar->staging, name);
    if (!path) return set_error(FIRE_ZSTD_OUT_OF_MEMORY, "Cannot allocate TAR extraction path");
    tar->current = fire_create_regular_file(path);
    free(path);
    if (!tar->current) {
        return set_error(FIRE_ZSTD_IO_ERROR, "Cannot create extracted file: %s", name);
    }
    memcpy(tar->filenames[tar->file_count], name, name_length + 1);
    ++tar->file_count;
    tar->total_bytes += size;
    tar->remaining = size;
    tar->padding = (size_t)((512u - (size % 512u)) % 512u);
    tar->state = FIRE_TAR_DATA;
    if (size == 0) {
        const int32_t status = fire_close_extracted_file(tar);
        if (status != FIRE_ZSTD_OK) return status;
        tar->state = tar->padding ? FIRE_TAR_PADDING : FIRE_TAR_HEADER;
    }
    return FIRE_ZSTD_OK;
}

static int32_t fire_feed_tar(FireTarExtractor *tar, const unsigned char *bytes, size_t length) {
    /* Bound TAR metadata and zero-padding expansion independently of file data. */
    const uint64_t overhead = 128u * 1024u;
    const uint64_t max_expanded = tar->max_bytes > UINT64_MAX - overhead
        ? UINT64_MAX : tar->max_bytes + overhead;
    if ((uint64_t)length > max_expanded - tar->expanded_bytes)
        return set_error(FIRE_ZSTD_LIMIT_EXCEEDED, "TAR stream exceeds size limit plus metadata allowance");
    tar->expanded_bytes += length;
    size_t offset = 0;
    while (offset < length) {
        if (tar->state == FIRE_TAR_END) {
            if (!fire_is_zero_block(bytes + offset, length - offset))
                return set_error(FIRE_ZSTD_INVALID_STREAM, "Nonzero TAR payload after end marker");
            return FIRE_ZSTD_OK;
        }
        if (tar->state == FIRE_TAR_HEADER) {
            const size_t needed = sizeof(tar->header) - tar->header_bytes;
            const size_t amount = length - offset < needed ? length - offset : needed;
            memcpy(tar->header + tar->header_bytes, bytes + offset, amount);
            tar->header_bytes += amount;
            offset += amount;
            if (tar->header_bytes == sizeof(tar->header)) {
                tar->header_bytes = 0;
                const int32_t status = fire_process_tar_header(tar);
                if (status != FIRE_ZSTD_OK) return status;
            }
        } else if (tar->state == FIRE_TAR_DATA) {
            const size_t amount = (uint64_t)(length - offset) < tar->remaining
                ? length - offset : (size_t)tar->remaining;
            if (fwrite(bytes + offset, 1, amount, tar->current) != amount)
                return set_error(FIRE_ZSTD_IO_ERROR, "Cannot write extracted TAR file");
            offset += amount;
            tar->remaining -= amount;
            if (tar->remaining == 0) {
                const int32_t status = fire_close_extracted_file(tar);
                if (status != FIRE_ZSTD_OK) return status;
                tar->state = tar->padding ? FIRE_TAR_PADDING : FIRE_TAR_HEADER;
            }
        } else {
            const size_t amount = length - offset < tar->padding ? length - offset : tar->padding;
            if (!fire_is_zero_block(bytes + offset, amount))
                return set_error(FIRE_ZSTD_INVALID_STREAM, "Nonzero TAR entry padding");
            offset += amount;
            tar->padding -= amount;
            if (tar->padding == 0) tar->state = FIRE_TAR_HEADER;
        }
    }
    return FIRE_ZSTD_OK;
}

int32_t fire_zstd_unpack_files(const char *archive_utf8, const char *destination_directory_utf8,
                               uint64_t max_output_bytes) {
    clear_error();
    if (!archive_utf8 || !archive_utf8[0] || !destination_directory_utf8 ||
        !destination_directory_utf8[0] || max_output_bytes == 0) {
        return set_error(FIRE_ZSTD_INVALID_ARGUMENT, "Archive, destination and positive size limit are required");
    }
    if (fire_directory_exists(destination_directory_utf8)) {
        return set_error(FIRE_ZSTD_INVALID_ARGUMENT, "Destination already exists or cannot be checked");
    }
    FILE *input = open_input(archive_utf8);
    if (!input) return set_error(FIRE_ZSTD_IO_ERROR, "Cannot open compressed TAR archive");
    FireTarExtractor tar = {0};
    tar.max_bytes = max_output_bytes;
    tar.staging = fire_make_staging_directory(destination_directory_utf8);
    ZSTD_DCtx *context = ZSTD_createDCtx();
    unsigned char *compressed = (unsigned char *)malloc(FIRE_IO_CHUNK);
    unsigned char *decompressed = (unsigned char *)malloc(FIRE_IO_CHUNK);
    int32_t status = FIRE_ZSTD_OK;
    int finished = 0;
    if (!tar.staging || !context || !compressed || !decompressed) {
        status = tar.staging
            ? set_error(FIRE_ZSTD_OUT_OF_MEMORY, "Cannot allocate archive extraction resources")
            : set_error(FIRE_ZSTD_IO_ERROR, "Cannot create private extraction directory");
        goto cleanup;
    }
    for (;;) {
        const size_t read_count = fread(compressed, 1, FIRE_IO_CHUNK, input);
        if (read_count == 0) {
            if (ferror(input)) status = set_error(FIRE_ZSTD_IO_ERROR, "Cannot read compressed TAR archive");
            break;
        }
        ZSTD_inBuffer in = {compressed, read_count, 0};
        while (in.pos < in.size) {
            const size_t previous = in.pos;
            ZSTD_outBuffer out = {decompressed, FIRE_IO_CHUNK, 0};
            const size_t remaining = ZSTD_decompressStream(context, &out, &in);
            if (ZSTD_isError(remaining)) {
                status = set_error(FIRE_ZSTD_CODEC_ERROR, "Invalid Zstd stream: %s", ZSTD_getErrorName(remaining));
                break;
            }
            status = fire_feed_tar(&tar, decompressed, out.pos);
            if (status != FIRE_ZSTD_OK) break;
            if (remaining == 0) {
                if (in.pos != in.size) {
                    status = set_error(FIRE_ZSTD_INVALID_STREAM, "Trailing data after Zstd frame");
                } else {
                    const int byte = fgetc(input);
                    if (byte != EOF) status = set_error(FIRE_ZSTD_INVALID_STREAM, "Additional data after Zstd frame");
                    else if (ferror(input)) status = set_error(FIRE_ZSTD_IO_ERROR, "Cannot finish reading Zstd stream");
                    else finished = 1;
                }
                break;
            }
            if (previous == in.pos && out.pos == 0) {
                status = set_error(FIRE_ZSTD_INVALID_STREAM, "Zstd decompressor made no progress");
                break;
            }
        }
        if (status != FIRE_ZSTD_OK || finished) break;
    }
    if (status == FIRE_ZSTD_OK && (!finished || tar.state != FIRE_TAR_END || tar.file_count == 0)) {
        status = set_error(FIRE_ZSTD_INVALID_STREAM, "Incomplete Zstd frame or invalid TAR file list");
    }
    if (fclose(input) != 0 && status == FIRE_ZSTD_OK)
        status = set_error(FIRE_ZSTD_IO_ERROR, "Cannot close compressed TAR archive");
    input = NULL;
    if (status == FIRE_ZSTD_OK && fire_publish_staging(tar.staging, destination_directory_utf8) != 0) {
        status = set_error(FIRE_ZSTD_IO_ERROR, "Cannot publish extracted directory (destination may exist)");
    }
    if (status == FIRE_ZSTD_OK) {
        free(tar.staging);
        tar.staging = NULL;
    }
cleanup:
    if (input && fclose(input) != 0 && status == FIRE_ZSTD_OK)
        status = set_error(FIRE_ZSTD_IO_ERROR, "Cannot close compressed TAR archive");
    fire_discard_staging(&tar);
    free(compressed);
    free(decompressed);
    ZSTD_freeDCtx(context);
    return status;
}
