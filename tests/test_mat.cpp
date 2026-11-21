#include "keel/mat.h"

#include "keel/fs.h"
#include "keel/math.h"

#include <cstdio>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL [line %d]: %s\n", __LINE__, #cond);                         \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

void test_mat5_roundtrip(keel::mat::MatVersion ver) {
    keel::Array<keel::mat::MatVar> vars;

    // 1. 2x3 double
    {
        const keel::usize d[2] = {2, 3};
        const keel::f64 vals[6] = {1.5, -2.25, 3.0, 4.75, 0.0, 99.125};
        vars.push_back(keel::mat::MatVar(keel::String::from("dbl"),
                                    keel::mat::MatArray::from_f64(keel::ArrayRef<keel::usize>(d, 2),
                                                             keel::ArrayRef<keel::f64>(vals, 6))));
    }

    // 2. 2x2x2 3D single
    {
        const keel::usize d[3] = {2, 2, 2};
        const keel::f32 vals[8] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
        vars.push_back(keel::mat::MatVar(keel::String::from("nd3"),
                                    keel::mat::MatArray::from_f32(keel::ArrayRef<keel::usize>(d, 3),
                                                             keel::ArrayRef<keel::f32>(vals, 8))));
    }

    // 3. Complex 2x2
    {
        const keel::usize d[2] = {2, 2};
        const keel::c64 vals[4] = {{1.0, 2.0}, {-3.5, 0.5}, {0.0, -4.0}, {7.0, 8.0}};
        vars.push_back(keel::mat::MatVar(keel::String::from("cx"),
                                    keel::mat::MatArray::from_c64(keel::ArrayRef<keel::usize>(d, 2),
                                                             keel::ArrayRef<keel::c64>(vals, 4))));
    }

    // 4. Int64 & Logical & Char
    {
        const keel::usize d[2] = {1, 3};
        keel::mat::MatArray i64_arr = keel::mat::MatArray::make_dense(keel::mat::MatClass::Int64, keel::ArrayRef<keel::usize>(d, 2));
        const keel::i64 raw[3] = {-9000000000000LL, 42LL, 9000000000000LL};
        keel::mem_copy(i64_arr.data.data(), raw, sizeof(raw));
        vars.push_back(keel::mat::MatVar(keel::String::from("i64v"), keel::move(i64_arr)));

        const keel::u8 log_vals[3] = {1, 0, 1};
        vars.push_back(keel::mat::MatVar(keel::String::from("flag"),
                                    keel::mat::MatArray::from_logical(keel::ArrayRef<keel::usize>(d, 2),
                                                                 keel::ArrayRef<keel::u8>(log_vals, 3))));

        vars.push_back(keel::mat::MatVar(keel::String::from("txt"), keel::mat::MatArray::from_string("Hello, keel::mat!")));
    }

    // 5. Nested Cell + Struct + Object
    {
        const keel::usize cd[2] = {1, 2};
        keel::mat::MatArray cell = keel::mat::MatArray::make_cell(keel::ArrayRef<keel::usize>(cd, 2));
        const keel::usize sd[2] = {1, 1};
        const keel::f64 sv = 123.5;
        *cell.cell_mut(0) =
            keel::mat::MatArray::from_f64(keel::ArrayRef<keel::usize>(sd, 2), keel::ArrayRef<keel::f64>(&sv, 1));
        *cell.cell_mut(1) = keel::mat::MatArray::from_string("nested");
        vars.push_back(keel::mat::MatVar(keel::String::from("c"), keel::move(cell)));

        const keel::StringRef fnames[2] = {"alpha", "beta"};
        keel::mat::MatArray st =
            keel::mat::MatArray::make_struct(keel::ArrayRef<keel::usize>(cd, 2), keel::ArrayRef<keel::StringRef>(fnames, 2));
        const keel::f64 v0 = 10.0, v1 = 20.0;
        *st.field_mut(0, 0) =
            keel::mat::MatArray::from_f64(keel::ArrayRef<keel::usize>(sd, 2), keel::ArrayRef<keel::f64>(&v0, 1));
        *st.field_mut(0, 1) = keel::mat::MatArray::from_string("row0");
        *st.field_mut(1, 0) =
            keel::mat::MatArray::from_f64(keel::ArrayRef<keel::usize>(sd, 2), keel::ArrayRef<keel::f64>(&v1, 1));
        *st.field_mut(1, 1) = keel::mat::MatArray::from_string("row1");
        vars.push_back(keel::mat::MatVar(keel::String::from("st"), keel::move(st)));

        const keel::StringRef pnames[1] = {"gain"};
        keel::mat::MatArray obj = keel::mat::MatArray::make_object("MyFilter", keel::ArrayRef<keel::usize>(sd, 2),
                                                         keel::ArrayRef<keel::StringRef>(pnames, 1));
        const keel::f64 g = 2.5;
        *obj.field_mut(0, 0) =
            keel::mat::MatArray::from_f64(keel::ArrayRef<keel::usize>(sd, 2), keel::ArrayRef<keel::f64>(&g, 1));
        vars.push_back(keel::mat::MatVar(keel::String::from("obj"), keel::move(obj)));
    }

    // 6. Sparse 3x3 CSC matrix (diagonal [10, 20] at (0,0) and (2,2))
    {
        keel::mat::MatArray sp;
        sp.kind = keel::mat::MatClass::Sparse;
        sp.dims.push_back(3);
        sp.dims.push_back(3);
        sp.composite = keel::make_unique_in<keel::mat::MatComposite>(nullptr, nullptr);
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
        vars.push_back(keel::mat::MatVar(keel::String::from("sp"), keel::move(sp)));
    }

    keel::mat::MatWriteOptions wopts;
    wopts.version = ver;
    auto enc = keel::mat::mat_encode(keel::ArrayRef<keel::mat::MatVar>(vars.data(), vars.size()), wopts);
    CHECK(enc.is_ok());
    if (!enc.is_ok()) return;

    auto det = keel::mat::mat_detect_version(keel::ArrayRef<keel::u8>(enc.value().data(), enc.value().size()));
    CHECK(det.has_value() && det.value() == ver);

    auto dec = keel::mat::mat_decode(keel::ArrayRef<keel::u8>(enc.value().data(), enc.value().size()));
    CHECK(dec.is_ok());
    if (!dec.is_ok()) return;

    const keel::mat::MatFile& file = dec.value();
    CHECK(file.version == ver);
    CHECK(!file.big_endian);
    CHECK(file.vars.size() == vars.size());

    const keel::mat::MatArray* dbl = file.find("dbl");
    CHECK(dbl != nullptr && dbl->kind == keel::mat::MatClass::Double && dbl->rows() == 2 && dbl->cols() == 3);
    CHECK(dbl->as_f64()[0] == 1.5 && dbl->as_f64()[5] == 99.125);

    const keel::mat::MatArray* nd3 = file.find("nd3");
    CHECK(nd3 != nullptr && nd3->rank() == 3 && nd3->dims[2] == 2 && nd3->as_f32()[7] == 8.0f);

    const keel::mat::MatArray* cx = file.find("cx");
    CHECK(cx != nullptr && cx->is_complex && cx->as_f64()[1] == -3.5 && cx->imag_f64()[1] == 0.5);

    const keel::mat::MatArray* i64v = file.find("i64v");
    CHECK(i64v != nullptr && i64v->kind == keel::mat::MatClass::Int64 && i64v->as_i64()[0] == -9000000000000LL);

    const keel::mat::MatArray* flag = file.find("flag");
    CHECK(flag != nullptr && flag->kind == keel::mat::MatClass::Logical && flag->as_logical()[0] == 1 &&
          flag->as_logical()[1] == 0);

    const keel::mat::MatArray* txt = file.find("txt");
    CHECK(txt != nullptr && txt->kind == keel::mat::MatClass::Char && txt->to_ascii().as_ref() == "Hello, keel::mat!");

    const keel::mat::MatArray* c = file.find("c");
    CHECK(c != nullptr && c->num_cells() == 2 && c->cell(0)->as_f64()[0] == 123.5 &&
          c->cell(1)->to_ascii().as_ref() == "nested");

    const keel::mat::MatArray* st = file.find("st");
    CHECK(st != nullptr && st->num_fields() == 2 && st->field("alpha", 0)->as_f64()[0] == 10.0 &&
          st->field("beta", 1)->to_ascii().as_ref() == "row1");

    const keel::mat::MatArray* obj = file.find("obj");
    CHECK(obj != nullptr && obj->kind == keel::mat::MatClass::Object && obj->object_class_name() == "MyFilter" &&
          obj->field("gain", 0)->as_f64()[0] == 2.5);

    const keel::mat::MatArray* sp = file.find("sp");
    CHECK(sp != nullptr && sp->kind == keel::mat::MatClass::Sparse && sp->composite && sp->composite->sparse_ir.size() == 2 &&
          sp->composite->sparse_ir[1] == 2 && sp->as_f64()[1] == 20.0);
}

