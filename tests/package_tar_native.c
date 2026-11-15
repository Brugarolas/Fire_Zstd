/* Native test fixture for a complete flat FireZstd package archive. */
#include "fire_zstd.h"

#include <stdio.h>
#include <stdlib.h>

static const char package_manifest[] =
    "index.napalm\n"
    "LICENSE.zstd\n"
    "libzstd-windows-x86_64.dll\n"
    "libzstd-windows-aarch64.dll\n"
    "libzstd-linux-x86_64.so\n"
    "libzstd-linux-aarch64.so\n"
    "libzstd-macos-x86_64.dylib\n"
    "libzstd-macos-aarch64.dylib";

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: package_tar_native SOURCE_DIRECTORY OUTPUT_TAR_ZST\n");
        return EXIT_FAILURE;
    }
    if (fire_zstd_abi_version() != 3) {
        fprintf(stderr, "Unexpected native FFI ABI version\n");
        return EXIT_FAILURE;
    }
    const int32_t status = fire_zstd_pack_files(argv[1], package_manifest, argv[2], 18);
    if (status != FIRE_ZSTD_OK) {
        fprintf(stderr, "Packing failed (%d): %s\n", (int)status, fire_zstd_last_error());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
