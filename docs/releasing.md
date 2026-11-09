# 发版

版本号只有一个来源：`CMakeLists.txt` 的 `project(rdaccel VERSION x.y.z)` 和 `include/rdaccel/rdaccel.h`
的 `RDA_VERSION_*` 必须一致，不一致时 CMake 配置直接报错。

## 步骤

1. 改 `CMakeLists.txt` 和 `rdaccel.h` 里的版本号（候选版另外把 `RDACCEL_PRERELEASE` 和 `RDA_VERSION_PRERELEASE`
   设成 `rc1` 之类，正式版设成空字符串；两处必须一致），`tests/test_capi.cpp` 和 `tests/test_api.cpp` 的版本断言，`examples/minimal` 和 README 里
   `find_package(rdaccel x.y)` 的版本。
2. 在 `CHANGELOG.md` 顶部写 `## x.y.z` 一节（候选版是 `## x.y.z-rc1`；发布说明直接取这一节）。
   完整版本号（标签去掉 `v`）用 `tools/version.sh` 看。
3. 在本地跑完检查（没有 CI，见 [building.md](building.md) 的“其他检查”）：Mac 和 Linux 上各跑一遍全部测试，
   `tools/abi_check.sh <上一版本标签>` 确认 C ABI 只加不改，然后合并到 main。
4. 打标签并推到 GitLab：`git tag vx.y.z && git push origin vx.y.z`（候选版是 `vx.y.z-rc1`）。
5. 按下面“本地打包”分别在 Linux（x86_64）和 macOS 上打包。macOS 的包要做成通用二进制
   （`-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"`，用 `lipo -archs` 逐个文件检查），每个包拿 `examples/minimal`
   编译运行一遍自检。发布附件：
   - `rdaccel-x.y.z-linux-x86_64.tar.gz`（共享库 + `rdaccel_serve` 等工具）
   - `rdaccel-x.y.z-linux-x86_64-static.tar.gz`（`librdaccel.a`；链接时还要系统的 FFmpeg、X11，CMake 包和 `pkg-config --static` 会带上）
   - `rdaccel-x.y.z-darwin-universal.tar.gz`
   - `rdaccel-x.y.z-darwin-universal-static.tar.gz`（只依赖系统框架和 zlib）
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

- 1.0 起：同一主版本内只加不改（新函数、配置结构体末尾加字段），soname 只带主版本（`librdaccel.so.1`、
  `librdaccel.1.dylib`），`find_package(rdaccel 1.0)` 接受任何 1.x。`rdaccel.hpp` 只承诺源码兼容。
- 1.0 之前：次版本（0.x）之间 C ABI 可能不兼容，soname 带次版本（`librdaccel.so.0.5`）。
- 改了 `include/rdaccel/` 的提交合并前，在 Linux 上跑 `tools/abi_check.sh origin/main`：用 libabigail 的
  `abidiff` 对比基线和当前的 `librdaccel.so`（只看 `include/rdaccel/` 声明的接口）。同一版本内出现不兼容改动
  （删函数、改参数、改字段位置……）时失败，需要先升版本；兼容的改动（加函数、结构体末尾加字段）只列出来。
- Linux 发布包里的共享库动态链接系统的 FFmpeg 和 X11（按 Ubuntu 24.04 构建）；其他发行版建议从源码构建。
  FFmpeg 可能带 x265（GPL），分发说明见 [NOTICE](../NOTICE)。
