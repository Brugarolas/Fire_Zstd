/**
 * FireZstd: build the Zstandard C API and the Napalm FFI bridge into one library.
 *
 * This is a TypeScript project definition: change PLATFORMS to change the build
 * matrix. There are no cross-target command-line flags.
 *
 * Usage: bun run builder/zstd.build.ts
 *        node --experimental-strip-types builder/zstd.build.ts
 */
import { existsSync } from 'node:fs';
import { copyFile, mkdir, rm } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import ProjectBuilder, { sourceTree } from './main-builder.ts';
import { parseCli, runBuildCli } from './cli.ts';

const root = resolve(import.meta.dirname, '..');
const outputDir = 'build-clank';

// Define the supported runtime platforms here, not on the command line.
// Use GNU/Linux builds for glibc-based Napalm runtimes. A musl runtime requires
// its own package directory and is deliberately not mixed with GNU/Linux.
const PLATFORMS = [
  { triple: 'x86_64-windows-gnu', os: 'windows', arch: 'x86_64' },
  { triple: 'aarch64-windows-gnu', os: 'windows', arch: 'aarch64' },
  { triple: 'x86_64-linux-gnu', os: 'linux', arch: 'x86_64' },
  { triple: 'aarch64-linux-gnu', os: 'linux', arch: 'aarch64' },
  { triple: 'x86_64-macos', os: 'macos', arch: 'x86_64' },
  { triple: 'aarch64-macos', os: 'macos', arch: 'aarch64' },
] as const;

const ZSTD_MULTITHREADED_SHARED = true;
const ZSTD_MULTITHREADED_STATIC = false;

const argv = process.argv.slice(2);
const parsed = parseCli(argv);
const inspectionOnly = parsed === 'help' || parsed.doctor || parsed.clean;

// The source revision is pinned: no implicit use of the moving dev branch.
const vendorDir = 'vendor/zstd';
const zstdSourceTask = 'zstd-source';
const builder = new ProjectBuilder({
  rootDir: root,
  buildDir: outputDir,
  toolchain: 'zig',
  profile: 'release',
  jobs: 8,
  includeDirs: [
    'native',
    `${vendorDir}/lib`,
    `${vendorDir}/lib/common`,
    `${vendorDir}/lib/compress`,
    `${vendorDir}/lib/decompress`,
  ],
  defines: {
    XXH_NAMESPACE: 'ZSTD_',
    DEBUGLEVEL: 0,
    ZSTD_DISABLE_ASM: 1,
    ZSTD_LEGACY_SUPPORT: 0,
  },
});

builder.addGitCheckout({
  name: zstdSourceTask,
  repository: 'https://github.com/facebook/zstd.git',
  ref: 'v1.5.7',
  commit: 'f8745da6ff1ad1e7bab384bd1f9d742439278e99',
  directory: vendorDir,
});

// Zstd's active compression and decompression modules only.
// Legacy, deprecated, CLI programs, dictionary-builder and assembly are excluded.
const zstdSources = [
  sourceTree(`${vendorDir}/lib/common`, { extensions: ['.c'] }),
  sourceTree(`${vendorDir}/lib/compress`, { extensions: ['.c'] }),
  sourceTree(`${vendorDir}/lib/decompress`, { extensions: ['.c'] }),
];
const embeddedSources = [...zstdSources, 'native/fire_zstd.c'];

// A static archive is useful for native consumers, but is NOT deployed by FFI.
builder.addStaticLibrary('zstd_static', {
  sources: embeddedSources,
  outputName: 'zstd',
  pic: true,
  threads: ZSTD_MULTITHREADED_STATIC,
  defines: { FIRE_ZSTD_STATIC: 1 },
  tasks: [zstdSourceTask],
});

// Compile the bridge directly into libzstd: no dependency on another zstd DLL.
// All ZSTD_* symbols and fire_zstd_* entry points reside in one binary.
builder.addSharedLibrary('zstd_shared', {
  sources: embeddedSources,
  outputName: 'zstd',
  pic: true,
  threads: ZSTD_MULTITHREADED_SHARED,
  defines: {
    FIRE_ZSTD_EXPORTS: 1,
    ...(ZSTD_MULTITHREADED_SHARED ? { ZSTD_MULTITHREAD: 1 } : {}),
  },
  tasks: [zstdSourceTask],
});

for (const platform of PLATFORMS) {
  const windows = platform.os === 'windows';
  builder.addCrossTarget(platform.triple, {
    targets: {
      zstd_static: {
        // PIC is needed for archives that may later be embedded in shared code.
        pic: !windows,
      },
      zstd_shared: {
        // Windows convention: libzstd.dll, not zstd.dll or liblibzstd.dll.
        outputName: windows ? 'libzstd' : 'zstd',
        pic: !windows,
        defines: windows ? {
          ZSTD_DLL_EXPORT: 1,
          ZSTD_HEAPMODE: 0,
          _CRT_SECURE_NO_WARNINGS: 1,
        } : { _POSIX_C_SOURCE: 200809 },
        // Zstd's public headers export the ZSTD_* API explicitly; the bridge
        // exports fire_zstd_* via FIRE_ZSTD_API.
        cFlags: windows ? [] : ['-fvisibility=hidden'],
      },
    },
  });
}

async function main(): Promise<void> {
  if (!inspectionOnly && !existsSync(join(root, 'native', 'fire_zstd.c'))) {
    throw new Error('Missing native/fire_zstd.c. The FFI bridge must be compiled into libzstd.');
  }

  const result = await runBuildCli(builder, undefined, argv);
  if (!result || parsed === 'help' || parsed.clean || parsed.doctor || parsed.dryRun) return;

  for (const platform of PLATFORMS) {
    const build = result.crossTargets?.[platform.triple];
    const library = build?.outputs.zstd_shared;
    if (!library) continue; // For example, when --target selects only zstd_static.
    const destDir = join(root, 'package', 'zstd');
    await mkdir(destDir, { recursive: true });
    const extension = platform.os === 'windows' ? '.dll' : platform.os === 'macos' ? '.dylib' : '.so';
    const dest = join(destDir, `libzstd-${platform.os}-${platform.arch}${extension}`);
    await copyFile(library, dest);

    console.log(`Staged ${dest}`);
  }

  // The former native/ tree was owned by this build script; remove it only
  // after an entire cross-platform build has staged all replacement libraries.
  if (PLATFORMS.every(platform => result.crossTargets?.[platform.triple]?.outputs.zstd_shared)) {
    await rm(join(root, 'package', 'zstd', 'native'), { recursive: true, force: true });
  }
}

main().catch(error => {
  console.error(error instanceof Error ? error.stack ?? error.message : String(error));
  process.exitCode = 1;
});
