"""Inventory ABI binaries without loading them (Python standard library only)."""
import hashlib
import json
from pathlib import Path
import struct
import sys


def inspect(path):
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[:2] != b"MZ" or data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"not PE: {path}")
    machine, count = struct.unpack_from("<HH", data, pe + 4)
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    if struct.unpack_from("<H", data, optional)[0] != 0x20B or machine != 0x8664:
        raise ValueError(f"not PE32+ AMD64: {path}")
    directories = optional + 112
    table = optional + optional_size

    def offset(rva):
        headers = struct.unpack_from("<I", data, optional + 60)[0]
        if rva < headers:
            return rva
        for i in range(count):
            size, start, raw_size, raw = struct.unpack_from("<IIII", data, table + i * 40 + 8)
            if start <= rva < start + raw_size:
                return raw + rva - start
        raise ValueError(f"unmapped RVA {rva:#x}")

    def string(rva):
        p = offset(rva)
        return data[p:data.index(0, p)].decode("ascii")

    imports = []
    for index, stride in ((1, 20), (13, 32)):
        rva, size = struct.unpack_from("<II", data, directories + index * 8)
        if not rva:
            continue
        for n in range(0, size, stride):
            base = offset(rva + n)
            if not any(data[base:base + stride]):
                break
            if index == 1:
                thunk, _, _, name, first = struct.unpack_from("<IIIII", data, base)
                thunk = thunk or first
            else:
                attr, name, _, _, thunk = struct.unpack_from("<IIIII", data, base)
                if attr != 1:
                    raise ValueError("delay imports must use RVAs")
            symbols = []
            i = 0
            while True:
                entry = struct.unpack_from("<Q", data, offset(thunk + i * 8))[0]
                if not entry:
                    break
                symbols.append(f"#{entry & 0xffff}" if entry >> 63 else string(entry + 2))
                i += 1
            imports.append(dict(dll=string(name), delayed=index == 13, symbols=symbols))
    exports = []
    rva, _ = struct.unpack_from("<II", data, directories)
    if rva:
        base = offset(rva)
        first, functions, names, addresses, name_table, ordinals = struct.unpack_from("<IIIIII", data, base + 16)
        for i in range(names):
            name = string(struct.unpack_from("<I", data, offset(name_table + i * 4))[0])
            ordinal = first + struct.unpack_from("<H", data, offset(ordinals + i * 2))[0]
            exports.append(dict(name=name, ordinal=ordinal))
    return dict(path=str(path), sha256=hashlib.sha256(data).hexdigest(), architecture="x64",
                subsystemVersion=list(struct.unpack_from("<HH", data, optional + 48)),
                imports=imports, exports=exports)


if __name__ == "__main__":
    records = [inspect(Path(p).resolve()) for p in sys.argv[1:]]
    forbidden = {"d3d12.dll", "dxgi.dll", "d3dcompiler_47.dll", "nvcuda.dll", "nvapi64.dll"}
    bad = [r["path"] for r in records if any(i["dll"].lower() in forbidden for i in r["imports"])]
    for record in records:
        record["directOptionalBackendImports"] = [i["dll"] for i in record["imports"] if i["dll"].lower() in forbidden]
        record["runtimeCompatibility"] = "Import inventory only; Windows 7 runtime validation remains required."
    print(json.dumps(records, ensure_ascii=False, indent=2))
    sys.exit(bool(bad))