void test_mat4_roundtrip() {
    keel::Array<keel::mat::MatVar> vars;
    const keel::usize d[2] = {2, 2};
    const keel::f64 rv[4] = {1.0, 2.0, 3.0, 4.0};
    const keel::c64 cv[4] = {{1.0, -1.0}, {2.0, -2.0}, {3.0, -3.0}, {4.0, -4.0}};
    vars.push_back(keel::mat::MatVar(keel::String::from("A"),
                                keel::mat::MatArray::from_f64(keel::ArrayRef<keel::usize>(d, 2),
                                                         keel::ArrayRef<keel::f64>(rv, 4))));
    vars.push_back(keel::mat::MatVar(keel::String::from("Z"),
                                keel::mat::MatArray::from_c64(keel::ArrayRef<keel::usize>(d, 2),
                                                         keel::ArrayRef<keel::c64>(cv, 4))));
    vars.push_back(keel::mat::MatVar(keel::String::from("S"), keel::mat::MatArray::from_string("mat4!")));

    keel::mat::MatWriteOptions wopts;
    wopts.version = keel::mat::MatVersion::V4;
    auto enc = keel::mat::mat_encode(keel::ArrayRef<keel::mat::MatVar>(vars.data(), vars.size()), wopts);
    CHECK(enc.is_ok());
    if (!enc.is_ok()) return;

    auto dec = keel::mat::mat_decode(keel::ArrayRef<keel::u8>(enc.value().data(), enc.value().size()));
    CHECK(dec.is_ok());
    if (!dec.is_ok()) return;

    CHECK(dec.value().version == keel::mat::MatVersion::V4);
    CHECK(dec.value().find("A")->as_f64()[3] == 4.0);
    CHECK(dec.value().find("Z")->imag_f64()[2] == -3.0);
    CHECK(dec.value().find("S")->to_ascii().as_ref() == "mat4!");
}

