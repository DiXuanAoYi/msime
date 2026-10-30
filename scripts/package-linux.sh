#!/usr/bin/env bash
# 打 Linux 安装包：fcitx5 输入法插件 + Rust 引擎库 + 桌面伴侣，产出 .deb 和 .tar.gz。
#
# 用法：scripts/package-linux.sh <版本> <资源目录> <输出目录>
#   资源目录：已准备好的词库目录（Windows 打包时生成的 installer/app_data，含 msime.db 等）。
# 需要：cargo、cmake、g++、pkg-config、libfcitx5core-dev、fcitx5-modules-dev、dpkg-deb。
set -euo pipefail

VERSION="${1:?版本，如 0.11.0-chat.2}"
RESOURCES="$(realpath "${2:?资源目录}")"
OUT="$(mkdir -p "${3:?输出目录}" && realpath "$3")"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# 在 /mnt/c 上编译会很慢，构建目录放到本机文件系统。
WORK="${MSIME_PACKAGE_WORK:-$(mktemp -d)}"
STAGE="$WORK/stage"
ARCH="$(dpkg --print-architecture)"
DEB_VERSION="${VERSION/-chat./~chat}"

rm -rf "$STAGE"
mkdir -p "$STAGE"

echo "== 构建 fcitx5 插件与引擎库"
cmake -S "$ROOT/engine-rs/fcitx5" -B "$WORK/fcitx5" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr -DMSIME_CARGO_TARGET_DIR="$WORK/cargo-core"
cmake --build "$WORK/fcitx5" -j"$(nproc)"
DESTDIR="$STAGE" cmake --install "$WORK/fcitx5"

echo "== 构建桌面伴侣"
CARGO_TARGET_DIR="$WORK/cargo-companion" cargo build --release --locked \
  --manifest-path "$ROOT/companion/Cargo.toml"
install -Dm755 "$WORK/cargo-companion/release/zhiyu-companion" "$STAGE/usr/bin/zhiyu-companion"

install -Dm644 /dev/stdin "$STAGE/usr/share/applications/zhiyu-companion.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=知语桌面伴侣
Comment=桌面宠物，输入时跳到候选窗上
Exec=zhiyu-companion
Terminal=false
Categories=Utility;
DESKTOP

echo "== 复制词库资源（不含 Windows 设置页 html）"
mkdir -p "$STAGE/usr/share/msime"
tar -C "$RESOURCES" --exclude=./html --exclude=./config.toml -cf - . | tar -C "$STAGE/usr/share/msime" -xf -

install -Dm644 "$ROOT/LICENSE" "$STAGE/usr/share/doc/msime/copyright"
install -Dm644 "$ROOT/THIRD_PARTY_NOTICES.txt" "$STAGE/usr/share/doc/msime/THIRD_PARTY_NOTICES.txt"

echo "== 生成 .deb"
mkdir -p "$STAGE/DEBIAN"
SIZE_KB="$(du -sk "$STAGE" | cut -f1)"
cat > "$STAGE/DEBIAN/control" <<CONTROL
Package: msime
Version: $DEB_VERSION
Section: utils
Priority: optional
Architecture: $ARCH
Depends: fcitx5
Installed-Size: $SIZE_KB
Maintainer: DiXuanAoYi <dixuanaoyi@gmail.com>
Homepage: https://github.com/DiXuanAoYi/msime
Description: 水杉输入法（知语）fcitx5 版与桌面伴侣
 Rust 引擎与 Windows 版共用同一份输入逻辑；附带桌面伴侣 zhiyu-companion。
CONTROL
dpkg-deb --root-owner-group -Zxz -z6 --build "$STAGE" "$OUT/msime_${VERSION}_${ARCH}.deb"

echo "== 生成 .tar.gz"
rm -rf "$STAGE/DEBIAN"
tar -C "$STAGE" -czf "$OUT/msime-linux-${ARCH}-${VERSION}.tar.gz" .
cat > "$OUT/INSTALL-linux.txt" <<'TXT'
.deb：sudo apt install ./msime_<版本>_<架构>.deb ，然后在 fcitx5 配置里添加“Metasequoia”，重启 fcitx5。
.tar.gz：sudo tar -C / -xzf msime-linux-*.tar.gz ，其余同上。
桌面伴侣：终端运行 zhiyu-companion，或在应用菜单里找“知语桌面伴侣”。
TXT
( cd "$OUT" && sha256sum msime_*"${VERSION}"*.deb msime-linux-*"${VERSION}"*.tar.gz > "SHA256SUMS-linux-${VERSION}.txt" )
ls -la "$OUT"
