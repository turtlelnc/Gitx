# Third Party Notices

gitx 自身代码以 MIT 许可证发布（见 LICENSE）。以下第三方组件以源码或链接形式随 gitx 分发，各自保留其许可证与版权。

## libgit2（third_party/libgit2-1.8.4）

- 许可证：GPLv2 with Linking Exception
- 版权：Copyright (c) the libgit2 contributors
- 说明：Linking Exception 允许任何程序链接该库而不受 GPL 传染；修改 libgit2 本身仍受 GPLv2 约束。

## zlib（third_party/zlib-1.3.1，Windows bundled-zlib 回退路径）

- 许可证：zlib License
- 版权：Copyright (c) 1995-2024 Jean-loup Gailly and Mark Adler
- 说明：仅当 libgit2 使用其裁剪版 zlib（通常发生在 Windows 上未找到系统 zlib 时）时，gitx_core 编译完整 zlib 的压缩 API 源码（compress.c/uncompr.c 及核心依赖）。

## libssh2（SSH 传输，macOS/Linux 可选依赖）

- 许可证：BSD-3-Clause
- 版权：Copyright (c) the libssh2 project contributors
- 说明：macOS 通过 Homebrew 安装，Linux 通过系统包管理器安装；不随仓库分发。

## OpenSSL（Linux HTTPS 后端，可选依赖）

- 许可证：Apache License 2.0
- 版权：Copyright (c) the OpenSSL Project
- 说明：仅 Linux 构建使用系统 OpenSSL；macOS 使用 SecureTransport，Windows 使用 WinHTTP，均不涉及 OpenSSL。
