// XStreaming Desktop Enhanced - 1:1 对齐 Magpie 原版 DLSS 5 神经风格管线 + 真 X2 插帧 + 极限低延迟默认配置 (v6)

export const ENHANCE_STORAGE_KEY = "XStreaming.dlss5EnhanceConfig.v6";

export type Wifi6JitterMode = "zero" | "ultra" | "wifi6" | "balanced" | "default";

export interface EnhanceExcludeRect {
  x: number;
  y: number;
  w: number;
  h: number;
}

export interface EnhanceSettings {
  // 全局增强总开关
  enabled: boolean;

  // Wi-Fi 6 + WebRTC + 音画解绑 + 手柄瞬发全链路降延迟控制
  wifi6JitterMode: Wifi6JitterMode;
  avSyncUnbind: boolean;
  turboGamepad: boolean;
  allowTearing: boolean;
  obsCaptureCompat: boolean;

  // 硬件光流低延迟 2X/3X 多倍插帧、四大专用补帧模型与 30-in-60 容器重复帧智能滤除
  fgEnabled: boolean;
  fgMultiplier: 2 | 3;
  fgModel: 0 | 1 | 2 | 3;
  dedup30In60: boolean;
  dedupSensitivity: number;
  fgMode: 0 | 1;
  fgSceneCutThreshold: number;
  fgUiProtection: number;

  // 内置真 DLSS 5 (nvngx_dlssnr.dll Feature 18) 单层 / 双层串联叠加神经渲染
  dlss5Enabled: boolean;
  dlss5Passes: 1 | 2;
  dlss5LatencyMode: 0 | 1;
  dlss5InputScale: number;
  dlss5Style: 0 | 1 | 2;       // 0: 自然均衡 (Natural), 1: 鲜明通透 (Vivid), 2: 电影光影 (Cinematic)
  dlss5Intensity: number;
  dlss5Pass2Style: 0 | 1 | 2;
  dlss5Pass2Intensity: number;
  dlss5LocalTone: number;
  dlss5LocalStructure: number;
  dlss5SkinStructure: number;
  dlss5AutoMask: boolean;
  dlss5UiCorrection: boolean;

  // Oklab 感知空间残差细节精调（默认全中性 1.0/0.0 对齐 Magpie 无损直通，100% 释放自然/鲜明/电影风格滤镜！）
  residualStrength: number;
  shadowStructure: number;
  highlightGlow: number;
  saturation: number;
  darkProtection: number;
  highlightProtection: number;
  antiFlicker: 0 | 1 | 2;

  // 硬件级超低延迟去噪锐化 (CAS / FSR RCAS)
  sharpenMode: 0 | 1 | 2;
  sharpenStrength: number;
  sharpenDenoise: boolean;

  // 实时分屏对比位置
  splitCompare: number;
}

export interface EnhanceTelemetry {
  running: boolean;
  ready?: boolean;
  vendor?: string;
  nrBackend?: string;
  flowBackend?: string;
  rawFps?: number;
  srcFps: number;
  dupSkipped?: number;
  dedup30In60?: boolean;
  outFps: number;
  dlss5Active: boolean;
  dlss5Passes?: number;
  dlss5Sync?: boolean;
  dlss5WorkRes: string;
  dlss5Ms: number;
  dlss5HostMs?: number;
  gpuPipeMs?: number;
  fgHoldMs?: number;
  totalAddedMs?: number;
  recordable?: boolean;
  nvofa: boolean;
  flowMs: number;
  composeMs: number;
  fgMs: number;
  fgActive: boolean;
  fgMode: number;
  fgMultiplier?: number;
  fgModel?: number;
  sharpenMode: number;
  gpu: string;
  webrtcJitterMs?: number;
}

