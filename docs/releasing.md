# 发版

版本号只有一个来源：`CMakeLists.txt` 的 `project(rdaccel VERSION x.y.z)` 和 `include/rdaccel/rdaccel.h`
的 `RDA_VERSION_*` 必须一致，不一致时 CMake 配置直接报错。

## 步骤

1. 改 `CMakeLists.txt` 和 `rdaccel.h` 里的版本号，`tests/test_capi.cpp` 和 `tests/test_api.cpp` 的版本断言，`examples/minimal` 和 README 里
   `find_package(rdaccel x.y)` 的版本。
2. 在 `CHANGELOG.md` 顶部写 `## x.y.z` 一节（发布说明直接取这一节）。
3. 合并到 main，确认 CI 全绿。
4. 打标签并推送：`git tag vx.y.z && git push origin vx.y.z`。
5. `release` 工作流在 Linux（ubuntu-24.04, x86_64）和 macOS（14）上各构建共享库 + 工具、静态库两种包，
   macOS 的包是通用二进制（arm64 + x86_64，逐个文件用 `lipo` 检查），每个包都会拿 `examples/minimal` 编译运行一遍自检，
   然后建一个**草稿** release，附上：
   - `rdaccel-x.y.z-linux-x86_64.tar.gz`（共享库 + `rdaccel_serve` 等工具）
   - `rdaccel-x.y.z-linux-x86_64-static.tar.gz`（`librdaccel.a`；链接时还要系统的 FFmpeg、X11，CMake 包和 `pkg-config --static` 会带上）
   - `rdaccel-x.y.z-darwin-universal.tar.gz`
   - `rdaccel-x.y.z-darwin-universal-static.tar.gz`（只依赖系统框架和 zlib）
   - `rdaccel-x.y.z.tar.gz`（源码）
   - `SHA256SUMS`
6. 在 GitHub 上检查草稿，点发布。

也可以手动运行 `release` 工作流（workflow_dispatch）只构建包、不建 release，包在工作流产物里。

## 本地打包

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DRDACCEL_INSTALL_TOOLS=ON
cmake --build build -j
(cd build && cpack && cpack --config CPackSourceConfig.cmake)
```

## 兼容性约定

- 1.0 之前：次版本（0.x）之间 C ABI 可能不兼容，所以共享库的 soname 带次版本（`librdaccel.so.0.3`、
  `librdaccel.0.3.dylib`），`find_package(rdaccel 0.3)` 只接受 0.3.x。
- 1.0 之后：同一主版本内只加不改（新函数、配置结构体末尾加字段），soname 只带主版本。
- CI 的 `abi` 作业在每个 PR 上用 libabigail 的 `abidiff` 对比基线分支和 PR 的 `librdaccel.so`（只看
  `include/rdaccel/` 声明的接口）。同一版本内出现不兼容改动（删函数、改参数、改字段位置……）时失败，需要先升版本；
  兼容的改动（加函数、结构体末尾加字段）只写进作业摘要。本地：`tools/abi_check.sh origin/main`。
- Linux 发布包里的共享库动态链接系统的 FFmpeg 和 X11（按 Ubuntu 24.04 构建）；其他发行版建议从源码构建。
  FFmpeg 可能带 x265（GPL），分发说明见 [NOTICE](../NOTICE)。
