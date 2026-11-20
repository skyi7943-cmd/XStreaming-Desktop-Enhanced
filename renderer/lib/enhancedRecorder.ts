// XStreaming Desktop Enhanced - 内置 60FPS 增强画面 + Xbox 纯净原声硬件编码录屏器 (F9)
// 配合原生引擎 WGC + WDA_NONE 直通覆盖层，直接录制叠加了双层 DLSS 5 + NVOFA X2 60FPS 补帧 + CAS/RCAS 锐化的最终游戏画面，
// 并实时内录 Xbox WebRTC 独立音频轨（零微信/系统通知杂音），流式直写 E:\XStreaming_Recordings\ 零内存堆积！

import Ipc from "./ipc";
import { cloneActiveXboxAudioTrack } from "./webrtcLowLatency";
import { steamDeckWebglEnhancer } from "./steamDeckWebglEnhancer";

export interface RecordingFinishResult {
  ok: boolean;
  filePath: string;
  dirPath: string;
  sizeMB: number;
  durationSec: number;
}

function selectPreferredMime(): { mimeType: string; ext: "mp4" | "webm" } {
  if (typeof MediaRecorder !== "undefined" && typeof MediaRecorder.isTypeSupported === "function") {
    const candidates: Array<{ mimeType: string; ext: "mp4" | "webm" }> = [
      { mimeType: "video/mp4;codecs=avc1.64002A,mp4a.40.2", ext: "mp4" },
      { mimeType: "video/mp4;codecs=avc1.42E01E,mp4a.40.2", ext: "mp4" },
      { mimeType: "video/mp4;codecs=avc1,mp4a.40.2", ext: "mp4" },
      { mimeType: "video/mp4", ext: "mp4" },
      { mimeType: "video/webm;codecs=h264,opus", ext: "webm" },
      { mimeType: "video/webm;codecs=vp9,opus", ext: "webm" },
      { mimeType: "video/webm", ext: "webm" },
    ];
    for (const item of candidates) {
      try {
        if (MediaRecorder.isTypeSupported(item.mimeType)) {
          return item;
        }
      } catch {
        // 忽略探测异常
      }
    }
  }
  return { mimeType: "video/webm", ext: "webm" };
}

class EnhancedScreenRecorder {
  private _active = false;
  private _startMs = 0;
  private _filePath = "";
  private _dirPath = "E:\\XStreaming_Recordings";
  private _recorder: MediaRecorder | null = null;
  private _desktopStream: MediaStream | null = null;
  private _croppedStream: MediaStream | null = null;
  private _clonedAudioTrack: MediaStreamTrack | null = null;
  private _cropTimer: ReturnType<typeof setInterval> | null = null;
  private _cropVideoEl: HTMLVideoElement | null = null;
  private _writeChain: Promise<any> = Promise.resolve();

  public isRecording(): boolean {
    return this._active;
  }

  public getElapsedSeconds(): number {
    if (!this._active || !this._startMs) return 0;
    return Math.max(0, Math.floor((Date.now() - this._startMs) / 1000));
  }

  public getCurrentFilePath(): string {
    return this._filePath;
  }

