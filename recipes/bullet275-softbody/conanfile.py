from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy, replace_in_file, save
import os


class Bullet275SoftBodyConan(ConanFile):
    """Bullet Physics 2.75 with a deterministic soft-body SDF cache key.

    Based on the version statically linked into
    MikuMikuDance v932 (import-free; MMD compiles Bullet into the EXE).

    Sources are staged from the repo's vendored ``bullet-2.75`` tree by
    scripts/export_bullet_sources.py before running ``conan create``.
    """

    name = "bullet275"
    version = "2.75"
    settings = "os", "compiler", "build_type", "arch"
    exports_sources = ("bullet-src/*",)

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        # The vendored tree's root CMakeLists requires CMake >= 2.4; the
        # wrapper below supersedes it (see build()), so no patch needed.
        tc.generate()

    def build(self):
        # Bullet 2.75 hashes the padding before the pointer on x64. Clear the
        # complete key so identical SDF queries share a stable cache entry.
        sdf = os.path.join(self.source_folder, "bullet-src", "src", "BulletSoftBody", "btSparseSDF.h")
        replace_in_file(self, sdf, "btS myset;", "btS myset;\n\t\tmemset(&myset, 0, sizeof(myset));")
        # ---- 修补软体头文件（修复 C2737 错误） ----
        header = os.path.join(self.source_folder, "bullet-src", "src", "BulletSoftBody", "btSoftBodyInternals.h")
        if os.path.exists(header):
            with open(header, 'r', encoding='utf-8') as f:
                content = f.read()
            # 替换未初始化的 const 对象（处理空格/制表符差异）
            content = content.replace("static const T\tzerodummy;", "static const T\tzerodummy = T();")
            content = content.replace("static const T zerodummy;", "static const T zerodummy = T();")
            with open(header, 'w', encoding='utf-8') as f:
                f.write(content)
        # ---- 修补结束 ----

        # Wrapper project written into the exported source folder (CMake's
        # source dir under conan's build cache): build only the three
        # libraries MMD links (LinearMath, BulletCollision, BulletDynamics)
        # - no demos, GL, Extras or SoftBody, matching what MMD.exe contains.
        wrapper = os.path.join(self.source_folder, "CMakeLists.txt")
        src = os.path.join(self.source_folder, "bullet-src").replace("\\", "/")
        save(self, wrapper,
             "cmake_minimum_required(VERSION 3.16)\n"
             "project(bullet275 LANGUAGES CXX)\n"
             "set(BULLET_VERSION 2.75)\n"  # normally set by bullet's root CMakeLists
             f"set(BULLET_SOURCE_DIR \"{src}\")\n"
             # VC9 generated a mixed floating-point model for MMD's Bullet:
             # collision-shape inertia routines use scalar SSE, while the
             # complete constraint-solver path (6DoF/spring row generation as
             # well as the sequential impulse loop) keeps selected
             # intermediates on the x87 stack.  Those VC9 expressions are
             # reproduced by targeted helpers in the vendored source; keep
             # the original default precise semantics globally because the
             # narrow-phase collision code itself uses scalar SSE.
             # /arch:IA32 restores the VC9 32-bit default of x87 FPU codegen
             # for scalar expressions (three-term dot products in
             # btMatrix3x3 multiplies keep extended intermediates and store
             # once) - the MSVC default of SSE2 double-rounds each add and
             # visibly diverges in the 6DOF calculateTransforms chain
             # (joint frame equilibrium drift, spring-force ULP cascade).
             "if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 4)\n"
             "  add_compile_options(/fp:precise)\n"
             "endif()\n"
             # Optional ASan instrumentation for heap-corruption hunts:
             # BULLET275_ASAN=1 conan create ... builds instrumented libs so
             # the MikuDanceStudio ASan build (build/asan) can see OOB writes inside
             # Bullet itself.
             + ("add_compile_options(/fsanitize=address /Zc:alignedNew-)\n"
                if os.environ.get("BULLET275_ASAN") == "1" else "")
             + "include_directories(${BULLET_SOURCE_DIR}/src)\n"
             "add_subdirectory(${BULLET_SOURCE_DIR}/src/LinearMath LinearMath)\n"
             "add_subdirectory(${BULLET_SOURCE_DIR}/src/BulletCollision BulletCollision)\n"
             "add_subdirectory(${BULLET_SOURCE_DIR}/src/BulletDynamics BulletDynamics)\n"
             "add_subdirectory(${BULLET_SOURCE_DIR}/src/BulletSoftBody BulletSoftBody)\n")
        # bullet 2.75 src CMakeLists use SUBDIRS-era commands; normalize the
        # minimum version inside each so modern CMake accepts them.
        for sub in ("LinearMath", "BulletCollision", "BulletDynamics", "BulletSoftBody"):
            f = os.path.join(src, sub, "CMakeLists.txt")
            if os.path.exists(f):
                replace_in_file(self, f, "cmake_minimum_required(VERSION 2.4)",
                                "cmake_minimum_required(VERSION 3.16)", strict=False)
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        # Copy the whole staged src tree preserving subfolders: Bullet's
        # headers cross-include via "LinearMath/...", "BulletCollision/..."
        # and "BulletDynamics/..." paths, so consumers need that exact
        # layout at the include root (plus the src-root bt*Common.h
        # umbrella headers).
        src = os.path.join(self.source_folder, "bullet-src", "src")
        copy(self, "*.h", src, os.path.join(self.package_folder, "include"))
        # Flatten: cmake_layout places each target's .lib under
        # build/<target>/Release/*.lib; CMakeDeps expects package/lib/*.lib.
        for sub in ("LinearMath", "BulletCollision", "BulletDynamics", "BulletSoftBody"):
            for cfg in ("Release", "Debug"):
                copy(self, "*.lib",
                     os.path.join(self.build_folder, sub, cfg),
                     os.path.join(self.package_folder, "lib"))
        copy(self, "*.a", self.build_folder, os.path.join(self.package_folder, "lib"))

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "BULLET275")
        self.cpp_info.set_property("cmake_target_name", "BULLET275::BULLET275")
        self.cpp_info.libs = ["BulletSoftBody", "BulletDynamics", "BulletCollision", "LinearMath"]
        self.cpp_info.includedirs = ["include"]