export const FG_MODEL_OPTIONS: Array<{
  id: 0 | 1 | 2 | 3;
  shortName: string;
  title: string;
  desc: string;
}> = [
  {
    id: 0,
    shortName: "均衡全能",
    title: "🎯 均衡全能 (Optical-Balanced v3)",
    desc: "双向光流遮挡自洽 + 5点中值去毛刺，适合 90% 综合 3A 游戏（默认推荐）",
  },
  {
    id: 1,
    shortName: "竞速大动态",
    title: "🏎️ 竞速射击大动态 (Action-UltraWide)",
    desc: "4层 ±92px 广域搜索 + 运动路面锐度保持，专为《地平线》/FPS 甩视角与 3X 补帧定制",
  },
  {
    id: 2,
    shortName: "轮廓零果冻",
    title: "🛡️ 轮廓零果冻 (Edge-Lock HaloFree)",
    desc: "3×3 九宫格中值 + 剪切边缘单侧防重影锁定，专为《黑神话》/魂类第三人称挥刀定制",
  },
  {
    id: 3,
    shortName: "字幕UI强护盾",
    title: "✨ 字幕UI强护盾 (Anime & HUD-Shield)",
    desc: "双环 ±5px 字幕小地图强护盾 + 二次元黑线防冲淡，专为 JRPG/《博德之门3》定制",
  },
];

// 默认配置：1:1 对齐 Magpie 原版中性直通参数，100% 无损输出 nvngx_dlssnr 自然/鲜明/电影三大风格！
export const DEFAULT_ENHANCE_SETTINGS: EnhanceSettings = {
  enabled: true,

  wifi6JitterMode: "zero",
  avSyncUnbind: true,
  turboGamepad: true,
  allowTearing: true,
  obsCaptureCompat: false,

  fgEnabled: true,
  fgMultiplier: 2,
  fgModel: 0,
  dedup30In60: true,
  dedupSensitivity: 5,
  fgMode: 1,
  fgSceneCutThreshold: 0.18,
  fgUiProtection: 0.75,

  dlss5Enabled: true,
  dlss5Passes: 1,
  dlss5LatencyMode: 1,
  dlss5InputScale: 75,
  dlss5Style: 0,
  dlss5Intensity: 1.0,
  dlss5Pass2Style: 0,
  dlss5Pass2Intensity: 0.85,
  dlss5LocalTone: 1.0,
  dlss5LocalStructure: 1.0,
  dlss5SkinStructure: 0.0,
  dlss5AutoMask: false,
  dlss5UiCorrection: false,

  residualStrength: 1.0,
  shadowStructure: 1.0,
  highlightGlow: 1.0,
  saturation: 1.0,
  darkProtection: 0.0,
  highlightProtection: 0.0,
  antiFlicker: 1,

  sharpenMode: 1,
  sharpenStrength: 0.5,
  sharpenDenoise: true,

  splitCompare: 0.0,
};

