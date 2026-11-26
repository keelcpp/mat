#pragma once

#include "keel/array.h"
#include "keel/complex.h"
#include "keel/core.h"
#include "keel/memory.h"
#include "keel/optional.h"
#include "keel/ref.h"
#include "keel/result.h"
#include "keel/smart_ptr.h"
#include "keel/string.h"

namespace keel::matfile {

// ============================================================================
// 1. MAT Format Enums & Constants
// ============================================================================

/// Supported MAT binary container versions.
enum class MatVersion : u8 {
    V4 = 4, // Level 4 (uncompressed 2D numeric/char matrices)
    V6 = 6, // Level 5 uncompressed
    V7 = 7, // Level 5 with per-variable ZLIB compression
};

/// Logical array class carried by a `MatArray`.
enum class MatClass : u8 {
    Double = 0,
    Single = 1,
    Int8 = 2,
    Uint8 = 3,
    Int16 = 4,
    Uint16 = 5,
    Int32 = 6,
    Uint32 = 7,
    Int64 = 8,
    Uint64 = 9,
    Logical = 10,
    Char = 11,   // Stored as UTF-16 code units (`u16`) in `data`
    Cell = 12,   // Child arrays stored in `composite->items`
    Struct = 13, // Field names + field arrays in `composite`
    Object = 14, // Class name + property names + values in `composite`
    Sparse = 15, // CSC sparse matrix (`data`, optional `imag_data`, `composite->sparse_ir`/`sparse_jc`)
};

inline constexpr usize kDefaultMaxMatDecompressBytes = 512ULL * 1024ULL * 1024ULL;
inline constexpr usize kDefaultMaxMatNestingDepth = 64;
inline constexpr usize kDefaultMaxMatRank = 32;

/// Element byte width for dense classes (returns 0 for Cell/Struct/Object/Sparse).
[[nodiscard]] constexpr usize mat_class_element_bytes(MatClass kind) noexcept {
    switch (kind) {
        case MatClass::Double:
        case MatClass::Int64:
        case MatClass::Uint64:
            return 8;
        case MatClass::Single:
        case MatClass::Int32:
        case MatClass::Uint32:
            return 4;
        case MatClass::Int16:
        case MatClass::Uint16:
        case MatClass::Char:
            return 2;
        case MatClass::Int8:
        case MatClass::Uint8:
        case MatClass::Logical:
            return 1;
        case MatClass::Cell:
        case MatClass::Struct:
        case MatClass::Object:
        case MatClass::Sparse:
            return 0;
    }
    return 0;
}

[[nodiscard]] constexpr bool mat_class_is_numeric(MatClass kind) noexcept {
    switch (kind) {
        case MatClass::Double:
        case MatClass::Single:
        case MatClass::Int8:
        case MatClass::Uint8:
        case MatClass::Int16:
        case MatClass::Uint16:
        case MatClass::Int32:
        case MatClass::Uint32:
        case MatClass::Int64:
        case MatClass::Uint64:
            return true;
        default:
            return false;
    }
}

// ============================================================================
// 2. Neutral MAT Data Model (Column-Major ND Arrays, Cells, Structs, Sparse)
// ============================================================================

struct MatComposite;

/// Value-neutral MAT array node (move-only, allocator-aware).
struct MatArray {
    MatClass kind = MatClass::Double;
    bool is_complex = false;
    Array<usize> dims;
    Array<u8> data;
    Array<u8> imag_data;
    UniquePtr<MatComposite> composite;

    explicit MatArray(Allocator* alloc = nullptr) noexcept
        : dims(alloc), data(alloc), imag_data(alloc), composite(nullptr) {}

    MatArray(MatArray&&) noexcept = default;
    MatArray& operator=(MatArray&&) noexcept = default;
    MatArray(const MatArray&) = delete;
    MatArray& operator=(const MatArray&) = delete;
    ~MatArray() noexcept = default;

