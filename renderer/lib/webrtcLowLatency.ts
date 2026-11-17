// XStreaming Desktop Enhanced - Wi-Fi 6 + WebRTC 0ms 零缓冲 + 音画同步解绑 + 手柄 0ms 瞬发引擎 (v2)
// 一次性解决四大前端串流延迟瓶颈：
//   1. WebRTC 视频接收器 playoutDelayHint / jitterBufferTarget 压至 0ms (零缓冲直通档)
//   2. 拦截 HTMLVideoElement.srcObject，将视频轨剥离至独立 MediaStream，彻底解除 Chromium 等待音频缓冲的 15~35ms Lip-Sync 扣押
//   3. 拦截 xstreaming-player 每 4ms (250次/秒) 无意义调用的 video.play() IPC 风暴
//   4. 接管 InputChannel 手柄队列：单槽最新态防积压 + 500Hz 非嵌套采样 + 按键/摇杆变化 0ms 事件级瞬发

import type { Wifi6JitterMode } from "../common/enhanceDefaults";

interface LowLatencyState {
  installed: boolean;
  mediaHooksInstalled: boolean;
  peers: Set<RTCPeerConnection>;
  videos: Set<HTMLVideoElement>;
  currentMode: Wifi6JitterMode;
  avSyncUnbind: boolean;
  turboGamepad: boolean;
  lastJitterBytesDelay: number;
  lastJitterEmitted: number;
  latestJitterMs: number;
}

const RESOLVED_PROMISE = Promise.resolve();

const state: LowLatencyState = {
  installed: false,
  mediaHooksInstalled: false,
  peers: new Set<RTCPeerConnection>(),
  videos: new Set<HTMLVideoElement>(),
  currentMode: "zero",
  avSyncUnbind: true,
  turboGamepad: true,
  lastJitterBytesDelay: 0,
  lastJitterEmitted: 0,
  latestJitterMs: 0,
};

let nativeSrcObjectDesc: PropertyDescriptor | undefined;

function modeToTargetMs(mode: Wifi6JitterMode): number | null {
  switch (mode) {
    case "zero":
      return 0;   // 0ms 极限零缓冲直通档（解码完成即刻上屏）
    case "ultra":
      return 2;   // 2ms 极速档
    case "wifi6":
      return 8;   // 8ms Wi-Fi 6 抗微突发抖动档
    case "balanced":
      return 20;  // 20ms 隔墙防丢包稳帧档
    case "default":
    default:
      return null;
  }
}

function applyReceiverLowLatency(receiver: RTCRtpReceiver, mode: Wifi6JitterMode): void {
  const targetMs = modeToTargetMs(mode);
  const anyRec = receiver as any;
  try {
    if (targetMs === null) {
      if ("playoutDelayHint" in anyRec) anyRec.playoutDelayHint = null;
      if ("jitterBufferTarget" in anyRec) anyRec.jitterBufferTarget = null;
      return;
    }
    const isVideo = receiver.track?.kind === "video";
    // 视频允许 0ms 零延迟直出，音频保留 4ms 最小底噪保护防爆音
    const effectiveMs = isVideo ? targetMs : Math.max(targetMs, 4);
    if ("playoutDelayHint" in anyRec) {
      anyRec.playoutDelayHint = effectiveMs / 1000.0;
    }
    if ("jitterBufferTarget" in anyRec) {
      anyRec.jitterBufferTarget = effectiveMs;
    }
  } catch {
    // 兼容只读实现
  }
}

