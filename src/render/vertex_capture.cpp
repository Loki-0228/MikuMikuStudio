// ===========================================================================
// D3D9 vertex-shader output capture -- GPU raster readback
// ===========================================================================
// Why this exists
//   MME/MMV effects deform vertices on the GPU, so the POSITION the rasterizer
//   receives only exists inside the vertex shader.  Alembic export needs the
//   deformed geometry; re-reading the input vertex buffer would export the
//   undeformed mesh.
//
// How the capture works
//   1. Read the bound vertex shader bytecode (GetFunction) and copy it.
//   2. Patch the copy so the shader's own POSITION output is re-routed to a
//      free texture-coordinate output, and add a raster position that comes
//      from an extra input register.  Every other instruction stays byte for
//      byte, so constants, streams, vertex textures and flow control still
//      produce the values the real draw produces.
//        vs_1_1 / vs_2_0: writes to oPos (RASTOUT 0) are re-pointed at a free
//                         oT#; a trailing `mov oPos, vExtra` supplies the
//                         raster position.
//        vs_3_0:          the output DCL carrying the POSITION semantic is
//                         re-declared as TEXCOORD#; a new `dcl_position oM`
//                         plus a trailing `mov oM, vExtra` supplies the raster
//                         position.
//      `vExtra` is fed from an extra vertex stream holding one clip-space pixel
//      centre per source vertex, so every source vertex rasterizes to its own
//      known pixel.
//   3. A generated pixel shader copies the re-routed POSITION to an
//      A32B32G32R32F render target with a POINTLIST draw: one point per source
//      vertex, one pixel per vertex.
//   4. GetRenderTargetData copies the render target into a system-memory
//      surface, which is locked and turned into clipPositions.
//
// Everything touched on the device (shaders, declaration, streams, render
// targets, depth stencil, viewport, render states) is saved first and put back
// before the function returns, on every path including failures.  The capture
// never calls BeginScene / EndScene / Present and never touches shader
// constants, textures or samplers, so it is safe inside an open scene.
//
// Bytecode token layout was measured against real D3DCompile output
// (d3dcompiler_47.dll, HLSL compiler 10.1) rather than taken from memory:
//   * instruction token: opcode = bits 0..15, length = bits 24..27, counting
//     the DWORDs *after* the instruction token.  `def c4, 1, 0, 0, 0` therefore
//     has length 5 and spans 6 DWORDs.
//   * comment token: opcode 0xFFFE, size = bits 16..30, also counted after the
//     token.  D3DX puts a CTAB comment *before* the instructions, so the
//     scanner has to skip it or it will read a second version token and float
//     literals as instructions.
//   * dcl: length 2 (usage token, destination token).
//   * def / defi: length 5 (destination token, four literals); defb: 2.
//   * end: 0x0000FFFF, optionally followed by trailing comments only.
// ===========================================================================

// The defines have to precede the header: it includes <d3d9.h>, which pulls in
// <Windows.h> and its min / max macros.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "mikumikustudio/vertex_capture.hpp"

#include <Windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>
#include <cstdint>
#include <climits>
#include <iterator>

