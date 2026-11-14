# Fire Zstd 0.1.0 — Clank Builder edition

This is the native Zstandard package for Fire/Napalm, built without CMake,
Makefiles, XMake or a generated build system. Its dependency on upstream Zstd
1.5.7 is **compiled from C sources into a static archive**, which Clank then
links into one native `fire_zstd` DLL/SO/dylib. The public API is implemented
in `package/zstd/index.napalm`.

## Building

```text
bun run builder/zstd.build.ts --release --compiler mingw --target fire_zstd
```

## Testing

```text
bun run builder/zstd.build.ts --release --compiler mingw --target native-smoke --run
```
