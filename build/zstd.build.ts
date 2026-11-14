/** Fire Zstd 0.1.0 native build, powered exclusively by Clank Builder. */
import { copyFile, mkdir } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import ProjectBuilder, { sourceTree } from './main-builder.ts';
import { parseCli, runBuildCli } from './cli.ts';

const root = resolve(import.meta.dirname, '..');
const isWindows = process.platform === 'win32';
const isMacOS = process.platform === 'darwin';
const platformName = isWindows ? 'windows' : isMacOS ? 'macos' : process.platform === 'linux' ? 'linux' : null;
if (!platformName) throw new Error(`Fire Zstd does not support ${process.platform}`);

const builder = new ProjectBuilder({
  rootDir: root,
  buildDir: 'build-clank',
  toolchain: 'auto',
  profile: 'release',
});

// The pinned revision is the commit referenced by the upstream v1.5.7 tag.
// The Git checkout is reproducible, rather than relying on a moving branch.
builder.addGitCheckout({
  name: 'zstd-source',
  repository: 'https://github.com/facebook/zstd.git',
  ref: 'v1.5.7',
  commit: 'f8745da6ff1ad1e7bab384bd1f9d742439278e99',
  directory: 'vendor/zstd',
});

// Compile all of Zstd's C compression/decompression translation units.
// DictBuilder, deprecated APIs, legacy format decoders, programs, and assembly
// are intentionally excluded, just like the streamlined CMake build.
builder.addStaticLibrary('zstd-static', {
  sources: [
    sourceTree('vendor/zstd/lib/common', { extensions: ['.c'] }),
    sourceTree('vendor/zstd/lib/compress', { extensions: ['.c'] }),
    sourceTree('vendor/zstd/lib/decompress', { extensions: ['.c'] }),
  ],
  includeDirs: [
    'vendor/zstd/lib',
    'vendor/zstd/lib/common',
    'vendor/zstd/lib/compress',
    'vendor/zstd/lib/decompress',
  ],
  defines: {
    ZSTD_DISABLE_ASM: 1,
    ZSTD_LEGACY_SUPPORT: 0,
    ...(isWindows ? { ZSTD_HEAPMODE: 0, _CRT_SECURE_NO_WARNINGS: 1 } : {}),
  },
  // Required when a static archive is embedded inside an ELF/Mach-O shared lib.
  // Clank Builder also propagates this automatically for shared dependencies.
  pic: true,
  tasks: ['zstd-source'],
});

builder.addSharedLibrary('fire_zstd', {
  sources: ['native/fire_zstd.c'],
  includeDirs: ['native', 'vendor/zstd/lib'],
  dependencies: ['zstd-static'],
  defines: {
    FIRE_ZSTD_EXPORTS: 1,
    ...(!isWindows ? { _POSIX_C_SOURCE: 200809 } : {}),
  },
  cFlags: isWindows ? [] : ['-Werror', '-Wpedantic', '-fvisibility=hidden'],
  linkFlags: isMacOS
    ? [`-Wl,-exported_symbols_list,${join(root, 'native', 'exports.macos.txt')}`]
    : isWindows ? [] : ['-Wl,--exclude-libs,ALL'],
  outputName: 'fire_zstd',
});

// Native integration test; dynamically loads fire_zstd while Zstd stays static.
builder.addExecutable('native-smoke', {
  sources: ['tests/native_smoke.c'],
  includeDirs: ['native'],
  dependencies: ['fire_zstd'],
  outputName: 'fire_zstd_native_smoke',
});

const argv = process.argv.slice(2);
const parsed = parseCli(argv);

async function main(): Promise<void> {
  const result = await runBuildCli(
    builder,
    'native-smoke',
    argv,
    '\nFire Zstd project:\n' +
      '  --target fire_zstd       Build and stage the shared library\n' +
      '  --target native-smoke --run   Run native integration checks\n',
  );
  if (!result || parsed === 'help' || parsed.dryRun) return;
  const library = result.outputs.fire_zstd;
  if (!library) return;
  const machine = result.arch === 'x64' ? 'x86_64' : result.arch === 'arm64' ? 'aarch64' : null;
  if (!machine) throw new Error(`Unsupported host architecture: ${result.arch}`);
  const destinationDirectory = join(root, 'package', 'zstd', 'native', `${platformName}-${machine}`);
  await mkdir(destinationDirectory, { recursive: true });
  const name = isWindows ? 'fire_zstd.dll' : isMacOS ? 'libfire_zstd.dylib' : 'libfire_zstd.so';
  const destination = join(destinationDirectory, name);
  await copyFile(library, destination);
  console.log(`Staged ${destination}`);
}

main().catch(error => {
  console.error(error instanceof Error ? error.stack ?? error.message : String(error));
  process.exitCode = 1;
});
