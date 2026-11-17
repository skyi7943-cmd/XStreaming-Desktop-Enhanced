// XStreaming Desktop Enhanced - 原生超低延迟神经渲染与 X2 补帧引擎进程管理器
// 负责绑定 BrowserWindow 原生 HWND、通过零开销管道实时同步参数与 UI 镂空矩形，并向前端推送 GPU 遥测。

import { spawn, spawnSync, ChildProcessWithoutNullStreams } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import readline from "node:readline";
import Application from "../application";

export class EnhanceEngineManager {
  private _application: Application;
  private _proc: ChildProcessWithoutNullStreams | null = null;
  private _latestConfig: Record<string, any> = {};
  private _latestExcludeRects: Array<{ x: number; y: number; w: number; h: number }> = [];
  private _telemetry: Record<string, any> = {
    running: false,
    ready: false,
    srcFps: 0,
    outFps: 0,
    dlss5Active: false,
    dlss5WorkRes: "--",
    dlss5Ms: 0,
    nvofa: false,
    flowMs: 0,
    composeMs: 0,
    fgMs: 0,
    fgActive: false,
    fgMode: 0,
    sharpenMode: 1,
    gpu: "",
  };

  constructor(application: Application) {
    this._application = application;
  }

  // 自动解析开发环境与打包发布环境下的原生引擎路径
  public resolveEngineBinaryPath(): string {
    const candidates = [
      path.join(process.cwd(), "native", "bin", "xstreaming-enhance-engine.exe"),
      path.join(process.resourcesPath || "", "native", "xstreaming-enhance-engine.exe"),
      path.join(__dirname, "..", "native", "bin", "xstreaming-enhance-engine.exe"),
      path.join(__dirname, "..", "..", "native", "bin", "xstreaming-enhance-engine.exe"),
    ];
    for (const candidate of candidates) {
      if (candidate && fs.existsSync(candidate)) {
        return candidate;
      }
    }
    return candidates[0];
  }

  private extractMainWindowHwndString(): string | null {
    const win = this._application._mainWindow;
    if (!win || win.isDestroyed()) return null;
    const buf: Buffer = win.getNativeWindowHandle();
    if (!buf || buf.length < 4) return null;
    if (buf.length >= 8) {
      return "0x" + buf.readBigUInt64LE(0).toString(16);
    }
    return "0x" + buf.readUInt32LE(0).toString(16);
  }

  public start(initialConfig: Record<string, any> = {}): Record<string, any> {
    if (process.platform !== "win32") {
      return { supported: false, running: false, reason: "仅支持 Windows DirectX 11/12 平台" };
    }

    this._latestConfig = { ...this._latestConfig, ...initialConfig };

    if (this._proc && !this._proc.killed) {
      this.sendPatch({ ...this._latestConfig, excludeRects: this._latestExcludeRects });
      return { supported: true, running: true, telemetry: this._telemetry };
    }

    const exePath = this.resolveEngineBinaryPath();
    if (!fs.existsSync(exePath)) {
      return {
        supported: false,
        running: false,
        reason: `未找到内置引擎可执行文件: ${exePath}`,
      };
    }

    const hwndHex = this.extractMainWindowHwndString();
    if (!hwndHex) {
      return { supported: false, running: false, reason: "主窗口尚未就绪" };
    }

    const proc = spawn(exePath, ["--hwnd", hwndHex], {
      cwd: path.dirname(exePath),
      windowsHide: true,
      stdio: ["pipe", "pipe", "pipe"],
    });

    this._proc = proc;
    this._telemetry = {
      ...this._telemetry,
      running: true,
    };

    const rl = readline.createInterface({ input: proc.stdout });
    rl.on("line", (line) => {
      const trimmed = line.trim();
      if (!trimmed.startsWith("{")) return;
      try {
        const msg = JSON.parse(trimmed);
        if (msg.type === "ready") {
          this._telemetry = {
            ...this._telemetry,
            running: true,
            ready: true,
            gpu: msg.gpu || this._telemetry.gpu,
            dlss5Active: !!msg.dlss5,
            nvofa: !!msg.nvofa,
            recordable: !!msg.recordable,
          };
          this.broadcastTelemetry();
        } else if (msg.type === "telemetry") {
          this._telemetry = {
            ...this._telemetry,
            ...msg,
            running: true,
            ready: true,
          };
          this.broadcastTelemetry();
        } else if (msg.type === "error") {
          this._application.log("enhance", "原生引擎报错:", msg.message);
        }
      } catch {
        // 忽略非 JSON 日志行
      }
    });

    proc.stderr.on("data", (chunk) => {
      this._application.log("enhance:stderr", String(chunk));
    });

    proc.on("exit", () => {
      if (this._proc === proc) {
        this._proc = null;
        this._telemetry = {
          ...this._telemetry,
          running: false,
          srcFps: 0,
          outFps: 0,
        };
        this.broadcastTelemetry();
      }
    });

    // 启动后立即下发初始配置与 UI 镂空区域
    this.sendPatch({
      ...this._latestConfig,
      excludeRects: this._latestExcludeRects,
    });

    return { supported: true, running: true, telemetry: this._telemetry };
  }

