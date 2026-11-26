#!/usr/bin/env python3
"""
Bidirectional differential & adversarial fuzzing verification suite for keel::matfile
against SciPy (scipy.io.loadmat/savemat) and MATLAB R2025b (when available).

Usage:
    python tests/verify_reference.py [path/to/test_matfile.exe] [--no-matlab]
"""

import json
import random
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
import scipy.io
import scipy.sparse


def find_exe() -> Path:
    for arg in sys.argv[1:]:
        if not arg.startswith("--"):
            p = Path(arg)
            if p.exists():
                return p
    root = Path(__file__).resolve().parent.parent
    for cand in [
        root / "build" / "Release" / "test_matfile.exe",
        root / "build" / "Release" / "test_matfile",
        root / "build" / "Debug" / "test_matfile.exe",
        root / "build" / "test_matfile.exe",
        root / "build" / "test_matfile",
    ]:
        if cand.exists():
            return cand
    raise FileNotFoundError("Could not locate test_matfile executable.")


def inspect_mat(exe: Path, path: Path) -> dict:
    proc = subprocess.run([str(exe), "--inspect", str(path)], capture_output=True, text=True, encoding="utf-8", check=False)
    if proc.returncode != 0:
        raise RuntimeError(f"test_matfile --inspect {path} failed ({proc.returncode}): {proc.stderr}")
    return json.loads(proc.stdout)


def array_from_keel_json(node: dict) -> np.ndarray:
    dtype_map = {
        "double": "<f8", "single": "<f4",
        "int8": "<i1", "uint8": "<u1", "logical": "<u1",
        "int16": "<i2", "uint16": "<u2",
        "int32": "<i4", "uint32": "<u4",
        "int64": "<i8", "uint64": "<u8",
    }
    dt = np.dtype(dtype_map[node["class"]])
    dims = tuple(node["dims"])
    re = np.frombuffer(bytes.fromhex(node["data_hex"]), dtype=dt).reshape(dims, order="F")
    if node["complex"]:
        im = np.frombuffer(bytes.fromhex(node["imag_hex"]), dtype=dt).reshape(dims, order="F")
        return re + 1j * im
    if node["class"] == "logical":
        return re.astype(bool)
    return re