function installMediaElementLowLatencyHooks(): void {
  if (typeof window === "undefined" || state.mediaHooksInstalled) return;

  // 1. 消除 xstreaming-player Video.ts 第 499 行 setInterval(() => videoRender.play(), 4) 造成的 250Hz 媒体线程 IPC 阻塞
  const origPlay = HTMLVideoElement.prototype.play;
  if (typeof origPlay === "function") {
    HTMLVideoElement.prototype.play = function (this: HTMLVideoElement) {
      if (!this.paused && !this.ended && this.readyState >= 2) {
        return RESOLVED_PROMISE;
      }
      return origPlay.call(this);
    };
  }

  // 2. 拦截 <video>.srcObject，剥离音画共享 MediaStream ID，彻底解除 Chromium RtpStreamsSynchronizer 15~35ms 唇音同步锁帧！
  nativeSrcObjectDesc =
    Object.getOwnPropertyDescriptor(HTMLMediaElement.prototype, "srcObject") ||
    Object.getOwnPropertyDescriptor(HTMLVideoElement.prototype, "srcObject");

  if (nativeSrcObjectDesc && nativeSrcObjectDesc.set && nativeSrcObjectDesc.get) {
    const origGet = nativeSrcObjectDesc.get;
    const origSet = nativeSrcObjectDesc.set;

    Object.defineProperty(HTMLVideoElement.prototype, "srcObject", {
      configurable: true,
      enumerable: true,
      get(this: HTMLVideoElement) {
        return origGet.call(this);
      },
      set(this: HTMLVideoElement, val: MediaProvider | null) {
        if (val && typeof MediaStream !== "undefined" && val instanceof MediaStream) {
          state.videos.add(this);
          (this as any).__rawXboxStream = val;
          const videoTracks = val.getVideoTracks();
          if (state.avSyncUnbind && videoTracks.length > 0) {
            const decoupled = new MediaStream(videoTracks);
            (this as any).__decoupledVideoStream = decoupled;
            origSet.call(this, decoupled);
            return;
          }
        }
        origSet.call(this, val);
      },
    });
  }

  state.mediaHooksInstalled = true;
}

export function setAvSyncUnbind(enabled: boolean): void {
  if (state.avSyncUnbind === enabled) return;
  state.avSyncUnbind = enabled;
  if (!nativeSrcObjectDesc?.set) return;

  state.videos.forEach((video) => {
    if (!video.isConnected) {
      state.videos.delete(video);
      return;
    }
    const rawStream: MediaStream | undefined = (video as any).__rawXboxStream;
    if (!rawStream) return;
    const vTracks = rawStream.getVideoTracks();
    if (vTracks.length === 0) return;

    try {
      if (enabled) {
        const decoupled = new MediaStream(vTracks);
        (video as any).__decoupledVideoStream = decoupled;
        nativeSrcObjectDesc!.set!.call(video, decoupled);
      } else {
        nativeSrcObjectDesc!.set!.call(video, rawStream);
      }
      void video.play().catch(() => undefined);
    } catch {
      // 忽略已卸载节点
    }
  });
}

export function setTurboGamepadEnabled(enabled: boolean): void {
  state.turboGamepad = enabled;
}

export function applyWifi6LowLatencyMode(mode: Wifi6JitterMode): void {
  state.currentMode = mode;
  state.peers.forEach((pc) => {
    if (pc.connectionState === "closed") {
      state.peers.delete(pc);
      return;
    }
    try {
      pc.getReceivers().forEach((rec) => applyReceiverLowLatency(rec, mode));
    } catch {
      // 忽略已销毁连接
    }
  });
}

export function installWebRtcLowLatencyHook(
  initialMode: Wifi6JitterMode = "zero",
  avUnbind = true,
  turboPad = true
): void {
  if (typeof window === "undefined") return;
  state.currentMode = initialMode;
  state.avSyncUnbind = avUnbind;
  state.turboGamepad = turboPad;

  installMediaElementLowLatencyHooks();

  if (state.installed) {
    applyWifi6LowLatencyMode(initialMode);
    return;
  }

  const NativePeerConnection = window.RTCPeerConnection;
  if (!NativePeerConnection) return;

  const WrappedPeerConnection = function (this: any, config?: RTCConfiguration, constraints?: any) {
    const pc: RTCPeerConnection =
      constraints !== undefined
        ? new (NativePeerConnection as any)(config, constraints)
        : new NativePeerConnection(config);
    state.peers.add(pc);

    pc.addEventListener("track", (ev: RTCTrackEvent) => {
      if (ev.receiver) {
        applyReceiverLowLatency(ev.receiver, state.currentMode);
      }
      setTimeout(() => applyWifi6LowLatencyMode(state.currentMode), 150);
      setTimeout(() => applyWifi6LowLatencyMode(state.currentMode), 900);
    });

    pc.addEventListener("connectionstatechange", () => {
      if (pc.connectionState === "connected") {
        applyWifi6LowLatencyMode(state.currentMode);
      } else if (pc.connectionState === "closed" || pc.connectionState === "failed") {
        state.peers.delete(pc);
      }
    });

    return pc;
  } as unknown as typeof RTCPeerConnection;

  WrappedPeerConnection.prototype = NativePeerConnection.prototype;
  Object.setPrototypeOf(WrappedPeerConnection, NativePeerConnection);
  try {
    (WrappedPeerConnection as any).generateCertificate =
      NativePeerConnection.generateCertificate.bind(NativePeerConnection);
  } catch {
    // 忽略静态方法绑定限制
  }

  window.RTCPeerConnection = WrappedPeerConnection;
  state.installed = true;
}

