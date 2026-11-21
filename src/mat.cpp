#include "keel/mat.h"

#include "keel/arena.h"
#include "keel/compress.h"
#include "keel/fs.h"
#include "keel/math.h"

namespace keel {

namespace {

// ============================================================================
// MAT5 Binary Constants
// ============================================================================

enum Mat5DataType : u32 {
    kMiInt8 = 1,
    kMiUint8 = 2,
    kMiInt16 = 3,
    kMiUint16 = 4,
    kMiInt32 = 5,
    kMiUint32 = 6,
    kMiSingle = 7,
    kMiDouble = 9,
    kMiInt64 = 12,
    kMiUint64 = 13,
    kMiMatrix = 14,
    kMiCompressed = 15,
    kMiUtf8 = 16,
    kMiUtf16 = 17,
    kMiUtf32 = 18,
};

enum Mat5ArrayClass : u8 {
    kMxCellClass = 1,
    kMxStructClass = 2,
    kMxObjectClass = 3,
    kMxCharClass = 4,
    kMxSparseClass = 5,
    kMxDoubleClass = 6,
    kMxSingleClass = 7,
    kMxInt8Class = 8,
    kMxUint8Class = 9,
    kMxInt16Class = 10,
    kMxUint16Class = 11,
    kMxInt32Class = 12,
    kMxUint32Class = 13,
    kMxInt64Class = 14,
    kMxUint64Class = 15,
};

inline constexpr u8 kMat5FlagLogical = 0x02u;
inline constexpr u8 kMat5FlagComplex = 0x08u;

inline Allocator* resolve_alloc(Allocator* alloc) noexcept {
    return alloc ? alloc : default_allocator();
}

[[nodiscard]] bool checked_mul(usize a, usize b, usize& out) noexcept {
    if (a == 0 || b == 0) {
        out = 0;
        return true;
    }
    if (a > Limits<usize>::kMax / b) return false;
    out = a * b;
    return true;
}

[[nodiscard]] bool checked_numel(ArrayRef<usize> dims, usize& out) noexcept {
    if (dims.empty()) {
        out = 0;
        return true;
    }
    usize acc = 1;
    for (usize i = 0; i < dims.size(); ++i) {
        if (!checked_mul(acc, dims[i], acc)) return false;
    }
    out = acc;
    return true;
}

[[nodiscard]] Array<usize> normalize_dims(ArrayRef<usize> dims, Allocator* alloc) noexcept {
    Array<usize> out(resolve_alloc(alloc));
    if (dims.empty()) {
        out.push_back(0);
        out.push_back(0);
        return out;
    }
    out.reserve(dims.size() < 2 ? 2 : dims.size());
    for (usize i = 0; i < dims.size(); ++i) out.push_back(dims[i]);
    if (out.size() == 1) out.push_back(1);
    return out;
}

// ============================================================================
// Binary Writer & Reader Helpers
// ============================================================================

class MatWriter {
public:
    Array<u8> bytes;

    explicit MatWriter(Allocator* alloc = nullptr) noexcept : bytes(resolve_alloc(alloc)) {}

    void write_u8(u8 v) noexcept { bytes.push_back(v); }

    void write_u16_le(u16 v) noexcept {
        bytes.push_back(static_cast<u8>(v & 0xFFu));
        bytes.push_back(static_cast<u8>((v >> 8) & 0xFFu));
    }

    void write_u32_le(u32 v) noexcept {
        bytes.push_back(static_cast<u8>(v & 0xFFu));
        bytes.push_back(static_cast<u8>((v >> 8) & 0xFFu));
        bytes.push_back(static_cast<u8>((v >> 16) & 0xFFu));
        bytes.push_back(static_cast<u8>((v >> 24) & 0xFFu));
    }

    void write_bytes(const void* data, usize count) noexcept {
        if (count == 0 || data == nullptr) return;
        const auto* p = static_cast<const u8*>(data);
        bytes.append(ArrayRef<u8>(p, count));
    }

    void write_tag(u32 type, const void* data, u32 byte_count) noexcept {
        if (byte_count > 0 && byte_count <= 4 && type != kMiCompressed) {
            const u32 tag = ((byte_count & 0xFFFFu) << 16) | (type & 0xFFFFu);
            write_u32_le(tag);
            u8 buf[4] = {0, 0, 0, 0};
            if (data != nullptr) {
                keel::mem_copy(buf, data, byte_count);
            }
            write_bytes(buf, 4);
        } else {
            write_u32_le(type);
            write_u32_le(byte_count);
            if (byte_count > 0 && data != nullptr) {
                write_bytes(data, byte_count);
            }
            if (type != kMiCompressed) {
                const usize bc = static_cast<usize>(byte_count);
                const usize pad = ((bc + 7u) & ~usize{7u}) - bc;
                for (usize i = 0; i < pad; ++i) write_u8(0);
            }
        }
    }
};

struct MatTag {
    u32 type = 0;
    u32 byte_count = 0;
    const u8* payload = nullptr;
    usize total_advance = 0;
};

class MatReader {
public:
    const u8* data = nullptr;
    usize len = 0;
    usize pos = 0;
    bool big_endian = false;

    MatReader(const u8* d, usize l, bool be = false) noexcept : data(d), len(l), pos(0), big_endian(be) {}

    [[nodiscard]] bool eof() const noexcept { return pos >= len; }
    [[nodiscard]] usize remaining() const noexcept { return (pos <= len) ? (len - pos) : 0; }

    [[nodiscard]] u16 peek_u16(const u8* p) const noexcept {
        if (big_endian) {
            return static_cast<u16>((static_cast<u16>(p[0]) << 8) | static_cast<u16>(p[1]));
        }
        return static_cast<u16>(static_cast<u16>(p[0]) | (static_cast<u16>(p[1]) << 8));
    }

    [[nodiscard]] u32 peek_u32(const u8* p) const noexcept {
        if (big_endian) {
            return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
                   (static_cast<u32>(p[2]) << 8) | static_cast<u32>(p[3]);
        }
        return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
               (static_cast<u32>(p[3]) << 24);
    }

    void read_scalar_bytes(const u8* p, usize sz, void* dst) const noexcept {
        if (big_endian) {
            auto* d = static_cast<u8*>(dst);
            for (usize k = 0; k < sz; ++k) d[k] = p[sz - 1 - k];
        } else {
            keel::mem_copy(dst, p, sz);
        }
    }