void test_big_endian_and_file_io() {
    // Construct a minimal big-endian ('MI') MAT5 file containing a 1x2 double matrix [3.5, -7.25] named "be".
    keel::Array<keel::u8> bytes(128, static_cast<keel::u8>(' '));
    bytes[124] = 0x01;
    bytes[125] = 0x00;
    bytes[126] = static_cast<keel::u8>('M');
    bytes[127] = static_cast<keel::u8>('I');

    auto push_u32_be = [](keel::Array<keel::u8>& out, keel::u32 v) {
        out.push_back(static_cast<keel::u8>((v >> 24) & 0xFFu));
        out.push_back(static_cast<keel::u8>((v >> 16) & 0xFFu));
        out.push_back(static_cast<keel::u8>((v >> 8) & 0xFFu));
        out.push_back(static_cast<keel::u8>(v & 0xFFu));
    };
    auto push_f64_be = [](keel::Array<keel::u8>& out, keel::f64 v) {
        keel::u8 raw[8];
        keel::mem_copy(raw, &v, 8);
        for (keel::usize k = 0; k < 8; ++k) out.push_back(raw[7 - k]);
    };

    keel::Array<keel::u8> inner;
    // Flags: miUINT32, 8 bytes -> [mxDOUBLE_CLASS=6, 0]
    push_u32_be(inner, 6);
    push_u32_be(inner, 8);
    push_u32_be(inner, 6);
    push_u32_be(inner, 0);
    // Dims: miINT32, 8 bytes -> [1, 2]
    push_u32_be(inner, 5);
    push_u32_be(inner, 8);
    push_u32_be(inner, 1);
    push_u32_be(inner, 2);
    // Name (SDEF): type=1 (miINT8), count=2 -> "be"
    push_u32_be(inner, (2u << 16) | 1u);
    inner.push_back(static_cast<keel::u8>('b'));
    inner.push_back(static_cast<keel::u8>('e'));
    inner.push_back(0);
    inner.push_back(0);
    // Data: miDOUBLE (9), 16 bytes -> [3.5, -7.25]
    push_u32_be(inner, 9);
    push_u32_be(inner, 16);
    push_f64_be(inner, 3.5);
    push_f64_be(inner, -7.25);

    push_u32_be(bytes, 14); // miMATRIX
    push_u32_be(bytes, static_cast<keel::u32>(inner.size()));
    bytes.append(keel::ArrayRef<keel::u8>(inner.data(), inner.size()));

    auto dec = keel::mat::mat_decode(keel::ArrayRef<keel::u8>(bytes.data(), bytes.size()));
    CHECK(dec.is_ok());
    if (dec.is_ok()) {
        CHECK(dec.value().big_endian);
        const keel::mat::MatArray* be = dec.value().find("be");
        CHECK(be != nullptr && be->as_f64().size() == 2 && be->as_f64()[0] == 3.5 && be->as_f64()[1] == -7.25);
    }

    // Disk write/read round-trip
    auto tmp_res = keel::fs_temp_dir();
    CHECK(tmp_res.is_ok());
    if (tmp_res.is_ok()) {
        const keel::String path = keel::path_join(tmp_res.value(), "keel_mat_unit_test.mat");
        keel::Array<keel::mat::MatVar> vars;
        const keel::usize d[2] = {1, 2};
        const keel::f64 vals[2] = {42.0, -0.5};
        vars.push_back(keel::mat::MatVar(keel::String::from("diskv"),
                                    keel::mat::MatArray::from_f64(keel::ArrayRef<keel::usize>(d, 2),
                                                             keel::ArrayRef<keel::f64>(vals, 2))));
        const keel::Error werr = keel::mat::mat_write_file(path, keel::ArrayRef<keel::mat::MatVar>(vars.data(), vars.size()));
        CHECK(werr.is_ok());
        auto rd = keel::mat::mat_read_file(path);
        CHECK(rd.is_ok());
        if (rd.is_ok()) {
            const keel::mat::MatArray* dv = rd.value().find("diskv");
            CHECK(dv != nullptr && dv->as_f64()[0] == 42.0 && dv->as_f64()[1] == -0.5);
        }
        (void)keel::fs_remove(path);
    }
}

