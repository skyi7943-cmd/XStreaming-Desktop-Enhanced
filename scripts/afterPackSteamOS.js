// SteamOS 3.x (Gamescope 掌机模式 + KDE 桌面模式) 打包后二进制封装钩子
// 解决 Electron 37 AppImage 在 SteamOS / SteamLinuxRuntime 下因 nosuid 挂载导致 chrome-sandbox 启动即崩溃的问题，
// 并在进程最底层强制注入 AMD Van Gogh / Sephiroth RDNA2 的 radeonsi VA-API 硬件视频解码驱动！

const fs = require("node:fs");
const path = require("node:path");

exports.default = async function afterPackSteamOS(context) {
  if (context.electronPlatformName !== "linux") {
    return;
  }

  const appOutDir = context.appOutDir;
  const exeName = context.packager.executableName || "xstreaming";
  const origBin = path.join(appOutDir, exeName);
  const realBin = path.join(appOutDir, `${exeName}.bin`);

  if (!fs.existsSync(origBin)) {
    throw new Error(`[afterPackSteamOS] 未找到 Linux 主程序二进制文件: ${origBin}`);
  }

  fs.renameSync(origBin, realBin);
  fs.chmodSync(realBin, 0o755);

  // 注意：必须严格使用 Unix LF (\n) 换行符，确保 SteamOS /bin/sh 100% 正常解析执行
  const wrapperLines = [
    "#!/bin/sh",
    'SELF_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"',
    "export ELECTRON_DISABLE_SANDBOX=1",
    'export LIBVA_DRIVER_NAME="${LIBVA_DRIVER_NAME:-radeonsi}"',
    "export mesa_glthread=true",
    "export vblank_mode=0",
    "export SDL_JOYSTICK_HIDAPI=1",
    "export SDL_JOYSTICK_HIDAPI_STEAMDECK=1",
    `exec "$SELF_DIR/${exeName}.bin" --no-sandbox --disable-setuid-sandbox --disable-dev-shm-usage --ignore-gpu-blocklist --enable-gpu-rasterization --enable-oop-rasterization --enable-accelerated-video-decode --enable-zero-copy --disable-frame-rate-limit --enable-features=VaapiVideoDecoder,VaapiIgnoreDriverChecks,CanvasOopRasterization,PlatformHEVCDecoderSupport,WebRtcHideLocalIpsWithMdns --ozone-platform=x11 "$@"`,
    "",
  ];

  fs.writeFileSync(origBin, wrapperLines.join("\n"), { encoding: "utf8", mode: 0o755 });
  fs.chmodSync(origBin, 0o755);

  console.log(`[afterPackSteamOS] 已完成 SteamOS 专属防崩溃与 RDNA2 VA-API 封装: ${origBin}`);
};