  public async start(): Promise<{ filePath: string; dirPath: string; ext: string }> {
    if (this._active) {
      return { filePath: this._filePath, dirPath: this._dirPath, ext: "mp4" };
    }

    const { mimeType, ext } = selectPreferredMime();
    const deckCanvas = steamDeckWebglEnhancer.isActive()
      ? steamDeckWebglEnhancer.getCanvas()
      : null;

    const prep: any = await Ipc.send("enhance", "prepareScreenRecording", {
      ext,
      canvasDirect: !!deckCanvas,
    });
    if (!prep || !prep.ok || (!prep.sourceId && !deckCanvas)) {
      throw new Error("初始化屏幕录制源失败");
    }

    this._filePath = String(prep.filePath || "");
    this._dirPath = String(prep.dirPath || "E:\\XStreaming_Recordings");

    let finalVideoTrack: MediaStreamTrack;

    if (deckCanvas) {
      // SteamOS / WebGL2 掌机模式：直接从 RDNA2 增强画布零弹窗直录 60FPS 视频流！
      this._croppedStream = deckCanvas.captureStream(60);
      finalVideoTrack = this._croppedStream.getVideoTracks()[0];
    } else {
      // Windows 桌面模式：获取承载了原生 WDA_NONE 增强覆盖层的显示器 60FPS GPU 画面流
      const desktopStream = await navigator.mediaDevices.getUserMedia({
        audio: false,
        video: {
          mandatory: {
            chromeMediaSource: "desktop",
            chromeMediaSourceId: prep.sourceId,
            minFrameRate: 60,
            maxFrameRate: 60,
          },
        } as any,
      });
      this._desktopStream = desktopStream;
      finalVideoTrack = desktopStream.getVideoTracks()[0];

      // 若当前为非全屏窗口模式，自动裁剪出纯游戏客户区画面，不把周围桌面壁纸录进去
      if (!prep.fullScreen && prep.crop) {
        const { normX, normY, normW, normH, targetW, targetH } = prep.crop;
        const vid = document.createElement("video");
        vid.muted = true;
        vid.playsInline = true;
        vid.srcObject = desktopStream;
        await vid.play().catch(() => undefined);
        this._cropVideoEl = vid;

        const canvas = document.createElement("canvas");
        canvas.width = Math.max(320, Number(targetW) || 1920);
        canvas.height = Math.max(180, Number(targetH) || 1080);
        const ctx = canvas.getContext("2d", { alpha: false });

        if (ctx) {
          const drawCrop = () => {
            if (!vid.videoWidth || !vid.videoHeight) return;
            const sx = Math.round(vid.videoWidth * normX);
            const sy = Math.round(vid.videoHeight * normY);
            const sw = Math.max(64, Math.round(vid.videoWidth * normW));
            const sh = Math.max(64, Math.round(vid.videoHeight * normH));
            ctx.drawImage(vid, sx, sy, sw, sh, 0, 0, canvas.width, canvas.height);
          };
          drawCrop();
          this._cropTimer = setInterval(drawCrop, 1000 / 60);
          this._croppedStream = canvas.captureStream(60);
          const ct = this._croppedStream.getVideoTracks()[0];
          if (ct) finalVideoTrack = ct;
        }
      }
    }

    const combinedTracks: MediaStreamTrack[] = [finalVideoTrack];
    const xboxAudio = cloneActiveXboxAudioTrack();
    if (xboxAudio) {
      this._clonedAudioTrack = xboxAudio;
      combinedTracks.push(xboxAudio);
    }

    const recordStream = new MediaStream(combinedTracks);
    const recorder = new MediaRecorder(recordStream, {
      mimeType,
      videoBitsPerSecond: 26_000_000, // 26 Mbps 高码率保留双层 DLSS 5 神经纹理细节
      audioBitsPerSecond: 192_000,
    });

    this._writeChain = Promise.resolve();
    recorder.ondataavailable = (ev: BlobEvent) => {
      if (!ev.data || ev.data.size === 0) return;
      const blob = ev.data;
      this._writeChain = this._writeChain
        .then(async () => {
          const ab = await blob.arrayBuffer();
          await Ipc.send("enhance", "writeRecordingChunk", {
            bytes: new Uint8Array(ab),
          });
        })
        .catch((err) => {
          console.error("[EnhancedRecorder] 写入录屏分片异常:", err);
        });
    };

    recorder.start(1000);
    this._recorder = recorder;
    this._startMs = Date.now();
    this._active = true;

    return {
      filePath: this._filePath,
      dirPath: this._dirPath,
      ext,
    };
  }

  public async stop(): Promise<RecordingFinishResult> {
    if (!this._active) {
      return {
        ok: false,
        filePath: this._filePath,
        dirPath: this._dirPath,
        sizeMB: 0,
        durationSec: 0,
      };
    }

    const durationSec = Math.max(1, Math.round((Date.now() - this._startMs) / 1000));
    this._active = false;

    const recorder = this._recorder;
    this._recorder = null;

    if (recorder && recorder.state !== "inactive") {
      await new Promise<void>((resolve) => {
        const done = () => resolve();
        recorder.addEventListener("stop", done, { once: true });
        try {
          recorder.stop();
        } catch {
          resolve();
        }
      });
    }

    if (this._cropTimer) {
      clearInterval(this._cropTimer);
      this._cropTimer = null;
    }
    if (this._cropVideoEl) {
      try {
        this._cropVideoEl.pause();
        this._cropVideoEl.srcObject = null;
      } catch {
        // 忽略
      }
      this._cropVideoEl = null;
    }
    if (this._croppedStream) {
      this._croppedStream.getTracks().forEach((t) => t.stop());
      this._croppedStream = null;
    }
    if (this._desktopStream) {
      this._desktopStream.getTracks().forEach((t) => t.stop());
      this._desktopStream = null;
    }
    if (this._clonedAudioTrack) {
      try {
        this._clonedAudioTrack.stop();
      } catch {
        // 忽略
      }
      this._clonedAudioTrack = null;
    }

    await this._writeChain.catch(() => undefined);
    const fin: any = await Ipc.send("enhance", "finishScreenRecording").catch(() => ({}));

    return {
      ok: !!fin?.ok,
      filePath: String(fin?.filePath || this._filePath),
      dirPath: String(fin?.dirPath || this._dirPath),
      sizeMB: Number(fin?.sizeMB || 0),
      durationSec,
    };
  }

  public async openFolder(filePath?: string): Promise<void> {
    await Ipc.send("enhance", "openRecordingFolder", filePath ? { filePath } : {}).catch(
      () => undefined
    );
  }
}

export const enhancedRecorder = new EnhancedScreenRecorder();
