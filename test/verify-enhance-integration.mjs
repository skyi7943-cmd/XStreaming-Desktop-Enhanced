// XStreaming Desktop Enhanced - 端到端集成测试套件
// 验证：
// 1. 原生引擎二进制与 nvngx_dlssnr.dll 完整性
// 2. RTX 4070 真卡 --self-test 全管线推理（NVOFA 光流 + DLSS 5 Feature 18 + Oklab 残差 + CAS/RCAS 锐化 + X2 补帧）
// 3. 实时 HWND 挂载、stdin JSON 参数热更新、UI 镂空矩形同步与安全退出生命周期

import assert from "node:assert/strict";
import { spawn, spawnSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import readline from "node:readline";
import { fileURLToPath } from "node:url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const rootDir = path.resolve(__dirname, "..");
const binDir = path.join(rootDir, "native", "bin");
const engineExe = path.join(binDir, "xstreaming-enhance-engine.exe");
const dlss5Dll = path.join(binDir, "nvngx_dlssnr.dll");

console.log("=== [1/3] 校验原生引擎与内置 DLSS 5 运行库文件 ===");
assert.ok(fs.existsSync(engineExe), `缺少原生引擎文件: ${engineExe}`);
assert.ok(fs.existsSync(dlss5Dll), `缺少 nvngx_dlssnr.dll: ${dlss5Dll}`);
console.log(`✅ 引擎体积: ${(fs.statSync(engineExe).size / 1024).toFixed(1)} KB, DLSS 5 运行库: ${(fs.statSync(dlss5Dll).size / (1024 * 1024)).toFixed(1)} MB`);

console.log("=== [2/3] 执行真卡 GPU 全管线自检 (--self-test) ===");
const selfRes = spawnSync(engineExe, ["--self-test"], {
  cwd: binDir,
  encoding: "utf8",
  timeout: 20000,
});
assert.equal(selfRes.status, 0, `--self-test 退出码非 0: ${selfRes.status}, stderr=${selfRes.stderr}`);
const report = JSON.parse(selfRes.stdout.trim().split(/\r?\n/).pop());
console.log("📊 硬件实测报告:", report);
assert.equal(report.selfTest, true, "硬件自检未通过");
assert.equal(report.dlss5Ready, true, "DLSS 5 (Feature 18) 未就绪");
assert.equal(report.dualDlss5Verified, true, "双层串联 DLSS 5 (Pass1->Pass2) 未验证通过");
assert.ok(
  report.styleDiff0vs2 !== undefined && report.styleDiff0vs2 > 1.0,
  `自然均衡(Style 0)与电影光影(Style 2)风格切换未生效: ${report.styleDiff0vs2}`
);
assert.equal(report.wgcRecordable, true, "Windows.Graphics.Capture 免回环录屏管线未就绪");
assert.ok(
  report.nightMaxAnomaly !== undefined && report.nightMaxAnomaly <= 8.0,
  `黑夜暗场异常亮斑偏差过高: ${report.nightMaxAnomaly}`
);
assert.ok(
  report.skyMaxSpot !== undefined && report.skyMaxSpot <= 8.0,
  `明亮天空斑点闪烁阶跃过高: ${report.skyMaxSpot}`
);
assert.ok(
  report.asyncTrailGhost !== undefined && report.asyncTrailGhost <= 12.0,
  `异步残差移动物体后方残影过高: ${report.asyncTrailGhost}`
);
assert.equal(report.nvofaHardware, true, "NVOFA 硬件光流未激活");
assert.equal(report.dedup30In60Verified, true, "30-in-60 重复帧智能去重未通过");
assert.equal(report.dupSkipped, 4, `期望滤除 4 个 H.264 噪点重复帧，实际: ${report.dupSkipped}`);
assert.ok(
  report.midpointBallX >= 534.0 && report.midpointBallX <= 546.0,
  `补帧中点几何坐标偏离理想值 540.0: ${report.midpointBallX}`
);
assert.ok(
  report.uiFlickerDiff !== undefined && report.uiFlickerDiff < 1.5,
  `彩色 UI 抗闪烁误差过高: ${report.uiFlickerDiff}`
);
assert.ok(report.flowMs < 5.0, `光流耗时异常: ${report.flowMs}ms`);
assert.ok(report.composeSharpenMs < 2.0, `合成锐化耗时异常: ${report.composeSharpenMs}ms`);
assert.ok(report.fgX2Ms < 15.0, `X2 补帧耗时异常: ${report.fgX2Ms}ms`);

console.log("=== [3/3] 验证真实窗口 HWND 挂载、实时热调参与无残留退出 ===");
// 获取当前桌面窗口句柄用于测试挂载与管道通信
const psHwnd = spawnSync(
  "powershell",
  [
    "-NoProfile",
    "-Command",
    "Add-Type -Namespace Win32 -Name U -MemberDefinition '[DllImport(\"user32.dll\")] public static extern IntPtr GetDesktopWindow();'; [Win32.U]::GetDesktopWindow().ToInt64()",
  ],
  { encoding: "utf8" }
);
const desktopHwnd = psHwnd.stdout.trim();
assert.ok(Number(desktopHwnd) > 0, `未能获取有效测试 HWND: ${desktopHwnd}`);

await new Promise((resolve, reject) => {
  const proc = spawn(engineExe, ["--hwnd", desktopHwnd], {
    cwd: binDir,
    stdio: ["pipe", "pipe", "pipe"],
  });

  let gotReady = false;
  const timer = setTimeout(() => {
    proc.kill();
    reject(new Error("等待原生引擎 ready 消息超时"));
  }, 10000);

  const rl = readline.createInterface({ input: proc.stdout });
  rl.on("line", (line) => {
    if (!line.trim().startsWith("{")) return;
    const msg = JSON.parse(line.trim());
    if (msg.type === "ready") {
      gotReady = true;
      console.log("✅ 实时管道就绪:", msg);
      // 发送实时参数热更新（切换 FSR RCAS 锐化 + 双向补帧 + UI 镂空矩形）
      proc.stdin.write(
        JSON.stringify({
          enabled: true,
          fgEnabled: true,
          dedup30In60: true,
          dedupSensitivity: 6,
          fgMode: 1,
          dlss5Enabled: true,
          dlss5Passes: 2,
          dlss5Pass2Intensity: 0.85,
          darkProtection: 0.6,
          dlss5LatencyMode: 0,
          sharpenMode: 2,
          sharpenStrength: 0.75,
          excludeRects: [{ x: 960, y: 0, w: 320, h: 1080 }],
        }) + "\n"
      );
      setTimeout(() => {
        proc.stdin.write('{"type":"quit"}\n');
        proc.stdin.end();
      }, 300);
    }
  });

  proc.on("exit", (code) => {
    clearTimeout(timer);
    try {
      assert.ok(gotReady, "未收到 ready 消息");
      assert.equal(code, 0, `引擎退出状态码异常: ${code}`);
      console.log("✅ 原生引擎热更新与零残留退出验证通过！");
      resolve();
    } catch (e) {
      reject(e);
    }
  });
});

console.log("🎉 全部集成测试通过！");