    [[nodiscard]] usize rank() const noexcept { return dims.size(); }
    [[nodiscard]] usize rows() const noexcept { return dims.size() >= 1 ? dims[0] : 0; }
    [[nodiscard]] usize cols() const noexcept { return dims.size() >= 2 ? dims[1] : (dims.size() == 1 ? 1 : 0); }
    [[nodiscard]] usize numel() const noexcept;
    [[nodiscard]] bool is_numeric() const noexcept { return mat_class_is_numeric(kind); }
    [[nodiscard]] bool is_empty() const noexcept { return numel() == 0; }

    // Typed views over dense real/value buffer `data`.
    [[nodiscard]] ArrayRef<f64> as_f64() const noexcept;
    [[nodiscard]] MutArrayRef<f64> as_f64_mut() noexcept;
    [[nodiscard]] ArrayRef<f32> as_f32() const noexcept;
    [[nodiscard]] MutArrayRef<f32> as_f32_mut() noexcept;
    [[nodiscard]] ArrayRef<i8> as_i8() const noexcept;
    [[nodiscard]] ArrayRef<u8> as_u8() const noexcept;
    [[nodiscard]] ArrayRef<i16> as_i16() const noexcept;
    [[nodiscard]] ArrayRef<u16> as_u16() const noexcept;
    [[nodiscard]] ArrayRef<i32> as_i32() const noexcept;
    [[nodiscard]] ArrayRef<u32> as_u32() const noexcept;
    [[nodiscard]] ArrayRef<i64> as_i64() const noexcept;
    [[nodiscard]] ArrayRef<u64> as_u64() const noexcept;
    [[nodiscard]] ArrayRef<u8> as_logical() const noexcept;
    [[nodiscard]] ArrayRef<u16> as_utf16() const noexcept;

    // Typed views over dense imaginary buffer `imag_data` (valid when `is_complex == true`).
    [[nodiscard]] ArrayRef<f64> imag_f64() const noexcept;
    [[nodiscard]] MutArrayRef<f64> imag_f64_mut() noexcept;
    [[nodiscard]] ArrayRef<f32> imag_f32() const noexcept;
    [[nodiscard]] MutArrayRef<f32> imag_f32_mut() noexcept;

    /// Convert `MatClass::Char` (UTF-16 code units) to an ASCII/Latin-1 byte string.
    [[nodiscard]] String to_ascii(Allocator* alloc = nullptr) const noexcept;

    /// Convert `MatClass::Char` (UTF-16 code units) to a UTF-8 string.
    [[nodiscard]] String to_utf8(Allocator* alloc = nullptr) const noexcept;

    // Composite accessors (Cell / Struct / Object).
    [[nodiscard]] usize num_cells() const noexcept;
    [[nodiscard]] const MatArray* cell(usize idx) const noexcept;
    [[nodiscard]] MatArray* cell_mut(usize idx) noexcept;

    [[nodiscard]] StringRef object_class_name() const noexcept;
    [[nodiscard]] usize num_fields() const noexcept;
    [[nodiscard]] ArrayRef<String> field_names() const noexcept;
    [[nodiscard]] Optional<usize> find_field(StringRef name) const noexcept;
    [[nodiscard]] const MatArray* field(usize elem_idx, usize field_idx) const noexcept;
    [[nodiscard]] MatArray* field_mut(usize elem_idx, usize field_idx) noexcept;
    [[nodiscard]] const MatArray* field(StringRef name, usize elem_idx = 0) const noexcept;