void test_malformed_and_safety_guards() {
    // 1. MAT5 32-bit padding wraparound (0xFFFFFFFC byte count)
    {
        keel::Array<keel::u8> bytes(128, static_cast<keel::u8>(' '));
        bytes[126] = static_cast<keel::u8>('I');
        bytes[127] = static_cast<keel::u8>('M');
        const keel::u8 tag[8] = {0x0E, 0x00, 0x00, 0x00, 0xFC, 0xFF, 0xFF, 0xFF};
        bytes.append(keel::ArrayRef<keel::u8>(tag, 8));
        auto res = keel::mat::mat_decode(keel::ArrayRef<keel::u8>(bytes.data(), bytes.size()));
        CHECK(!res.is_ok());
    }

    // 2. MAT4 missing NUL terminator in variable name
    {
        keel::Array<keel::u8> bytes(24, 0);
        bytes[4] = 1;
        bytes[8] = 1;
        bytes[16] = 4;
        bytes[20] = 'a';
        bytes[21] = 'b';
        bytes[22] = 'c';
        bytes[23] = 'd';
        auto res = keel::mat::mat_decode(keel::ArrayRef<keel::u8>(bytes.data(), bytes.size()));
        CHECK(!res.is_ok());
    }

    // 3. Decompression cap guard on v7 compressed file
    {
        keel::Array<keel::mat::MatVar> vars;
        const keel::usize d[2] = {1, 2000};
        keel::mat::MatArray zeros = keel::mat::MatArray::make_dense(keel::mat::MatClass::Double, keel::ArrayRef<keel::usize>(d, 2));
        vars.push_back(keel::mat::MatVar(keel::String::from("big"), keel::move(zeros)));
        auto enc = keel::mat::mat_encode(keel::ArrayRef<keel::mat::MatVar>(vars.data(), vars.size()));
        CHECK(enc.is_ok());
        if (enc.is_ok()) {
            keel::mat::MatReadOptions ropts;
            ropts.max_decompress_bytes = 512;
            auto dec = keel::mat::mat_decode(keel::ArrayRef<keel::u8>(enc.value().data(), enc.value().size()), ropts);
            CHECK(!dec.is_ok());
        }
    }
}

} // namespace

int main() {
    test_mat5_roundtrip(keel::mat::MatVersion::V6);
    test_mat5_roundtrip(keel::mat::MatVersion::V7);
    test_mat4_roundtrip();
    test_big_endian_and_file_io();
    test_malformed_and_safety_guards();

    if (g_failures == 0) {
        std::printf("All keel::mat tests passed.\n");
        return 0;
    }
    std::fprintf(stderr, "%d test failure(s).\n", g_failures);
    return 1;
}
