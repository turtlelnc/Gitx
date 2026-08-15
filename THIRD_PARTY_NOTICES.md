# Third Party Notices

gitx 自身代码以 MIT 许可证发布（见 LICENSE）。以下第三方组件以源码形式随 gitx 分发（位于 `third_party/`），各自保留其许可证与版权。

## libgit2（third_party/libgit2-1.8.4）

- 许可证：GPLv2 with Linking Exception
- 版权：Copyright (c) the libgit2 contributors
- 说明：Linking Exception 允许任何程序链接该库而不受 GPL 传染；修改 libgit2 本身仍受 GPLv2 约束。已打补丁：禁用其测试/示例构建。

## libssh2（third_party/libssh2-1.11.1）

- 许可证：BSD-3-Clause
- 版权：Copyright (c) the libssh2 project contributors
- 说明：SSH 传输后端（macOS/Linux 使用；Windows 使用系统 OpenSSH exec 后端，不编译本组件）。已打补丁：`BUILD_TESTING` 改名 `LIBSSH2_BUILD_TESTING` 以避免与 CTest 选项冲突。其加密后端在 macOS/Linux 依赖系统 OpenSSL（平台加密基础设施，不随仓库分发）。

## curl（third_party/curl-8.7.1）

- 许可证：curl License（MIT 系）
- 版权：Copyright (c) 1996-2024 Daniel Stenberg and contributors
- 说明：AI 工具链的 HTTP 客户端。仅编译 libcurl 静态库（`BUILD_CURL_EXE=OFF`、`HTTP_ONLY=ON`、禁用 LDAP/FTP 等协议）。macOS 使用系统 SecureTransport，Windows 使用系统 Schannel，Linux 不启用 TLS 后端（AI 调用本地服务时无需 TLS；如需 HTTPS 请启用 OpenSSL）。已打补丁：`BUILD_TESTING` 改名 `CURL_BUILD_TESTING` 以避免与 CTest 选项冲突。

## zlib（third_party/zlib-1.3.1）

- 许可证：zlib License
- 版权：Copyright (c) 1995-2024 Jean-loup Gailly and Mark Adler
- 说明：仅当 libgit2 使用其裁剪版 zlib（通常发生在 Windows 上未找到系统 zlib 时）时，gitx_core 编译完整 zlib 的压缩 API 源码（compress.c/uncompr.c 及核心依赖）。

## OpenSSL（平台系统库，不随仓库分发）

- 许可证：Apache License 2.0
- 版权：Copyright (c) the OpenSSL Project
- 说明：libssh2 在 macOS/Linux 上的加密后端使用系统 OpenSSL（Linux 需 `libssl-dev`；macOS 需 Homebrew openssl 或系统库）。macOS 的 HTTPS 使用 SecureTransport、Windows 使用 WinHTTP/Schannel，均不涉及 OpenSSL。

## 依赖边界说明

- **vendored 到 third_party/**：libgit2、libssh2（源码）、curl（源码）、zlib（源码）——克隆仓库即可离线构建。
- **仍需系统提供**：C++20 编译器、CMake 3.24+、OpenSSL 开发包（Linux 上 libssh2 加密所需）。