def verify_scipy_bidirectional(exe: Path, tmp: Path) -> int:
    checks = 0
    rng = np.random.default_rng(20261126)

    # Direction A: SciPy savemat -> keel::matfile decode (v6, v7, v4)
    dense_v5 = {
        "f64": np.array([[1.5, -2.25, 0.0], [np.pi, -1e-100, 1e100]], dtype=np.float64),
        "f32": np.array([[1.25, -0.5], [3.5, 42.0]], dtype=np.float32),
        "i8": np.array([[-128, 0, 127]], dtype=np.int8),
        "u8": np.array([[0, 128, 255]], dtype=np.uint8),
        "i16": np.array([[-32768, 42, 32767]], dtype=np.int16),
        "u16": np.array([[0, 1000, 65535]], dtype=np.uint16),
        "i32": np.array([[-2147483648, 7, 2147483647]], dtype=np.int32),
        "u32": np.array([[0, 123456, 4294967295]], dtype=np.uint32),
        "i64": np.array([[-9007199254740995, 0, 9007199254740997]], dtype=np.int64),
        "u64": np.array([[0, 18014398509481987, 18446744073709551615]], dtype=np.uint64),
        "logv": np.array([[True, False, True], [False, True, False]], dtype=bool),
        "cx": np.array([[1 + 2j, -3.5 + 0.5j], [0 - 4j, 7 + 8j]], dtype=np.complex128),
        "nd3": rng.standard_normal((3, 4, 2)),
        "nd4": rng.integers(-50, 50, size=(2, 3, 2, 2), dtype=np.int32),
        "ascii_s": "Hello SciPy!",
        "uni_s": "Привет, мир! αβγ",
        "sp_real": scipy.sparse.csc_matrix(np.array([[1.5, 0.0, 0.0], [0.0, 0.0, -3.25], [2.0, 0.0, 4.0]])),
        "sp_cx": scipy.sparse.csc_matrix(np.array([[1 + 2j, 0j], [0j, -3 + 4j]], dtype=np.complex128)),
        "sp_zero": scipy.sparse.csc_matrix((3, 4), dtype=np.float64),
    }

    for label, comp in (("scipy_v6", False), ("scipy_v7", True)):
        p = tmp / f"{label}.mat"
        scipy.io.savemat(str(p), dense_v5, format="5", do_compression=comp)
        info = inspect_mat(exe, p)
        assert info["version"] == (7 if comp else 6), f"version mismatch for {label}: {info['version']}"
        for name in ("f64", "f32", "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "logv", "cx", "nd3", "nd4"):
            got = array_from_keel_json(info["vars"][name])
            exp = dense_v5[name]
            assert got.shape == exp.shape and got.dtype == exp.dtype, f"{label}.{name} shape/dtype {got.shape}/{got.dtype} vs {exp.shape}/{exp.dtype}"
            assert np.array_equal(got, exp), f"{label}.{name} value mismatch"
            checks += 1
        assert info["vars"]["ascii_s"]["utf8"] == "Hello SciPy!"
        assert info["vars"]["uni_s"]["utf8"] == "Привет, мир! αβγ"
        checks += 2

        # Sparse CSC comparison
        for sp_name, is_cx in (("sp_real", False), ("sp_cx", True), ("sp_zero", False)):
            node = info["vars"][sp_name]
            exp_sp = dense_v5[sp_name]
            assert node["class"] == "sparse" and tuple(node["dims"]) == exp_sp.shape
            re = np.frombuffer(bytes.fromhex(node["data_hex"]), dtype="<f8")
            vals = re + 1j * np.frombuffer(bytes.fromhex(node["imag_hex"]), dtype="<f8") if is_cx else re
            got_sp = scipy.sparse.csc_matrix((vals, node["ir"], node["jc"]), shape=tuple(node["dims"]))
            assert np.array_equal(got_sp.toarray(), exp_sp.toarray()), f"{label}.{sp_name} sparse mismatch"
            checks += 1

    # MAT v4 via SciPy -> keel::matfile
    p4 = tmp / "scipy_v4.mat"
    v4_dict = {
        "A": np.array([[1.0, 2.0], [3.0, 4.0]], dtype=np.float64),
        "Z": np.array([[1 - 1j, 2 + 3j]], dtype=np.complex128),
        "S": "mat4scipy",
    }
    scipy.io.savemat(str(p4), v4_dict, format="4")
    info4 = inspect_mat(exe, p4)
    assert info4["version"] == 4
    assert np.array_equal(array_from_keel_json(info4["vars"]["A"]), v4_dict["A"])
    assert np.array_equal(array_from_keel_json(info4["vars"]["Z"]), v4_dict["Z"])
    assert info4["vars"]["S"]["utf8"] == "mat4scipy"
    checks += 3

    # Direction B: keel::matfile encode -> SciPy loadmat (v4, v6, v7)
    for ver in ("v4", "v6", "v7"):
        out_p = tmp / f"keel_{ver}.mat"
        proc = subprocess.run([str(exe), "--generate", ver, str(out_p)], capture_output=True, text=True, check=False)
        assert proc.returncode == 0, f"generate {ver} failed: {proc.stderr}"
        loaded = scipy.io.loadmat(str(out_p))
        assert np.array_equal(loaded["dbl"], np.array([[1.5, 3.0, 0.0], [-2.25, 4.75, 99.125]], dtype=np.float64))
        assert np.array_equal(loaded["sgl"], np.array([[-1.25, 3.5], [0.5, 16.0]], dtype=np.float32))
        assert np.array_equal(loaded["cx"], np.array([[1 + 2j, -4j], [-3.5 + 0.5j, 7 + 8j]], dtype=np.complex128))
        assert np.array_equal(loaded["i32v"], np.array([[-100000, 0, 100000]], dtype=np.int32))
        assert np.array_equal(loaded["u8v"], np.array([[0, 127, 255]], dtype=np.uint8))
        assert loaded["txt"][0] == "Hello MAT!"
        checks += 6
        if ver != "v4":
            assert loaded["nd3"].shape == (2, 2, 2) and loaded["nd3"][1, 1, 1] == 8.0
            assert np.array_equal(loaded["i64v"], np.array([[-9007199254740995, 9007199254740997]], dtype=np.int64))
            assert np.array_equal(loaded["u64v"], np.array([[18014398509481987, 42]], dtype=np.uint64))
            assert np.array_equal(loaded["flag"], np.array([[1, 0]], dtype=np.uint8))
            assert loaded["utxt"][0] == "Привет"
            assert loaded["c"][0, 0][0, 0] == 3.25 and loaded["c"][0, 1][0] == "inside"
            assert loaded["st"][0, 0]["a"][0, 0] == 10.0 and loaded["st"][0, 1]["b"][0] == "r1"
            assert loaded["sp"].toarray()[0, 0] == 10.5 and loaded["sp"].toarray()[2, 2] == -20.25
            checks += 8

    print(f"SciPy bidirectional differential: {checks}/{checks} checks PASSED (SciPy {scipy.__version__})")
    return checks


def verify_matlab_bidirectional(exe: Path, tmp: Path) -> int:
    matlab_bin = shutil.which("matlab")
    if not matlab_bin or "--no-matlab" in sys.argv:
        print("MATLAB R2025b: skipped")
        return 0

    m_dir = str(tmp).replace("\\", "/")
    m_script = (
        f"cd('{m_dir}'); "
        "dbl = [1.5 3.0 0.0; -2.25 4.75 99.125]; "
        "sgl = single([-1.25 3.5; 0.5 16.0]); "
        "cx = [1+2i -4i; -3.5+0.5i 7+8i]; "
        "i64v = [int64(-9007199254740992)-int64(3), int64(9007199254740992)+int64(5)]; "
        "u64v = [uint64(18014398509481984)+uint64(3), uint64(42)]; "
        "flag = logical([1 0; 0 1]); "
        "nd3 = reshape(1:8, [2 2 2]); "
        "txt = 'Hello MATLAB!'; "
        "utxt = char([1055 1088 1080 1074 1077 1090]); "
        "c = {3.25, 'inside'}; "
        "st = struct('a', {10.0, 20.0}, 'b', {'r0', 'r1'}); "
        "sp = sparse([1 3], [1 3], [10.5 -20.25], 3, 3); "
        "spz = sparse(3, 4); "
        "save('matlab_v6.mat', 'dbl','sgl','cx','i64v','u64v','flag','nd3','txt','utxt','c','st','sp','spz', '-v6'); "
        "save('matlab_v7.mat', 'dbl','sgl','cx','i64v','u64v','flag','nd3','txt','utxt','c','st','sp','spz', '-v7'); "
        "save('matlab_v4.mat', 'dbl','cx','txt', '-v4'); "
        # Now load files GENERATED by keel::matfile and verify exact values inside MATLAB R2025b!
        "for vf = {'keel_v6.mat', 'keel_v7.mat'}, "
        "  S = load(vf{1}); "
        "  assert(isequal(S.dbl, dbl), [vf{1} ':dbl']); "
        "  assert(isequal(S.sgl, sgl), [vf{1} ':sgl']); "
        "  assert(isequal(S.cx, cx), [vf{1} ':cx']); "
        "  assert(isequal(S.i64v, i64v), [vf{1} ':i64v']); "
        "  assert(isequal(S.u64v, u64v), [vf{1} ':u64v']); "
        "  assert(isequal(S.flag, logical([1 0])), [vf{1} ':flag']); "
        "  assert(isequal(S.nd3, nd3), [vf{1} ':nd3']); "
        "  assert(isequal(S.txt, 'Hello MAT!'), [vf{1} ':txt']); "
        "  assert(isequal(S.utxt, utxt), [vf{1} ':utxt:' class(S.utxt) ':' mat2str(double(S.utxt))]); "
        "  assert(isequal(S.c, c), [vf{1} ':c']); "
        "  assert(isequal(S.st, st), [vf{1} ':st']); "
        "  assert(issparse(S.sp) && isequal(full(S.sp), full(sp)), [vf{1} ':sp']); "
        "end; "
        "S4 = load('keel_v4.mat'); "
        "assert(isequal(S4.dbl, dbl), 'v4:dbl'); "
        "assert(isequal(S4.cx, cx), 'v4:cx'); "
        "assert(isequal(S4.txt, 'Hello MAT!'), ['v4:txt:' mat2str(double(S4.txt))]); "
        "disp('MATLAB_LOAD_KEEL_OK');"
    )
    proc = subprocess.run([matlab_bin, "-batch", m_script], capture_output=True, text=True, timeout=120, check=False)
    if proc.returncode != 0 or "MATLAB_LOAD_KEEL_OK" not in proc.stdout:
        raise RuntimeError(f"MATLAB R2025b verification failed:\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}")

    checks = 15  # MATLAB verified loading keel v4/v6/v7!

    # Now verify keel::matfile decodes MATLAB R2025b's own v6, v7, v4 files bit-exact!
    for label, expected_ver in (("matlab_v6.mat", 6), ("matlab_v7.mat", 7)):
        info = inspect_mat(exe, tmp / label)
        assert info["version"] == expected_ver
        assert np.array_equal(array_from_keel_json(info["vars"]["dbl"]), np.array([[1.5, 3.0, 0.0], [-2.25, 4.75, 99.125]]))
        assert np.array_equal(array_from_keel_json(info["vars"]["sgl"]), np.array([[-1.25, 3.5], [0.5, 16.0]], dtype=np.float32))
        assert np.array_equal(array_from_keel_json(info["vars"]["cx"]), np.array([[1 + 2j, -4j], [-3.5 + 0.5j, 7 + 8j]]))
        assert np.array_equal(array_from_keel_json(info["vars"]["i64v"]), np.array([[-9007199254740995, 9007199254740997]], dtype=np.int64))
        assert np.array_equal(array_from_keel_json(info["vars"]["u64v"]), np.array([[18014398509481987, 42]], dtype=np.uint64))
        assert np.array_equal(array_from_keel_json(info["vars"]["flag"]), np.array([[True, False], [False, True]]))
        assert np.array_equal(array_from_keel_json(info["vars"]["nd3"]), np.arange(1, 9, dtype=np.float64).reshape((2, 2, 2), order="F"))
        assert info["vars"]["txt"]["utf8"] == "Hello MATLAB!"
        if expected_ver == 7:
            assert info["vars"]["utxt"]["utf8"] == "Привет"
        else:
            # MATLAB -v6 replaces non-ASCII characters with 0x1A on save.
            assert info["vars"]["utxt"]["dims"] == [1, 6]
        assert array_from_keel_json(info["vars"]["c"]["cells"][0])[0, 0] == 3.25
        assert info["vars"]["c"]["cells"][1]["utf8"] == "inside"
        assert info["vars"]["st"]["fields"] == ["a", "b"]
        assert array_from_keel_json(info["vars"]["st"]["values"][0][0])[0, 0] == 10.0
        assert info["vars"]["st"]["values"][1][1]["utf8"] == "r1"
        sp = info["vars"]["sp"]
        assert sp["ir"] == [0, 2] and sp["jc"] == [0, 1, 1, 2]
        assert np.array_equal(np.frombuffer(bytes.fromhex(sp["data_hex"]), dtype="<f8"), np.array([10.5, -20.25]))
        spz = info["vars"]["spz"]
        assert spz["dims"] == [3, 4] and spz["ir"] == [] and spz["jc"] == [0, 0, 0, 0, 0]
        checks += 16

    info4 = inspect_mat(exe, tmp / "matlab_v4.mat")
    assert info4["version"] == 4
    assert np.array_equal(array_from_keel_json(info4["vars"]["dbl"]), np.array([[1.5, 3.0, 0.0], [-2.25, 4.75, 99.125]]))
    assert np.array_equal(array_from_keel_json(info4["vars"]["cx"]), np.array([[1 + 2j, -4j], [-3.5 + 0.5j, 7 + 8j]]))
    assert info4["vars"]["txt"]["utf8"] == "Hello MATLAB!"
    checks += 3

    print(f"MATLAB R2025b bidirectional differential: {checks}/{checks} checks PASSED")
    return checks


def run_fuzz_suite(exe: Path, tmp: Path) -> int:
    fuzz_dir = tmp / "fuzz"
    fuzz_dir.mkdir()
    rng = random.Random(0xF022AA7)

    # Seed valid v4/v6/v7 files and mutate/truncate them heavily.
    seeds = [
        (tmp / "keel_v4.mat").read_bytes(),
        (tmp / "keel_v6.mat").read_bytes(),
        (tmp / "keel_v7.mat").read_bytes(),
    ]

    idx = 0
    for s in seeds:
        # Truncations
        for cut in (0, 1, 15, 20, 127, 128, 136, len(s) // 2, len(s) - 1):
            if cut <= len(s):
                (fuzz_dir / f"trunc_{idx}.mat").write_bytes(s[:cut])
                idx += 1
        # Bit/byte flips
        for _ in range(25):
            b = bytearray(s)
            for _ in range(rng.randint(1, 8)):
                pos = rng.randrange(len(b))
                b[pos] ^= rng.randint(1, 255)
            (fuzz_dir / f"mut_{idx}.mat").write_bytes(bytes(b))
            idx += 1

    # Crafted OOM bombs (huge Cell/Struct/Char/Double dims inside tiny files)
    def make_mat5_hdr() -> bytearray:
        h = bytearray(b" " * 128)
        h[124:128] = b"\x00\x01IM"
        return h

    for mx_cls in (1, 2, 4, 5, 6):  # Cell, Struct, Char, Sparse, Double
        inner = bytearray()
        inner += struct.pack("<IIII", 6, 8, mx_cls, 0)          # flags
        inner += struct.pack("<IIII", 5, 8, 100000000, 10)      # huge dims = 10^9
        inner += struct.pack("<II", (1 << 16) | 1, ord("x"))    # SDEF name "x"
        inner += struct.pack("<II", 9, 0)                       # empty data tag
        blob = make_mat5_hdr() + struct.pack("<II", 14, len(inner)) + inner
        (fuzz_dir / f"oom_cls{mx_cls}.mat").write_bytes(bytes(blob))
        idx += 1

    # Pure random blobs
    for _ in range(25):
        (fuzz_dir / f"rand_{idx}.mat").write_bytes(rng.randbytes(rng.randint(1, 512)))
        idx += 1

    proc = subprocess.run([str(exe), "--fuzz", str(fuzz_dir)], capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise RuntimeError(f"fuzz failed ({proc.returncode}): {proc.stderr}")
    print(f"Adversarial fuzz suite: {proc.stdout.strip()} (0 crashes/OOM)")
    return idx


def main() -> int:
    exe = find_exe()
    with tempfile.TemporaryDirectory(prefix="keel_mat_ref_") as td:
        tmp = Path(td)
        c1 = verify_scipy_bidirectional(exe, tmp)
        c2 = verify_matlab_bidirectional(exe, tmp)
        c3 = run_fuzz_suite(exe, tmp)
    print(f"verify_reference: ALL PASSED (scipy={c1}, matlab={c2}, fuzz_files={c3})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