namespace mikumikustudio {
namespace vertex_capture {
namespace {

using u32 = DWORD;

// ---- shader bytecode tokens ----------------------------------------------
constexpr u32 kOpComment = 0x0000FFFE;
constexpr u32 kOpEnd = 0x0000FFFF;
constexpr u32 kOpDcl = 0x0000001F;
constexpr u32 kOpDef = 0x00000051;
constexpr u32 kOpDefI = 0x00000030;
constexpr u32 kOpDefB = 0x0000002F;

constexpr u32 kInstLengthMask = 0x0F000000;
constexpr u32 kInstLengthShift = 24;
constexpr u32 kCommentSizeMask = 0x7FFF0000;
constexpr u32 kCommentSizeShift = 16;
constexpr u32 kOpcodeMask = 0x0000FFFF;

constexpr u32 kRegTemp = 0;
constexpr u32 kRegInput = 1;
constexpr u32 kRegTexture = 3;
constexpr u32 kRegRastOut = 4;
constexpr u32 kRegTexCrdOut = 6;  // also D3DSPR_OUTPUT for vs_3_0

constexpr u32 kRegTypeShift = 28;
constexpr u32 kRegTypeMask = 0x70000000;
constexpr u32 kRegNumMask = 0x000007FF;
constexpr u32 kWriteMaskMask = 0x000F0000;
constexpr u32 kTokenFlag = 0x80000000;  // set on both source and dest tokens
constexpr u32 kSwizzleXyzw = 0x00E40000;

constexpr u32 kUsagePosition = 0;
constexpr u32 kUsageBlendWeight = 1;
constexpr u32 kUsageBlendIndices = 2;
constexpr u32 kUsagePSize = 4;
constexpr u32 kUsageTexCoord = 5;
constexpr u32 kUsageTangent = 6;
constexpr u32 kUsageBinormal = 7;
constexpr u32 kUsageTessFactor = 8;
constexpr u32 kUsageColor = 10;

constexpr u32 kRastOutPosition = 0;
constexpr u32 kRastOutPointSize = 2;

// Register ceilings per shader model.
constexpr u32 kMaxTempRegisters = 12;       // vs_1_1 / vs_2_0: r0..r11
constexpr u32 kMaxOutputRegistersVs3 = 12;  // vs_3_0: o0..o11
constexpr u32 kMaxInputRegisters = 16;      // v0..v15
constexpr u32 kNoRegister = 0xFFFFFFFFu;

// Pixel shader tokens, reproducing what D3DCompile emits for
// `dcl t<N> / mov oC0, t<N>` (ps_2_0) and
// `dcl_texcoord<N> v0 / mov oC0, v0` (ps_3_0).
constexpr u32 kPsVersion2_0 = 0xFFFF0200;
constexpr u32 kPsVersion3_0 = 0xFFFF0300;
constexpr u32 kOpDclLength2 = 0x0200001F;  // dcl, length 2
constexpr u32 kOpMovLength2 = 0x02000001;  // mov, length 2
constexpr u32 kPsColorOut0 = 0x800F0800;   // oC0 destination token
constexpr u32 kPsTextureInputBase = 0xB0000000 | kSwizzleXyzw;   // t0 source
constexpr u32 kPsTexCoordInputBase = 0x90000000 | kSwizzleXyzw;  // v0 source

// ---- small helpers --------------------------------------------------------
std::string HResultText(HRESULT result) {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "0x%08lX",
                  static_cast<unsigned long>(result));
    return buffer;
}

u32 RegType(u32 token) { return (token & kRegTypeMask) >> kRegTypeShift; }
u32 RegNum(u32 token) { return token & kRegNumMask; }
u32 WriteMask(u32 token) { return (token & kWriteMaskMask) >> 16; }

u32 DestToken(u32 regType, u32 regNum, u32 writeMask) {
    return kTokenFlag | (regType << kRegTypeShift) | (writeMask << 16) | regNum;
}

u32 SourceToken(u32 regType, u32 regNum) {
    return kTokenFlag | (regType << kRegTypeShift) | kSwizzleXyzw | regNum;
}

u32 DclUsageToken(u32 usage, u32 usageIndex) {
    return kTokenFlag | (usageIndex << 16) | usage;
}

DWORD FloatBits(float value) {
    DWORD bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

template <typename T>
struct ComPtr {
    T* ptr = nullptr;
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr() { Reset(); }
    void Reset(T* value = nullptr) {
        if (ptr != nullptr) ptr->Release();
        ptr = value;
    }
    T** Out() {
        Reset();
        return &ptr;
    }
    T* operator->() const { return ptr; }
    explicit operator bool() const { return ptr != nullptr; }
};

// True for opcodes whose first operand token is not a destination register.
// Reading a source token as if it were a destination would let constant
// literals or predicate sources masquerade as oPos writes.
bool HasNoDestToken(u32 opcode) {
    switch (opcode) {
        case 0x0000:  // nop
        case 0x0019:  // call
        case 0x001A:  // callnz
        case 0x001B:  // loop
        case 0x001C:  // ret
        case 0x001D:  // endloop
        case 0x001E:  // label
        case 0x0026:  // rep
        case 0x0027:  // endrep
        case 0x0028:  // if
        case 0x0029:  // ifc
        case 0x002A:  // else
        case 0x002B:  // endif
        case 0x002C:  // break
        case 0x002D:  // breakc
        case 0x004E:  // breakp
        case 0x0070:  // phase
            return true;
        default:
            return false;
    }
}

struct DclInfo {
    u32 usage = 0;
    u32 usageIndex = 0;
    u32 regType = 0;
    u32 regNum = 0;
    std::size_t usageToken = 0;
    std::size_t destToken = 0;
};

struct Semantic {
    u32 usage = 0;
    u32 usageIndex = 0;
};

// Everything the patcher needs to know about one bound vertex shader.
struct VsScan {
    bool isVs3 = false;
    u32 major = 0;
    u32 minor = 0;
    std::size_t endToken = 0;      // index of the end token
    std::size_t lastDclToken = 0;  // index of the last dcl instruction token
    std::size_t lastInputDclToken = 0;  // index of the last input dcl
    bool hasDcl = false;
    bool hasInputDcl = false;
    bool inputDeclared[kMaxInputRegisters]{};
    bool tempUsed[kMaxTempRegisters]{};
    bool texCoordOutputUsed[8]{};
    bool outputRegisterUsed[16]{};
    bool texCoordSemanticUsed[16]{};
    std::vector<std::size_t> positionWrites;   // dest tokens writing oPos
    std::vector<std::size_t> pointSizeWrites;  // dest tokens writing oPts
    std::vector<DclInfo> dcls;
    int positionDclCount = 0;
    DclInfo positionDcl{};
    std::vector<DclInfo> pointSizeDcls;
    bool usesSubroutines = false;
};

bool ScanVertexShader(const std::vector<u32>& code, VsScan& scan,
                      std::string& error) {
    if (code.size() < 2) {
        error = "vertex shader bytecode is too short";
        return false;
    }
    const u32 version = code[0];
    if (((version >> 16) & 0xFFFF) != 0xFFFE) {
        error = "the bound shader is not a vertex shader";
        return false;
    }
    scan.major = (version >> 8) & 0xFF;
    scan.minor = version & 0xFF;
    if (scan.major < 1 || scan.major > 3) {
        error = "unsupported vertex shader version " +
                std::to_string(scan.major) + "." + std::to_string(scan.minor);
        return false;
    }
    scan.isVs3 = scan.major >= 3;

    int flowDepth = 0;
    bool endFound = false;
    std::size_t index = 1;
    while (index < code.size()) {
        const u32 token = code[index];
        const u32 opcode = token & kOpcodeMask;
        std::size_t dwords = 0;
        if (opcode == kOpComment) {
            dwords = 1 + ((token & kCommentSizeMask) >> kCommentSizeShift);
        } else if (opcode == kOpEnd) {
            if (flowDepth != 0) {
                error = "vertex shader has unbalanced flow control before end";
                return false;
            }
            scan.endToken = index;
            endFound = true;
            index += 1;
            // Only trailing comments may follow end.
            while (index < code.size()) {
                const u32 trailing = code[index];
                if ((trailing & kOpcodeMask) != kOpComment) {
                    error = "unexpected shader token after end";
                    return false;
                }
                index +=
                    1 + ((trailing & kCommentSizeMask) >> kCommentSizeShift);
            }
            break;
        } else {
            dwords = 1 + ((token & kInstLengthMask) >> kInstLengthShift);
            if (opcode == kOpDef || opcode == kOpDefI) {
                if (dwords < 6) dwords = 6;  // dest token + four literals
            } else if (opcode == kOpDefB) {
                if (dwords < 3) dwords = 3;  // dest token + one literal
            }
        }
        if (index + dwords > code.size()) {
            error = "vertex shader bytecode is truncated";
            return false;
        }

        if (opcode == kOpDcl) {
            if (dwords != 3) {
                error = "malformed dcl instruction in vertex shader bytecode";
                return false;
            }
            DclInfo info;
            info.usageToken = index + 1;
            info.destToken = index + 2;
            info.usage = code[index + 1] & 0xF;
            info.usageIndex = (code[index + 1] >> 16) & 0xF;
            info.regType = RegType(code[index + 2]);
            info.regNum = RegNum(code[index + 2]);
            scan.dcls.push_back(info);
            scan.lastDclToken = index;
            scan.hasDcl = true;
            if (info.regType == kRegInput) {
                if (info.regNum < kMaxInputRegisters)
                    scan.inputDeclared[info.regNum] = true;
                scan.lastInputDclToken = index;
                scan.hasInputDcl = true;
            } else if (info.regType == kRegTexCrdOut) {
                if (info.regNum < 16) scan.outputRegisterUsed[info.regNum] = true;
                if (info.usage == kUsageTexCoord && info.usageIndex < 16)
                    scan.texCoordSemanticUsed[info.usageIndex] = true;
                if (info.usage == kUsagePosition) {
                    ++scan.positionDclCount;
                    if (scan.positionDclCount == 1) scan.positionDcl = info;
                } else if (info.usage == kUsagePSize) {
                    scan.pointSizeDcls.push_back(info);
                }
            }
        } else if (opcode == kOpDef || opcode == kOpDefI ||
                   opcode == kOpDefB) {
            // Literal constants: skipped, never patched.
        } else if (!HasNoDestToken(opcode)) {
            const u32 dest = code[index + 1];
            const u32 regType = RegType(dest);
            const u32 regNum = RegNum(dest);
            if (regType == kRegTemp) {
                if (regNum < kMaxTempRegisters) scan.tempUsed[regNum] = true;
            } else if (regType == kRegTexCrdOut) {
                if (regNum < 8) scan.texCoordOutputUsed[regNum] = true;
                if (regNum < 16) scan.outputRegisterUsed[regNum] = true;
            } else if (regType == kRegRastOut) {
                if (regNum == kRastOutPosition)
                    scan.positionWrites.push_back(index + 1);
                else if (regNum == kRastOutPointSize)
                    scan.pointSizeWrites.push_back(index + 1);
            }
        }

        // Flow control bookkeeping.  Subroutines are rejected by the caller:
        // code appended after a subroutine definition may never execute.
        switch (opcode) {
            case 0x0028:  // if
            case 0x0029:  // ifc
            case 0x001B:  // loop
            case 0x0026:  // rep
                ++flowDepth;
                break;
            case 0x002B:  // endif
            case 0x001D:  // endloop
            case 0x0027:  // endrep
                if (flowDepth > 0) --flowDepth;
                break;
            case 0x0019:  // call
            case 0x001A:  // callnz
            case 0x001C:  // ret
            case 0x001E:  // label
                scan.usesSubroutines = true;
                break;
            default:
                break;
        }

        index += dwords;
    }
    if (!endFound) {
        error = "vertex shader bytecode has no end token";
        return false;
    }
    return true;
}

// ---- register / semantic selection ---------------------------------------
bool FreeTexCoordOutputRegister(const VsScan& scan, u32& regNum,
                                std::string& error) {
    const u32 limit = scan.major <= 1 ? 4u : 8u;
    for (u32 candidate = 0; candidate < limit; ++candidate) {
        if (!scan.texCoordOutputUsed[candidate]) {
            regNum = candidate;
            return true;
        }
    }
    error = "vertex shader writes every texture coordinate output, so no free "
            "oT register is left to carry the captured POSITION";
    return false;
}

bool FreeTexCoordSemanticIndex(const VsScan& scan, u32& semanticIndex,
                              std::string& error) {
    for (u32 candidate = 0; candidate < 16; ++candidate) {
        if (!scan.texCoordSemanticUsed[candidate]) {
            semanticIndex = candidate;
            return true;
        }
    }
    error = "vs_3_0 vertex shader uses every TEXCOORD output semantic";
    return false;
}

bool FreeOutputRegister(const VsScan& scan, u32& regNum, std::string& error) {
    for (u32 candidate = 0; candidate < kMaxOutputRegistersVs3; ++candidate) {
        if (!scan.outputRegisterUsed[candidate]) {
            regNum = candidate;
            return true;
        }
    }
    error = "vs_3_0 vertex shader uses every output register, so no free o "
            "register is left for the capture POSITION";
    return false;
}

bool FreeTempRegister(const VsScan& scan, u32& regNum, std::string& error) {
    for (u32 candidate = 0; candidate < kMaxTempRegisters; ++candidate) {
        if (!scan.tempUsed[candidate]) {
            regNum = candidate;
            return true;
        }
    }
    error = "vertex shader uses every temporary register, so the point size "
            "output cannot be neutralised";
    return false;
}

// Picks the semantic that feeds the capture position from the candidates the
// vertex declaration leaves free.  `reservedTexCoordIndex` keeps the input
// distinct from the TEXCOORD the shader writes the captured POSITION to; pass
// kNoRegister when there is no such output semantic (vs_2_0 / vs_1_1).
bool SelectCaptureInputSemantic(const VsScan& scan,
                                const std::vector<Semantic>& candidates,
                                u32 reservedTexCoordIndex, Semantic& chosen,
                                u32& inputReg, std::string& error) {
    for (const Semantic& candidate : candidates) {
        if (candidate.usage == kUsageTexCoord &&
            reservedTexCoordIndex != kNoRegister &&
            candidate.usageIndex == reservedTexCoordIndex) {
            continue;
        }
        bool clashesWithShader = false;
        for (const DclInfo& dcl : scan.dcls) {
            if (dcl.regType == kRegInput && dcl.usage == candidate.usage &&
                dcl.usageIndex == candidate.usageIndex) {
                clashesWithShader = true;
                break;
            }
        }
        if (clashesWithShader) continue;
        for (u32 candidateReg = 0; candidateReg < kMaxInputRegisters;
             ++candidateReg) {
            if (!scan.inputDeclared[candidateReg]) {
                chosen = candidate;
                inputReg = candidateReg;
                return true;
            }
        }
        error = "vertex shader declares every input register, so no free v "
                "register is left for the capture position";
        return false;
    }
    error = "no free vertex declaration semantic is left for the capture "
            "position input";
    return false;
}

// ---- shader patching ------------------------------------------------------
// One group of instructions to splice into the bytecode at a token index.
struct Insertion {
    std::size_t at = 0;
    std::vector<u32> tokens;
};

// Rebuilds the bytecode with the shader's own POSITION output re-routed and a
// trailing write of the capture position.  `rewrites` maps token index to its
// replacement; `insertions` adds whole instructions at given token indexes.
bool RebuildShader(const std::vector<u32>& code, const VsScan& scan,
                   const std::vector<std::pair<std::size_t, u32>>& rewrites,
                   const std::vector<Insertion>& insertions,
                   const std::vector<u32>& trailingInstructions,
                   std::vector<u32>& patched) {
    patched.clear();
    std::size_t inserted = 0;
    patched.reserve(code.size() + 16);
    auto emitInsertionsAt = [&](std::size_t index) {
        while (inserted < insertions.size() &&
               insertions[inserted].at == index) {
            patched.insert(patched.end(), insertions[inserted].tokens.begin(),
                           insertions[inserted].tokens.end());
            ++inserted;
        }
    };
    for (std::size_t index = 0; index < scan.endToken; ++index) {
        emitInsertionsAt(index);
        u32 token = code[index];
        for (const auto& rewrite : rewrites) {
            if (rewrite.first == index) {
                token = rewrite.second;
                break;
            }
        }
        patched.push_back(token);
    }
    emitInsertionsAt(scan.endToken);
    patched.insert(patched.end(), trailingInstructions.begin(),
                   trailingInstructions.end());
    patched.push_back(kOpEnd);
    for (std::size_t index = scan.endToken + 1; index < code.size(); ++index)
        patched.push_back(code[index]);
    return true;
}

std::vector<u32> DclInstruction(u32 usage, u32 usageIndex, u32 regType,
                                u32 regNum) {
    return {kOpDclLength2, DclUsageToken(usage, usageIndex),
            DestToken(regType, regNum, 0xF)};
}

std::vector<u32> MoveInstruction(u32 destRegType, u32 destRegNum,
                                 u32 srcRegType, u32 srcRegNum) {
    return {kOpMovLength2, DestToken(destRegType, destRegNum, 0xF),
            SourceToken(srcRegType, srcRegNum)};
}

// vs_1_1 / vs_2_0: oPos writes move to a free oT#, a free input register is
// declared and oPos is written from it at the end.
bool BuildVs2Shader(const std::vector<u32>& code, const VsScan& scan,
                    const Semantic& captureSemantic, u32 captureInputReg,
                    u32 captureTexCoordReg, std::vector<u32>& patched,
                    std::string& error) {
    if (scan.positionWrites.empty()) {
        error = "vertex shader writes no POSITION output";
        return false;
    }

    std::vector<std::pair<std::size_t, u32>> rewrites;
    for (std::size_t tokenIndex : scan.positionWrites) {
        const u32 token = code[tokenIndex];
        rewrites.emplace_back(
            tokenIndex,
            DestToken(kRegTexCrdOut, captureTexCoordReg, WriteMask(token)));
    }
    // A shader-written point size would stretch every capture point over many
    // pixels; redirect those writes into an unused temporary instead.
    if (!scan.pointSizeWrites.empty()) {
        u32 temp = 0;
        if (!FreeTempRegister(scan, temp, error)) return false;
        for (std::size_t tokenIndex : scan.pointSizeWrites) {
            const u32 token = code[tokenIndex];
            rewrites.emplace_back(tokenIndex,
                                  DestToken(kRegTemp, temp, WriteMask(token)));
        }
    }

    const std::vector<u32> inserted = DclInstruction(
        captureSemantic.usage, captureSemantic.usageIndex, kRegInput,
        captureInputReg);
    const std::vector<u32> trailing =
        MoveInstruction(kRegRastOut, kRastOutPosition, kRegInput,
                        captureInputReg);
    const std::vector<Insertion> insertions = {
        {scan.hasDcl ? scan.lastDclToken + 3 : 1, inserted}};
    return RebuildShader(code, scan, rewrites, insertions, trailing, patched);
}

// vs_3_0: the POSITION output DCL becomes a TEXCOORD# output, an additional
// output register is declared with the POSITION semantic and written from the
// capture input register.
bool BuildVs3Shader(const std::vector<u32>& code, const VsScan& scan,
                    const Semantic& captureSemantic, u32 captureInputReg,
                    u32 captureOutputReg, u32 captureTexCoordIndex,
                    std::vector<u32>& patched, std::string& error) {
    if (scan.positionDclCount == 0) {
        error = "vs_3_0 vertex shader declares no POSITION output";
        return false;
    }
    if (scan.positionDclCount > 1) {
        error = "vs_3_0 vertex shader declares more than one POSITION output";
        return false;
    }

    std::vector<std::pair<std::size_t, u32>> rewrites;
    rewrites.emplace_back(
        scan.positionDcl.usageToken,
        DclUsageToken(kUsageTexCoord, captureTexCoordIndex));
    // dcl_psize would resize each capture point; make those outputs inert.
    u32 usedSemantics[16]{};
    for (u32 index = 0; index < 16; ++index)
        usedSemantics[index] = scan.texCoordSemanticUsed[index] ? 1u : 0u;
    usedSemantics[captureTexCoordIndex] = 1;
    for (const DclInfo& psize : scan.pointSizeDcls) {
        u32 freeIndex = 16;
        for (u32 candidate = 0; candidate < 16; ++candidate) {
            if (usedSemantics[candidate] == 0) {
                freeIndex = candidate;
                break;
            }
        }
        if (freeIndex == 16) {
            error = "vs_3_0 vertex shader uses every TEXCOORD output semantic";
            return false;
        }
        usedSemantics[freeIndex] = 1;
        rewrites.emplace_back(psize.usageToken,
                              DclUsageToken(kUsageTexCoord, freeIndex));
    }

    // The capture input declaration goes with the other input declarations;
    // the new POSITION output declaration goes with the output declarations.
    const std::size_t inputInsertAt =
        scan.hasInputDcl ? scan.lastInputDclToken + 3 : 1;
    const std::size_t outputInsertAt =
        scan.hasDcl ? scan.lastDclToken + 3 : 1;
    std::vector<Insertion> insertions;
    if (inputInsertAt <= outputInsertAt) {
        insertions.push_back({inputInsertAt,
                              DclInstruction(captureSemantic.usage,
                                             captureSemantic.usageIndex,
                                             kRegInput, captureInputReg)});
        insertions.push_back({outputInsertAt,
                              DclInstruction(kUsagePosition, 0,
                                             kRegTexCrdOut,
                                             captureOutputReg)});
    } else {
        insertions.push_back({outputInsertAt,
                              DclInstruction(kUsagePosition, 0,
                                             kRegTexCrdOut,
                                             captureOutputReg)});
        insertions.push_back({inputInsertAt,
                              DclInstruction(captureSemantic.usage,
                                             captureSemantic.usageIndex,
                                             kRegInput, captureInputReg)});
    }
    const std::vector<u32> trailing = MoveInstruction(
        kRegTexCrdOut, captureOutputReg, kRegInput, captureInputReg);
    return RebuildShader(code, scan, rewrites, insertions, trailing, patched);
}

// Pixel shader that copies the re-routed POSITION of the capture draw into oC0:
// `dcl t<N> / mov oC0, t<N>` for ps_2_0, `dcl_texcoord<N> v0 / mov oC0, v0`
// for ps_3_0.
std::vector<u32> BuildCapturePixelShader(bool vs3, u32 texCoordIndex) {
    std::vector<u32> shader;
    shader.push_back(vs3 ? kPsVersion3_0 : kPsVersion2_0);
    shader.push_back(kOpDclLength2);
    if (vs3) {
        shader.push_back(DclUsageToken(kUsageTexCoord, texCoordIndex));
        shader.push_back(DestToken(kRegInput, 0, 0xF));
        shader.push_back(kOpMovLength2);
        shader.push_back(kPsColorOut0);
        shader.push_back(kPsTexCoordInputBase);
    } else {
        shader.push_back(kTokenFlag | kUsagePosition);
        shader.push_back(DestToken(kRegTexture, texCoordIndex, 0xF));
        shader.push_back(kOpMovLength2);
        shader.push_back(kPsColorOut0);
        shader.push_back(kPsTextureInputBase | texCoordIndex);
    }
    shader.push_back(kOpEnd);
    return shader;
}

// ---- device state save / restore -----------------------------------------
const D3DRENDERSTATETYPE kSavedRenderStates[] = {
    D3DRS_ZENABLE,           D3DRS_ZWRITEENABLE,
    D3DRS_ZFUNC,             D3DRS_FILLMODE,
    D3DRS_CULLMODE,          D3DRS_ALPHATESTENABLE,
    D3DRS_ALPHABLENDENABLE,  D3DRS_SEPARATEALPHABLENDENABLE,
    D3DRS_BLENDOP,           D3DRS_SRCBLEND,
    D3DRS_DESTBLEND,         D3DRS_FOGENABLE,
    D3DRS_STENCILENABLE,     D3DRS_CLIPPING,
    D3DRS_SCISSORTESTENABLE, D3DRS_COLORWRITEENABLE,
    D3DRS_SRGBWRITEENABLE,   D3DRS_MULTISAMPLEANTIALIAS,
    D3DRS_POINTSIZE,         D3DRS_POINTSIZE_MIN,
    D3DRS_POINTSIZE_MAX,     D3DRS_POINTSPRITEENABLE,
    D3DRS_POINTSCALEENABLE,
};

DWORD CaptureRenderStateValue(D3DRENDERSTATETYPE state) {
    switch (state) {
        case D3DRS_ZENABLE:
        case D3DRS_ZWRITEENABLE:
        case D3DRS_ALPHATESTENABLE:
        case D3DRS_ALPHABLENDENABLE:
        case D3DRS_SEPARATEALPHABLENDENABLE:
        case D3DRS_FOGENABLE:
        case D3DRS_STENCILENABLE:
        case D3DRS_CLIPPING:
        case D3DRS_SCISSORTESTENABLE:
        case D3DRS_SRGBWRITEENABLE:
        case D3DRS_MULTISAMPLEANTIALIAS:
        case D3DRS_POINTSPRITEENABLE:
        case D3DRS_POINTSCALEENABLE:
            return FALSE;
        case D3DRS_ZFUNC:
            return D3DCMP_ALWAYS;
        case D3DRS_FILLMODE:
            return D3DFILL_SOLID;
        case D3DRS_CULLMODE:
            return D3DCULL_NONE;
        case D3DRS_BLENDOP:
            return D3DBLENDOP_ADD;
        case D3DRS_SRCBLEND:
            return D3DBLEND_ONE;
        case D3DRS_DESTBLEND:
            return D3DBLEND_ZERO;
        case D3DRS_COLORWRITEENABLE:
            return 0x0000000F;
        case D3DRS_POINTSIZE:
        case D3DRS_POINTSIZE_MIN:
        case D3DRS_POINTSIZE_MAX:
            return FloatBits(1.0f);
        default:
            return 0;
    }
}

struct StreamState {
    ComPtr<IDirect3DVertexBuffer9> buffer;
    UINT offset = 0;
    UINT stride = 0;
    UINT frequency = 1;
};

class DeviceStateGuard {
public:
    explicit DeviceStateGuard(IDirect3DDevice9* device) : device_(device) {}
    DeviceStateGuard(const DeviceStateGuard&) = delete;
    DeviceStateGuard& operator=(const DeviceStateGuard&) = delete;
    ~DeviceStateGuard() { Restore(); }

    bool Save(unsigned streamCount, std::string& error) {
        streamCount_ = streamCount;
        if (FAILED(device_->GetVertexShader(vertexShader_.Out())) ||
            FAILED(device_->GetPixelShader(pixelShader_.Out())) ||
            FAILED(device_->GetVertexDeclaration(declaration_.Out()))) {
            error = "cannot read the current vertex shader state";
            return false;
        }
        device_->GetFVF(&fvf_);
        for (unsigned index = 0; index < streamCount_; ++index) {
            StreamState& stream = streams_[index];
            const HRESULT result = device_->GetStreamSource(
                index, stream.buffer.Out(), &stream.offset, &stream.stride);
            if (FAILED(result)) {
                error = "GetStreamSource(" + std::to_string(index) +
                        ") failed with " + HResultText(result);
                return false;
            }
            UINT frequency = 1;
            if (FAILED(device_->GetStreamSourceFreq(index, &frequency)))
                frequency = 1;
            stream.frequency = frequency;
        }
        // Render targets 1..3 only exist when the device exposes them, and
        // caps.NumSimultaneousRTs is not a reliable bound on every driver, so
        // they are probed until the runtime refuses.
        renderTargetCount_ = 1;
        {
            const HRESULT result =
                device_->GetRenderTarget(0, renderTargets_[0].Out());
            if (FAILED(result)) {
                error = "GetRenderTarget(0) failed with " + HResultText(result);
                return false;
            }
        }
        for (unsigned index = 1; index < 4; ++index) {
            IDirect3DSurface9* surface = nullptr;
            if (FAILED(device_->GetRenderTarget(index, &surface))) break;
            renderTargets_[index].Reset(surface);
            ++renderTargetCount_;
        }
        const HRESULT depthResult = device_->GetDepthStencilSurface(depthStencil_.Out());
        if (FAILED(depthResult) && depthResult != D3DERR_NOTFOUND) {
            error = "GetDepthStencilSurface failed";
            return false;
        }
        if (FAILED(device_->GetViewport(&viewport_))) {
            error = "GetViewport failed";
            return false;
        }
        states_.clear();
        for (D3DRENDERSTATETYPE state : kSavedRenderStates) {
            DWORD value = 0;
            const HRESULT result = device_->GetRenderState(state, &value);
            if (FAILED(result)) {
                error = "GetRenderState failed with " + HResultText(result);
                return false;
            }
            states_.push_back({state, value});
        }
        saved_ = true;
        return true;
    }

    const StreamState& stream(unsigned index) const { return streams_[index]; }
    unsigned renderTargetCount() const { return renderTargetCount_; }

    void Restore() {
        if (!saved_ || restored_) return;
        restored_ = true;
        for (const auto& state : states_)
            device_->SetRenderState(state.first, state.second);
        // The render targets come first: binding one makes the runtime reset the
        // viewport, so a viewport restored before them would be lost.
        for (unsigned index = 0; index < renderTargetCount_; ++index)
            device_->SetRenderTarget(index, renderTargets_[index].ptr);
        device_->SetDepthStencilSurface(depthStencil_.ptr);
        device_->SetViewport(&viewport_);
        for (unsigned index = 0; index < streamCount_; ++index) {
            const StreamState& stream = streams_[index];
            device_->SetStreamSource(index, stream.buffer.ptr, stream.offset,
                                     stream.stride);
            device_->SetStreamSourceFreq(index, stream.frequency);
        }
        // Restore the input layout through whichever state the caller used.
        // The saved declaration decides, because GetFVF also reports an FVF
        // equivalent for FVF-expressible declarations and would otherwise
        // replace the caller's own declaration object.
        if (declaration_.ptr != nullptr) {
            device_->SetVertexDeclaration(declaration_.ptr);
        } else {
            device_->SetFVF(fvf_);
        }
        device_->SetPixelShader(pixelShader_.ptr);
        device_->SetVertexShader(vertexShader_.ptr);
        for (StreamState& stream : streams_) stream.buffer.Reset();
        for (auto& target : renderTargets_) target.Reset();
        depthStencil_.Reset();
        declaration_.Reset();
        pixelShader_.Reset();
        vertexShader_.Reset();
    }

private:
    IDirect3DDevice9* device_ = nullptr;
    bool saved_ = false;
    bool restored_ = false;
    unsigned streamCount_ = 0;
    unsigned renderTargetCount_ = 1;
    ComPtr<IDirect3DVertexShader9> vertexShader_;
    ComPtr<IDirect3DPixelShader9> pixelShader_;
    ComPtr<IDirect3DVertexDeclaration9> declaration_;
    DWORD fvf_ = 0;
    StreamState streams_[16];
    ComPtr<IDirect3DSurface9> renderTargets_[4];
    ComPtr<IDirect3DSurface9> depthStencil_;
    D3DVIEWPORT9 viewport_{};
    std::vector<std::pair<D3DRENDERSTATETYPE, DWORD>> states_;
};

void AppendSemanticCandidates(const std::vector<D3DVERTEXELEMENT9>& elements,
                              std::vector<Semantic>& candidates) {
    auto inDeclaration = [&elements](u32 usage, u32 usageIndex) {
        for (const D3DVERTEXELEMENT9& element : elements) {
            if (element.Usage == usage && element.UsageIndex == usageIndex)
                return true;
        }
        return false;
    };
    auto add = [&](u32 usage, u32 usageIndex) {
        if (!inDeclaration(usage, usageIndex))
            candidates.push_back({usage, usageIndex});
    };
    for (u32 usageIndex = 0; usageIndex < 16; ++usageIndex)
        add(kUsageTexCoord, usageIndex);
    for (u32 usageIndex = 0; usageIndex < 4; ++usageIndex) {
        add(kUsageBlendWeight, usageIndex);
        add(kUsageBlendIndices, usageIndex);
        add(kUsageTangent, usageIndex);
        add(kUsageBinormal, usageIndex);
        add(kUsageTessFactor, usageIndex);
        add(kUsagePSize, usageIndex);
    }
    for (u32 usageIndex = 0; usageIndex < 2; ++usageIndex)
        add(kUsageColor, usageIndex);
}

unsigned NextPowerOfTwo(unsigned value) {
    unsigned result = 1;
    while (result < value && result < 0x40000000u) result <<= 1;
    return result;
}

// A capture chunk never exceeds this many pixels, keeping the render target
// and the position vertex buffer bounded (4 MB each) for long draws.
constexpr unsigned kMaxChunkPixels = 1u << 18;  // 262144

struct CapturePlan {
    std::vector<u32> shaderCode;
    bool vs3 = false;
    u32 texCoordIndex = 0;
    unsigned extraStream = 0;
    Semantic extraSemantic{};
};

// Runs the capture draw and readback with the device state saved and restored
// around it.
bool RunCapture(IDirect3DDevice9* device, const CapturePlan& plan,
                unsigned firstVertex, unsigned vertexCount,
                const std::vector<D3DVERTEXELEMENT9>& elements,
                unsigned streamCount,
                std::vector<std::array<float, 4>>& clipPositions,
                std::string& error) {
    DeviceStateGuard guard(device);
    if (!guard.Save(streamCount, error)) return false;

    D3DCAPS9 caps{};
    if (FAILED(device->GetDeviceCaps(&caps))) {
        error = "GetDeviceCaps failed";
        return false;
    }

    // ---- extended vertex declaration -------------------------------------
    std::vector<D3DVERTEXELEMENT9> extended = elements;
    D3DVERTEXELEMENT9 extraElement{};
    extraElement.Stream = static_cast<WORD>(plan.extraStream);
    extraElement.Offset = 0;
    extraElement.Type = D3DDECLTYPE_FLOAT4;
    extraElement.Method = D3DDECLMETHOD_DEFAULT;
    extraElement.Usage = static_cast<BYTE>(plan.extraSemantic.usage);
    extraElement.UsageIndex = static_cast<BYTE>(plan.extraSemantic.usageIndex);
    extended.push_back(extraElement);
    extended.push_back(D3DDECL_END());

    ComPtr<IDirect3DVertexDeclaration9> declaration;
    HRESULT result =
        device->CreateVertexDeclaration(extended.data(), declaration.Out());
    if (FAILED(result)) {
        error = "CreateVertexDeclaration failed with " + HResultText(result);
        return false;
    }

    // ---- shaders ---------------------------------------------------------
    ComPtr<IDirect3DVertexShader9> vertexShader;
    result =
        device->CreateVertexShader(plan.shaderCode.data(), vertexShader.Out());
    if (FAILED(result)) {
        error = "CreateVertexShader failed for the patched shader with " +
                HResultText(result);
        return false;
    }
    const std::vector<u32> pixelCode =
        BuildCapturePixelShader(plan.vs3, plan.texCoordIndex);
    ComPtr<IDirect3DPixelShader9> pixelShader;
    result = device->CreatePixelShader(pixelCode.data(), pixelShader.Out());
    if (FAILED(result)) {
        error = "CreatePixelShader failed with " + HResultText(result);
        return false;
    }

    // ---- render target and readback surface ------------------------------
    unsigned maxDimension = static_cast<unsigned>(
        std::min(caps.MaxTextureWidth, caps.MaxTextureHeight));
    if (maxDimension == 0) maxDimension = 2048;
    maxDimension = std::min(maxDimension, 4096u);

    unsigned width = std::min(maxDimension, 1024u);
    if (width > vertexCount) width = std::max(1u, vertexCount);
    unsigned height = std::min<unsigned>(
        maxDimension,
        static_cast<unsigned>((vertexCount + width - 1) / width));
    if (height == 0) height = 1;
    if (static_cast<unsigned long long>(width) * height > kMaxChunkPixels)
        height = std::max(1u, kMaxChunkPixels / width);
    if ((caps.TextureCaps & D3DPTEXTURECAPS_POW2) != 0) {
        width = std::min(maxDimension, NextPowerOfTwo(width));
        height = std::min(maxDimension, NextPowerOfTwo(height));
    }
    const unsigned chunkPixels = width * height;

    ComPtr<IDirect3DTexture9> target;
    result = device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET,
                                   D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT,
                                   target.Out(), nullptr);
    if (FAILED(result)) {
        error = "CreateTexture(A32B32G32R32F render target " +
                std::to_string(width) + "x" + std::to_string(height) +
                ") failed with " + HResultText(result);
        return false;
    }
    ComPtr<IDirect3DSurface9> targetSurface;
    result = target->GetSurfaceLevel(0, targetSurface.Out());
    if (FAILED(result)) {
        error = "GetSurfaceLevel failed with " + HResultText(result);
        return false;
    }

    ComPtr<IDirect3DSurface9> readback;
    result = device->CreateOffscreenPlainSurface(
        width, height, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, readback.Out(),
        nullptr);
    if (FAILED(result)) {
        error = "CreateOffscreenPlainSurface for the readback failed with " +
                HResultText(result);
        return false;
    }

    // ---- position vertex buffer ------------------------------------------
    const unsigned bufferBytes = chunkPixels * 4 * sizeof(float);
    ComPtr<IDirect3DVertexBuffer9> positions;
    bool dynamicBuffer = true;
    result = device->CreateVertexBuffer(
        bufferBytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT,
        positions.Out(), nullptr);
    if (FAILED(result)) {
        dynamicBuffer = false;
        result = device->CreateVertexBuffer(bufferBytes, 0, 0, D3DPOOL_MANAGED,
                                            positions.Out(), nullptr);
        if (FAILED(result)) {
            error = "CreateVertexBuffer for the capture positions failed with " +
                    HResultText(result);
            return false;
        }
    }

    // ---- apply the capture state -----------------------------------------
    for (D3DRENDERSTATETYPE state : kSavedRenderStates) {
        result = device->SetRenderState(state, CaptureRenderStateValue(state));
        if (FAILED(result)) {
            error = "SetRenderState failed with " + HResultText(result);
            return false;
        }
    }
    const unsigned renderTargetCount = guard.renderTargetCount();
    for (unsigned index = 0; index < renderTargetCount; ++index) {
        result = device->SetRenderTarget(
            index, index == 0 ? targetSurface.ptr : nullptr);
        if (FAILED(result)) {
            error = "SetRenderTarget(" + std::to_string(index) +
                    ") failed with " + HResultText(result);
            return false;
        }
    }
    if (FAILED(device->SetDepthStencilSurface(nullptr))) {
        error = "SetDepthStencilSurface(nullptr) failed";
        return false;
    }
    D3DVIEWPORT9 viewport{};
    viewport.X = 0;
    viewport.Y = 0;
    viewport.Width = width;
    viewport.Height = height;
    viewport.MinZ = 0.0f;
    viewport.MaxZ = 1.0f;
    if (FAILED(device->SetViewport(&viewport))) {
        error = "SetViewport failed";
        return false;
    }
    // The real draw's streams are re-based per chunk on (firstVertex + chunk
    // base) so that every chunk shades its own slice of the vertex buffer; the
    // capture stream always starts at zero and is refilled for each chunk.
    auto bindSourceStreams = [&](unsigned baseVertex) -> bool {
        for (unsigned index = 0; index < streamCount; ++index) {
            const StreamState& stream = guard.stream(index);
            if (stream.buffer.ptr == nullptr) continue;
            const unsigned long long shifted =
                static_cast<unsigned long long>(stream.offset) +
                static_cast<unsigned long long>(baseVertex) * stream.stride;
            if (shifted > 0xFFFFFFFFull) {
                error = "vertex stream offset overflows for the vertex range";
                return false;
            }
            const HRESULT streamResult = device->SetStreamSource(
                index, stream.buffer.ptr, static_cast<UINT>(shifted),
                stream.stride);
            if (FAILED(streamResult)) {
                error = "SetStreamSource(" + std::to_string(index) +
                        ") failed with " + HResultText(streamResult);
                return false;
            }
        }
        return true;
    };
    result = device->SetStreamSource(plan.extraStream, positions.ptr, 0,
                                     4 * sizeof(float));
    if (FAILED(result)) {
        error = "SetStreamSource for the capture stream failed";
        return false;
    }
    result = device->SetVertexDeclaration(declaration.ptr);
    if (SUCCEEDED(result)) result = device->SetVertexShader(vertexShader.ptr);
    if (SUCCEEDED(result)) result = device->SetPixelShader(pixelShader.ptr);
    if (FAILED(result)) {
        error = "binding the capture shaders failed with " +
                HResultText(result);
        return false;
    }

    // ---- draw every chunk and read it back -------------------------------
    clipPositions.assign(vertexCount,
                         std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
    const unsigned chunks = (vertexCount + chunkPixels - 1) / chunkPixels;
    const DWORD lockFlags = dynamicBuffer ? D3DLOCK_DISCARD : 0;
    for (unsigned chunk = 0; chunk < chunks; ++chunk) {
        const unsigned base = chunk * chunkPixels;
        const unsigned count = std::min(chunkPixels, vertexCount - base);
        if (!bindSourceStreams(firstVertex + base)) return false;
        void* destination = nullptr;
        result = positions->Lock(0, count * 4 * sizeof(float), &destination,
                                 lockFlags);
        if (FAILED(result)) {
            error = "locking the capture vertex buffer failed with " +
                    HResultText(result);
            return false;
        }
        float* pixels = static_cast<float*>(destination);
        for (unsigned index = 0; index < count; ++index) {
            const unsigned x = index % width;
            const unsigned y = index / width;
            // Clip-space centre of pixel (x, y); w = 1 keeps the point inside
            // the frustum and free of clipping.
            pixels[index * 4 + 0] =
                (static_cast<float>(x) + 0.5f) * 2.0f /
                    static_cast<float>(width) -
                1.0f;
            pixels[index * 4 + 1] =
                1.0f -
                (static_cast<float>(y) + 0.5f) * 2.0f /
                    static_cast<float>(height);
            pixels[index * 4 + 2] = 0.5f;
            pixels[index * 4 + 3] = 1.0f;
        }
        positions->Unlock();

        device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0),
                      1.0f, 0);
        result = device->DrawPrimitive(D3DPT_POINTLIST, 0, count);
        if (FAILED(result)) {
            error = "DrawPrimitive for the capture failed with " +
                    HResultText(result);
            return false;
        }
        result = device->GetRenderTargetData(targetSurface.ptr, readback.ptr);
        if (FAILED(result)) {
            error = "GetRenderTargetData failed with " + HResultText(result);
            return false;
        }
        D3DLOCKED_RECT locked{};
        result = readback->LockRect(&locked, nullptr, D3DLOCK_READONLY);
        if (FAILED(result)) {
            error = "LockRect on the readback surface failed with " +
                    HResultText(result);
            return false;
        }
        const unsigned char* source =
            static_cast<const unsigned char*>(locked.pBits);
        for (unsigned index = 0; index < count; ++index) {
            const unsigned x = index % width;
            const unsigned y = index / width;
            const unsigned char* pixel =
                source + static_cast<std::size_t>(y) * locked.Pitch + x * 16;
            std::memcpy(clipPositions[base + index].data(), pixel, 16);
        }
        readback->UnlockRect();
    }
    return true;
}

}  // namespace

