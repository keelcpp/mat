#include "keel/matfile.h"

#include "keel/format.h"
#include "keel/fs.h"
#include "keel/math.h"

namespace {

keel::i32 g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            keel::eprintln("FAIL [line {}]: {}", __LINE__, #cond);                                 \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

keel::String bytes_to_hex(keel::ArrayRef<keel::u8> data) {
    static constexpr char kHex[16] = {'0', '1', '2', '3', '4', '5', '6', '7',
                                      '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    keel::String out;
    out.reserve(data.size() * 2);
    for (keel::usize i = 0; i < data.size(); ++i) {
        out.push_back(kHex[(data[i] >> 4) & 0xFu]);
        out.push_back(kHex[data[i] & 0xFu]);
    }
    return out;
}

keel::StringRef class_to_str(keel::matfile::MatClass k) noexcept {
    using keel::matfile::MatClass;
    switch (k) {
        case MatClass::Double: return "double";
        case MatClass::Single: return "single";
        case MatClass::Int8: return "int8";
        case MatClass::Uint8: return "uint8";
        case MatClass::Int16: return "int16";
        case MatClass::Uint16: return "uint16";
        case MatClass::Int32: return "int32";
        case MatClass::Uint32: return "uint32";
        case MatClass::Int64: return "int64";
        case MatClass::Uint64: return "uint64";
        case MatClass::Logical: return "logical";
        case MatClass::Char: return "char";
        case MatClass::Cell: return "cell";
        case MatClass::Struct: return "struct";
        case MatClass::Object: return "object";
        case MatClass::Sparse: return "sparse";
    }
    return "unknown";
}

void json_escape_append(keel::String& out, keel::StringRef s) {
    out.push_back('"');
    for (keel::usize i = 0; i < s.size(); ++i) {
        const keel::u8 c = keel::as<keel::u8>(s[i]);
        if (c == '"') out.append("\\\"");
        else if (c == '\\') out.append("\\\\");
        else if (c == '\n') out.append("\\n");
        else if (c == '\r') out.append("\\r");
        else if (c == '\t') out.append("\\t");
        else if (c < 0x20u) {
            char buf[8];
            const keel::StringRef r = keel::format_buf(buf, "\\u%04x", keel::as<keel::u32>(c));
            out.append(r);
        } else {
            out.push_back(s[i]);
        }
    }
    out.push_back('"');
}

void dump_array_json(keel::String& out, const keel::matfile::MatArray& a) {
    using keel::matfile::MatClass;
    out.append("{\"class\":");
    json_escape_append(out, class_to_str(a.kind));
    out.append(",\"complex\":");
    out.append(a.is_complex ? "true" : "false");
    out.append(",\"dims\":[");
    for (keel::usize i = 0; i < a.dims.size(); ++i) {
        if (i > 0) out.push_back(',');
        char buf[32];
        const keel::usize n = keel::format_u64(buf, sizeof(buf), keel::as<keel::u64>(a.dims[i]));
        out.append(keel::StringRef(buf, n));
    }
    out.push_back(']');

    if (a.kind == MatClass::Cell) {
        out.append(",\"cells\":[");
        const keel::usize nc = a.num_cells();
        for (keel::usize i = 0; i < nc; ++i) {
            if (i > 0) out.push_back(',');
            dump_array_json(out, *a.cell(i));
        }
        out.push_back(']');
    } else if (a.kind == MatClass::Struct || a.kind == MatClass::Object) {
        if (a.kind == MatClass::Object) {
            out.append(",\"object_class\":");
            json_escape_append(out, a.object_class_name());
        }
        out.append(",\"fields\":[");
        const auto fnames = a.field_names();
        for (keel::usize fi = 0; fi < fnames.size(); ++fi) {
            if (fi > 0) out.push_back(',');
            json_escape_append(out, fnames[fi].as_ref());
        }
        out.append("],\"values\":[");
        const keel::usize n = a.numel();
        for (keel::usize ei = 0; ei < n; ++ei) {
            if (ei > 0) out.push_back(',');
            out.push_back('[');
            for (keel::usize fi = 0; fi < fnames.size(); ++fi) {
                if (fi > 0) out.push_back(',');
                dump_array_json(out, *a.field(ei, fi));
            }
            out.push_back(']');
        }
        out.push_back(']');
    } else if (a.kind == MatClass::Sparse) {
        out.append(",\"ir\":[");
        if (a.composite) {
            for (keel::usize i = 0; i < a.composite->sparse_ir.size(); ++i) {
                if (i > 0) out.push_back(',');
                char buf[32];
                const keel::usize n = keel::format_u64(buf, sizeof(buf), a.composite->sparse_ir[i]);
                out.append(keel::StringRef(buf, n));
            }
        }
        out.append("],\"jc\":[");
        if (a.composite) {
            for (keel::usize i = 0; i < a.composite->sparse_jc.size(); ++i) {
                if (i > 0) out.push_back(',');
                char buf[32];
                const keel::usize n = keel::format_u64(buf, sizeof(buf), a.composite->sparse_jc[i]);
                out.append(keel::StringRef(buf, n));
            }
        }
        out.append("],\"data_hex\":");
        json_escape_append(out, bytes_to_hex(keel::ArrayRef<keel::u8>(a.data)).as_ref());
        if (a.is_complex) {
            out.append(",\"imag_hex\":");
            json_escape_append(out, bytes_to_hex(keel::ArrayRef<keel::u8>(a.imag_data)).as_ref());
        }
    } else if (a.kind == MatClass::Char) {
        out.append(",\"utf8\":");
        json_escape_append(out, a.to_utf8().as_ref());
        out.append(",\"data_hex\":");
        json_escape_append(out, bytes_to_hex(keel::ArrayRef<keel::u8>(a.data)).as_ref());
    } else {
        out.append(",\"data_hex\":");
        json_escape_append(out, bytes_to_hex(keel::ArrayRef<keel::u8>(a.data)).as_ref());
        if (a.is_complex) {
            out.append(",\"imag_hex\":");
            json_escape_append(out, bytes_to_hex(keel::ArrayRef<keel::u8>(a.imag_data)).as_ref());
        }
    }
    out.push_back('}');
}

keel::i32 cli_inspect(keel::StringRef path) {
    auto res = keel::matfile::mat_read_file(path);
    if (!res.is_ok()) {
        keel::eprintln("mat_read_file failed");
        return 2;
    }
    const keel::matfile::MatFile& f = res.value();
    keel::String out;
    out.append("{\"version\":");
    char vbuf[16];
    const keel::usize vn = keel::format_u64(vbuf, sizeof(vbuf), keel::as<keel::u64>(static_cast<keel::u8>(f.version)));
    out.append(keel::StringRef(vbuf, vn));
    out.append(",\"big_endian\":");
    out.append(f.big_endian ? "true" : "false");
    out.append(",\"vars\":{");
    for (keel::usize i = 0; i < f.vars.size(); ++i) {
        if (i > 0) out.push_back(',');
        json_escape_append(out, f.vars[i].name.as_ref());
        out.push_back(':');
        dump_array_json(out, f.vars[i].value);
    }
    out.append("}}");
    keel::println("{}", out);
    return 0;
}

keel::i32 cli_generate(keel::StringRef ver_str, keel::StringRef out_path) {
    using namespace keel::matfile;
    MatVersion ver = MatVersion::V7;
    if (ver_str == "v4") ver = MatVersion::V4;
    else if (ver_str == "v6") ver = MatVersion::V6;
    else if (ver_str == "v7") ver = MatVersion::V7;
    else return 2;

    keel::Array<MatVar> vars;
    // Double 2x3
    {
        const keel::usize d[2] = {2, 3};
        const keel::f64 v[6] = {1.5, -2.25, 3.0, 4.75, 0.0, 99.125};
        vars.push_back(MatVar(keel::String::from("dbl"),
                              MatArray::from_f64(keel::ArrayRef<keel::usize>(d, 2), keel::ArrayRef<keel::f64>(v, 6))));
    }
    // Single 2x2
    {
        const keel::usize d[2] = {2, 2};
        const keel::f32 v[4] = {-1.25f, 0.5f, 3.5f, 16.0f};
        vars.push_back(MatVar(keel::String::from("sgl"),
                              MatArray::from_f32(keel::ArrayRef<keel::usize>(d, 2), keel::ArrayRef<keel::f32>(v, 4))));
    }
    // Complex double 2x2
    {
        const keel::usize d[2] = {2, 2};
        const keel::c64 v[4] = {{1.0, 2.0}, {-3.5, 0.5}, {0.0, -4.0}, {7.0, 8.0}};
        vars.push_back(MatVar(keel::String::from("cx"),
                              MatArray::from_c64(keel::ArrayRef<keel::usize>(d, 2), keel::ArrayRef<keel::c64>(v, 4))));
    }
    // Int32 & Uint8 & Char
    {
        const keel::usize d[2] = {1, 3};
        MatArray i32a = MatArray::make_dense(MatClass::Int32, keel::ArrayRef<keel::usize>(d, 2));
        const keel::i32 iv[3] = {-100000, 0, 100000};
        keel::mem_copy(i32a.data.data(), iv, sizeof(iv));
        vars.push_back(MatVar(keel::String::from("i32v"), keel::move(i32a)));

        MatArray u8a = MatArray::make_dense(MatClass::Uint8, keel::ArrayRef<keel::usize>(d, 2));
        u8a.data[0] = 0; u8a.data[1] = 127; u8a.data[2] = 255;
        vars.push_back(MatVar(keel::String::from("u8v"), keel::move(u8a)));

        vars.push_back(MatVar(keel::String::from("txt"), MatArray::from_string("Hello MAT!")));
    }

    if (ver != MatVersion::V4) {
        // 3D array
        {
            const keel::usize d[3] = {2, 2, 2};
            const keel::f64 v[8] = {1, 2, 3, 4, 5, 6, 7, 8};
            vars.push_back(MatVar(keel::String::from("nd3"),
                                  MatArray::from_f64(keel::ArrayRef<keel::usize>(d, 3), keel::ArrayRef<keel::f64>(v, 8))));
        }
        // Int64 (> 2^53 precision check) & Uint64 & Logical
        {
            const keel::usize d[2] = {1, 2};
            MatArray i64a = MatArray::make_dense(MatClass::Int64, keel::ArrayRef<keel::usize>(d, 2));
            const keel::i64 iv[2] = {-9007199254740995LL, 9007199254740997LL};
            keel::mem_copy(i64a.data.data(), iv, sizeof(iv));
            vars.push_back(MatVar(keel::String::from("i64v"), keel::move(i64a)));

            MatArray u64a = MatArray::make_dense(MatClass::Uint64, keel::ArrayRef<keel::usize>(d, 2));
            const keel::u64 uv[2] = {18014398509481987ULL, 42ULL};
            keel::mem_copy(u64a.data.data(), uv, sizeof(uv));
            vars.push_back(MatVar(keel::String::from("u64v"), keel::move(u64a)));

            const keel::u8 lv[2] = {1, 0};
            vars.push_back(MatVar(keel::String::from("flag"),
                                  MatArray::from_logical(keel::ArrayRef<keel::usize>(d, 2), keel::ArrayRef<keel::u8>(lv, 2))));
        }
        // Unicode UTF-8 text (Cyrillic "Привет")
        {
            const char utf8_cyr[] = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";
            vars.push_back(MatVar(keel::String::from("utxt"), MatArray::from_utf8(keel::StringRef(utf8_cyr, sizeof(utf8_cyr) - 1))));
        }
        // Cell & Struct
        {
            const keel::usize cd[2] = {1, 2};
            const keel::usize sd[2] = {1, 1};
            MatArray cell = MatArray::make_cell(keel::ArrayRef<keel::usize>(cd, 2));
            const keel::f64 sv = 3.25;
            *cell.cell_mut(0) = MatArray::from_f64(keel::ArrayRef<keel::usize>(sd, 2), keel::ArrayRef<keel::f64>(&sv, 1));
            *cell.cell_mut(1) = MatArray::from_string("inside");
            vars.push_back(MatVar(keel::String::from("c"), keel::move(cell)));

            const keel::StringRef fnames[2] = {"a", "b"};
            MatArray st = MatArray::make_struct(keel::ArrayRef<keel::usize>(cd, 2), keel::ArrayRef<keel::StringRef>(fnames, 2));
            const keel::f64 v0 = 10.0, v1 = 20.0;
            *st.field_mut(0, 0) = MatArray::from_f64(keel::ArrayRef<keel::usize>(sd, 2), keel::ArrayRef<keel::f64>(&v0, 1));
            *st.field_mut(0, 1) = MatArray::from_string("r0");
            *st.field_mut(1, 0) = MatArray::from_f64(keel::ArrayRef<keel::usize>(sd, 2), keel::ArrayRef<keel::f64>(&v1, 1));
            *st.field_mut(1, 1) = MatArray::from_string("r1");
            vars.push_back(MatVar(keel::String::from("st"), keel::move(st)));
        }
        // Real & Complex Sparse 3x3
        {
            MatArray sp;
            sp.kind = MatClass::Sparse;
            sp.dims.push_back(3); sp.dims.push_back(3);
            sp.composite = keel::make_unique_in<MatComposite>(nullptr, nullptr);
            sp.composite->sparse_nzmax = 2;
            sp.composite->sparse_ir.push_back(0);
            sp.composite->sparse_ir.push_back(2);
            sp.composite->sparse_jc.push_back(0);
            sp.composite->sparse_jc.push_back(1);
            sp.composite->sparse_jc.push_back(1);
            sp.composite->sparse_jc.push_back(2);
            sp.data.resize(2 * sizeof(keel::f64));
            const keel::f64 nz[2] = {10.5, -20.25};
            keel::mem_copy(sp.data.data(), nz, sizeof(nz));
            vars.push_back(MatVar(keel::String::from("sp"), keel::move(sp)));
        }
    }

    MatWriteOptions wopts;
    wopts.version = ver;
    const keel::Error err = mat_write_file(out_path, keel::ArrayRef<MatVar>(vars.data(), vars.size()), wopts);
    return err.is_ok() ? 0 : 2;
}

keel::i32 cli_fuzz_dir(keel::StringRef dir_path) {
    keel::usize count = 0;
    keel::matfile::MatReadOptions ropts;
    ropts.max_decompress_bytes = 2ULL * 1024ULL * 1024ULL;
    ropts.max_nesting_depth = 16;

    const keel::Error err = keel::fs_read_dir(dir_path, [&](const keel::DirEntry& e) -> bool {
        if (!e.is_file) return true;
        auto raw = keel::fs_read_file(e.full_path);
        if (raw.is_ok()) {
            const auto ref = keel::ArrayRef<keel::u8>(raw.value().data(), raw.value().size());
            (void)keel::matfile::mat_detect_version(ref);
            (void)keel::matfile::mat_decode(ref, ropts);
            ++count;
        }
        return true;
    });
    if (err.is_err()) return 2;
    keel::println("fuzzed {} files cleanly", count);
    return 0;
}

void test_matfile5_roundtrip(keel::matfile::MatVersion ver) {
    keel::Array<keel::matfile::MatVar> vars;

    // 1. 2x3 double
    {
        const keel::usize d[2] = {2, 3};
        const keel::f64 vals[6] = {1.5, -2.25, 3.0, 4.75, 0.0, 99.125};
        vars.push_back(keel::matfile::MatVar(keel::String::from("dbl"),
                                    keel::matfile::MatArray::from_f64(keel::ArrayRef<keel::usize>(d, 2),
                                                             keel::ArrayRef<keel::f64>(vals, 6))));
    }

    // 2. 2x2x2 3D single
    {
        const keel::usize d[3] = {2, 2, 2};
        const keel::f32 vals[8] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
        vars.push_back(keel::matfile::MatVar(keel::String::from("nd3"),
                                    keel::matfile::MatArray::from_f32(keel::ArrayRef<keel::usize>(d, 3),
                                                             keel::ArrayRef<keel::f32>(vals, 8))));
    }

    // 3. Complex 2x2
    {
        const keel::usize d[2] = {2, 2};
        const keel::c64 vals[4] = {{1.0, 2.0}, {-3.5, 0.5}, {0.0, -4.0}, {7.0, 8.0}};
        vars.push_back(keel::matfile::MatVar(keel::String::from("cx"),
                                    keel::matfile::MatArray::from_c64(keel::ArrayRef<keel::usize>(d, 2),
                                                             keel::ArrayRef<keel::c64>(vals, 4))));
    }

    // 4. Int64 & Logical & Char (ASCII + UTF-8 Cyrillic/Emoji)
    {
        const keel::usize d[2] = {1, 3};
        keel::matfile::MatArray i64_arr = keel::matfile::MatArray::make_dense(keel::matfile::MatClass::Int64, keel::ArrayRef<keel::usize>(d, 2));
        const keel::i64 raw[3] = {-9000000000000LL, 42LL, 9000000000000LL};
        keel::mem_copy(i64_arr.data.data(), raw, sizeof(raw));
        vars.push_back(keel::matfile::MatVar(keel::String::from("i64v"), keel::move(i64_arr)));

        const keel::u8 log_vals[3] = {1, 0, 1};
        vars.push_back(keel::matfile::MatVar(keel::String::from("flag"),
                                    keel::matfile::MatArray::from_logical(keel::ArrayRef<keel::usize>(d, 2),
                                                                 keel::ArrayRef<keel::u8>(log_vals, 3))));

        vars.push_back(keel::matfile::MatVar(keel::String::from("txt"), keel::matfile::MatArray::from_string("Hello, keel::matfile!")));

        const char utf8_mix[] = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xF0\x9F\x9A\x80";
        vars.push_back(keel::matfile::MatVar(keel::String::from("utxt"),
                                    keel::matfile::MatArray::from_utf8(keel::StringRef(utf8_mix, sizeof(utf8_mix) - 1))));
    }

    // 5. Nested Cell + Struct + Object
    {
        const keel::usize cd[2] = {1, 2};
        keel::matfile::MatArray cell = keel::matfile::MatArray::make_cell(keel::ArrayRef<keel::usize>(cd, 2));
        const keel::usize sd[2] = {1, 1};
        const keel::f64 sv = 123.5;
        *cell.cell_mut(0) =
            keel::matfile::MatArray::from_f64(keel::ArrayRef<keel::usize>(sd, 2), keel::ArrayRef<keel::f64>(&sv, 1));
        *cell.cell_mut(1) = keel::matfile::MatArray::from_string("nested");
        vars.push_back(keel::matfile::MatVar(keel::String::from("c"), keel::move(cell)));

        const keel::StringRef fnames[2] = {"alpha", "beta"};
        keel::matfile::MatArray st =
            keel::matfile::MatArray::make_struct(keel::ArrayRef<keel::usize>(cd, 2), keel::ArrayRef<keel::StringRef>(fnames, 2));
        const keel::f64 v0 = 10.0, v1 = 20.0;
        *st.field_mut(0, 0) =
            keel::matfile::MatArray::from_f64(keel::ArrayRef<keel::usize>(sd, 2), keel::ArrayRef<keel::f64>(&v0, 1));
        *st.field_mut(0, 1) = keel::matfile::MatArray::from_string("row0");
        *st.field_mut(1, 0) =
            keel::matfile::MatArray::from_f64(keel::ArrayRef<keel::usize>(sd, 2), keel::ArrayRef<keel::f64>(&v1, 1));
        *st.field_mut(1, 1) = keel::matfile::MatArray::from_string("row1");
        vars.push_back(keel::matfile::MatVar(keel::String::from("st"), keel::move(st)));

        const keel::StringRef pnames[1] = {"gain"};
        keel::matfile::MatArray obj = keel::matfile::MatArray::make_object("MyFilter", keel::ArrayRef<keel::usize>(sd, 2),
                                                         keel::ArrayRef<keel::StringRef>(pnames, 1));
        const keel::f64 g = 2.5;
        *obj.field_mut(0, 0) =
            keel::matfile::MatArray::from_f64(keel::ArrayRef<keel::usize>(sd, 2), keel::ArrayRef<keel::f64>(&g, 1));
        vars.push_back(keel::matfile::MatVar(keel::String::from("obj"), keel::move(obj)));
    }

    // 6. Sparse 3x3 CSC matrix (diagonal [10, 20] at (0,0) and (2,2))
    {
        keel::matfile::MatArray sp;
        sp.kind = keel::matfile::MatClass::Sparse;
        sp.dims.push_back(3);
        sp.dims.push_back(3);
        sp.composite = keel::make_unique_in<keel::matfile::MatComposite>(nullptr, nullptr);
        sp.composite->sparse_nzmax = 2;
        sp.composite->sparse_ir.push_back(0);
        sp.composite->sparse_ir.push_back(2);
        sp.composite->sparse_jc.push_back(0);
        sp.composite->sparse_jc.push_back(1);
        sp.composite->sparse_jc.push_back(1);
        sp.composite->sparse_jc.push_back(2);
        sp.data.resize(2 * sizeof(keel::f64));
        const keel::f64 nz[2] = {10.0, 20.0};
        keel::mem_copy(sp.data.data(), nz, sizeof(nz));
        vars.push_back(keel::matfile::MatVar(keel::String::from("sp"), keel::move(sp)));
    }

    keel::matfile::MatWriteOptions wopts;
    wopts.version = ver;
    auto enc = keel::matfile::mat_encode(keel::ArrayRef<keel::matfile::MatVar>(vars.data(), vars.size()), wopts);
    CHECK(enc.is_ok());
    if (!enc.is_ok()) return;

    auto det = keel::matfile::mat_detect_version(keel::ArrayRef<keel::u8>(enc.value().data(), enc.value().size()));
    CHECK(det.has_value() && det.value() == ver);

    auto dec = keel::matfile::mat_decode(keel::ArrayRef<keel::u8>(enc.value().data(), enc.value().size()));
    CHECK(dec.is_ok());
    if (!dec.is_ok()) return;

    const keel::matfile::MatFile& file = dec.value();
    CHECK(file.version == ver);
    CHECK(!file.big_endian);
    CHECK(file.vars.size() == vars.size());

    const keel::matfile::MatArray* dbl = file.find("dbl");
    CHECK(dbl != nullptr && dbl->kind == keel::matfile::MatClass::Double && dbl->rows() == 2 && dbl->cols() == 3);
    CHECK(dbl->as_f64()[0] == 1.5 && dbl->as_f64()[5] == 99.125);

    const keel::matfile::MatArray* nd3 = file.find("nd3");
    CHECK(nd3 != nullptr && nd3->rank() == 3 && nd3->dims[2] == 2 && nd3->as_f32()[7] == 8.0f);

    const keel::matfile::MatArray* cx = file.find("cx");
    CHECK(cx != nullptr && cx->is_complex && cx->as_f64()[1] == -3.5 && cx->imag_f64()[1] == 0.5);

    const keel::matfile::MatArray* i64v = file.find("i64v");
    CHECK(i64v != nullptr && i64v->kind == keel::matfile::MatClass::Int64 && i64v->as_i64()[0] == -9000000000000LL);

    const keel::matfile::MatArray* flag = file.find("flag");
    CHECK(flag != nullptr && flag->kind == keel::matfile::MatClass::Logical && flag->as_logical()[0] == 1 &&
          flag->as_logical()[1] == 0);

    const keel::matfile::MatArray* txt = file.find("txt");
    CHECK(txt != nullptr && txt->kind == keel::matfile::MatClass::Char && txt->to_ascii().as_ref() == "Hello, keel::matfile!");

    const keel::matfile::MatArray* utxt = file.find("utxt");
    const char expected_utf8[] = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xF0\x9F\x9A\x80";
    CHECK(utxt != nullptr && utxt->to_utf8().as_ref() == keel::StringRef(expected_utf8, sizeof(expected_utf8) - 1));

    const keel::matfile::MatArray* c = file.find("c");
    CHECK(c != nullptr && c->num_cells() == 2 && c->cell(0)->as_f64()[0] == 123.5 &&
          c->cell(1)->to_ascii().as_ref() == "nested");

    const keel::matfile::MatArray* st = file.find("st");
    CHECK(st != nullptr && st->num_fields() == 2 && st->field("alpha", 0)->as_f64()[0] == 10.0 &&
          st->field("beta", 1)->to_ascii().as_ref() == "row1");

    const keel::matfile::MatArray* obj = file.find("obj");
    CHECK(obj != nullptr && obj->kind == keel::matfile::MatClass::Object && obj->object_class_name() == "MyFilter" &&
          obj->field("gain", 0)->as_f64()[0] == 2.5);

    const keel::matfile::MatArray* sp = file.find("sp");
    CHECK(sp != nullptr && sp->kind == keel::matfile::MatClass::Sparse && sp->composite && sp->composite->sparse_ir.size() == 2 &&
          sp->composite->sparse_ir[1] == 2 && sp->as_f64()[1] == 20.0);
}

void test_matfile4_roundtrip() {
    keel::Array<keel::matfile::MatVar> vars;
    const keel::usize d[2] = {2, 2};
    const keel::f64 rv[4] = {1.0, 2.0, 3.0, 4.0};
    const keel::c64 cv[4] = {{1.0, -1.0}, {2.0, -2.0}, {3.0, -3.0}, {4.0, -4.0}};
    vars.push_back(keel::matfile::MatVar(keel::String::from("A"),
                                keel::matfile::MatArray::from_f64(keel::ArrayRef<keel::usize>(d, 2),
                                                         keel::ArrayRef<keel::f64>(rv, 4))));
    vars.push_back(keel::matfile::MatVar(keel::String::from("Z"),
                                keel::matfile::MatArray::from_c64(keel::ArrayRef<keel::usize>(d, 2),
                                                         keel::ArrayRef<keel::c64>(cv, 4))));
    vars.push_back(keel::matfile::MatVar(keel::String::from("S"), keel::matfile::MatArray::from_string("mat4!")));

    // Int64 widened to f64 cleanly in MAT4.
    {
        const keel::usize d1[2] = {1, 2};
        keel::matfile::MatArray i64a = keel::matfile::MatArray::make_dense(keel::matfile::MatClass::Int64, keel::ArrayRef<keel::usize>(d1, 2));
        const keel::i64 iv[2] = {-123456LL, 789012LL};
        keel::mem_copy(i64a.data.data(), iv, sizeof(iv));
        vars.push_back(keel::matfile::MatVar(keel::String::from("W"), keel::move(i64a)));
    }

    keel::matfile::MatWriteOptions wopts;
    wopts.version = keel::matfile::MatVersion::V4;
    auto enc = keel::matfile::mat_encode(keel::ArrayRef<keel::matfile::MatVar>(vars.data(), vars.size()), wopts);
    CHECK(enc.is_ok());
    if (!enc.is_ok()) return;

    auto dec = keel::matfile::mat_decode(keel::ArrayRef<keel::u8>(enc.value().data(), enc.value().size()));
    CHECK(dec.is_ok());
    if (!dec.is_ok()) return;

    CHECK(dec.value().version == keel::matfile::MatVersion::V4);
    CHECK(dec.value().find("A")->as_f64()[3] == 4.0);
    CHECK(dec.value().find("Z")->imag_f64()[2] == -3.0);
    CHECK(dec.value().find("S")->to_ascii().as_ref() == "mat4!");
    CHECK(dec.value().find("W")->as_f64()[0] == -123456.0 && dec.value().find("W")->as_f64()[1] == 789012.0);
}

void test_big_endian_and_file_io() {
    // Minimal big-endian ('MI') MAT5 file containing a 1x2 double matrix [3.5, -7.25] named "be".
    keel::Array<keel::u8> bytes(128, keel::as<keel::u8>(' '));
    bytes[124] = 0x01;
    bytes[125] = 0x00;
    bytes[126] = keel::as<keel::u8>('M');
    bytes[127] = keel::as<keel::u8>('I');

    auto push_u32_be = [](keel::Array<keel::u8>& out, keel::u32 v) {
        out.push_back(keel::as<keel::u8>((v >> 24) & 0xFFu));
        out.push_back(keel::as<keel::u8>((v >> 16) & 0xFFu));
        out.push_back(keel::as<keel::u8>((v >> 8) & 0xFFu));
        out.push_back(keel::as<keel::u8>(v & 0xFFu));
    };
    auto push_f64_be = [](keel::Array<keel::u8>& out, keel::f64 v) {
        keel::u8 raw[8];
        keel::mem_copy(raw, &v, 8);
        for (keel::usize k = 0; k < 8; ++k) out.push_back(raw[7 - k]);
    };

    keel::Array<keel::u8> inner;
    push_u32_be(inner, 6); push_u32_be(inner, 8); push_u32_be(inner, 6); push_u32_be(inner, 0);
    push_u32_be(inner, 5); push_u32_be(inner, 8); push_u32_be(inner, 1); push_u32_be(inner, 2);
    push_u32_be(inner, (2u << 16) | 1u);
    inner.push_back(keel::as<keel::u8>('b'));
    inner.push_back(keel::as<keel::u8>('e'));
    inner.push_back(0); inner.push_back(0);
    push_u32_be(inner, 9); push_u32_be(inner, 16);
    push_f64_be(inner, 3.5);
    push_f64_be(inner, -7.25);

    push_u32_be(bytes, 14);
    push_u32_be(bytes, keel::as<keel::u32>(inner.size()));
    bytes.append(keel::ArrayRef<keel::u8>(inner.data(), inner.size()));

    auto dec = keel::matfile::mat_decode(keel::ArrayRef<keel::u8>(bytes.data(), bytes.size()));
    CHECK(dec.is_ok());
    if (dec.is_ok()) {
        CHECK(dec.value().big_endian);
        const keel::matfile::MatArray* be = dec.value().find("be");
        CHECK(be != nullptr && be->as_f64().size() == 2 && be->as_f64()[0] == 3.5 && be->as_f64()[1] == -7.25);
    }

    // Big-endian MAT4 file (m_type=1000 -> IEEE BE double)
    {
        keel::Array<keel::u8> be4;
        push_u32_be(be4, 1000u); // BE double
        push_u32_be(be4, 1u);
        push_u32_be(be4, 2u);
        push_u32_be(be4, 0u);
        push_u32_be(be4, 3u); // "x4\0"
        be4.push_back(keel::as<keel::u8>('x'));
        be4.push_back(keel::as<keel::u8>('4'));
        be4.push_back(0u);
        push_f64_be(be4, 11.25);
        push_f64_be(be4, -4.5);
        auto det4 = keel::matfile::mat_detect_version(keel::ArrayRef<keel::u8>(be4));
        CHECK(det4.has_value() && *det4 == keel::matfile::MatVersion::V4);
        auto d4 = keel::matfile::mat_decode(keel::ArrayRef<keel::u8>(be4));
        CHECK(d4.is_ok() && d4.value().big_endian);
        if (d4.is_ok()) {
            const auto* v = d4.value().find("x4");
            CHECK(v != nullptr && v->as_f64()[0] == 11.25 && v->as_f64()[1] == -4.5);
        }
    }

    // Disk write/read round-trip
    auto tmp_res = keel::fs_temp_dir();
    CHECK(tmp_res.is_ok());
    if (tmp_res.is_ok()) {
        const keel::String path = keel::path_join(tmp_res.value(), "keel_matfile_unit_test.mat");
        keel::Array<keel::matfile::MatVar> vars;
        const keel::usize d[2] = {1, 2};
        const keel::f64 vals[2] = {42.0, -0.5};
        vars.push_back(keel::matfile::MatVar(keel::String::from("diskv"),
                                    keel::matfile::MatArray::from_f64(keel::ArrayRef<keel::usize>(d, 2),
                                                             keel::ArrayRef<keel::f64>(vals, 2))));
        const keel::Error werr = keel::matfile::mat_write_file(path, keel::ArrayRef<keel::matfile::MatVar>(vars.data(), vars.size()));
        CHECK(werr.is_ok());
        auto rd = keel::matfile::mat_read_file(path);
        CHECK(rd.is_ok());
        if (rd.is_ok()) {
            const keel::matfile::MatArray* dv = rd.value().find("diskv");
            CHECK(dv != nullptr && dv->as_f64()[0] == 42.0 && dv->as_f64()[1] == -0.5);
        }
        (void)keel::fs_remove(path);
    }
}

void test_malformed_and_safety_guards() {
    // 1. MAT5 32-bit padding wraparound (0xFFFFFFFC byte count)
    {
        keel::Array<keel::u8> bytes(128, keel::as<keel::u8>(' '));
        bytes[126] = keel::as<keel::u8>('I');
        bytes[127] = keel::as<keel::u8>('M');
        const keel::u8 tag[8] = {0x0E, 0x00, 0x00, 0x00, 0xFC, 0xFF, 0xFF, 0xFF};
        bytes.append(keel::ArrayRef<keel::u8>(tag, 8));
        auto res = keel::matfile::mat_decode(keel::ArrayRef<keel::u8>(bytes.data(), bytes.size()));
        CHECK(!res.is_ok());
    }

    // 2. Top-level SDEF miMATRIX (regression test for payload - 8 underflow!)
    {
        keel::Array<keel::u8> bytes(128, keel::as<keel::u8>(' '));
        bytes[126] = keel::as<keel::u8>('I');
        bytes[127] = keel::as<keel::u8>('M');
        // SDEF: type=14 (miMATRIX), byte_count=4 in upper 16 bits -> 0x0004000E
        const keel::u8 sdef[8] = {0x0E, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00};
        bytes.append(keel::ArrayRef<keel::u8>(sdef, 8));
        auto res = keel::matfile::mat_decode(keel::ArrayRef<keel::u8>(bytes.data(), bytes.size()));
        CHECK(!res.is_ok());
    }

    // 3. Random non-MAT bytes must NOT be detected as MAT4.
    {
        const keel::u8 png_sig[24] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
                                      0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 0, 1, 0, 0, 0, 1};
        CHECK(!keel::matfile::mat_detect_version(keel::ArrayRef<keel::u8>(png_sig, 24)).has_value());
    }

    // 4. MAT4 missing NUL terminator in variable name
    {
        keel::Array<keel::u8> bytes(24, 0);
        bytes[4] = 1;
        bytes[8] = 1;
        bytes[16] = 4;
        bytes[20] = 'a';
        bytes[21] = 'b';
        bytes[22] = 'c';
        bytes[23] = 'd';
        auto res = keel::matfile::mat_decode(keel::ArrayRef<keel::u8>(bytes.data(), bytes.size()));
        CHECK(!res.is_ok());
    }

    // 5. Decompression cap guard on v7 compressed file
    {
        keel::Array<keel::matfile::MatVar> vars;
        const keel::usize d[2] = {1, 2000};
        keel::matfile::MatArray zeros = keel::matfile::MatArray::make_dense(keel::matfile::MatClass::Double, keel::ArrayRef<keel::usize>(d, 2));
        vars.push_back(keel::matfile::MatVar(keel::String::from("big"), keel::move(zeros)));
        auto enc = keel::matfile::mat_encode(keel::ArrayRef<keel::matfile::MatVar>(vars.data(), vars.size()));
        CHECK(enc.is_ok());
        if (enc.is_ok()) {
            keel::matfile::MatReadOptions ropts;
            ropts.max_decompress_bytes = 512;
            auto dec = keel::matfile::mat_decode(keel::ArrayRef<keel::u8>(enc.value().data(), enc.value().size()), ropts);
            CHECK(!dec.is_ok());
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && keel::StringRef(argv[1]) == "--inspect") {
        return cli_inspect(keel::StringRef(argv[2]));
    }
    if (argc == 4 && keel::StringRef(argv[1]) == "--generate") {
        return cli_generate(keel::StringRef(argv[2]), keel::StringRef(argv[3]));
    }
    if (argc == 3 && keel::StringRef(argv[1]) == "--fuzz") {
        return cli_fuzz_dir(keel::StringRef(argv[2]));
    }

    test_matfile5_roundtrip(keel::matfile::MatVersion::V6);
    test_matfile5_roundtrip(keel::matfile::MatVersion::V7);
    test_matfile4_roundtrip();
    test_big_endian_and_file_io();
    test_malformed_and_safety_guards();

    if (g_failures == 0) {
        keel::println("All keel::matfile tests passed.");
        return 0;
    }
    keel::eprintln("{} test failure(s).", g_failures);
    return 1;
}