  public sendPatch(patch: Record<string, any>): boolean {
    if (patch && typeof patch === "object") {
      const { excludeRects, ...rest } = patch;
      if (Object.keys(rest).length > 0) {
        this._latestConfig = { ...this._latestConfig, ...rest };
      }
      if (Array.isArray(excludeRects)) {
        this._latestExcludeRects = excludeRects;
      }
    }
    if (!this._proc || this._proc.killed || !this._proc.stdin.writable) {
      return false;
    }
    try {
      this._proc.stdin.write(JSON.stringify(patch) + "\n");
      return true;
    } catch {
      return false;
    }
  }

  public stop(): boolean {
    if (!this._proc) return true;
    const proc = this._proc;
    this._proc = null;
    try {
      if (proc.stdin.writable) {
        proc.stdin.write('{"type":"quit"}\n');
        proc.stdin.end();
      }
      setTimeout(() => {
        if (!proc.killed) {
          try {
            proc.kill();
          } catch {
            // 忽略进程已退出
          }
        }
      }, 250);
    } catch {
      try {
        proc.kill();
      } catch {
        // 忽略
      }
    }
    this._telemetry = {
      ...this._telemetry,
      running: false,
      srcFps: 0,
      outFps: 0,
    };
    this.broadcastTelemetry();
    return true;
  }

  public getStatus(): Record<string, any> {
    return {
      supported: process.platform === "win32" && fs.existsSync(this.resolveEngineBinaryPath()),
      enginePath: this.resolveEngineBinaryPath(),
      ...this._telemetry,
    };
  }

  public runSelfTest(): Record<string, any> {
    const exePath = this.resolveEngineBinaryPath();
    if (process.platform !== "win32" || !fs.existsSync(exePath)) {
      return { selfTest: false, error: "Engine binary missing" };
    }
    const res = spawnSync(exePath, ["--self-test"], {
      cwd: path.dirname(exePath),
      windowsHide: true,
      timeout: 15000,
      encoding: "utf8",
    });
    const out = String(res.stdout || "").trim();
    const lines = out.split(/\r?\n/);
    for (let i = lines.length - 1; i >= 0; i--) {
      if (lines[i].startsWith("{")) {
        try {
          return JSON.parse(lines[i]);
        } catch {
          // 继续查找
        }
      }
    }
    return { selfTest: false, raw: out, stderr: String(res.stderr || "") };
  }

  private broadcastTelemetry(): void {
    const win = this._application._mainWindow;
    if (!win || win.isDestroyed()) return;
    try {
      win.webContents.send("enhance", {
        action: "telemetry",
        id: 0,
        data: this._telemetry,
      });
    } catch {
      // 忽略窗口关闭瞬间的发送异常
    }
  }
}
