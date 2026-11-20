#!/usr/bin/env bash
set -euo pipefail

# XStreaming Enhanced - Steam Deck (SteamOS 3.x) 一键部署与自动写入 Steam 游戏库脚本
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_DIR="$HOME/Applications/XStreaming-Enhanced"
APPIMAGE_SRC="$(ls "$SCRIPT_DIR"/XStreaming-SteamDeck-SteamOS-*.AppImage 2>/dev/null | head -n 1 || true)"

if [[ -z "$APPIMAGE_SRC" ]]; then
  echo "❌ 未在当前目录找到 XStreaming-SteamDeck-SteamOS-x86_64.AppImage！"
  exit 1
fi

mkdir -p "$INSTALL_DIR"
mkdir -p "$HOME/.local/share/applications"
mkdir -p "$HOME/Desktop"

TARGET_APPIMAGE="$INSTALL_DIR/XStreaming-SteamDeck-SteamOS-x86_64.AppImage"
cp -f "$APPIMAGE_SRC" "$TARGET_APPIMAGE"
chmod +x "$TARGET_APPIMAGE"

if [[ -f "$SCRIPT_DIR/icon.png" ]]; then
  cp -f "$SCRIPT_DIR/icon.png" "$INSTALL_DIR/icon.png"
fi
ICON_PATH="$INSTALL_DIR/icon.png"
[[ ! -f "$ICON_PATH" ]] && ICON_PATH="applications-games"

DESKTOP_FILE="$HOME/.local/share/applications/xstreaming-steamos-enhanced.desktop"
cat > "$DESKTOP_FILE" <<EOF
[Desktop Entry]
Name=XStreaming Enhanced (Xbox 60FPS)
Comment=Xbox Series X|S Stream Client with Steam Deck RDNA2 Relight & True X2 60FPS Frame Gen
Exec="$TARGET_APPIMAGE" --no-sandbox %U
Icon=$ICON_PATH
Terminal=false
Type=Application
Categories=Game;
StartupNotify=true
EOF

chmod +x "$DESKTOP_FILE"
cp -f "$DESKTOP_FILE" "$HOME/Desktop/XStreaming-Enhanced.desktop"
chmod +x "$HOME/Desktop/XStreaming-Enhanced.desktop"

# 自动写入 Steam 非 Steam 游戏快捷方式 (shortcuts.vdf)，无需手动添加即可出现在掌机模式游戏库！
if command -v python3 >/dev/null 2>&1; then
  python3 - "$TARGET_APPIMAGE" "$INSTALL_DIR" "$ICON_PATH" <<'PYEOF' || true
import glob, os, struct, sys, zlib

exe_path, start_dir, icon_path = sys.argv[1], sys.argv[2], sys.argv[3]
app_name = "XStreaming Enhanced (Xbox 60FPS)"
userdata_dirs = glob.glob(os.path.expanduser("~/.steam/steam/userdata/*/config")) + \
                glob.glob(os.path.expanduser("~/.local/share/Steam/userdata/*/config"))

def make_entry(idx: int) -> bytes:
    appid = (zlib.crc32((f'"{exe_path}"' + app_name).encode("utf-8")) | 0x80000000) & 0xFFFFFFFF
    parts = [
        b"\x00" + str(idx).encode("ascii") + b"\x00",
        b"\x02appid\x00" + struct.pack("<I", appid),
        b"\x01AppName\x00" + app_name.encode("utf-8") + b"\x00",
        b"\x01Exe\x00" + f'"{exe_path}"'.encode("utf-8") + b"\x00",
        b"\x01StartDir\x00" + f'"{start_dir}"'.encode("utf-8") + b"\x00",
        b"\x01icon\x00" + (icon_path if os.path.isfile(icon_path) else "").encode("utf-8") + b"\x00",
        b"\x01ShortcutPath\x00\x00",
        b"\x01LaunchOptions\x00--no-sandbox\x00",
        b"\x02IsHidden\x00" + struct.pack("<I", 0),
        b"\x02AllowDesktopConfig\x00" + struct.pack("<I", 1),
        b"\x02AllowOverlay\x00" + struct.pack("<I", 1),
        b"\x02OpenVR\x00" + struct.pack("<I", 0),
        b"\x00tags\x00\x08",
        b"\x08",
    ]
    return b"".join(parts)

for cfg_dir in set(userdata_dirs):
    vdf_path = os.path.join(cfg_dir, "shortcuts.vdf")
    data = b"\x00shortcuts\x00\x08\x08"
    if os.path.isfile(vdf_path):
        with open(vdf_path, "rb") as f:
            cur = f.read()
        if exe_path.encode("utf-8") in cur:
            print(f"[SteamOS] Steam 快捷方式已存在: {vdf_path}")
            continue
        if cur.endswith(b"\x08\x08"):
            # 统计已有条目索引并追加在末尾
            idx = max(0, cur.count(b"\x01AppName\x00"))
            new_data = cur[:-2] + make_entry(idx) + b"\x08\x08"
            with open(vdf_path, "wb") as f:
                f.write(new_data)
            print(f"[SteamOS] 已自动注入 Steam 掌机游戏库: {vdf_path}")
            continue
    os.makedirs(cfg_dir, exist_ok=True)
    new_data = b"\x00shortcuts\x00" + make_entry(0) + b"\x08\x08"
    with open(vdf_path, "wb") as f:
        f.write(new_data)
    print(f"[SteamOS] 已创建并注入 Steam 掌机游戏库: {vdf_path}")
PYEOF
fi

echo "✅ 安装完成！"
echo "   1. 程序已安装至: $TARGET_APPIMAGE"
echo "   2. 桌面与应用菜单快捷方式已创建"
echo "   3. 已自动写入 Steam 游戏库（重启 Steam 或切回掌机 Gaming Mode 即可在『非 Steam 游戏』中直接开玩！）"