    // Factories.
    [[nodiscard]] static MatArray make_dense(MatClass kind, ArrayRef<usize> dims, bool complex = false,
                                             Allocator* alloc = nullptr) noexcept;
    [[nodiscard]] static MatArray from_f64(ArrayRef<usize> dims, ArrayRef<f64> vals,
                                           Allocator* alloc = nullptr) noexcept;
    [[nodiscard]] static MatArray from_c64(ArrayRef<usize> dims, ArrayRef<c64> vals,
                                           Allocator* alloc = nullptr) noexcept;
    [[nodiscard]] static MatArray from_f32(ArrayRef<usize> dims, ArrayRef<f32> vals,
                                           Allocator* alloc = nullptr) noexcept;
    [[nodiscard]] static MatArray from_logical(ArrayRef<usize> dims, ArrayRef<u8> vals,
                                               Allocator* alloc = nullptr) noexcept;
    [[nodiscard]] static MatArray from_ascii(ArrayRef<usize> dims, StringRef chars,
                                             Allocator* alloc = nullptr) noexcept;
    [[nodiscard]] static MatArray from_string(StringRef str, Allocator* alloc = nullptr) noexcept;
    [[nodiscard]] static MatArray from_utf8(StringRef utf8_str, Allocator* alloc = nullptr) noexcept;
    [[nodiscard]] static MatArray make_cell(ArrayRef<usize> dims, Allocator* alloc = nullptr) noexcept;
    [[nodiscard]] static MatArray make_struct(ArrayRef<usize> dims, ArrayRef<StringRef> field_names,
                                              Allocator* alloc = nullptr) noexcept;
    [[nodiscard]] static MatArray make_object(StringRef class_name, ArrayRef<usize> dims,
                                              ArrayRef<StringRef> field_names, Allocator* alloc = nullptr) noexcept;
};

/// Auxiliary storage for Cell, Struct, Object, and Sparse `MatArray` instances.
struct MatComposite {
    String class_name;
    Array<String> field_names;
    Array<MatArray> items; // Cell elements, or Struct/Object fields[field_idx * numel + elem_idx]
    usize sparse_nzmax = 0;
    Array<u32> sparse_ir;
    Array<u32> sparse_jc;

    explicit MatComposite(Allocator* alloc = nullptr) noexcept
        : class_name(alloc), field_names(alloc), items(alloc), sparse_ir(alloc), sparse_jc(alloc) {}
};

/// Named variable stored in a MAT file.
struct MatVar {
    String name;
    MatArray value;

    explicit MatVar(Allocator* alloc = nullptr) noexcept : name(alloc), value(alloc) {}
    MatVar(String var_name, MatArray var_val) noexcept : name(keel::move(var_name)), value(keel::move(var_val)) {}
};

/// Decoded MAT file container.
struct MatFile {
    String header_text;
    MatVersion version = MatVersion::V7;
    bool big_endian = false;
    Array<MatVar> vars;

    explicit MatFile(Allocator* alloc = nullptr) noexcept : header_text(alloc), vars(alloc) {}

    [[nodiscard]] const MatArray* find(StringRef name) const noexcept;
    [[nodiscard]] MatArray* find_mut(StringRef name) noexcept;
};

// ============================================================================
// 3. Codec Options & Public API
// ============================================================================

struct MatReadOptions {
    usize max_decompress_bytes = kDefaultMaxMatDecompressBytes;
    usize max_nesting_depth = kDefaultMaxMatNestingDepth;
    usize max_rank = kDefaultMaxMatRank;
    Allocator* allocator = nullptr;
};

struct MatWriteOptions {
    MatVersion version = MatVersion::V7;
    i32 compression_level = 6;
    StringRef header_text = {};
    Allocator* allocator = nullptr;
};

/// Inspect raw bytes and detect whether they look like a MAT Level-5 or Level-4 file.
[[nodiscard]] Optional<MatVersion> mat_detect_version(ArrayRef<u8> bytes) noexcept;

/// Decode a MAT Level-4 or Level-5 (v6/v7) binary buffer into a `MatFile`.
[[nodiscard]] Result<MatFile> mat_decode(ArrayRef<u8> bytes, const MatReadOptions& opts = {}) noexcept;

/// Encode a list of named variables into a MAT binary buffer (v4, v6, or v7).
[[nodiscard]] Result<Array<u8>> mat_encode(ArrayRef<MatVar> vars, const MatWriteOptions& opts = {}) noexcept;

/// Read and decode a `.mat` file from disk via `keel::fs_read_file`.
[[nodiscard]] Result<MatFile> mat_read_file(StringRef path, const MatReadOptions& opts = {}) noexcept;

/// Encode and write a `.mat` file to disk via `keel::fs_write_file`.
[[nodiscard]] Error mat_write_file(StringRef path, ArrayRef<MatVar> vars,
                                   const MatWriteOptions& opts = {}) noexcept;

} // namespace keel::matfile