    [[nodiscard]] Result<MatTag> read_tag() noexcept {
        if (pos > len || len - pos < 8) return ErrorCode::EndOfFile;
        const u32 tag0 = peek_u32(data + pos);

        MatTag t;
        if ((tag0 >> 16) != 0u) {
            // Small Data Element Format (SDEF).
            t.type = tag0 & 0xFFFFu;
            t.byte_count = (tag0 >> 16) & 0xFFFFu;
            if (t.byte_count > 4u) return ErrorCode::InvalidData;
            t.payload = data + pos + 4;
            t.total_advance = 8;
            pos += 8;
        } else {
            t.type = tag0;
            t.byte_count = peek_u32(data + pos + 4);
            const usize bc = static_cast<usize>(t.byte_count);
            const usize avail_after_hdr = len - pos - 8;
            if (bc > avail_after_hdr) return ErrorCode::InvalidData;
            t.payload = data + pos + 8;

            usize padded = (bc + 7u) & ~usize{7u};
            if (t.type == kMiCompressed) {
                const usize unpadded_end = pos + 8 + bc;
                if (unpadded_end == len) {
                    padded = bc;
                } else if (len - unpadded_end >= 8) {
                    const u32 next_type = peek_u32(data + unpadded_end);
                    if (next_type == kMiCompressed || next_type == kMiMatrix) {
                        padded = bc;
                    }
                }
            }
            if (padded > avail_after_hdr) padded = bc;
            t.total_advance = 8 + padded;
            pos += t.total_advance;
        }
        return t;
    }
};

// ============================================================================
// MAT5 Encoding
// ============================================================================

[[nodiscard]] u8 mat_class_to_mx(MatClass kind) noexcept {
    switch (kind) {
        case MatClass::Double: return kMxDoubleClass;
        case MatClass::Single: return kMxSingleClass;
        case MatClass::Int8: return kMxInt8Class;
        case MatClass::Uint8: return kMxUint8Class;
        case MatClass::Int16: return kMxInt16Class;
        case MatClass::Uint16: return kMxUint16Class;
        case MatClass::Int32: return kMxInt32Class;
        case MatClass::Uint32: return kMxUint32Class;
        case MatClass::Int64: return kMxInt64Class;
        case MatClass::Uint64: return kMxUint64Class;
        case MatClass::Logical: return kMxUint8Class;
        case MatClass::Char: return kMxCharClass;
        case MatClass::Cell: return kMxCellClass;
        case MatClass::Struct: return kMxStructClass;
        case MatClass::Object: return kMxObjectClass;
        case MatClass::Sparse: return kMxSparseClass;
    }
    return kMxDoubleClass;
}

[[nodiscard]] Mat5DataType mat_class_to_mi(MatClass kind) noexcept {
    switch (kind) {
        case MatClass::Double: return kMiDouble;
        case MatClass::Single: return kMiSingle;
        case MatClass::Int8: return kMiInt8;
        case MatClass::Uint8:
        case MatClass::Logical: return kMiUint8;
        case MatClass::Int16: return kMiInt16;
        case MatClass::Uint16: return kMiUint16;
        case MatClass::Int32: return kMiInt32;
        case MatClass::Uint32: return kMiUint32;
        case MatClass::Int64: return kMiInt64;
        case MatClass::Uint64: return kMiUint64;
        case MatClass::Char: return kMiUtf16;
        default: return kMiDouble;
    }
}

[[nodiscard]] Error encode_mat5_matrix(MatWriter& mw, StringRef name, const MatArray& arr,
                                       Allocator* scratch_alloc) noexcept {
    MatWriter inner(scratch_alloc);

    // 1. Array Flags
    const u8 array_class = mat_class_to_mx(arr.kind);
    u8 flags = 0;
    if (arr.kind == MatClass::Logical) flags |= kMat5FlagLogical;
    if (arr.is_complex) flags |= kMat5FlagComplex;

    u32 nzmax_word = 0;
    if (arr.kind == MatClass::Sparse && arr.composite) {
        nzmax_word = static_cast<u32>(arr.composite->sparse_nzmax);
    }
    const u32 flags_buf[2] = {
        static_cast<u32>(array_class) | (static_cast<u32>(flags) << 8),
        nzmax_word,
    };
    inner.write_tag(kMiUint32, flags_buf, 8);

    // 2. Dimensions Array (at least 2 dims, strip trailing singletons above rank 2)
    usize nd = arr.dims.size();
    while (nd > 2 && arr.dims[nd - 1] == 1) --nd;
    if (nd < 2) nd = 2;

    Array<i32> dims_i32(nd, i32{1}, scratch_alloc);
    for (usize i = 0; i < nd; ++i) {
        const usize di = (i < arr.dims.size()) ? arr.dims[i] : (arr.dims.empty() ? usize{0} : usize{1});
        if (di > static_cast<usize>(Limits<i32>::kMax)) return ErrorCode::OutOfRange;
        dims_i32[i] = static_cast<i32>(di);
    }
    inner.write_tag(kMiInt32, dims_i32.data(), static_cast<u32>(dims_i32.size() * sizeof(i32)));

    // 3. Array Name
    if (name.size() > static_cast<usize>(Limits<u32>::kMax)) return ErrorCode::OutOfRange;
    inner.write_tag(kMiInt8, name.data(), static_cast<u32>(name.size()));

    // 4. Data Sub-elements
    const usize n_elems = arr.numel();

    if (arr.kind == MatClass::Cell) {
        const usize n_cells = arr.num_cells();
        MatArray empty_placeholder(scratch_alloc);
        for (usize i = 0; i < n_elems; ++i) {
            const MatArray& elem = (i < n_cells) ? arr.composite->items[i] : empty_placeholder;
            const Error err = encode_mat5_matrix(inner, StringRef(), elem, scratch_alloc);
            if (!err.is_ok()) return err;
        }
    } else if (arr.kind == MatClass::Struct || arr.kind == MatClass::Object) {
        if (arr.kind == MatClass::Object) {
            const StringRef cname = arr.object_class_name();
            inner.write_tag(kMiInt8, cname.data(), static_cast<u32>(cname.size()));
        }

        const ArrayRef<String> fnames = arr.field_names();
        const usize n_fields = fnames.size();

        // Compute fixed field-name slot width (MAT5 default 32, or longer if any name >= 32 bytes).
        u32 slot_len = 32;
        for (usize fi = 0; fi < n_fields; ++fi) {
            const usize needed = fnames[fi].size() + 1;
            if (needed > static_cast<usize>(slot_len)) {
                slot_len = static_cast<u32>((needed + 3u) & ~usize{3u});
            }
        }
        const i32 slot_len_i32 = static_cast<i32>(slot_len);
        inner.write_tag(kMiInt32, &slot_len_i32, sizeof(i32));

        Array<u8> name_table(n_fields * static_cast<usize>(slot_len), u8{0}, scratch_alloc);
        for (usize fi = 0; fi < n_fields; ++fi) {
            const usize copy_len = keel::min(fnames[fi].size(), static_cast<usize>(slot_len) - 1);
            if (copy_len > 0) {
                keel::mem_copy(name_table.data() + fi * static_cast<usize>(slot_len), fnames[fi].data(), copy_len);
            }
        }
        inner.write_tag(kMiInt8, name_table.data(), static_cast<u32>(name_table.size()));

        MatArray empty_placeholder(scratch_alloc);
        for (usize ei = 0; ei < n_elems; ++ei) {
            for (usize fi = 0; fi < n_fields; ++fi) {
                const MatArray* fv = arr.field(ei, fi);
                const Error err = encode_mat5_matrix(inner, StringRef(), fv ? *fv : empty_placeholder, scratch_alloc);
                if (!err.is_ok()) return err;
            }
        }
    } else if (arr.kind == MatClass::Sparse) {
        if (!arr.composite) return ErrorCode::InvalidArgument;
        const auto& ir = arr.composite->sparse_ir;
        const auto& jc = arr.composite->sparse_jc;
        inner.write_tag(kMiInt32, ir.data(), static_cast<u32>(ir.size() * sizeof(u32)));
        inner.write_tag(kMiInt32, jc.data(), static_cast<u32>(jc.size() * sizeof(u32)));
        inner.write_tag(kMiDouble, arr.data.data(), static_cast<u32>(arr.data.size()));
        if (arr.is_complex) {
            inner.write_tag(kMiDouble, arr.imag_data.data(), static_cast<u32>(arr.imag_data.size()));
        }
    } else if (arr.kind == MatClass::Char) {
        if (arr.data.size() > static_cast<usize>(Limits<u32>::kMax)) return ErrorCode::OutOfRange;
        inner.write_tag(kMiUtf16, arr.data.data(), static_cast<u32>(arr.data.size()));
    } else {
        // Dense numeric or logical.
        const Mat5DataType dt = mat_class_to_mi(arr.kind);
        if (arr.data.size() > static_cast<usize>(Limits<u32>::kMax)) return ErrorCode::OutOfRange;
        inner.write_tag(dt, arr.data.data(), static_cast<u32>(arr.data.size()));
        if (arr.is_complex) {
            if (arr.imag_data.size() > static_cast<usize>(Limits<u32>::kMax)) return ErrorCode::OutOfRange;
            inner.write_tag(dt, arr.imag_data.data(), static_cast<u32>(arr.imag_data.size()));
        }
    }

    if (inner.bytes.size() > static_cast<usize>(Limits<u32>::kMax)) return ErrorCode::OutOfRange;
    mw.write_tag(kMiMatrix, inner.bytes.data(), static_cast<u32>(inner.bytes.size()));
    return Error::ok();
}

// ============================================================================
// MAT5 Decoding
// ============================================================================

[[nodiscard]] Result<f64> read_tag_elem_as_f64(const MatReader& ir, const MatTag& tag, usize idx) noexcept {
    if (tag.payload == nullptr) return ErrorCode::EndOfFile;
    const usize avail = static_cast<usize>(tag.byte_count);
    auto check_bounds = [avail](usize idx0, usize sz) noexcept -> bool {
        return idx0 <= avail / sz && (avail - idx0 * sz) >= sz;
    };

    switch (tag.type) {
        case kMiDouble: {
            if (!check_bounds(idx, 8)) return ErrorCode::EndOfFile;
            f64 v = 0.0;
            ir.read_scalar_bytes(tag.payload + idx * 8, 8, &v);
            return v;
        }
        case kMiSingle: {
            if (!check_bounds(idx, 4)) return ErrorCode::EndOfFile;
            f32 v = 0.0f;
            ir.read_scalar_bytes(tag.payload + idx * 4, 4, &v);
            return static_cast<f64>(v);
        }
        case kMiInt8: {
            if (!check_bounds(idx, 1)) return ErrorCode::EndOfFile;
            return static_cast<f64>(reinterpret_cast<const i8*>(tag.payload)[idx]);
        }
        case kMiUint8: {
            if (!check_bounds(idx, 1)) return ErrorCode::EndOfFile;
            return static_cast<f64>(tag.payload[idx]);
        }
        case kMiInt16: {
            if (!check_bounds(idx, 2)) return ErrorCode::EndOfFile;
            i16 v = 0;
            ir.read_scalar_bytes(tag.payload + idx * 2, 2, &v);
            return static_cast<f64>(v);
        }
        case kMiUint16: {
            if (!check_bounds(idx, 2)) return ErrorCode::EndOfFile;
            u16 v = 0;
            ir.read_scalar_bytes(tag.payload + idx * 2, 2, &v);
            return static_cast<f64>(v);
        }
        case kMiInt32: {
            if (!check_bounds(idx, 4)) return ErrorCode::EndOfFile;
            i32 v = 0;
            ir.read_scalar_bytes(tag.payload + idx * 4, 4, &v);
            return static_cast<f64>(v);
        }
        case kMiUint32: {
            if (!check_bounds(idx, 4)) return ErrorCode::EndOfFile;
            u32 v = 0;
            ir.read_scalar_bytes(tag.payload + idx * 4, 4, &v);
            return static_cast<f64>(v);
        }
        case kMiInt64: {
            if (!check_bounds(idx, 8)) return ErrorCode::EndOfFile;
            i64 v = 0;
            ir.read_scalar_bytes(tag.payload + idx * 8, 8, &v);
            return static_cast<f64>(v);
        }
        case kMiUint64: {
            if (!check_bounds(idx, 8)) return ErrorCode::EndOfFile;
            u64 v = 0;
            ir.read_scalar_bytes(tag.payload + idx * 8, 8, &v);
            return static_cast<f64>(v);
        }
        default:
            return 0.0;
    }
}

[[nodiscard]] Error decode_numeric_buffer(const MatReader& ir, const MatTag& tag, MatClass kind, usize n_elems,
                                          Array<u8>& dst) noexcept {
    const usize elem_sz = mat_class_element_bytes(kind);
    usize total_bytes = 0;
    if (!checked_mul(n_elems, elem_sz, total_bytes)) return ErrorCode::OutOfRange;
    dst.resize(total_bytes);
    if (total_bytes == 0) return Error::ok();
    if (tag.payload == nullptr) return ErrorCode::EndOfFile;

    const bool exact_le_match =
        !ir.big_endian &&
        ((kind == MatClass::Double && tag.type == kMiDouble) || (kind == MatClass::Single && tag.type == kMiSingle) ||
         (kind == MatClass::Int8 && tag.type == kMiInt8) || (kind == MatClass::Uint8 && tag.type == kMiUint8) ||
         (kind == MatClass::Int16 && tag.type == kMiInt16) || (kind == MatClass::Uint16 && tag.type == kMiUint16) ||
         (kind == MatClass::Int32 && tag.type == kMiInt32) || (kind == MatClass::Uint32 && tag.type == kMiUint32) ||
         (kind == MatClass::Int64 && tag.type == kMiInt64) || (kind == MatClass::Uint64 && tag.type == kMiUint64) ||
         (kind == MatClass::Logical && (tag.type == kMiUint8 || tag.type == kMiInt8)));

    if (exact_le_match) {
        if (static_cast<usize>(tag.byte_count) < total_bytes) return ErrorCode::EndOfFile;
        keel::mem_copy(dst.data(), tag.payload, total_bytes);
        if (kind == MatClass::Logical) {
            for (usize i = 0; i < n_elems; ++i) dst[i] = (dst[i] != 0) ? u8{1} : u8{0};
        }
        return Error::ok();
    }

    // Exact big-endian byte-swap path (avoids precision loss for i64/u64).
    const bool exact_be_match =
        ir.big_endian &&
        ((kind == MatClass::Double && tag.type == kMiDouble) || (kind == MatClass::Single && tag.type == kMiSingle) ||
         (kind == MatClass::Int16 && tag.type == kMiInt16) || (kind == MatClass::Uint16 && tag.type == kMiUint16) ||
         (kind == MatClass::Int32 && tag.type == kMiInt32) || (kind == MatClass::Uint32 && tag.type == kMiUint32) ||
         (kind == MatClass::Int64 && tag.type == kMiInt64) || (kind == MatClass::Uint64 && tag.type == kMiUint64));
    if (exact_be_match) {
        if (static_cast<usize>(tag.byte_count) < total_bytes) return ErrorCode::EndOfFile;
        for (usize i = 0; i < n_elems; ++i) {
            ir.read_scalar_bytes(tag.payload + i * elem_sz, elem_sz, dst.data() + i * elem_sz);
        }
        return Error::ok();
    }

    // General MAT5 automatic numeric compression widening/conversion path.
    for (usize i = 0; i < n_elems; ++i) {
        auto val_res = read_tag_elem_as_f64(ir, tag, i);
        if (!val_res.is_ok()) return val_res.error();
        const f64 v = val_res.value();
        switch (kind) {
            case MatClass::Double: {
                const f64 x = v;
                keel::mem_copy(dst.data() + i * 8, &x, 8);
                break;
            }
            case MatClass::Single: {
                const f32 x = static_cast<f32>(v);
                keel::mem_copy(dst.data() + i * 4, &x, 4);
                break;
            }
            case MatClass::Logical: {
                dst[i] = (v != 0.0) ? u8{1} : u8{0};
                break;
            }
            case MatClass::Int8: {
                const i8 x = static_cast<i8>(v);
                keel::mem_copy(dst.data() + i, &x, 1);
                break;
            }
            case MatClass::Uint8: {
                dst[i] = static_cast<u8>(v);
                break;
            }
            case MatClass::Int16: {
                const i16 x = static_cast<i16>(v);
                keel::mem_copy(dst.data() + i * 2, &x, 2);
                break;
            }
            case MatClass::Uint16: {
                const u16 x = static_cast<u16>(v);
                keel::mem_copy(dst.data() + i * 2, &x, 2);
                break;
            }
            case MatClass::Int32: {
                const i32 x = static_cast<i32>(v);
                keel::mem_copy(dst.data() + i * 4, &x, 4);
                break;
            }
            case MatClass::Uint32: {
                const u32 x = static_cast<u32>(v);
                keel::mem_copy(dst.data() + i * 4, &x, 4);
                break;
            }
            case MatClass::Int64: {
                const i64 x = static_cast<i64>(v);
                keel::mem_copy(dst.data() + i * 8, &x, 8);
                break;
            }
            case MatClass::Uint64: {
                const u64 x = static_cast<u64>(v);
                keel::mem_copy(dst.data() + i * 8, &x, 8);
                break;
            }
            default:
                break;
        }
    }
    return Error::ok();
}

[[nodiscard]] Result<MatVar> decode_mat5_matrix(MatReader& mr, const MatReadOptions& opts, Allocator* alloc,
                                                usize depth) noexcept {
    if (depth > opts.max_nesting_depth) return ErrorCode::InvalidData;

    auto matrix_tag_res = mr.read_tag();
    if (!matrix_tag_res.is_ok()) return matrix_tag_res.error();
    const MatTag matrix_tag = matrix_tag_res.value();
    if (matrix_tag.type != kMiMatrix) return ErrorCode::InvalidData;

    MatReader ir(matrix_tag.payload, matrix_tag.byte_count, mr.big_endian);

    // 1. Array Flags
    auto flags_tag_res = ir.read_tag();
    if (!flags_tag_res.is_ok()) return flags_tag_res.error();
    const MatTag flags_tag = flags_tag_res.value();
    if (flags_tag.byte_count < 4u || flags_tag.payload == nullptr) return ErrorCode::InvalidData;
    const u32 flags_word = ir.peek_u32(flags_tag.payload);
    const u32 nzmax_word = (flags_tag.byte_count >= 8u) ? ir.peek_u32(flags_tag.payload + 4) : 0u;
    const u8 array_class = static_cast<u8>(flags_word & 0xFFu);
    const u8 flags = static_cast<u8>((flags_word >> 8) & 0xFFu);
    const bool is_logical = (flags & kMat5FlagLogical) != 0u;
    const bool is_complex = (flags & kMat5FlagComplex) != 0u;

    // 2. Dimensions Array
    auto dims_tag_res = ir.read_tag();
    if (!dims_tag_res.is_ok()) return dims_tag_res.error();
    const MatTag dims_tag = dims_tag_res.value();
    const usize raw_nd = static_cast<usize>(dims_tag.byte_count) / sizeof(i32);
    if (raw_nd > opts.max_rank) return ErrorCode::OutOfRange;
    const usize nd = (raw_nd < 2) ? 2 : raw_nd;

    MatVar var(alloc);
    var.value.dims.resize(nd);
    usize n_elems = 1;
    for (usize i = 0; i < nd; ++i) {
        usize di = 1;
        if (i < raw_nd) {
            if (dims_tag.payload == nullptr) return ErrorCode::InvalidData;
            const i32 raw_dim = static_cast<i32>(ir.peek_u32(dims_tag.payload + i * 4));
            if (raw_dim < 0) return ErrorCode::InvalidData;
            di = static_cast<usize>(raw_dim);
        }
        var.value.dims[i] = di;
        if (!checked_mul(n_elems, di, n_elems)) return ErrorCode::OutOfRange;
    }

    // 3. Array Name
    auto name_tag_res = ir.read_tag();
    if (!name_tag_res.is_ok()) return name_tag_res.error();
    const MatTag name_tag = name_tag_res.value();
    if (name_tag.byte_count > 0 && name_tag.payload != nullptr) {
        var.name = String::from(StringRef(reinterpret_cast<const char*>(name_tag.payload), name_tag.byte_count), alloc);
        while (!var.name.empty() && var.name.back() == '\0') var.name.pop_back();
    }

    // 4. Data Sub-elements
    if (array_class == kMxCellClass) {
        var.value.kind = MatClass::Cell;
        var.value.composite = make_unique_in<MatComposite>(alloc, alloc);
        var.value.composite->items.reserve(n_elems);
        for (usize i = 0; i < n_elems; ++i) {
            auto child = decode_mat5_matrix(ir, opts, alloc, depth + 1);
            if (!child.is_ok()) return child.error();
            var.value.composite->items.push_back(keel::move(child.value().value));
        }
    } else if (array_class == kMxStructClass || array_class == kMxObjectClass) {
        var.value.kind = (array_class == kMxObjectClass) ? MatClass::Object : MatClass::Struct;
        var.value.composite = make_unique_in<MatComposite>(alloc, alloc);

        if (array_class == kMxObjectClass) {
            auto cname_tag_res = ir.read_tag();
            if (!cname_tag_res.is_ok()) return cname_tag_res.error();
            const MatTag cname_tag = cname_tag_res.value();
            if (cname_tag.payload != nullptr && cname_tag.byte_count > 0) {
                var.value.composite->class_name = String::from(
                    StringRef(reinterpret_cast<const char*>(cname_tag.payload), cname_tag.byte_count), alloc);
                while (!var.value.composite->class_name.empty() && var.value.composite->class_name.back() == '\0') {
                    var.value.composite->class_name.pop_back();
                }
            }
        }

        auto fnamelen_tag_res = ir.read_tag();
        if (!fnamelen_tag_res.is_ok()) return fnamelen_tag_res.error();
        const MatTag fnamelen_tag = fnamelen_tag_res.value();
        u32 fn_len = 32;
        if (fnamelen_tag.byte_count >= 4 && fnamelen_tag.payload != nullptr) {
            fn_len = ir.peek_u32(fnamelen_tag.payload);
        }
        if (fn_len == 0) fn_len = 32;

        auto fnames_tag_res = ir.read_tag();
        if (!fnames_tag_res.is_ok()) return fnames_tag_res.error();
        const MatTag fnames_tag = fnames_tag_res.value();
        const usize num_fields = static_cast<usize>(fnames_tag.byte_count) / static_cast<usize>(fn_len);
        var.value.composite->field_names.reserve(num_fields);
        for (usize fi = 0; fi < num_fields; ++fi) {
            const char* fn_ptr = reinterpret_cast<const char*>(fnames_tag.payload + fi * static_cast<usize>(fn_len));
            usize cur_len = 0;
            while (cur_len < static_cast<usize>(fn_len) && fn_ptr[cur_len] != '\0') ++cur_len;
            var.value.composite->field_names.push_back(String::from(StringRef(fn_ptr, cur_len), alloc));
        }

        usize total_slots = 0;
        if (!checked_mul(num_fields, n_elems, total_slots)) return ErrorCode::OutOfRange;
        var.value.composite->items.reserve(total_slots);
        for (usize i = 0; i < total_slots; ++i) {
            var.value.composite->items.push_back(MatArray(alloc));
        }
        for (usize ei = 0; ei < n_elems; ++ei) {
            for (usize fi = 0; fi < num_fields; ++fi) {
                auto child = decode_mat5_matrix(ir, opts, alloc, depth + 1);
                if (!child.is_ok()) return child.error();
                var.value.composite->items[fi * n_elems + ei] = keel::move(child.value().value);
            }
        }
    } else if (array_class == kMxSparseClass) {
        var.value.kind = MatClass::Sparse;
        var.value.is_complex = is_complex;
        var.value.composite = make_unique_in<MatComposite>(alloc, alloc);
        var.value.composite->sparse_nzmax = static_cast<usize>(nzmax_word);

        auto ir_tag_res = ir.read_tag();
        if (!ir_tag_res.is_ok()) return ir_tag_res.error();
        const MatTag ir_tag = ir_tag_res.value();
        const usize ir_count = static_cast<usize>(ir_tag.byte_count) / sizeof(u32);
        var.value.composite->sparse_ir.resize(ir_count);
        for (usize i = 0; i < ir_count; ++i) {
            var.value.composite->sparse_ir[i] = ir.peek_u32(ir_tag.payload + i * 4);
        }

        auto jc_tag_res = ir.read_tag();
        if (!jc_tag_res.is_ok()) return jc_tag_res.error();
        const MatTag jc_tag = jc_tag_res.value();
        const usize jc_count = static_cast<usize>(jc_tag.byte_count) / sizeof(u32);
        var.value.composite->sparse_jc.resize(jc_count);
        for (usize i = 0; i < jc_count; ++i) {
            var.value.composite->sparse_jc[i] = ir.peek_u32(jc_tag.payload + i * 4);
        }

        const usize nnz = (jc_count > 0) ? static_cast<usize>(var.value.composite->sparse_jc[jc_count - 1]) : 0;
        const MatClass val_kind = is_logical ? MatClass::Logical : MatClass::Double;

        auto pr_tag_res = ir.read_tag();
        if (!pr_tag_res.is_ok()) return pr_tag_res.error();
        const Error pr_err = decode_numeric_buffer(ir, pr_tag_res.value(), val_kind, nnz, var.value.data);
        if (!pr_err.is_ok()) return pr_err;

        if (is_complex) {
            auto pi_tag_res = ir.read_tag();
            if (!pi_tag_res.is_ok()) return pi_tag_res.error();
            const Error pi_err = decode_numeric_buffer(ir, pi_tag_res.value(), val_kind, nnz, var.value.imag_data);
            if (!pi_err.is_ok()) return pi_err;
        }
    } else if (array_class == kMxCharClass) {
        var.value.kind = MatClass::Char;
        auto data_tag_res = ir.read_tag();
        if (!data_tag_res.is_ok()) return data_tag_res.error();
        const MatTag data_tag = data_tag_res.value();

        usize total_bytes = 0;
        if (!checked_mul(n_elems, usize{2}, total_bytes)) return ErrorCode::OutOfRange;
        var.value.data.assign(total_bytes, u8{0});
        auto* out_u16 = reinterpret_cast<u16*>(var.value.data.data());

        if (data_tag.type == kMiUint16 || data_tag.type == kMiUtf16) {
            const usize char_count = keel::min(n_elems, static_cast<usize>(data_tag.byte_count) / 2);
            if (char_count > 0 && data_tag.payload != nullptr) {
                for (usize i = 0; i < char_count; ++i) {
                    out_u16[i] = ir.peek_u16(data_tag.payload + i * 2);
                }
            }
        } else if (data_tag.type == kMiUtf8 || data_tag.type == kMiUint8 || data_tag.type == kMiInt8) {
            const usize char_count = keel::min(n_elems, static_cast<usize>(data_tag.byte_count));
            if (char_count > 0 && data_tag.payload != nullptr) {
                for (usize i = 0; i < char_count; ++i) {
                    out_u16[i] = static_cast<u16>(data_tag.payload[i]);
                }
            }
        }
    } else {
        MatClass kind = MatClass::Double;
        if (is_logical) {
            kind = MatClass::Logical;
        } else {
            switch (array_class) {
                case kMxDoubleClass: kind = MatClass::Double; break;
                case kMxSingleClass: kind = MatClass::Single; break;
                case kMxInt8Class: kind = MatClass::Int8; break;
                case kMxUint8Class: kind = MatClass::Uint8; break;
                case kMxInt16Class: kind = MatClass::Int16; break;
                case kMxUint16Class: kind = MatClass::Uint16; break;
                case kMxInt32Class: kind = MatClass::Int32; break;
                case kMxUint32Class: kind = MatClass::Uint32; break;
                case kMxInt64Class: kind = MatClass::Int64; break;
                case kMxUint64Class: kind = MatClass::Uint64; break;
                default: kind = MatClass::Double; break;
            }
        }
        var.value.kind = kind;
        var.value.is_complex = is_complex;

        auto real_tag_res = ir.read_tag();
        if (!real_tag_res.is_ok()) return real_tag_res.error();
        const Error re_err = decode_numeric_buffer(ir, real_tag_res.value(), kind, n_elems, var.value.data);
        if (!re_err.is_ok()) return re_err;

        if (is_complex) {
            auto imag_tag_res = ir.read_tag();
            if (!imag_tag_res.is_ok()) return imag_tag_res.error();
            const Error im_err = decode_numeric_buffer(ir, imag_tag_res.value(), kind, n_elems, var.value.imag_data);
            if (!im_err.is_ok()) return im_err;
        }
    }

    return var;
}

// ============================================================================
// MAT4 Codec
// ============================================================================

[[nodiscard]] Result<Array<u8>> encode_mat4(ArrayRef<MatVar> vars, Allocator* alloc) noexcept {
    MatWriter mw(alloc);
    for (usize vi = 0; vi < vars.size(); ++vi) {
        const MatVar& entry = vars[vi];
        const MatArray& arr = entry.value;
        if (!arr.is_numeric() && arr.kind != MatClass::Char) continue;

        const usize r = arr.rows();
        const usize c = arr.cols();
        if (r > static_cast<usize>(Limits<i32>::kMax) || c > static_cast<usize>(Limits<i32>::kMax)) {
            return ErrorCode::OutOfRange;
        }

        i32 p_type = 0;
        if (arr.kind == MatClass::Single) p_type = 1;
        else if (arr.kind == MatClass::Int32) p_type = 2;
        else if (arr.kind == MatClass::Int16) p_type = 3;
        else if (arr.kind == MatClass::Uint16) p_type = 4;
        else if (arr.kind == MatClass::Uint8 || arr.kind == MatClass::Char) p_type = 5;

        const i32 t_type = (arr.kind == MatClass::Char) ? 1 : 0;
        const i32 m_type = p_type * 10 + t_type;
        const i32 imagf = arr.is_complex ? 1 : 0;
        const usize namlen_u = entry.name.size() + 1;
        if (namlen_u > 256) return ErrorCode::OutOfRange;

        mw.write_u32_le(static_cast<u32>(m_type));
        mw.write_u32_le(static_cast<u32>(r));
        mw.write_u32_le(static_cast<u32>(c));
        mw.write_u32_le(static_cast<u32>(imagf));
        mw.write_u32_le(static_cast<u32>(namlen_u));
        mw.write_bytes(entry.name.c_str(), namlen_u);

        if (arr.kind == MatClass::Char) {
            const ArrayRef<u16> u16s = arr.as_utf16();
            for (usize i = 0; i < u16s.size(); ++i) {
                mw.write_u8(static_cast<u8>(u16s[i] & 0xFFu));
            }
        } else {
            mw.write_bytes(arr.data.data(), arr.data.size());
            if (arr.is_complex) {
                mw.write_bytes(arr.imag_data.data(), arr.imag_data.size());
            }
        }
    }
    return keel::move(mw.bytes);
}

[[nodiscard]] Result<MatFile> decode_mat4(ArrayRef<u8> bytes, Allocator* alloc) noexcept {
    MatFile file(alloc);
    file.version = MatVersion::V4;
    file.big_endian = false;

    const u8* p = bytes.data();
    const usize len = bytes.size();
    usize pos = 0;

    auto read_i32_le = [](const u8* src) noexcept -> i32 {
        const u32 u = static_cast<u32>(src[0]) | (static_cast<u32>(src[1]) << 8) | (static_cast<u32>(src[2]) << 16) |
                      (static_cast<u32>(src[3]) << 24);
        return static_cast<i32>(u);
    };

    while (len - pos >= 20) {
        const i32 m_type = read_i32_le(p + pos);
        const i32 rows = read_i32_le(p + pos + 4);
        const i32 cols = read_i32_le(p + pos + 8);
        const i32 imagf = read_i32_le(p + pos + 12);
        const i32 namlen = read_i32_le(p + pos + 16);
        pos += 20;

        const i32 m_endian = m_type / 1000;
        const i32 m_format = (m_type / 100) % 10;
        const i32 p_type = (m_type / 10) % 10;
        const i32 t_type = m_type % 10;

        if (m_endian < 0 || m_endian > 4 || m_format != 0 || p_type < 0 || p_type > 5 || t_type < 0 || t_type > 2) {
            return ErrorCode::InvalidData;
        }
        if (rows < 0 || cols < 0 || (imagf != 0 && imagf != 1) || namlen <= 0 || namlen > 256) {
            return ErrorCode::InvalidData;
        }

        const usize name_bytes = static_cast<usize>(namlen);
        if (name_bytes > len - pos) return ErrorCode::EndOfFile;

        usize actual_name_len = name_bytes;
        for (usize i = 0; i < name_bytes; ++i) {
            if (p[pos + i] == 0u) {
                actual_name_len = i;
                break;
            }
        }
        if (actual_name_len == name_bytes) return ErrorCode::InvalidData; // Missing NUL terminator.
        String name = String::from(StringRef(reinterpret_cast<const char*>(p + pos), actual_name_len), alloc);
        pos += name_bytes;

        const usize r_sz = static_cast<usize>(rows);
        const usize c_sz = static_cast<usize>(cols);
        usize n_elems = 0;
        if (!checked_mul(r_sz, c_sz, n_elems)) return ErrorCode::OutOfRange;

        MatClass kind = MatClass::Double;
        usize src_elem_sz = 8;
        if (p_type == 1) {
            kind = MatClass::Single;
            src_elem_sz = 4;
        } else if (p_type == 2) {
            kind = MatClass::Int32;
            src_elem_sz = 4;
        } else if (p_type == 3) {
            kind = MatClass::Int16;
            src_elem_sz = 2;
        } else if (p_type == 4) {
            kind = MatClass::Uint16;
            src_elem_sz = 2;
        } else if (p_type == 5) {
            kind = MatClass::Uint8;
            src_elem_sz = 1;
        }
        if (t_type == 1) kind = MatClass::Char;

        const usize dims_arr[2] = {r_sz, c_sz};
        MatArray arr = MatArray::make_dense(kind, ArrayRef<usize>(dims_arr, 2), imagf == 1, alloc);

        if (kind == MatClass::Char) {
            usize data_bytes = 0;
            if (!checked_mul(n_elems, src_elem_sz, data_bytes)) return ErrorCode::OutOfRange;
            if (data_bytes > len - pos) return ErrorCode::EndOfFile;
            auto* out_u16 = reinterpret_cast<u16*>(arr.data.data());
            if (src_elem_sz == 1) {
                for (usize i = 0; i < n_elems; ++i) out_u16[i] = static_cast<u16>(p[pos + i]);
            } else if (src_elem_sz == 8) {
                for (usize i = 0; i < n_elems; ++i) {
                    f64 v = 0.0;
                    keel::mem_copy(&v, p + pos + i * 8, 8);
                    out_u16[i] = static_cast<u16>(v);
                }
            } else if (src_elem_sz == 2) {
                keel::mem_copy(out_u16, p + pos, n_elems * 2);
            } else if (src_elem_sz == 4) {
                for (usize i = 0; i < n_elems; ++i) {
                    u32 v = 0;
                    keel::mem_copy(&v, p + pos + i * 4, 4);
                    out_u16[i] = static_cast<u16>(v);
                }
            }
            pos += data_bytes;
        } else if (imagf == 1) {
            usize plane_bytes = 0;
            usize total_bytes = 0;
            if (!checked_mul(n_elems, src_elem_sz, plane_bytes)) return ErrorCode::OutOfRange;
            if (!checked_mul(plane_bytes, usize{2}, total_bytes)) return ErrorCode::OutOfRange;
            if (total_bytes > len - pos) return ErrorCode::EndOfFile;
            if (plane_bytes > 0) {
                keel::mem_copy(arr.data.data(), p + pos, plane_bytes);
                keel::mem_copy(arr.imag_data.data(), p + pos + plane_bytes, plane_bytes);
            }
            pos += total_bytes;
        } else {
            usize data_bytes = 0;
            if (!checked_mul(n_elems, src_elem_sz, data_bytes)) return ErrorCode::OutOfRange;
            if (data_bytes > len - pos) return ErrorCode::EndOfFile;
            if (data_bytes > 0) {
                keel::mem_copy(arr.data.data(), p + pos, data_bytes);
                pos += data_bytes;
            }
        }

        if (!name.empty()) {
            file.vars.push_back(MatVar(keel::move(name), keel::move(arr)));
        }
    }

    if (file.vars.empty() && len > 0) return ErrorCode::InvalidData;
    return file;
}

} // namespace

// ============================================================================
// MatArray & MatFile Methods
// ============================================================================

usize MatArray::numel() const noexcept {
    usize n = 0;
    return checked_numel(ArrayRef<usize>(dims.data(), dims.size()), n) ? n : 0;
}

ArrayRef<f64> MatArray::as_f64() const noexcept {
    if (kind != MatClass::Double && kind != MatClass::Sparse) return {};
    return ArrayRef<f64>(reinterpret_cast<const f64*>(data.data()), data.size() / sizeof(f64));
}

MutArrayRef<f64> MatArray::as_f64_mut() noexcept {
    if (kind != MatClass::Double && kind != MatClass::Sparse) return {};
    return MutArrayRef<f64>(reinterpret_cast<f64*>(data.data()), data.size() / sizeof(f64));
}

ArrayRef<f32> MatArray::as_f32() const noexcept {
    if (kind != MatClass::Single) return {};
    return ArrayRef<f32>(reinterpret_cast<const f32*>(data.data()), data.size() / sizeof(f32));
}

MutArrayRef<f32> MatArray::as_f32_mut() noexcept {
    if (kind != MatClass::Single) return {};
    return MutArrayRef<f32>(reinterpret_cast<f32*>(data.data()), data.size() / sizeof(f32));
}

ArrayRef<i8> MatArray::as_i8() const noexcept {
    if (kind != MatClass::Int8) return {};
    return ArrayRef<i8>(reinterpret_cast<const i8*>(data.data()), data.size());
}

ArrayRef<u8> MatArray::as_u8() const noexcept {
    if (kind != MatClass::Uint8) return {};
    return ArrayRef<u8>(data.data(), data.size());
}

ArrayRef<i16> MatArray::as_i16() const noexcept {
    if (kind != MatClass::Int16) return {};
    return ArrayRef<i16>(reinterpret_cast<const i16*>(data.data()), data.size() / sizeof(i16));
}

ArrayRef<u16> MatArray::as_u16() const noexcept {
    if (kind != MatClass::Uint16) return {};
    return ArrayRef<u16>(reinterpret_cast<const u16*>(data.data()), data.size() / sizeof(u16));
}

ArrayRef<i32> MatArray::as_i32() const noexcept {
    if (kind != MatClass::Int32) return {};
    return ArrayRef<i32>(reinterpret_cast<const i32*>(data.data()), data.size() / sizeof(i32));
}

ArrayRef<u32> MatArray::as_u32() const noexcept {
    if (kind != MatClass::Uint32) return {};
    return ArrayRef<u32>(reinterpret_cast<const u32*>(data.data()), data.size() / sizeof(u32));
}

ArrayRef<i64> MatArray::as_i64() const noexcept {
    if (kind != MatClass::Int64) return {};
    return ArrayRef<i64>(reinterpret_cast<const i64*>(data.data()), data.size() / sizeof(i64));
}

ArrayRef<u64> MatArray::as_u64() const noexcept {
    if (kind != MatClass::Uint64) return {};
    return ArrayRef<u64>(reinterpret_cast<const u64*>(data.data()), data.size() / sizeof(u64));
}

ArrayRef<u8> MatArray::as_logical() const noexcept {
    if (kind != MatClass::Logical) return {};
    return ArrayRef<u8>(data.data(), data.size());
}

ArrayRef<u16> MatArray::as_utf16() const noexcept {
    if (kind != MatClass::Char) return {};
    return ArrayRef<u16>(reinterpret_cast<const u16*>(data.data()), data.size() / sizeof(u16));
}

ArrayRef<f64> MatArray::imag_f64() const noexcept {
    if (!is_complex || (kind != MatClass::Double && kind != MatClass::Sparse)) return {};
    return ArrayRef<f64>(reinterpret_cast<const f64*>(imag_data.data()), imag_data.size() / sizeof(f64));
}

MutArrayRef<f64> MatArray::imag_f64_mut() noexcept {
    if (!is_complex || (kind != MatClass::Double && kind != MatClass::Sparse)) return {};
    return MutArrayRef<f64>(reinterpret_cast<f64*>(imag_data.data()), imag_data.size() / sizeof(f64));
}

ArrayRef<f32> MatArray::imag_f32() const noexcept {
    if (!is_complex || kind != MatClass::Single) return {};
    return ArrayRef<f32>(reinterpret_cast<const f32*>(imag_data.data()), imag_data.size() / sizeof(f32));
}

MutArrayRef<f32> MatArray::imag_f32_mut() noexcept {
    if (!is_complex || kind != MatClass::Single) return {};
    return MutArrayRef<f32>(reinterpret_cast<f32*>(imag_data.data()), imag_data.size() / sizeof(f32));
}

String MatArray::to_ascii(Allocator* alloc) const noexcept {
    const ArrayRef<u16> u16s = as_utf16();
    String out(resolve_alloc(alloc));
    out.reserve(u16s.size());
    for (usize i = 0; i < u16s.size(); ++i) {
        out.push_back(static_cast<char>(u16s[i] & 0xFFu));
    }
    return out;
}

usize MatArray::num_cells() const noexcept {
    return (kind == MatClass::Cell && composite) ? composite->items.size() : 0;
}

const MatArray* MatArray::cell(usize idx) const noexcept {
    if (kind != MatClass::Cell || !composite || idx >= composite->items.size()) return nullptr;
    return &composite->items[idx];
}

MatArray* MatArray::cell_mut(usize idx) noexcept {
    if (kind != MatClass::Cell || !composite || idx >= composite->items.size()) return nullptr;
    return &composite->items[idx];
}

StringRef MatArray::object_class_name() const noexcept {
    return (kind == MatClass::Object && composite) ? composite->class_name.as_ref() : StringRef();
}

usize MatArray::num_fields() const noexcept {
    return ((kind == MatClass::Struct || kind == MatClass::Object) && composite) ? composite->field_names.size() : 0;
}

ArrayRef<String> MatArray::field_names() const noexcept {
    if ((kind != MatClass::Struct && kind != MatClass::Object) || !composite) return {};
    return ArrayRef<String>(composite->field_names.data(), composite->field_names.size());
}

Optional<usize> MatArray::find_field(StringRef name) const noexcept {
    const ArrayRef<String> names = field_names();
    for (usize i = 0; i < names.size(); ++i) {
        if (names[i].as_ref() == name) return i;
    }
    return {};
}

const MatArray* MatArray::field(usize elem_idx, usize field_idx) const noexcept {
    if ((kind != MatClass::Struct && kind != MatClass::Object) || !composite) return nullptr;
    const usize n = numel();
    if (elem_idx >= n || field_idx >= composite->field_names.size()) return nullptr;
    const usize slot = field_idx * n + elem_idx;
    if (slot >= composite->items.size()) return nullptr;
    return &composite->items[slot];
}

MatArray* MatArray::field_mut(usize elem_idx, usize field_idx) noexcept {
    if ((kind != MatClass::Struct && kind != MatClass::Object) || !composite) return nullptr;
    const usize n = numel();
    if (elem_idx >= n || field_idx >= composite->field_names.size()) return nullptr;
    const usize slot = field_idx * n + elem_idx;
    if (slot >= composite->items.size()) return nullptr;
    return &composite->items[slot];
}

const MatArray* MatArray::field(StringRef name, usize elem_idx) const noexcept {
    const Optional<usize> idx = find_field(name);
    if (!idx.has_value()) return nullptr;
    return field(elem_idx, idx.value());
}

MatArray MatArray::make_dense(MatClass kind, ArrayRef<usize> dims, bool complex, Allocator* alloc) noexcept {
    Allocator* a = resolve_alloc(alloc);
    MatArray arr(a);
    arr.kind = kind;
    arr.is_complex = complex;
    arr.dims = normalize_dims(dims, a);
    const usize elem_sz = mat_class_element_bytes(kind);
    const usize n = arr.numel();
    usize total_bytes = 0;
    if (checked_mul(n, elem_sz, total_bytes)) {
        arr.data.assign(total_bytes, u8{0});
        if (complex) arr.imag_data.assign(total_bytes, u8{0});
    }
    return arr;
}

MatArray MatArray::from_f64(ArrayRef<usize> dims, ArrayRef<f64> vals, Allocator* alloc) noexcept {
    MatArray arr = make_dense(MatClass::Double, dims, false, alloc);
    const usize count = keel::min(arr.numel(), vals.size());
    if (count > 0) keel::mem_copy(arr.data.data(), vals.data(), count * sizeof(f64));
    return arr;
}

MatArray MatArray::from_c64(ArrayRef<usize> dims, ArrayRef<c64> vals, Allocator* alloc) noexcept {
    MatArray arr = make_dense(MatClass::Double, dims, true, alloc);
    const usize count = keel::min(arr.numel(), vals.size());
    MutArrayRef<f64> re = arr.as_f64_mut();
    MutArrayRef<f64> im = arr.imag_f64_mut();
    for (usize i = 0; i < count; ++i) {
        re[i] = vals[i].real();
        im[i] = vals[i].imag();
    }
    return arr;
}

MatArray MatArray::from_f32(ArrayRef<usize> dims, ArrayRef<f32> vals, Allocator* alloc) noexcept {
    MatArray arr = make_dense(MatClass::Single, dims, false, alloc);
    const usize count = keel::min(arr.numel(), vals.size());
    if (count > 0) keel::mem_copy(arr.data.data(), vals.data(), count * sizeof(f32));
    return arr;
}

MatArray MatArray::from_logical(ArrayRef<usize> dims, ArrayRef<u8> vals, Allocator* alloc) noexcept {
    MatArray arr = make_dense(MatClass::Logical, dims, false, alloc);
    const usize count = keel::min(arr.numel(), vals.size());
    for (usize i = 0; i < count; ++i) arr.data[i] = (vals[i] != 0) ? u8{1} : u8{0};
    return arr;
}

MatArray MatArray::from_ascii(ArrayRef<usize> dims, StringRef chars, Allocator* alloc) noexcept {
    MatArray arr = make_dense(MatClass::Char, dims, false, alloc);
    const usize count = keel::min(arr.numel(), chars.size());
    auto* dst = reinterpret_cast<u16*>(arr.data.data());
    for (usize i = 0; i < count; ++i) dst[i] = static_cast<u8>(chars[i]);
    return arr;
}

MatArray MatArray::from_string(StringRef str, Allocator* alloc) noexcept {
    const usize d[2] = {str.empty() ? usize{0} : usize{1}, str.size()};
    return from_ascii(ArrayRef<usize>(d, 2), str, alloc);
}

MatArray MatArray::make_cell(ArrayRef<usize> dims, Allocator* alloc) noexcept {
    Allocator* a = resolve_alloc(alloc);
    MatArray arr(a);
    arr.kind = MatClass::Cell;
    arr.dims = normalize_dims(dims, a);
    arr.composite = make_unique_in<MatComposite>(a, a);
    const usize n = arr.numel();
    arr.composite->items.reserve(n);
    for (usize i = 0; i < n; ++i) arr.composite->items.push_back(MatArray(a));
    return arr;
}

MatArray MatArray::make_struct(ArrayRef<usize> dims, ArrayRef<StringRef> names, Allocator* alloc) noexcept {
    Allocator* a = resolve_alloc(alloc);
    MatArray arr(a);
    arr.kind = MatClass::Struct;
    arr.dims = normalize_dims(dims, a);
    arr.composite = make_unique_in<MatComposite>(a, a);
    arr.composite->field_names.reserve(names.size());
    for (usize i = 0; i < names.size(); ++i) arr.composite->field_names.push_back(String::from(names[i], a));
    const usize n = arr.numel();
    usize slots = 0;
    if (checked_mul(names.size(), n, slots)) {
        arr.composite->items.reserve(slots);
        for (usize i = 0; i < slots; ++i) arr.composite->items.push_back(MatArray(a));
    }
    return arr;
}

MatArray MatArray::make_object(StringRef class_name, ArrayRef<usize> dims, ArrayRef<StringRef> names,
                               Allocator* alloc) noexcept {
    MatArray arr = make_struct(dims, names, alloc);
    arr.kind = MatClass::Object;
    arr.composite->class_name = String::from(class_name, resolve_alloc(alloc));
    return arr;
}

const MatArray* MatFile::find(StringRef name) const noexcept {
    for (usize i = 0; i < vars.size(); ++i) {
        if (vars[i].name.as_ref() == name) return &vars[i].value;
    }
    return nullptr;
}

MatArray* MatFile::find_mut(StringRef name) noexcept {
    for (usize i = 0; i < vars.size(); ++i) {
        if (vars[i].name.as_ref() == name) return &vars[i].value;
    }
    return nullptr;
}

// ============================================================================
// Public Codec Entry Points
// ============================================================================

Optional<MatVersion> mat_detect_version(ArrayRef<u8> bytes) noexcept {
    if (bytes.size() < 16) return {};
    if (bytes.size() >= 128 &&
        ((bytes[126] == static_cast<u8>('I') && bytes[127] == static_cast<u8>('M')) ||
         (bytes[126] == static_cast<u8>('M') && bytes[127] == static_cast<u8>('I')))) {
        const bool be = (bytes[126] == static_cast<u8>('M'));
        if (bytes.size() >= 136) {
            const MatReader mr(bytes.data() + 128, bytes.size() - 128, be);
            const u32 first_type = mr.peek_u32(bytes.data() + 128);
            if (first_type == kMiCompressed) return MatVersion::V7;
        }
        return MatVersion::V6;
    }
    if (bytes.size() >= 20) return MatVersion::V4;
    return {};
}

Result<MatFile> mat_decode(ArrayRef<u8> bytes, const MatReadOptions& opts) noexcept {
    Allocator* alloc = resolve_alloc(opts.allocator);
    if (bytes.size() < 16) return ErrorCode::EndOfFile;

    // MAT5 header check: bytes 126..127 == 'IM' (LE) or 'MI' (BE).
    if (bytes.size() >= 128 &&
        ((bytes[126] == static_cast<u8>('I') && bytes[127] == static_cast<u8>('M')) ||
         (bytes[126] == static_cast<u8>('M') && bytes[127] == static_cast<u8>('I')))) {
        const bool is_be = (bytes[126] == static_cast<u8>('M') && bytes[127] == static_cast<u8>('I'));

        MatFile file(alloc);
        file.big_endian = is_be;
        file.version = MatVersion::V6;
        {
            usize hlen = 116;
            while (hlen > 0 && (bytes[hlen - 1] == 0u || bytes[hlen - 1] == static_cast<u8>(' '))) --hlen;
            file.header_text = String::from(StringRef(reinterpret_cast<const char*>(bytes.data()), hlen), alloc);
        }

        MatReader mr(bytes.data() + 128, bytes.size() - 128, is_be);
        while (!mr.eof()) {
            if (mr.remaining() < 8) break;
            auto tag_res = mr.read_tag();
            if (!tag_res.is_ok()) return tag_res.error();
            const MatTag tag = tag_res.value();

            if (tag.type == kMiCompressed) {
                file.version = MatVersion::V7;
                ArenaTemp scratch = get_scratch(alloc);
                InflateOptions iopts;
                iopts.max_output_bytes = opts.max_decompress_bytes;
                iopts.allocator = scratch.allocator();
                auto decomp = zlib_decompress(ArrayRef<u8>(tag.payload, tag.byte_count), iopts);
                if (!decomp.is_ok()) return decomp.error();

                const Array<u8>& buf = decomp.value();
                MatReader decomp_reader(buf.data(), buf.size(), is_be);
                while (!decomp_reader.eof()) {
                    if (decomp_reader.remaining() < 8) break;
                    auto item_res = decode_mat5_matrix(decomp_reader, opts, alloc, 0);
                    if (!item_res.is_ok()) return item_res.error();
                    if (!item_res.value().name.empty()) {
                        file.vars.push_back(keel::move(item_res.value()));
                    }
                }
            } else if (tag.type == kMiMatrix) {
                MatReader sub_reader(tag.payload - 8, tag.total_advance, is_be);
                auto item_res = decode_mat5_matrix(sub_reader, opts, alloc, 0);
                if (!item_res.is_ok()) return item_res.error();
                if (!item_res.value().name.empty()) {
                    file.vars.push_back(keel::move(item_res.value()));
                }
            }
        }

        if (file.vars.empty()) return ErrorCode::InvalidData;
        return file;
    }

    return decode_mat4(bytes, alloc);
}

Result<Array<u8>> mat_encode(ArrayRef<MatVar> vars, const MatWriteOptions& opts) noexcept {
    Allocator* alloc = resolve_alloc(opts.allocator);
    if (opts.version == MatVersion::V4) {
        return encode_mat4(vars, alloc);
    }

    MatWriter mw(alloc);

    // Level 5 128-byte header (standard format prefix for MAT5 interoperability).
    const StringRef default_hdr("MATLAB 5.0 MAT-file, Platform: keel::mat, Created by keel::mat");
    const StringRef hdr_src = opts.header_text.empty() ? default_hdr : opts.header_text;
    u8 hdr_buf[116];
    for (usize i = 0; i < 116; ++i) hdr_buf[i] = static_cast<u8>(' ');
    const usize copy_len = keel::min(hdr_src.size(), usize{116});
    if (copy_len > 0) keel::mem_copy(hdr_buf, hdr_src.data(), copy_len);
    mw.write_bytes(hdr_buf, 116);

    const u8 zero_subsys[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    mw.write_bytes(zero_subsys, 8);
    mw.write_u16_le(0x0100u);
    mw.write_u8(static_cast<u8>('I'));
    mw.write_u8(static_cast<u8>('M'));

    ArenaTemp scratch = get_scratch(alloc);
    for (usize i = 0; i < vars.size(); ++i) {
        MatWriter item_writer(scratch.allocator());
        const Error err = encode_mat5_matrix(item_writer, vars[i].name.as_ref(), vars[i].value, scratch.allocator());
        if (!err.is_ok()) return err;

        if (opts.version == MatVersion::V7) {
            Array<u8> comp = zlib_compress(ArrayRef<u8>(item_writer.bytes.data(), item_writer.bytes.size()),
                                           opts.compression_level, scratch.allocator());
            if (comp.size() > static_cast<usize>(Limits<u32>::kMax)) return ErrorCode::OutOfRange;
            mw.write_tag(kMiCompressed, comp.data(), static_cast<u32>(comp.size()));
        } else {
            mw.write_bytes(item_writer.bytes.data(), item_writer.bytes.size());
        }
    }

    return keel::move(mw.bytes);
}

Result<MatFile> mat_read_file(StringRef path, const MatReadOptions& opts) noexcept {
    Allocator* alloc = resolve_alloc(opts.allocator);
    ArenaTemp scratch = get_scratch(alloc);
    auto raw = fs_read_file(path, scratch.allocator());
    if (!raw.is_ok()) return raw.error();
    return mat_decode(ArrayRef<u8>(raw.value().data(), raw.value().size()), opts);
}

Error mat_write_file(StringRef path, ArrayRef<MatVar> vars, const MatWriteOptions& opts) noexcept {
    Allocator* alloc = resolve_alloc(opts.allocator);
    ArenaTemp scratch = get_scratch(alloc);
    MatWriteOptions enc_opts = opts;
    enc_opts.allocator = scratch.allocator();
    auto encoded = mat_encode(vars, enc_opts);
    if (!encoded.is_ok()) return encoded.error();
    return fs_write_file(path, ArrayRef<u8>(encoded.value().data(), encoded.value().size()));
}

} // namespace keel
