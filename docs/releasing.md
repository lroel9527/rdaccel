# 发版

版本号只有一个来源：`CMakeLists.txt` 的 `project(rdaccel VERSION x.y.z)` 和 `include/rdaccel/rdaccel.h`
的 `RDA_VERSION_*` 必须一致，不一致时 CMake 配置直接报错。

## 步骤

1. 改 `CMakeLists.txt` 和 `rdaccel.h` 里的版本号（候选版另外把 `RDACCEL_PRERELEASE` 和 `RDA_VERSION_PRERELEASE`
   设成 `rc1` 之类，正式版设成空字符串；两处必须一致），`tests/test_capi.cpp` 和 `tests/test_api.cpp` 的版本断言，`examples/minimal` 和 README 里
   `find_package(rdaccel x.y)` 的版本。
2. 在 `CHANGELOG.md` 顶部写 `## x.y.z` 一节（候选版是 `## x.y.z-rc1`；发布说明直接取这一节）。
   完整版本号（标签去掉 `v`）用 `tools/version.sh` 看。
3. 在本地跑完检查（没有 CI，见 [building.md](building.md) 的“其他检查”）：在 Mac 上跑一遍全部测试，
   对照公开头文件的改动确认 C ABI 在同一主版本内只加不改（没有自动检查，见 building.md），然后合并到 main。
4. 打标签并推到 GitLab：`git tag vx.y.z && git push origin vx.y.z`（候选版是 `vx.y.z-rc1`）。
5. 按下面“本地打包”在 macOS 上打包，做成通用二进制
   （`-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"`，用 `lipo -archs` 逐个文件检查），每个包拿 `examples/minimal`
   编译运行一遍自检。发布附件：
   - `rdaccel-x.y.z-darwin-universal.tar.gz`（共享库 + `rdaccel_serve` 等工具）
   - `rdaccel-x.y.z-darwin-universal-static.tar.gz`（`librdaccel.a`；只依赖系统框架和 zlib）
   - `rdaccel-x.y.z.tar.gz`（源码）
   - `SHA256SUMS`（`shasum -a 256 rdaccel-*.tar.gz > SHA256SUMS`）
6. 在 GitLab 上建 release，说明取 `CHANGELOG.md` 里这一版的一节：
   `glab release create vx.y.z rdaccel-x.y.z-*.tar.gz SHA256SUMS --notes-file <这一节>`。

## 本地打包

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DRDACCEL_INSTALL_TOOLS=ON
cmake --build build -j
(cd build && cpack && cpack --config CPackSourceConfig.cmake)
```

## 兼容性约定

- 1.0 起：同一主版本内只加不改（新函数、配置结构体末尾加字段），soname 只带主版本（`librdaccel.3.dylib`），
  `find_package(rdaccel 3.0)` 接受任何 3.x。`rdaccel.hpp` 只承诺源码兼容。
- 主版本之间可以不兼容：2.0.0 和 3.0.0 删了函数和字段（见 [CHANGELOG](../CHANGELOG.md)），旧主版本的程序要重新编译。
- 1.0 之前：次版本（0.x）之间 C ABI 可能不兼容，soname 带次版本。
- 同一主版本内出现不兼容改动（删函数、改参数、改字段位置……）需要先升主版本；兼容的改动是加函数、结构体末尾加字段。
  这一条现在靠人对照头文件的改动来查（1.x 在 Linux 上用 abidiff 自动查的脚本在 3.0.0 删掉了）。