// 一键场景预设模板
export const ENHANCE_PRESETS: Record<string, { label: string; desc: string; patch: Partial<EnhanceSettings> }> = {
  steamDeckHandheld: {
    label: "🎮 Steam Deck / 掌机黄金预设 (800P 鲜明通透 + 真插60)",
    desc: "专为 Steam Deck LCD/OLED 与 15W 掌机定制：鲜明通透色彩补偿 + 85% 黄金能效工作集 + 真 30→60 补帧 + 无线稳帧",
    patch: {
      enabled: true,
      wifi6JitterMode: "wifi6",
      avSyncUnbind: true,
      turboGamepad: true,
      allowTearing: true,
      obsCaptureCompat: false,
      fgEnabled: true,
      dedup30In60: true,
      dedupSensitivity: 5,
      fgMode: 1,
      fgUiProtection: 0.8,
      dlss5Enabled: true,
      dlss5Passes: 1,
      dlss5LatencyMode: 1,
      dlss5InputScale: 85,
      dlss5Style: 1,
      dlss5Intensity: 1.05,
      dlss5LocalTone: 1.05,
      dlss5LocalStructure: 1.1,
      dlss5SkinStructure: 0.0,
      dlss5AutoMask: false,
      dlss5UiCorrection: false,
      residualStrength: 1.0,
      shadowStructure: 1.0,
      highlightGlow: 1.0,
      saturation: 1.0,
      darkProtection: 0.0,
      highlightProtection: 0.0,
      antiFlicker: 1,
      sharpenMode: 1,
      sharpenStrength: 0.55,
      sharpenDenoise: true,
      splitCompare: 0.0,
    },
  },
  xbox30To90UltraHighRefresh: {
    label: "⚡ 3X 超高刷极限丝滑预设 (30帧直插90帧 / 60帧插180帧 + 竞速大动态模型)",
    desc: "120Hz/144Hz/165Hz/240Hz 电脑高刷屏专属：真 3X 三倍光流补帧 + 竞速大动态模型 + DLSS 5",
    patch: {
      enabled: true,
      wifi6JitterMode: "zero",
      avSyncUnbind: true,
      turboGamepad: true,
      allowTearing: true,
      obsCaptureCompat: false,
      fgEnabled: true,
      fgMultiplier: 3,
      fgModel: 1,
      dedup30In60: true,
      dedupSensitivity: 5,
      fgMode: 1,
      fgUiProtection: 0.75,
      dlss5Enabled: true,
      dlss5Passes: 1,
      dlss5LatencyMode: 1,
      dlss5InputScale: 75,
      dlss5Style: 0,
      dlss5Intensity: 1.0,
      dlss5LocalTone: 1.0,
      dlss5LocalStructure: 1.0,
      dlss5SkinStructure: 0.0,
      dlss5AutoMask: false,
      dlss5UiCorrection: false,
      residualStrength: 1.0,
      shadowStructure: 1.0,
      highlightGlow: 1.0,
      saturation: 1.0,
      darkProtection: 0.0,
      highlightProtection: 0.0,
      antiFlicker: 1,
      sharpenMode: 1,
      sharpenStrength: 0.55,
      sharpenDenoise: true,
      splitCompare: 0.0,
    },
  },
  xbox30To60UltraLowLat: {
    label: "🔥 30帧游戏真插60 + 原汁原味 DLSS 5 自然均衡 (默认推荐)",
    desc: "1:1 对齐 Magpie 无损神经光影 + 真 2X (30→60) 补帧 + 逐帧零残影 + 0ms 音画解绑",
    patch: {
      enabled: true,
      wifi6JitterMode: "zero",
      avSyncUnbind: true,
      turboGamepad: true,
      allowTearing: true,
      obsCaptureCompat: false,
      fgEnabled: true,
      fgMultiplier: 2,
      fgModel: 0,
      dedup30In60: true,
      dedupSensitivity: 5,
      fgMode: 1,
      fgUiProtection: 0.75,
      dlss5Enabled: true,
      dlss5Passes: 1,
      dlss5LatencyMode: 1,
      dlss5InputScale: 75,
      dlss5Style: 0,
      dlss5Intensity: 1.0,
      dlss5LocalTone: 1.0,
      dlss5LocalStructure: 1.0,
      dlss5SkinStructure: 0.0,
      dlss5AutoMask: false,
      dlss5UiCorrection: false,
      residualStrength: 1.0,
      shadowStructure: 1.0,
      highlightGlow: 1.0,
      saturation: 1.0,
      darkProtection: 0.0,
      highlightProtection: 0.0,
      antiFlicker: 1,
      sharpenMode: 1,
      sharpenStrength: 0.5,
      sharpenDenoise: true,
      splitCompare: 0.0,
    },
  },
  cinematicDualDlss5: {
    label: "🎬 电影光影 · 双层 DLSS 5 串联精雕 + 轮廓零果冻补帧",
    desc: "电影光影风格 (Style 2) + 双层串联重塑 + 轮廓零果冻补帧模型 (适合黑神话/魂类/大镖客)",
    patch: {
      enabled: true,
      wifi6JitterMode: "zero",
      avSyncUnbind: true,
      turboGamepad: true,
      allowTearing: true,
      obsCaptureCompat: false,
      fgEnabled: true,
      fgMultiplier: 2,
      fgModel: 2,
      dedup30In60: true,
      dedupSensitivity: 5,
      fgMode: 1,
      fgUiProtection: 0.8,
      dlss5Enabled: true,
      dlss5Passes: 2,
      dlss5LatencyMode: 1,
      dlss5InputScale: 75,
      dlss5Style: 2,
      dlss5Intensity: 1.0,
      dlss5Pass2Style: 0,
      dlss5Pass2Intensity: 0.8,
      dlss5LocalTone: 1.0,
      dlss5LocalStructure: 1.05,
      dlss5SkinStructure: 0.0,
      dlss5AutoMask: false,
      dlss5UiCorrection: false,
      residualStrength: 1.0,
      shadowStructure: 1.0,
      highlightGlow: 1.0,
      saturation: 1.0,
      darkProtection: 0.0,
      highlightProtection: 0.0,
      antiFlicker: 1,
      sharpenMode: 2,
      sharpenStrength: 0.55,
      sharpenDenoise: true,
      splitCompare: 0.0,
    },
  },
  vividFullRes: {
    label: "✨ 鲜明通透 · 100% 满分辨率 DLSS 5 + 竞速大动态补帧",
    desc: "鲜明通透风格 (Style 1) + 100% 原生分辨率推理 + 竞速大动态补帧模型 + FSR RCAS 锐化",
    patch: {
      enabled: true,
      wifi6JitterMode: "zero",
      avSyncUnbind: true,
      turboGamepad: true,
      allowTearing: true,
      obsCaptureCompat: false,
      fgEnabled: true,
      fgMultiplier: 2,
      fgModel: 1,
      dedup30In60: true,
      dedupSensitivity: 5,
      fgMode: 1,
      fgUiProtection: 0.75,
      dlss5Enabled: true,
      dlss5Passes: 1,
      dlss5LatencyMode: 1,
      dlss5InputScale: 100,
      dlss5Style: 1,
      dlss5Intensity: 1.05,
      dlss5LocalTone: 1.0,
      dlss5LocalStructure: 1.05,
      dlss5SkinStructure: 0.0,
      dlss5AutoMask: false,
      dlss5UiCorrection: false,
      residualStrength: 1.0,
      shadowStructure: 1.0,
      highlightGlow: 1.0,
      saturation: 1.0,
      darkProtection: 0.0,
      highlightProtection: 0.0,
      antiFlicker: 1,
      sharpenMode: 2,
      sharpenStrength: 0.55,
      sharpenDenoise: true,
      splitCompare: 0.0,
    },
  },
  esportsNoFg: {
    label: "🎯 原生60帧竞技直通 (仅 DLSS 5 + 锐化 + 0ms)",
    desc: "关闭补帧，开启 0ms 音画解绑 + 手柄瞬发 + 逐帧零残影 DLSS 5 + CAS 锐化",
    patch: {
      enabled: true,
      wifi6JitterMode: "zero",
      avSyncUnbind: true,
      turboGamepad: true,
      allowTearing: true,
      obsCaptureCompat: false,
      fgEnabled: false,
      dlss5Enabled: true,
      dlss5Passes: 1,
      dlss5LatencyMode: 1,
      dlss5InputScale: 75,
      dlss5Style: 0,
      dlss5Intensity: 1.0,
      dlss5AutoMask: false,
      dlss5UiCorrection: false,
      residualStrength: 1.0,
      shadowStructure: 1.0,
      highlightGlow: 1.0,
      saturation: 1.0,
      darkProtection: 0.0,
      highlightProtection: 0.0,
      sharpenMode: 1,
      sharpenStrength: 0.6,
      sharpenDenoise: true,
      splitCompare: 0.0,
    },
  },
};

export function loadSavedEnhanceSettings(): EnhanceSettings {
  if (typeof window === "undefined") {
    return { ...DEFAULT_ENHANCE_SETTINGS };
  }
  const isLinuxOrSteamOs =
    typeof navigator !== "undefined" && /Linux/i.test(navigator.platform || navigator.userAgent || "");
  const platformDefault: EnhanceSettings = isLinuxOrSteamOs
    ? { ...DEFAULT_ENHANCE_SETTINGS, ...ENHANCE_PRESETS.steamDeckHandheld.patch }
    : { ...DEFAULT_ENHANCE_SETTINGS };
  try {
    const raw = window.localStorage.getItem(ENHANCE_STORAGE_KEY);
    if (!raw) return platformDefault;
    const parsed = JSON.parse(raw);
    return {
      ...platformDefault,
      ...parsed,
      splitCompare: 0.0,
    };
  } catch {
    return platformDefault;
  }
}

export function saveEnhanceSettings(settings: EnhanceSettings): void {
  if (typeof window === "undefined") return;
  try {
    const toStore = { ...settings, splitCompare: 0.0 };
    window.localStorage.setItem(ENHANCE_STORAGE_KEY, JSON.stringify(toStore));
  } catch {
    // 忽略存储配额异常
  }
}