static bool PrepareCapture(IDirect3DDevice9* device, CapturePlan& plan,
    std::vector<D3DVERTEXELEMENT9>& elements, unsigned& streamCount, std::string& error) {
    // ---- bound vertex shader ---------------------------------------------
    ComPtr<IDirect3DVertexShader9> boundShader;
    HRESULT result = device->GetVertexShader(boundShader.Out());
    if (FAILED(result)) {
        error = "GetVertexShader failed with " + HResultText(result);
        return false;
    }
    if (boundShader.ptr == nullptr) {
        error = "no vertex shader is bound to the device";
        return false;
    }
    UINT bytecodeSize = 0;
    result = boundShader->GetFunction(nullptr, &bytecodeSize);
    if (FAILED(result) || bytecodeSize == 0) {
        error = "GetFunction on the bound vertex shader failed with " +
                HResultText(result);
        return false;
    }
    if (bytecodeSize % 4 != 0) {
        error = "vertex shader bytecode is not a whole number of DWORDs";
        return false;
    }
    std::vector<u32> bytecode(bytecodeSize / 4);
    result = boundShader->GetFunction(bytecode.data(), &bytecodeSize);
    if (FAILED(result)) {
        error = "GetFunction on the bound vertex shader failed with " +
                HResultText(result);
        return false;
    }

    VsScan scan;
    if (!ScanVertexShader(bytecode, scan, error)) return false;
    if (scan.usesSubroutines) {
        error = "vertex shader uses subroutines (call / callnz / ret / label), "
                "which cannot carry an appended capture write";
        return false;
    }

    // ---- bound vertex declaration ----------------------------------------
    ComPtr<IDirect3DVertexDeclaration9> declaration;
    result = device->GetVertexDeclaration(declaration.Out());
    if (FAILED(result)) {
        error = "GetVertexDeclaration failed with " + HResultText(result);
        return false;
    }
    if (declaration.ptr == nullptr) {
        error = "no vertex declaration is bound to the device, so the capture "
                "stream cannot be added to the input layout";
        return false;
    }
    // GetDeclaration writes into the caller's array and takes its capacity in
    // *pNumElements on input, so it cannot be used as a size query.  The
    // reported count includes the trailing D3DDECL_END() marker, whose Stream
    // is 0xFF, so that marker is dropped here and re-added when the extended
    // declaration is built.
    D3DVERTEXELEMENT9 rawElements[MAXD3DDECLLENGTH + 1]{};
    UINT elementCount = MAXD3DDECLLENGTH + 1;
    result = declaration->GetDeclaration(rawElements, &elementCount);
    if (FAILED(result)) {
        error = "GetDeclaration on the bound vertex declaration failed with " +
                HResultText(result);
        return false;
    }
    if (elementCount == 0 || elementCount > MAXD3DDECLLENGTH + 1) {
        error = "the bound vertex declaration reports " +
                std::to_string(elementCount) + " elements";
        return false;
    }
    elements.assign(rawElements, rawElements + elementCount);
    while (!elements.empty() &&
           (elements.back().Stream == 0xFF ||
            elements.back().Type == D3DDECLTYPE_UNUSED)) {
        elements.pop_back();
    }
    if (elements.empty()) {
        error = "the bound vertex declaration is empty";
        return false;
    }

    D3DCAPS9 caps{};
    if (FAILED(device->GetDeviceCaps(&caps))) {
        error = "GetDeviceCaps failed";
        return false;
    }

    // ---- a free vertex stream for the capture positions ------------------
    bool streamUsed[16]{};
    unsigned highestStream = 0;
    for (const D3DVERTEXELEMENT9& element : elements) {
        if (element.Stream >= 16) {
            error = "the vertex declaration references an invalid stream";
            return false;
        }
        if (element.Stream < 16) streamUsed[element.Stream] = true;
        highestStream = std::max<unsigned>(highestStream, element.Stream);
    }
    unsigned maxStreams = caps.MaxStreams == 0 ? 16 : caps.MaxStreams;
    if (maxStreams > 16) maxStreams = 16;
    unsigned extraStream = maxStreams;
    for (unsigned candidate = 0; candidate < maxStreams; ++candidate) {
        if (!streamUsed[candidate]) {
            extraStream = candidate;
            break;
        }
    }
    if (extraStream >= maxStreams) {
        error = "the vertex declaration uses every vertex stream (MaxStreams=" +
                std::to_string(maxStreams) +
                "), so the capture positions cannot be fed";
        return false;
    }
    streamCount = std::max(highestStream + 1, extraStream + 1);

    // Instanced streams would make the capture draw consume instance data the
    // real draw does not, so they are rejected instead of guessed at.
    for (unsigned index = 0; index < streamCount; ++index) {
        UINT frequency = 1;
        if (SUCCEEDED(device->GetStreamSourceFreq(index, &frequency)) &&
            frequency != 1) {
            error = "vertex stream " + std::to_string(index) +
                    " uses instancing (frequency " + std::to_string(frequency) +
                    "), which the capture draw cannot reproduce";
            return false;
        }
    }

    std::vector<Semantic> candidates;
    AppendSemanticCandidates(elements, candidates);
    if (candidates.empty()) {
        error = "the vertex declaration leaves no free semantic for the "
                "capture position input";
        return false;
    }

    // ---- patch the shader ------------------------------------------------
    plan.vs3 = scan.isVs3;
    plan.extraStream = extraStream;
    u32 captureInputReg = 0;
    if (scan.isVs3) {
        u32 captureOutputReg = 0;
        Semantic captureSemantic{};
        if (!FreeTexCoordSemanticIndex(scan, plan.texCoordIndex, error))
            return false;
        if (!SelectCaptureInputSemantic(scan, candidates, plan.texCoordIndex,
                                        captureSemantic, captureInputReg,
                                        error))
            return false;
        if (!FreeOutputRegister(scan, captureOutputReg, error)) return false;
        if (!BuildVs3Shader(bytecode, scan, captureSemantic, captureInputReg,
                            captureOutputReg, plan.texCoordIndex,
                            plan.shaderCode, error))
            return false;
        plan.extraSemantic = captureSemantic;
    } else {
        u32 captureTexCoordReg = 0;
        Semantic captureSemantic{};
        if (!FreeTexCoordOutputRegister(scan, captureTexCoordReg, error))
            return false;
        if (!SelectCaptureInputSemantic(scan, candidates, kNoRegister,
                                        captureSemantic, captureInputReg,
                                        error))
            return false;
        if (!BuildVs2Shader(bytecode, scan, captureSemantic, captureInputReg,
                            captureTexCoordReg, plan.shaderCode, error))
            return false;
        plan.texCoordIndex = captureTexCoordReg;
        plan.extraSemantic = captureSemantic;
    }

    return true;
}

bool Capture(IDirect3DDevice9* device, unsigned firstVertex, unsigned vertexCount,
             std::vector<std::array<float, 4>>& clipPositions,
             std::string& error) {
    error.clear();
    clipPositions.clear();
    if (device == nullptr) {
        error = "device is null";
        return false;
    }
    if (vertexCount == 0) return true;
    if (firstVertex > 0xFFFFFFFFu - vertexCount) {
        error = "vertex range overflows the 32-bit vertex index space";
        return false;
    }

    CapturePlan plan;
    std::vector<D3DVERTEXELEMENT9> elements;
    unsigned streamCount=0;
    if(!PrepareCapture(device,plan,elements,streamCount,error))return false;
    return RunCapture(device, plan, firstVertex, vertexCount, elements,
                      streamCount, clipPositions, error);
}

#include "vertex_capture_gpu.inc"


}  // namespace vertex_capture
}  // namespace mikumikustudio
