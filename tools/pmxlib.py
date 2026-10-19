"""PMX 2.0/2.1 reader / writer.

只做本工程需要的部分：完整解析 PMX 结构，并保证「未修改即逐字节往返」。
生成标准表情（morph）只需要读改写 morph 段与表示枠段，其余段以原始字节
保存在 :attr:`PmxModel.raw_sections` 中，写回时原样输出，避免重新序列化
几何/材质/骨骼/物理数据带来的风险。

用法::

    from pmxlib import PmxModel
    m = PmxModel.from_file("model.pmx")
    for mo in m.morphs:
        print(mo.name_local, mo.panel, mo.type)
    m.save("out.pmx")
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Sequence

class PmxError(Exception):
    """PMX 读写错误。"""


# ---------------------------------------------------------------------------
# 常量
# ---------------------------------------------------------------------------

PANEL_NAMES = {
    0: "システム",
    1: "眉",
    2: "目",
    3: "口",
    4: "その他",
}

PANEL_BY_NAME = {v: k for k, v in PANEL_NAMES.items()}

MORPH_TYPE_NAMES = {
    0: "group",
    1: "vertex",
    2: "bone",
    3: "uv",
    4: "uv1",
    5: "uv2",
    6: "uv3",
    7: "uv4",
    8: "material",
    9: "flip",
    10: "impulse",
}

MORPH_TYPE_BY_NAME = {v: k for k, v in MORPH_TYPE_NAMES.items()}

# 可以把别的 morph 组合起来、且 MMD 会递归展开的类型
GROUPABLE_TARGET_TYPES = frozenset({0})

# 可以被 group morph 引用的类型（MMD 支持 group 引用任何非 group 类型，
# 也支持 group 套 group；这里允许除 impulse 之外的全部）
REFERENCEABLE_TYPES = frozenset({0, 1, 2, 3, 4, 5, 6, 7, 8, 9})


def _struct_index_fmt(size: int) -> str:
    return {1: "<b", 2: "<h", 4: "<i"}[size]


def _struct_uindex_fmt(size: int) -> str:
    return {1: "<B", 2: "<H", 4: "<I"}[size]


# ---------------------------------------------------------------------------
# 数据模型
# ---------------------------------------------------------------------------


@dataclass
class Morph:
    """单个表情。``offsets`` 按类型解释：

    - group: ``(morph_index:int, weight:float)``
    - vertex: ``(vertex_index:int, x:float, y:float, z:float)``
    - bone: ``(bone_index:int, tx,ty,tz, qx,qy,qz,qw)`` —— 回転量是四元数（4 float），
      与 include/mikumikustudio/subrecord_layout.hpp 中 PmxBoneMorphEntry 一致
    - uv*: ``(vertex_index:int, x,y,z,w)``
    - material: ``(material_index:int, calc_mode:int, diffuse4, specular3,
      shininess, ambient3, edge_color4, edge_size, tex_tint4, sphere_tint4,
      toon_tint4)``
    - flip: ``(morph_index:int, weight:float)``
    - impulse: ``(rigid_index:int, local:int, vx,vy,vz, tx,ty,tz)``
    """

    name_local: str
    name_en: str
    panel: int
    type: int
    offsets: list = field(default_factory=list)
    raw: bytes = b""  # 读入时的原始字节（含名称字段）
    dirty: bool = False
    source: str = ""  # 生成来源说明（工具用，不写入文件）

    @property
    def type_name(self) -> str:
        return MORPH_TYPE_NAMES.get(self.type, f"unknown({self.type})")

    @property
    def panel_name(self) -> str:
        return PANEL_NAMES.get(self.panel, f"panel{self.panel}")

    def to_bytes(self, m: "PmxModel") -> bytes:
        if self.raw and not self.dirty:
            return self.raw
        out = bytearray()
        out += m.encode_text(self.name_local)
        out += m.encode_text(self.name_en)
        out.append(self.panel & 0xFF)
        out.append(self.type & 0xFF)
        out += struct.pack("<i", len(self.offsets))
        out += self._offsets_bytes(m)
        return bytes(out)

    def _offsets_bytes(self, m: "PmxModel") -> bytes:
        t = self.type
        if t == 0:  # group
            fmt = _struct_index_fmt(m.morph_index_size)
            buf = bytearray()
            for idx, w in self.offsets:
                buf += struct.pack(fmt, idx)
                buf += struct.pack("<f", w)
            return bytes(buf)
        if t == 1:  # vertex
            fmt = _struct_index_fmt(m.vertex_index_size)
            buf = bytearray()
            for idx, x, y, z in self.offsets:
                buf += struct.pack(fmt, idx)
                buf += struct.pack("<3f", x, y, z)
            return bytes(buf)
        if t == 2:  # bone（回転量是四元数）
            fmt = _struct_index_fmt(m.bone_index_size)
            buf = bytearray()
            for off in self.offsets:
                buf += struct.pack(fmt, off[0])
                buf += struct.pack("<7f", *off[1:8])
            return bytes(buf)
        if 3 <= t <= 7:  # uv
            fmt = _struct_index_fmt(m.vertex_index_size)
            buf = bytearray()
            for off in self.offsets:
                buf += struct.pack(fmt, off[0])
                buf += struct.pack("<4f", *off[1:5])
            return bytes(buf)
        if t == 8:  # material
            fmt = _struct_index_fmt(m.material_index_size)
            buf = bytearray()
            for off in self.offsets:
                buf += struct.pack(fmt, off[0])
                buf.append(off[1] & 0xFF)
                buf += struct.pack("<4f", *off[2:6])
                buf += struct.pack("<3f", *off[6:9])
                buf += struct.pack("<f", off[9])
                buf += struct.pack("<3f", *off[10:13])
                buf += struct.pack("<4f", *off[13:17])
                buf += struct.pack("<f", off[17])
                buf += struct.pack("<4f", *off[18:22])
                buf += struct.pack("<4f", *off[22:26])
                buf += struct.pack("<4f", *off[26:30])
            return bytes(buf)
        if t == 9:  # flip
            fmt = _struct_index_fmt(m.morph_index_size)
            buf = bytearray()
            for idx, w in self.offsets:
                buf += struct.pack(fmt, idx)
                buf += struct.pack("<f", w)
            return bytes(buf)
        if t == 10:  # impulse
            fmt = _struct_index_fmt(m.rigid_index_size)
            buf = bytearray()
            for off in self.offsets:
                buf += struct.pack(fmt, off[0])
                buf.append(off[1] & 0xFF)
                buf += struct.pack("<3f", *off[2:5])
                buf += struct.pack("<3f", *off[5:8])
            return bytes(buf)
        raise PmxError(f"无法序列化的 morph 类型: {t}")


@dataclass
class Material:
    """材质记录（只解析本工具需要的字段；写回时整段沿用原始字节）。"""

    name_local: str = ""
    name_en: str = ""
    diffuse: tuple = (1.0, 1.0, 1.0, 1.0)
    specular: tuple = (0.0, 0.0, 0.0)
    specularity: float = 0.0
    ambient: tuple = (0.0, 0.0, 0.0)
    flags: int = 0
    edge_color: tuple = (0.0, 0.0, 0.0, 1.0)
    edge_size: float = 0.0
    texture_index: int = -1
    sphere_index: int = -1
    sphere_mode: int = 0
    toon_index: int = -1
    toon_internal: bool = True


@dataclass
class FrameElement:
    kind: int  # 0 = bone, 1 = morph
    index: int


@dataclass
class DisplayFrame:
    name_local: str
    name_en: str
    special: int
    elements: list  # list[FrameElement]
    raw: bytes = b""
    dirty: bool = False

    def to_bytes(self, m: "PmxModel") -> bytes:
        if self.raw and not self.dirty:
            return self.raw
        out = bytearray()
        out += m.encode_text(self.name_local)
        out += m.encode_text(self.name_en)
        out.append(self.special & 0xFF)
        out += struct.pack("<i", len(self.elements))
        bfmt = _struct_index_fmt(m.bone_index_size)
        mfmt = _struct_index_fmt(m.morph_index_size)
        for el in self.elements:
            out.append(el.kind & 0xFF)
            out += struct.pack(bfmt if el.kind == 0 else mfmt, el.index)
        return bytes(out)


class _Reader:
    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0

    def u8(self) -> int:
        v = self.data[self.pos]
        self.pos += 1
        return v

    def u16(self) -> int:
        v = struct.unpack_from("<H", self.data, self.pos)[0]
        self.pos += 2
        return v

    def i32(self) -> int:
        v = struct.unpack_from("<i", self.data, self.pos)[0]
        self.pos += 4
        return v

    def f32(self) -> float:
        v = struct.unpack_from("<f", self.data, self.pos)[0]
        self.pos += 4
        return v

    def f32s(self, n: int) -> tuple:
        v = struct.unpack_from(f"<{n}f", self.data, self.pos)
        self.pos += 4 * n
        return v

    def raw(self, n: int) -> bytes:
        v = self.data[self.pos : self.pos + n]
        self.pos += n
        return v

    def text(self, enc: str) -> str:
        n = self.i32()
        if n < 0:
            raise PmxError(f"文本长度为负: {n} @ {self.pos}")
        b = self.raw(n)
        try:
            return b.decode(enc)
        except UnicodeDecodeError:
            return b.decode(enc, errors="replace")

    def ridx(self, size: int) -> int:
        fmt = _struct_index_fmt(size)
        v = struct.unpack_from(fmt, self.data, self.pos)[0]
        self.pos += size
        return v


# ---------------------------------------------------------------------------
# 模型
# ---------------------------------------------------------------------------


class PmxModel:
    def __init__(self) -> None:
        self.version = 2.0
        self.encoding = 0
        self.add_uv = 0
        self.vertex_index_size = 2
        self.texture_index_size = 1
        self.material_index_size = 1
        self.bone_index_size = 2
        self.morph_index_size = 2
        self.rigid_index_size = 2
        self.name_local = ""
        self.name_en = ""
        self.comment_local = ""
        self.comment_en = ""
        self.textures: list[str] = []
        self.materials: list[Material] = []
        self.morphs: list[Morph] = []
        self.frames: list[DisplayFrame] = []
        # 未解析段落的原始字节
        self.raw_sections: dict[str, bytes] = {}

    # -- 编码 ------------------------------------------------------------
    @property
    def codec(self) -> str:
        return "utf-16-le" if self.encoding == 0 else "utf-8"

    def encode_text(self, s: str) -> bytes:
        b = s.encode(self.codec)
        return struct.pack("<i", len(b)) + b

    # -- 读 --------------------------------------------------------------
    @classmethod
    def from_file(cls, path: str | Path) -> "PmxModel":
        return cls.from_bytes(Path(path).read_bytes())

    @classmethod
    def from_bytes(cls, data: bytes) -> "PmxModel":
        r = _Reader(data)
        m = cls()
        if data[:4] != b"PMX ":
            raise PmxError("不是 PMX 文件（缺少 'PMX ' 魔数）")
        r.pos = 4
        m.version = round(r.f32(), 2)
        gcount = r.u8()
        if gcount < 8:
            raise PmxError(f"PMX 头部全局设定数量异常: {gcount}")
        g = [r.u8() for _ in range(gcount)]
        (m.encoding, m.add_uv, m.vertex_index_size, m.texture_index_size,
         m.material_index_size, m.bone_index_size, m.morph_index_size,
         m.rigid_index_size) = g[:8]
        if m.vertex_index_size not in (1, 2, 4) or m.morph_index_size not in (1, 2, 4):
            raise PmxError(
                f"PMX 索引长度异常: vertex={m.vertex_index_size} morph={m.morph_index_size}"
            )
        enc = m.codec
        m.name_local = r.text(enc)
        m.name_en = r.text(enc)
        m.comment_local = r.text(enc)
        m.comment_en = r.text(enc)

        # --- 顶点 ---
        start = r.pos
        nv = r.i32()
        vskip = 12 + 12 + 8 + m.add_uv * 16
        for _ in range(nv):
            r.pos += vskip
            wt = r.u8()
            if wt == 0:
                r.pos += m.bone_index_size
            elif wt == 1:
                r.pos += 2 * m.bone_index_size + 4
            elif wt in (2, 4):
                r.pos += 4 * m.bone_index_size + 16
            elif wt == 3:
                r.pos += 2 * m.bone_index_size + 4 + 36
            else:
                raise PmxError(f"未知顶点权重类型 {wt}")
            r.pos += 4
        m.raw_sections["vertices"] = data[start : r.pos]

        # --- 面 ---
        start = r.pos
        nf = r.i32()  # PMX 中该字段是「顶点索引个数」，不是三角形数
        r.pos += nf * m.vertex_index_size
        m.raw_sections["faces"] = data[start : r.pos]

        # --- 贴图 ---
        start = r.pos
        ntex = r.i32()
        for _ in range(ntex):
            m.textures.append(r.text(enc))
        m.raw_sections["textures"] = data[start : r.pos]

        # --- 材质 ---
        start = r.pos
        nmat = r.i32()
        for _ in range(nmat):
            mat = Material()
            mat.name_local = r.text(enc)
            mat.name_en = r.text(enc)
            mat.diffuse = r.f32s(4)
            mat.specular = r.f32s(3)
            mat.specularity = r.f32()
            mat.ambient = r.f32s(3)
            mat.flags = r.u8()
            mat.edge_color = r.f32s(4)
            mat.edge_size = r.f32()
            mat.texture_index = r.ridx(m.texture_index_size)
            mat.sphere_index = r.ridx(m.texture_index_size)
            mat.sphere_mode = r.u8()
            toon = r.u8()
            if toon == 0:  # 使用贴图索引
                mat.toon_index = r.ridx(m.texture_index_size)
                mat.toon_internal = False
            else:  # 内置 toon
                mat.toon_index = r.u8()
                mat.toon_internal = True
            r.text(enc)  # 备注
            r.pos += 4  # 该材质的面顶点数
            m.materials.append(mat)
        m.raw_sections["materials"] = data[start : r.pos]

        # --- 骨骼 ---
        # 标志位按 MMD 实际读取的顺序（见 src/model/pmx_load.cpp 与
        # include/mikumikustudio/bone_layout.hpp）。
        start = r.pos
        nb = r.i32()
        for _ in range(nb):
            r.text(enc)
            r.text(enc)
            r.pos += 12  # position
            r.pos += m.bone_index_size  # parent
            r.pos += 4  # layer
            flags = r.u16()
            if flags & 0x0001:  # 接続先为骨骼索引
                r.pos += m.bone_index_size
            else:  # 接続先为 3 float 偏移
                r.pos += 12
            if flags & 0x0300:  # 付与親（回転/移動付与共用一条记录）
                r.pos += m.bone_index_size + 4
            if flags & 0x0400:  # 轴固定
                r.pos += 12
            if flags & 0x0800:  # ローカル軸
                r.pos += 24
            if flags & 0x2000:  # 外部親変形
                r.pos += 4
            if flags & 0x0020:  # IK
                r.pos += m.bone_index_size  # 目标骨骼
                r.pos += 4 + 4  # 循环次数 + 单位角
                nlink = r.i32()
                if nlink < 0:
                    raise PmxError(f"IK 链接数为负: {nlink}")
                for _ in range(nlink):
                    r.pos += m.bone_index_size
                    lim = r.u8()
                    if lim:
                        r.pos += 24
        m.raw_sections["bones"] = data[start : r.pos]

        # --- 表情 ---
        nm = r.i32()
        for _ in range(nm):
            mstart = r.pos
            m.morphs.append(_read_morph(r, m, enc, mstart))

        # --- 表示枠 ---
        nfrm = r.i32()
        for _ in range(nfrm):
            fstart = r.pos
            fl = r.text(enc)
            fe = r.text(enc)
            special = r.u8()
            n = r.i32()
            elts = []
            for _ in range(n):
                kind = r.u8()
                idx = r.ridx(m.bone_index_size if kind == 0 else m.morph_index_size)
                elts.append(FrameElement(kind, idx))
            m.frames.append(
                DisplayFrame(fl, fe, special, elts, raw=data[fstart : r.pos])
            )

        # --- 刚体 ---
        start = r.pos
        nr = r.i32()
        for _ in range(nr):
            r.text(enc)  # 刚体名
            r.text(enc)
            r.pos += m.bone_index_size  # 关联骨骼
            r.pos += 1 + 2  # 组 + 非碰撞组掩码
            r.pos += 1  # 形状
            r.pos += 12  # 尺寸
            r.pos += 12  # 位置
            r.pos += 12  # 旋转（欧拉角）
            r.pos += 4 * 5  # 质量/移动衰减/旋转衰减/反弹力/摩擦
            r.pos += 1  # 物理模式
        m.raw_sections["rigid_bodies"] = data[start : r.pos]

        # --- 关节 ---
        start = r.pos
        nj = r.i32()
        for _ in range(nj):
            r.text(enc)  # 关节名
            r.text(enc)
            r.pos += 1  # 类型
            r.pos += m.rigid_index_size * 2  # 刚体 A/B
            r.pos += 12 + 12  # 位置 + 旋转
            r.pos += 12 + 12  # 移动限制 下/上
            r.pos += 12 + 12  # 旋转限制 下/上
            r.pos += 12 + 12  # 弹簧 移动/旋转
        m.raw_sections["joints"] = data[start : r.pos]

        # --- 软体（PMX 2.1）---
        if m.version >= 2.1:
            start = r.pos
            nsb = r.i32()
            if nsb:
                raise PmxError(
                    f"该 PMX 含 {nsb} 个软体（PMX 2.1），本工具尚未实现软体段解析；"
                    "请先用其他工具转成 PMX 2.0 或移除软体。"
                )
            m.raw_sections["soft_bodies"] = data[start : r.pos]

        m.raw_sections["tail"] = data[r.pos :]
        return m

    # -- 写 --------------------------------------------------------------
    def to_bytes(self) -> bytes:
        out = bytearray()
        out += b"PMX "
        out += struct.pack("<f", self.version)
        out.append(8)
        out += bytes(
            [
                self.encoding,
                self.add_uv,
                self.vertex_index_size,
                self.texture_index_size,
                self.material_index_size,
                self.bone_index_size,
                self.morph_index_size,
                self.rigid_index_size,
            ]
        )
        out += self.encode_text(self.name_local)
        out += self.encode_text(self.name_en)
        out += self.encode_text(self.comment_local)
        out += self.encode_text(self.comment_en)
        for key in ("vertices", "faces", "textures", "materials", "bones"):
            out += self.raw_sections[key]
        out += struct.pack("<i", len(self.morphs))
        for mo in self.morphs:
            out += mo.to_bytes(self)
        out += struct.pack("<i", len(self.frames))
        for fr in self.frames:
            out += fr.to_bytes(self)
        for key in ("rigid_bodies", "joints", "soft_bodies", "tail"):
            if key in self.raw_sections:
                out += self.raw_sections[key]
        return bytes(out)

    def save(self, path: str | Path) -> None:
        Path(path).write_bytes(self.to_bytes())

    # -- 辅助 ------------------------------------------------------------
    def morph_index(self, name: str) -> int:
        for i, mo in enumerate(self.morphs):
            if mo.name_local == name:
                return i
        return -1

    def find_morphs(self, name: str) -> list[int]:
        return [i for i, mo in enumerate(self.morphs) if mo.name_local == name]

    # -- 段内计数（直接从原始字节头部取，避免重复解析） --------------------
    def _count_of(self, section: str) -> int:
        raw = self.raw_sections.get(section)
        if not raw:
            return 0
        return struct.unpack_from("<i", raw, 0)[0]

    @property
    def vertex_count(self) -> int:
        return self._count_of("vertices")

    @property
    def face_index_count(self) -> int:
        return self._count_of("faces")

    @property
    def texture_count(self) -> int:
        return len(self.textures)

    @property
    def material_count(self) -> int:
        return self._count_of("materials")

    @property
    def bone_count(self) -> int:
        return self._count_of("bones")

    @property
    def rigid_count(self) -> int:
        return self._count_of("rigid_bodies")

    @property
    def joint_count(self) -> int:
        return self._count_of("joints")

    def index_capacity(self, size: int) -> int:
        """该索引用途能表示的最大索引数（PMX 用有符号索引）。"""
        return {1: 128, 2: 32768, 4: 2147483648}[size]


def _read_morph(r: _Reader, m: PmxModel, enc: str, mstart: int) -> Morph:
    name_local = r.text(enc)
    name_en = r.text(enc)
    panel = r.u8()
    mtype = r.u8()
    n = r.i32()
    if n < 0:
        raise PmxError(f"morph '{name_local}' 偏移数量为负: {n}")
    offsets: list = []
    if mtype == 0:
        for _ in range(n):
            idx = r.ridx(m.morph_index_size)
            offsets.append((idx, r.f32()))
    elif mtype == 1:
        for _ in range(n):
            idx = r.ridx(m.vertex_index_size)
            offsets.append((idx, r.f32(), r.f32(), r.f32()))
    elif mtype == 2:
        for _ in range(n):
            idx = r.ridx(m.bone_index_size)
            offsets.append((idx, *r.f32s(7)))
    elif 3 <= mtype <= 7:
        for _ in range(n):
            idx = r.ridx(m.vertex_index_size)
            offsets.append((idx, *r.f32s(4)))
    elif mtype == 8:
        for _ in range(n):
            idx = r.ridx(m.material_index_size)
            cmode = r.u8()
            rest = r.f32s(4 + 3 + 1 + 3 + 4 + 1 + 4 + 4 + 4)
            offsets.append((idx, cmode, *rest))
    elif mtype == 9:
        for _ in range(n):
            idx = r.ridx(m.morph_index_size)
            offsets.append((idx, r.f32()))
    elif mtype == 10:
        for _ in range(n):
            idx = r.ridx(m.rigid_index_size)
            local = r.u8()
            offsets.append((idx, local, *r.f32s(6)))
    else:
        raise PmxError(f"morph '{name_local}' 类型未知: {mtype}")
    raw = r.data[mstart : r.pos]
    return Morph(name_local, name_en, panel, mtype, offsets, raw=raw)
