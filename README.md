# FireZstd: a single Zstandard native library with a Napalm FFI API

`builder/zstd.build.ts` compiles upstream Zstandard **and** `native/fire_zstd.c`
into each `libzstd.dll`, `libzstd.so`, or `libzstd.dylib`.
There is no separate `fire_zstd.dll` to deploy. `libzstd.a` remains available for
other native consumers, but Napalm's FFI does not load archives.

## Layout

```text
FireZstd/
  builder/zstd.build.ts           # Configure the cross-build matrix in TypeScript
  builder/main-builder.ts        # Clank Builder and its companion source files
  native/fire_zstd.c             # Portable file streaming and error handling
  native/fire_zstd.h             # Stable C ABI for Napalm
  native/exports.macos.txt       # Optional FFI-only symbol list (not used by default)
  package/zstd/index.napalm      # Loads the native library via FFI.open()
  tests/native_smoke.c           # Dynamic-load integration test
  vendor/zstd/                  # Clank downloads pinned v1.5.7 sources here
```

## Build

Install Zig, Bun (or Node >= 22 with type stripping), and Git. Run from the
project root:

```sh
bun run builder/zstd.build.ts
```

Cross-compile platforms are declared in `PLATFORMS` in the TypeScript file.
No cross-target flags are necessary or supported. The default matrix builds
Windows, GNU/Linux and macOS for x86_64 and aarch64. Each build has its own
cache and object files. Only supported, nondeprecated Zstd code is built.

The build copies the shared libraries to:

```text
package/zstd/native/windows-x86_64/libzstd.dll
package/zstd/native/windows-aarch64/libzstd.dll
package/zstd/native/linux-x86_64/libzstd.so
package/zstd/native/linux-aarch64/libzstd.so
package/zstd/native/macos-x86_64/libzstd.dylib
package/zstd/native/macos-aarch64/libzstd.dylib
```

`libzstd.dll.a` is an import archive produced when linking a Windows DLL; it
is not a second DLL. The static `libzstd.a` archive is not required by Napalm
at runtime.

Linux glibc and musl builds are not interchangeable: if you add musl targets,
give them a separate package path and update the runtime's libc selection.

Cross-linking macOS dynamic libraries can require a compatible Zig version
and Apple SDK/sysroot setup. Verify artifacts on each target OS before release.

## Napalm FFI

`index.napalm` opens a platform-specific native library and binds five
versioned C functions:

```c
int32_t fire_zstd_abi_version(void);
int32_t fire_zstd_version_number(void);
const char *fire_zstd_last_error(void);
int32_t fire_zstd_compress_file(const char *, const char *, int32_t);
int32_t fire_zstd_decompress_file(const char *, const char *, uint64_t);
```

The bridge performs streaming compression/decompression with bounded memory,
UTF-8 path handling on Windows, bounded decompressed sizes, and atomic temporary
output replacement. The error string belongs to the calling thread and should
be consumed immediately after a failed operation.

`FIRE_ZSTD_ROOT` can point to an unpacked `package/zstd` directory. Without it,
`index.napalm` searches known module locations upward from the working directory.

This depends on Napalm exposing `System.os`, `System.arch`, `Process.getenv`,
`Fs.exists`, `Path`, and `FFI.open().function(...).call()` with the primitive
signature names appearing in `index.napalm`. These bindings have not been tested
against a local Napalm runtime.

## Native smoke test

On Linux:

```sh
cc -std=c11 -Wall -Wextra -Werror -pedantic -Inative tests/native_smoke.c -ldl -o native_smoke
./native_smoke "$(pwd)/package/zstd/native/linux-x86_64/libzstd.so"
```

On macOS, compile without `-ldl`. On Windows, compile with MinGW or MSVC, and
run the executable with the path to `libzstd.dll`.

`native/exports.macos.txt` from the earlier FFI-only package deliberately isn't
passed to the macOS linker: restricting exports to only `fire_zstd_*` would
hide the upstream `ZSTD_*` API, contrary to packaging the normal `libzstd`.

## Licensing

The bundled `package/zstd/LICENSE.zstd` contains the upstream Zstandard BSD
license notice required when redistributing the compiled libraries. Retain this
notice in released FireZstd packages.