const DIGITAL_BUTTON_KEYS = [
  "A",
  "B",
  "X",
  "Y",
  "LeftShoulder",
  "RightShoulder",
  "View",
  "Menu",
  "LeftThumb",
  "RightThumb",
  "DPadUp",
  "DPadDown",
  "DPadLeft",
  "DPadRight",
  "Nexus",
] as const;

const ANALOG_AXIS_KEYS = [
  "LeftTrigger",
  "RightTrigger",
  "LeftThumbXAxis",
  "LeftThumbYAxis",
  "RightThumbXAxis",
  "RightThumbYAxis",
] as const;

function hasMeaningfulGamepadDelta(prev: any, next: any): boolean {
  if (!prev || !next) return true;
  for (let i = 0; i < DIGITAL_BUTTON_KEYS.length; i++) {
    const k = DIGITAL_BUTTON_KEYS[i];
    if ((prev[k] || 0) !== (next[k] || 0)) return true;
  }
  for (let i = 0; i < ANALOG_AXIS_KEYS.length; i++) {
    const k = ANALOG_AXIS_KEYS[i];
    if (Math.abs((prev[k] || 0) - (next[k] || 0)) >= 0.008) return true;
  }
  return false;
}

// 为 xStreamingPlayer 挂载手柄 0ms 瞬态直发钩子与 500Hz 非嵌套采样器（返回清理函数）
export function attachTurboInputToPlayer(xPlayer: any): () => void {
  if (!xPlayer || typeof xPlayer.getChannelProcessor !== "function") {
    return () => undefined;
  }

  const inputChannel = xPlayer.getChannelProcessor("input");
  if (!inputChannel || (inputChannel as any).__turboHooked) {
    return () => undefined;
  }
  (inputChannel as any).__turboHooked = true;

  const origQueueState = inputChannel.queueGamepadState.bind(inputChannel);
  const lastSentByIdx: Record<number, any> = {};
  let lastInstantFlushAt = 0;

  const tryInstantFlush = () => {
    const dc = xPlayer.getChannel?.("input");
    if (!dc || dc.readyState !== "open") return false;
    const metadataQueue = inputChannel.getMetadataQueue();
    const gamepadQueue = inputChannel.getGamepadQueue();
    const pointerQueue = inputChannel.getPointerQueue();
    const mouseQueue = inputChannel.getMouseQueue();
    const keyboardQueue = inputChannel.getKeyboardQueue();
    if (gamepadQueue.length === 0 && pointerQueue.length === 0 && mouseQueue.length === 0 && keyboardQueue.length === 0) {
      return false;
    }
    const packetBuf = inputChannel._createInputPacket(
      2,
      metadataQueue,
      gamepadQueue,
      pointerQueue,
      mouseQueue,
      keyboardQueue
    );
    inputChannel._metadataFps?.count?.();
    inputChannel.send(packetBuf);
    return true;
  };

  inputChannel.queueGamepadState = function (input: any) {
    if (!input) return 0;
    if (!state.turboGamepad) {
      return origQueueState(input);
    }

    const gpIdx = Number(input.GamepadIndex) || 0;
    // 单槽最新态覆盖：绝不堆积历史旧帧，消除最高 29 帧输入滞后
    const frames: any[] = inputChannel._gamepadFrames;
    let replaced = false;
    for (let i = 0; i < frames.length; i++) {
      if ((Number(frames[i]?.GamepadIndex) || 0) === gpIdx) {
        frames[i] = input;
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      frames.push(input);
    }

    const now = performance.now();
    const prev = lastSentByIdx[gpIdx];
    if (now - lastInstantFlushAt >= 1.0 && hasMeaningfulGamepadDelta(prev, input)) {
      if (tryInstantFlush()) {
        lastSentByIdx[gpIdx] = { ...input };
        lastInstantFlushAt = now;
      }
    }
    return inputChannel._gamepadFrames.length;
  };

  // 非嵌套顶层 2ms (500Hz) 采样器：绕开 Gamepad.ts 递归 setTimeout 在 Chromium 中的 5 层 4ms 节流上限！
  const fastPollTimer = setInterval(() => {
    if (!state.turboGamepad) return;
    const dc = xPlayer.getChannel?.("input");
    if (!dc || dc.readyState !== "open") return;
    const driver = xPlayer._inputDriver;
    if (!driver || driver._isVirtualButtonPressing) return;

    let states: any[] = [];
    if (xPlayer._gamepad_kernal === "Native") {
      if ((globalThis as any).gpState) {
        states = [(globalThis as any).gpState];
      }
    } else if (typeof driver.requestStates === "function") {
      states = driver.requestStates();
      if (states[0] && driver._nexusOverrideN === true) {
        states[0].Nexus = 1;
      }
    }

    for (let i = 0; i < states.length; i++) {
      if (states[i]) {
        inputChannel.queueGamepadState(states[i]);
      }
    }
  }, 2);

  return () => {
    clearInterval(fastPollTimer);
  };
}

// 实时读取 WebRTC 视频接收端真实抖动缓冲延迟 (ms)
export async function queryWebRtcJitterBufferMs(): Promise<number | undefined> {
  for (const pc of Array.from(state.peers)) {
    if (pc.connectionState !== "connected") continue;
    try {
      const stats = await pc.getStats();
      let found: number | undefined;
      stats.forEach((report: any) => {
        if (report.type === "inbound-rtp" && report.kind === "video") {
          const totalDelaySec = Number(report.jitterBufferDelay);
          const totalEmitted = Number(report.jitterBufferEmittedCount);
          if (
            Number.isFinite(totalDelaySec) &&
            Number.isFinite(totalEmitted) &&
            totalEmitted > state.lastJitterEmitted
          ) {
            const deltaDelayMs = (totalDelaySec - state.lastJitterBytesDelay) * 1000.0;
            const deltaCount = totalEmitted - state.lastJitterEmitted;
            state.lastJitterBytesDelay = totalDelaySec;
            state.lastJitterEmitted = totalEmitted;
            if (deltaCount > 0 && deltaDelayMs >= 0 && deltaDelayMs / deltaCount < 500) {
              state.latestJitterMs = Number((deltaDelayMs / deltaCount).toFixed(1));
            }
          }
          found = state.latestJitterMs;
        }
      });
      if (found !== undefined) return found;
    } catch {
      // 忽略单次采样异常
    }
  }
  return undefined;
}

// 克隆当前活跃的 Xbox WebRTC 原始游戏音频轨，供内置 60FPS 增强录屏器纯净内录（零系统通知杂音）
export function cloneActiveXboxAudioTrack(): MediaStreamTrack | null {
  for (const pc of Array.from(state.peers)) {
    if (pc.connectionState === "closed") continue;
    try {
      for (const rec of pc.getReceivers()) {
        if (rec.track && rec.track.kind === "audio" && rec.track.readyState === "live") {
          return rec.track.clone();
        }
      }
    } catch {
      // 忽略
    }
  }
  if (typeof document !== "undefined") {
    const mediaEls = Array.from(document.querySelectorAll("audio, video")) as HTMLMediaElement[];
    for (const el of mediaEls) {
      const stream: MediaStream | undefined =
        (el as any).__rawXboxStream ||
        (el.srcObject instanceof MediaStream ? el.srcObject : undefined);
      if (stream) {
        const audioTrack = stream.getAudioTracks().find((t) => t.readyState === "live");
        if (audioTrack) return audioTrack.clone();
      }
    }
  }
  return null;
}
