// XStreaming Desktop Enhanced - DLSS 5 + 低延迟 X2 插帧 + 锐化 + 60FPS 增强录屏 IPC 通道处理器

import fs from "node:fs";
import path from "node:path";
import { app as ElectronApp, desktopCapturer, screen, shell } from "electron";
import IpcBase from "./base";
import Application from "../application";
import { EnhanceEngineManager } from "../helpers/enhanceEngineManager";

export default class IpcEnhance extends IpcBase {
  public _manager: EnhanceEngineManager;
  private _recStream: fs.WriteStream | null = null;
  private _recFilePath = "";
  private _recDirPath = "";
  private _recBytes = 0;

  constructor(application: Application) {
    super(application);
    this._manager = new EnhanceEngineManager(application);
  }

  // 针对高频录屏二进制分片跳过 JSON 文本日志打印，避免主线程序列化阻塞
  override onEvent(channel: any, event: any, args: any) {
    if (args && args.action === "writeRecordingChunk") {
      this.writeRecordingChunk(args.data || {})
        .then((result) => {
          this.send(channel, {
            action: args.action,
            id: args.id,
            data: result,
          });
        })
        .catch((error) => {
          this._application._mainWindow?.webContents.send(channel, {
            action: args.action,
            id: args.id,
            data: {},
            error: String(error),
          });
        });
      return;
    }
    super.onEvent(channel, event, args);
  }

  private resolveRecordingDir(): string {
    if (process.platform === "win32" && fs.existsSync("E:\\")) {
      return "E:\\XStreaming_Recordings";
    }
    return path.join(ElectronApp.getPath("videos"), "XStreaming_Recordings");
  }

  startEngine(config: Record<string, any> = {}) {
    return Promise.resolve(this._manager.start(config));
  }

  stopEngine() {
    return Promise.resolve({ stopped: this._manager.stop() });
  }

  updateConfig(patch: Record<string, any> = {}) {
    return Promise.resolve({ updated: this._manager.sendPatch(patch) });
  }

  updateExcludeRects(args: { rects?: Array<{ x: number; y: number; w: number; h: number }> } = {}) {
    const rects = Array.isArray(args?.rects) ? args.rects : [];
    return Promise.resolve({ updated: this._manager.sendPatch({ excludeRects: rects }) });
  }

  getStatus() {
    return Promise.resolve(this._manager.getStatus());
  }

  selfTest() {
    return Promise.resolve(this._manager.runSelfTest());
  }

  async prepareScreenRecording(args: { ext?: string; canvasDirect?: boolean } = {}) {
    await this.finishScreenRecording().catch(() => undefined);

    const dirPath = this.resolveRecordingDir();
    fs.mkdirSync(dirPath, { recursive: true });

    const safeExt = args?.ext === "mp4" ? "mp4" : "webm";
    const now = new Date();
    const pad2 = (n: number) => String(n).padStart(2, "0");
    const stamp =
      `${now.getFullYear()}${pad2(now.getMonth() + 1)}${pad2(now.getDate())}_` +
      `${pad2(now.getHours())}${pad2(now.getMinutes())}${pad2(now.getSeconds())}`;
    const filePath = path.join(dirPath, `XStreaming_DLSS5_${stamp}.${safeExt}`);

    // SteamOS / WebGL2 画布直录模式：无需请求桌面屏幕源权限，直接创建输出文件流！
    if (args?.canvasDirect) {
      this._recStream = fs.createWriteStream(filePath);
      this._recFilePath = filePath;
      this._recDirPath = dirPath;
      this._recBytes = 0;
      return {
        ok: true,
        canvasDirect: true,
        sourceId: "webgl-canvas-direct",
        filePath,
        dirPath,
        fullScreen: true,
      };
    }

    const win = this._application._mainWindow;
    if (!win || win.isDestroyed()) {
      throw new Error("主窗口尚未就绪");
    }

    const winBounds = win.getBounds();
    const contentBounds = win.getContentBounds();
    const display = screen.getDisplayMatching(winBounds);
    const sources = await desktopCapturer.getSources({
      types: ["screen"],
      thumbnailSize: { width: 0, height: 0 },
    });
    if (!sources || sources.length === 0) {
      throw new Error("未找到可用的屏幕捕获源");
    }

    const matched =
      sources.find((s) => String(s.display_id) === String(display.id)) || sources[0];

    const db = display.bounds;
    const scale = display.scaleFactor || 1;
    const relX = Math.max(0, contentBounds.x - db.x);
    const relY = Math.max(0, contentBounds.y - db.y);
    const relW = Math.max(128, Math.min(contentBounds.width, db.width - relX));
    const relH = Math.max(128, Math.min(contentBounds.height, db.height - relY));

    const normX = relX / Math.max(1, db.width);
    const normY = relY / Math.max(1, db.height);
    const normW = relW / Math.max(1, db.width);
    const normH = relH / Math.max(1, db.height);
    const fullScreen = normW >= 0.96 && normH >= 0.96;

    const targetW = Math.max(128, Math.round(relW * scale)) & ~1;
    const targetH = Math.max(128, Math.round(relH * scale)) & ~1;

    this._recStream = fs.createWriteStream(filePath);
    this._recFilePath = filePath;
    this._recDirPath = dirPath;
    this._recBytes = 0;

    return {
      ok: true,
      sourceId: matched.id,
      filePath,
      dirPath,
      fullScreen,
      crop: {
        normX,
        normY,
        normW,
        normH,
        targetW,
        targetH,
      },
    };
  }

  writeRecordingChunk(args: { bytes?: Uint8Array | Buffer | ArrayBuffer } = {}) {
    return new Promise((resolve, reject) => {
      if (!this._recStream) {
        resolve({ written: 0 });
        return;
      }
      const raw = args?.bytes;
      if (!raw) {
        resolve({ written: 0 });
        return;
      }
      const buf = Buffer.isBuffer(raw)
        ? raw
        : raw instanceof Uint8Array
        ? Buffer.from(raw.buffer, raw.byteOffset, raw.byteLength)
        : Buffer.from(raw as ArrayBuffer);

      this._recBytes += buf.byteLength;
      this._recStream.write(buf, (err) => {
        if (err) reject(err);
        else resolve({ written: buf.byteLength });
      });
    });
  }

  finishScreenRecording() {
    return new Promise((resolve) => {
      const stream = this._recStream;
      const filePath = this._recFilePath;
      const dirPath = this._recDirPath || this.resolveRecordingDir();
      const sizeMB = Number((this._recBytes / (1024 * 1024)).toFixed(2));
      this._recStream = null;
      this._recFilePath = "";
      this._recBytes = 0;

      if (!stream) {
        resolve({ ok: false, filePath, dirPath, sizeMB });
        return;
      }
      stream.end(() => {
        resolve({ ok: true, filePath, dirPath, sizeMB });
      });
    });
  }

  openRecordingFolder(args: { filePath?: string } = {}) {
    const dirPath = this.resolveRecordingDir();
    fs.mkdirSync(dirPath, { recursive: true });
    if (args?.filePath && fs.existsSync(args.filePath)) {
      shell.showItemInFolder(args.filePath);
      return Promise.resolve({ opened: true, path: args.filePath });
    }
    return shell.openPath(dirPath).then(() => ({ opened: true, path: dirPath }));
  }
}
