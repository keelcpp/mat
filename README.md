# keel::mat

Zero-dependency C++20 binary `.mat` (Level 4, Level 5 uncompressed `-v6`, and Level 5 compressed `-v7`) reader and writer for the [keel](https://github.com/keelcpp/keel) ecosystem — powered by `keel::compress`, zero STL headers in public API, `-fno-exceptions -fno-rtti` compatible.

## Features

- **MAT Level 5 (`-v6` & `-v7`)**:
  - Full read & write support for dense ND numeric arrays (`f64`, `f32`, `i8`, `u8`, `i16`, `u16`, `i32`, `u32`, `i64`, `u64`), complex arrays, logical arrays, UTF-16/UTF-8 character arrays, cell arrays, structs & struct arrays, objects, and CSC sparse matrices.
  - ZLIB stream compression/decompression via `keel::compress` with configurable output byte cap (`max_decompress_bytes`).
  - Automatic MAT5 numeric down-cast/widening, Small Data Element Format (SDEF), and both little-endian (`IM`) and big-endian (`MI`) readers.
- **MAT Level 4 (`-v4`)**:
  - Read & write support for 2D real/complex numeric and character matrices.
- **Hardened & Allocator-Aware**:
  - Pure `keel::Result<T>` error reporting (no exceptions required).
  - Guards against 32-bit padding wraparound, nesting depth bombs, dimension overflow, truncated payloads, and decompression bombs.

## Build & Test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```
