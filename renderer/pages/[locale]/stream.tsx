import moment from "moment";
import { useTranslation } from "next-i18next";
import { useRouter } from "next/router";
import { useCallback, useEffect, useRef, useState } from "react";
import xStreamingPlayer from "xstreaming-player";
import {
  addToast,
  Modal,
  ModalContent,
  ModalHeader,
  ModalBody,
  ModalFooter,
  Button,
  Input,
} from "@heroui/react";
import ActionBar from "../../components/ActionBar";
import Display from "../../components/Display";
import FSRDisplay from "../../components/FSRDisplay";
import Audio from "../../components/Audio";
import FailedModal from "../../components/FailedModal";
import Loading from "../../components/Loading";
import Perform from "../../components/Perform";
import WarningModal from "../../components/WarningModal";
import TextModal from "../../components/TextModal";
import EnhanceControlDrawer from "../../components/EnhanceControlDrawer";
import { useSettings } from "../../context/userContext";
import { getStaticPaths, makeStaticProperties } from "../../lib/get-static";
import Ipc from "../../lib/ipc";
import { DISPLAY_KEY, FSR_DISPLAY_KEY } from "../../common/constans";
import {
  EnhanceSettings,
  EnhanceTelemetry,
  loadSavedEnhanceSettings,
  saveEnhanceSettings,
} from "../../common/enhanceDefaults";
import {
  applyWifi6LowLatencyMode,
  attachTurboInputToPlayer,
  installWebRtcLowLatencyHook,
  queryWebRtcJitterBufferMs,
  setAvSyncUnbind,
  setTurboGamepadEnabled,
} from "../../lib/webrtcLowLatency";
import { enhancedRecorder } from "../../lib/enhancedRecorder";
import { steamDeckWebglEnhancer } from "../../lib/steamDeckWebglEnhancer";
import type {
  NativeGamepadTestControllerSnapshot,
  NativeGamepadTestSnapshot,
} from "../../common/nativeGamepadTest";

const XCLOUD_PREFIX = "xcloud_";
const SYSTEM_UI_TARGET_SHOW_MESSAGE_DIALOG =
  "/streaming/systemUi/messages/ShowMessageDialog";
const SYSTEM_UI_TARGET_SHOW_VIRTUAL_KEYBOARD =
  "/streaming/systemUi/messages/ShowVirtualKeyboard";
const STREAMING_TOUCHCONTROLS_SCOPE = "/streaming/touchcontrols";

const DEFAULT_OPTIONS = {
  sharpness: 5,
  saturation: 100,
  contrast: 100,
  brightness: 100,
};

const FSR_DEFAULT_OPTIONS = {
  sharpness: 2,
};

const createEmptyNativeGamepadState = () => ({
  GamepadIndex: 0,
  A: 0,
  B: 0,
  X: 0,
  Y: 0,
  LeftShoulder: 0,
  RightShoulder: 0,
  LeftTrigger: 0,
  RightTrigger: 0,
  View: 0,
  Menu: 0,
  LeftThumb: 0,
  RightThumb: 0,
  DPadUp: 0,
  DPadDown: 0,
  DPadLeft: 0,
  DPadRight: 0,
  Nexus: 0,
  LeftThumbXAxis: 0,
  LeftThumbYAxis: 0,
  RightThumbXAxis: 0,
  RightThumbYAxis: 0,
});

const resolveGamepadKernel = (settings: any): "Web" | "Native" => {
  if (settings?.coop) {
    return "Web";
  }

  const normalized = String(settings?.gamepad_kernel || settings?.gamepad_kernal || "")
    .trim()
    .toLowerCase();
  return normalized === "native" ? "Native" : "Web";
};

const toButtonValue = (value: unknown) => (value ? 1 : 0);

const clampUnit = (value: unknown) => {
  const numeric = Number(value);
  if (!Number.isFinite(numeric) || numeric <= 0) {
    return 0;
  }
  return Math.min(1, numeric);
};

const clampSignedUnit = (value: unknown) => {
  const numeric = Number(value);
  if (!Number.isFinite(numeric)) {
    return 0;
  }
  return Math.max(-1, Math.min(1, numeric));
};

const normalizeNativeGamepadSnapshot = (value: unknown): NativeGamepadTestSnapshot => {
  const snapshot = value as NativeGamepadTestSnapshot | null;
  return {
    started: !!snapshot?.started,
    available: !!snapshot?.available,
    error: snapshot?.error ? String(snapshot.error) : null,
    logs: Array.isArray(snapshot?.logs) ? snapshot.logs.map((item) => String(item)) : [],
    updatedAt: Number(snapshot?.updatedAt) || 0,
    activeDeviceId: snapshot?.activeDeviceId ? String(snapshot.activeDeviceId) : null,
    controllers: Array.isArray(snapshot?.controllers) ? snapshot.controllers : [],
  };
};

const selectNativeController = (snapshot: NativeGamepadTestSnapshot) => {
  return (
    snapshot.controllers.find((controller) => controller.id === snapshot.activeDeviceId) ||
    snapshot.controllers[0] ||
    null
  );
};

const nativeControllerToGamepadState = (
  controller: NativeGamepadTestControllerSnapshot | null
) => {
  if (!controller) {
    return createEmptyNativeGamepadState();
  }

  const buttons = (controller.buttons || {}) as Record<string, any>;
  const axes = (controller.axes || {}) as Record<string, any>;

  return {
    GamepadIndex: 0,
    A: toButtonValue(buttons.a),
    B: toButtonValue(buttons.b),
    X: toButtonValue(buttons.x),
    Y: toButtonValue(buttons.y),
    LeftShoulder: toButtonValue(buttons.leftShoulder),
    RightShoulder: toButtonValue(buttons.rightShoulder),
    LeftTrigger: clampUnit(axes.leftTrigger),
    RightTrigger: clampUnit(axes.rightTrigger),
    View: toButtonValue(buttons.back),
    Menu: toButtonValue(buttons.start),
    LeftThumb: toButtonValue(buttons.leftStick),
    RightThumb: toButtonValue(buttons.rightStick),
    DPadUp: toButtonValue(buttons.dpadUp),
    DPadDown: toButtonValue(buttons.dpadDown),
    DPadLeft: toButtonValue(buttons.dpadLeft),
    DPadRight: toButtonValue(buttons.dpadRight),
    Nexus: toButtonValue(buttons.guide),
    LeftThumbXAxis: clampSignedUnit(axes.leftStickX),
    LeftThumbYAxis: clampSignedUnit(axes.leftStickY),
    RightThumbXAxis: clampSignedUnit(axes.rightStickX),
    RightThumbYAxis: clampSignedUnit(axes.rightStickY),
  };
};

function format(source: string, ...args: string[]): string {
  let n = 0
  return source.replace(/%(s)/, function() {
    return args[n++]
  })
}

function Stream() {
  const router = useRouter();
  const { settings } = useSettings();
  const { t } = useTranslation("cloud");

  const [loading, setLoading] = useState(true);
  const [loadingText, setLoadingText] = useState("");
  const [xPlayer, setxPlayer] = useState<xStreamingPlayer>(undefined);
  const [connectState, setConnectState] = useState("");
  const [sessionId, setSessionId] = useState("");
  const [showPerformance, setShowPerformance] = useState(false);
  const [showConnectError, setShowConnectError] = useState(false);
  const [connectErrorContent, setConnectErrorContent] = useState("");
  const [showWarning, setShowWarning] = useState(false);
  const [showDisplay, setShowDisplay] = useState(false);
  const [showFSRDisplay, setShowFSRDisplay] = useState(false);
  const [showEnhanceDrawer, setShowEnhanceDrawer] = useState(false);
  const [enhanceSettings, setEnhanceSettings] = useState<EnhanceSettings>(() =>
    loadSavedEnhanceSettings()
  );
  const [enhanceTelemetry, setEnhanceTelemetry] = useState<EnhanceTelemetry | null>(null);
  const [isRecording, setIsRecording] = useState(false);
  const [lastRecordingPath, setLastRecordingPath] = useState("");
  const enhanceSettingsRef = useRef<EnhanceSettings>(enhanceSettings);
  const lastExcludeRectsJsonRef = useRef<string>("[]");
  const [showAudio, setShowAudio] = useState(false);
  const [showTextModal, setShowTextModal] = useState(false);
  const [showSystemMessageModal, setShowSystemMessageModal] = useState(false);
  const [systemMessageTitle, setSystemMessageTitle] = useState("");
  const [systemMessageContent, setSystemMessageContent] = useState("");
  const [systemMessageButtons, setSystemMessageButtons] = useState<
    Array<{ label: string; index: number }>
  >([]);
  const [showSystemKeyboardModal, setShowSystemKeyboardModal] = useState(false);
  const [systemKeyboardTitle, setSystemKeyboardTitle] = useState("");
  const [systemKeyboardDescription, setSystemKeyboardDescription] = useState("");
  const [systemKeyboardText, setSystemKeyboardText] = useState("");
  const [systemKeyboardMaxLength, setSystemKeyboardMaxLength] = useState<
    number | undefined
  >(undefined);
  const [systemKeyboardInputScope, setSystemKeyboardInputScope] = useState(0);
  const [showActionbar, setShowActionbar] = useState(false);
  const [volume, setVolume] = useState(1);
  const [openMicro, setOpenMicro] = useState(false);
  const [streamingType, setStreamingType] = useState('');
  const [consoleId, setConsoleId] = useState('');
  const connectStateRef = useRef("");
  const keepaliveInterval = useRef(null);
  const streamStateInterval = useRef(null);
  const systemMessageTransactionRef = useRef<any>(null);
  const systemMessageActionRef = useRef<{
    defaultIndex?: number;
    cancelIndex?: number;
  }>({});
  const systemKeyboardTransactionRef = useRef<any>(null);
  const leavingAfterErrorRef = useRef(false);

  /**
   * Show a blocking error and drop the loading overlay. The stream may not be
   * usable anymore, so the only action offered is to go back.
   */
  const reportConnectError = useCallback((content: string) => {
    setLoading(false);
    setShowWarning(false);
    setConnectErrorContent(content);
    setShowConnectError(true);
  }, []);

  /**
   * Leave the stream page and return to the previous page (console list /
   * cloud games), releasing the failed session on the way out so the user can
   * start a new stream right away.
   */
  const leaveStreamOnError = useCallback(() => {
    if (leavingAfterErrorRef.current) {
      return;
    }
    leavingAfterErrorRef.current = true;

    setShowConnectError(false);
    setConnectErrorContent("");
    setShowWarning(false);
    setLoading(false);

    if (streamStateInterval.current) {
      clearInterval(streamStateInterval.current);
      streamStateInterval.current = null;
    }

    if (keepaliveInterval.current) {
      clearInterval(keepaliveInterval.current);
      keepaliveInterval.current = null;
    }

    if (xPlayer) {
      try {
        xPlayer.close();
      } catch (error) {
        console.log("Failed to close player after connection error:", error);
      }
    }

    if (sessionId) {
      try {
        Ipc.send("streaming", "stopStream", { sessionId }).catch((error) => {
          console.log("Failed to stop stream after connection error:", error);
        });
      } catch (error) {
        console.log("Failed to stop stream after connection error:", error);
      }
    }

    if (enhancedRecorder.isRecording()) {
      void enhancedRecorder.stop().catch(() => undefined);
      setIsRecording(false);
    }
    steamDeckWebglEnhancer.stop();
    void Ipc.send("enhance", "stopEngine").catch(() => undefined);

    router.back();
  }, [router, sessionId, xPlayer]);

  const closeSystemMessageModal = () => {
    setShowSystemMessageModal(false);
    setSystemMessageTitle("");
    setSystemMessageContent("");
    setSystemMessageButtons([]);
    systemMessageTransactionRef.current = null;
    systemMessageActionRef.current = {};
  };

  const completeSystemMessage = (result?: number) => {
    const transaction = systemMessageTransactionRef.current;
    if (
      transaction &&
      transaction.isTransaction &&
      transaction.completion &&
      typeof transaction.completion.complete === "function"
    ) {
      transaction.completion.complete(
        JSON.stringify({
          Result: result,
        })
      );
    }
    closeSystemMessageModal();
  };

  const cancelSystemMessage = () => {
    const transaction = systemMessageTransactionRef.current;
    const { cancelIndex, defaultIndex } = systemMessageActionRef.current || {};
    const fallbackIndex =
      typeof cancelIndex === "number"
        ? cancelIndex
        : typeof defaultIndex === "number"
        ? defaultIndex
        : undefined;

    if (typeof fallbackIndex === "number") {
      completeSystemMessage(fallbackIndex);
      return;
    }

    if (
      transaction &&
      transaction.isTransaction &&
      transaction.completion &&
      typeof transaction.completion.cancel === "function"
    ) {
      transaction.completion.cancel();
    }
    closeSystemMessageModal();
  };

  const closeSystemKeyboardModal = useCallback(() => {
    if (xPlayer) {
      xPlayer.setKeyboardInput(true);
    }
    setShowSystemKeyboardModal(false);
    setSystemKeyboardTitle("");
    setSystemKeyboardDescription("");
    setSystemKeyboardText("");
    setSystemKeyboardMaxLength(undefined);
    setSystemKeyboardInputScope(0);
    systemKeyboardTransactionRef.current = null;
  }, [xPlayer]);

  const completeSystemKeyboard = () => {
    const transaction = systemKeyboardTransactionRef.current;
    if (
      transaction &&
      transaction.isTransaction &&
      transaction.completion &&
      typeof transaction.completion.complete === "function"
    ) {
      transaction.completion.complete(
        JSON.stringify({
          Text: systemKeyboardText,
        })
      );
    }
    closeSystemKeyboardModal();
  };

  const cancelSystemKeyboard = () => {
    const transaction = systemKeyboardTransactionRef.current;
    if (
      transaction &&
      transaction.isTransaction &&
      transaction.completion &&
      typeof transaction.completion.cancel === "function"
    ) {
      transaction.completion.cancel();
    }
    closeSystemKeyboardModal();
  };

  const resolveSystemKeyboardProps = () => {
    let type:
      | "text"
      | "url"
      | "email"
      | "password"
      | "tel"
      | "search" = "text";
    let inputMode:
      | "text"
      | "url"
      | "email"
      | "numeric"
      | "tel"
      | "search" = "text";

    switch (systemKeyboardInputScope) {
      case 1:
        type = "url";
        inputMode = "url";
        break;
      case 5:
        type = "email";
        inputMode = "email";
        break;
      case 29:
        type = "text";
        inputMode = "numeric";
        break;
      case 31:
        type = "password";
        inputMode = "text";
        break;
      case 32:
        type = "tel";
        inputMode = "tel";
        break;
      case 50:
        type = "search";
        inputMode = "search";
        break;
      default:
        type = "text";
        inputMode = "text";
        break;
    }

    return {
      type,
      inputMode,
    };
  };

  const handleSystemUiEvent = useCallback((event) => {
    if (!event || !event.target) {
      return false;
    }

    if (event.target === SYSTEM_UI_TARGET_SHOW_MESSAGE_DIALOG) {
      const payload = event.payload || {};
      const toValidIndex = (value: any) => {
        if (typeof value === "number" && Number.isFinite(value)) {
          return value;
        }
        if (typeof value === "string" && value.trim() !== "") {
          const parsed = Number(value);
          if (Number.isFinite(parsed)) {
            return parsed;
          }
        }
        return undefined;
      };

      const parsedButtons: Array<{ label: string; index: number }> = [];
      const pushButton = (label: any, index: number) => {
        if (typeof label !== "string") {
          return;
        }
        const nextLabel = label.trim();
        if (!nextLabel) {
          return;
        }
        parsedButtons.push({
          label: nextLabel,
          index,
        });
      };

      // Align with Android mapping first.
      pushButton(payload.CommandLabel1, 0);
      pushButton(payload.CommandLabel2, 1);
      pushButton(payload.CommandLabel3, 2);
      pushButton(payload.CommandLabel4, 3);

      // Compatibility fallback for other payload shapes.
      if (!parsedButtons.length && Array.isArray(payload.Commands)) {
        payload.Commands.forEach((command: any, idx: number) => {
          if (typeof command === "string") {
            pushButton(command, idx);
            return;
          }
          const label =
            command?.Label ?? command?.label ?? command?.Text ?? command?.text;
          const commandIndex =
            toValidIndex(command?.Result) ??
            toValidIndex(command?.result) ??
            toValidIndex(command?.Index) ??
            toValidIndex(command?.index) ??
            idx;
          pushButton(label, commandIndex);
        });
      }

      setSystemMessageTitle(payload.TitleText || "");
      setSystemMessageContent(payload.ContentText || "");
      setSystemMessageButtons(parsedButtons);
      systemMessageActionRef.current = {
        defaultIndex: toValidIndex(payload.DefaultIndex),
        cancelIndex: toValidIndex(payload.CancelIndex),
      };
      systemMessageTransactionRef.current = {
        id: event.id,
        isTransaction: event.isTransaction,
        completion: event.completion,
      };
      setShowSystemMessageModal(true);
      return true;
    }

    if (event.target === SYSTEM_UI_TARGET_SHOW_VIRTUAL_KEYBOARD) {
      const payload = event.payload || {};
      if (xPlayer) {
        xPlayer.setKeyboardInput(false);
      }
      setSystemKeyboardTitle(payload.TitleText || "");
      setSystemKeyboardDescription(payload.DescriptionText || "");
      setSystemKeyboardText(payload.DefaultText || "");
      setSystemKeyboardInputScope(payload.InputScope ?? 0);
      setSystemKeyboardMaxLength(
        typeof payload.MaxLength === "number" && payload.MaxLength > 0
          ? payload.MaxLength
          : undefined
      );
      systemKeyboardTransactionRef.current = {
        id: event.id,
        isTransaction: event.isTransaction,
        completion: event.completion,
      };
      if (
        event.isTransaction &&
        event.completion &&
        typeof event.completion.setOnRemoteCancellation === "function"
      ) {
        const transactionId = event.id;
        event.completion.setOnRemoteCancellation(() => {
          if (systemKeyboardTransactionRef.current?.id === transactionId) {
            closeSystemKeyboardModal();
          }
        });
      }
      setShowSystemKeyboardModal(true);
      return true;
    }

    return false;
  }, [closeSystemKeyboardModal, xPlayer]);

  const handleStreamingMessage = useCallback((event) => {
    if (!event || typeof event.target !== "string") {
      return false;
    }

    if (!event.target.startsWith(STREAMING_TOUCHCONTROLS_SCOPE)) {
      return false;
    }

    if (
      event.isTransaction &&
      event.completion &&
      typeof event.completion.cancel === "function"
    ) {
      event.completion.cancel();
    }

    return true;
  }, []);

  useEffect(() => {
    const gamepadKernel = resolveGamepadKernel({
      gamepad_kernel: settings?.gamepad_kernel,
      gamepad_kernal: settings?.gamepad_kernal,
      coop: settings?.coop,
    });

    if (gamepadKernel !== "Native") {
      window.gpState = undefined;
      return;
    }

    let active = true;
    const pollingRate = Number(settings?.polling_rate) || 500;
    // 解除原版 16ms (60Hz) 下限锁死，支持最高 500Hz (2ms) 原生手柄极低延迟轮询
    const pollingIntervalMs = Math.max(2, Math.round(1000 / pollingRate));

    window.gpState = createEmptyNativeGamepadState();

    const refreshNativeGamepadState = async (action: "start" | "refresh") => {
      try {
        const result =
          action === "start"
            ? await Ipc.send("app", "startNativeGamepadTestSession")
            : await Ipc.send("app", "getNativeGamepadTestSnapshot");

        if (!active) {
          return;
        }

        const snapshot = normalizeNativeGamepadSnapshot(result);
        window.gpState = nativeControllerToGamepadState(selectNativeController(snapshot));
      } catch {
        if (active) {
          window.gpState = createEmptyNativeGamepadState();
        }
      }
    };

    void refreshNativeGamepadState("start");
    const pollTimer = setInterval(() => {
      void refreshNativeGamepadState("refresh");
    }, pollingIntervalMs);

    return () => {
      active = false;
      if (pollTimer) {
        clearInterval(pollTimer);
      }
      window.gpState = createEmptyNativeGamepadState();
      void Ipc.send("app", "stopNativeGamepadTestSession").catch(() => undefined);
    };
  }, [settings?.coop, settings?.gamepad_kernel, settings?.gamepad_kernal, settings?.polling_rate]);

  // 同步并热更新内置 DLSS 5 + 低延迟 X2 插帧 + 锐化 + Wi-Fi 6 抖动缓冲 + 音画解绑 + 手柄瞬发参数
  const handleEnhanceSettingsChange = useCallback((next: EnhanceSettings) => {
    enhanceSettingsRef.current = next;
    setEnhanceSettings(next);
    saveEnhanceSettings(next);
    applyWifi6LowLatencyMode(next.wifi6JitterMode);
    setAvSyncUnbind(next.avSyncUnbind);
    setTurboGamepadEnabled(next.turboGamepad);
    steamDeckWebglEnhancer.updateSettings(next);
    // 录制期间若用户打开 F8 实时调参，强制保持 obsCaptureCompat=true，避免覆盖层切回防捕获锁死导致录屏丢失增强层
    const enginePatch = enhancedRecorder.isRecording()
      ? { ...next, obsCaptureCompat: true }
      : next;
    void Ipc.send("enhance", "updateConfig", enginePatch).catch(() => undefined);

    // 若开启原生增强引擎则移除浏览器慢速 SVG 卷积滤镜，确保零拷贝硬件直通最低延迟
    const videoStyle = document.getElementById("video-css");
    if (videoStyle && next.enabled) {
      videoStyle.textContent = "";
    }
  }, []);

  // 一键启动 / 停止 60FPS 增强画面 + Xbox 游戏原声硬件编码录屏 (F9)
  const handleToggleRecording = useCallback(async () => {
    try {
      if (enhancedRecorder.isRecording()) {
        const res = await enhancedRecorder.stop();
        setIsRecording(false);
        // 停止录制后自动恢复日常 DXGI 零遮挡满帧插帧捕获模式
        void Ipc.send("enhance", "updateConfig", {
          obsCaptureCompat: !!enhanceSettingsRef.current.obsCaptureCompat,
        }).catch(() => undefined);
        if (res.filePath) {
          setLastRecordingPath(res.filePath);
        }
        addToast({
          title: `✅ 60FPS 增强录屏已保存 (${res.durationSec}s · ${res.sizeMB}MB)`,
          description: res.filePath || "已保存至 E:\\XStreaming_Recordings",
          color: "success",
        });
        return;
      }

      if (connectStateRef.current !== "connected") {
        addToast({
          title: "请等待串流画面连接成功后再开始录屏",
          color: "warning",
        });
        return;
      }

      setShowEnhanceDrawer(false);
      setShowActionbar(false);
      // 录屏期间自动无缝开启 WGC + Alpha 254 开放录屏覆盖层，确保录下完整增强画面
      await Ipc.send("enhance", "updateConfig", { obsCaptureCompat: true }).catch(
        () => undefined
      );
      await new Promise((r) => setTimeout(r, 120));
      const started = await enhancedRecorder.start();
      setIsRecording(true);
      setLastRecordingPath(started.filePath);
      addToast({
        title: "🔴 已开始录制 60FPS 增强画面 + 游戏原声",
        description: `按 F9 停止并保存至 ${started.dirPath}`,
        color: "danger",
      });
    } catch (err) {
      setIsRecording(false);
      void Ipc.send("enhance", "updateConfig", {
        obsCaptureCompat: !!enhanceSettingsRef.current.obsCaptureCompat,
      }).catch(() => undefined);
      addToast({
        title: "录屏启动失败",
        description: String(err),
        color: "danger",
      });
    }
  }, []);

  // 挂载 WebRTC Wi-Fi 6 低延迟钩子、监听原生引擎遥测、快捷键 (F6/F7/F8) 与 UI 镂空避让
  useEffect(() => {
    installWebRtcLowLatencyHook(
      enhanceSettingsRef.current.wifi6JitterMode,
      enhanceSettingsRef.current.avSyncUnbind,
      enhanceSettingsRef.current.turboGamepad
    );

    const listener = Ipc.onAction("enhance", "telemetry", (data: any) => {
      setEnhanceTelemetry((prev) => ({
        ...(prev || ({} as EnhanceTelemetry)),
        ...data,
      }));
    });

    const jitterTimer = setInterval(async () => {
      if (connectStateRef.current !== "connected") return;
      const jitMs = await queryWebRtcJitterBufferMs();
      if (jitMs !== undefined) {
        setEnhanceTelemetry((prev) =>
          prev ? { ...prev, webrtcJitterMs: jitMs } : null
        );
      }
    }, 1000);

    // 动态收集页面上可见的弹窗、抽屉、顶部菜单和监控条坐标，通知 C++ 覆盖层精准镂空，实现零 UI 污染实时调参
    const syncUiCutouts = () => {
      if (connectStateRef.current !== "connected") return;
      const dpr = window.devicePixelRatio || 1;
      const selectors = [
        "#enhance-control-drawer",
        "#actionBar",
        '[role="dialog"]',
        "#performances",
        "#performances-x",
        "[data-sonner-toaster]",
      ];
      const rects: Array<{ x: number; y: number; w: number; h: number }> = [];
      selectors.forEach((sel) => {
        document.querySelectorAll(sel).forEach((el) => {
          const htmlEl = el as HTMLElement;
          if (!htmlEl) return;
          const st = window.getComputedStyle(htmlEl);
          if (st.display === "none" || st.visibility === "hidden" || Number(st.opacity) === 0) {
            return;
          }
          const r = htmlEl.getBoundingClientRect();
          if (r.width > 4 && r.height > 4) {
            const pad = 6;
            rects.push({
              x: Math.max(0, Math.floor((r.left - pad) * dpr)),
              y: Math.max(0, Math.floor((r.top - pad) * dpr)),
              w: Math.ceil((r.width + pad * 2) * dpr),
              h: Math.ceil((r.height + pad * 2) * dpr),
            });
          }
        });
      });
      const json = JSON.stringify(rects);
      if (json !== lastExcludeRectsJsonRef.current) {
        lastExcludeRectsJsonRef.current = json;
        void Ipc.send("enhance", "updateExcludeRects", { rects }).catch(() => undefined);
      }
    };
    const cutoutTimer = setInterval(syncUiCutouts, 60);

    const toggleEnhanceCompare = () => {
      const toggled = !enhanceSettingsRef.current.enabled;
      const next = { ...enhanceSettingsRef.current, enabled: toggled };
      handleEnhanceSettingsChange(next);
      addToast({
        title: toggled ? "🚀 光影精雕 + 真 X2 增强已开启" : "👁️ 已切换至原画直出对比",
        color: toggled ? "success" : "warning",
      });
    };

    const onHotkey = (e: KeyboardEvent) => {
      if (e.key === "F8") {
        e.preventDefault();
        setShowEnhanceDrawer((prev) => !prev);
      } else if (e.key === "F6") {
        e.preventDefault();
        toggleEnhanceCompare();
      } else if (e.key === "F7") {
        e.preventDefault();
        setShowPerformance((prev) => !prev);
      } else if (e.key === "F9") {
        e.preventDefault();
        void handleToggleRecording();
      }
    };
    window.addEventListener("keydown", onHotkey);

    // Steam Deck / Windows 掌机无物理 F 键手柄组合快捷键：
    // 按住左视图键 View (⧉, Button 8) + R3(11)=F8调参 / L3(10)=F7监控 / Y(3)=F6对比 / X(2)=F9录屏
    let prevChord = "";
    const padChordTimer = setInterval(() => {
      if (connectStateRef.current !== "connected") return;
      let viewPressed = false;
      let r3Pressed = false;
      let l3Pressed = false;
      let yPressed = false;
      let xPressed = false;

      const gpNative: any = (window as any).gpState;
      if (gpNative) {
        viewPressed = !!gpNative.View;
        r3Pressed = !!gpNative.RightThumb;
        l3Pressed = !!gpNative.LeftThumb;
        yPressed = !!gpNative.Y;
        xPressed = !!gpNative.X;
      }
      if (!viewPressed && typeof navigator !== "undefined" && navigator.getGamepads) {
        const pads = navigator.getGamepads();
        for (let i = 0; i < pads.length; i++) {
          const p = pads[i];
          if (p && p.connected && p.buttons && p.buttons.length >= 12) {
            if (p.buttons[8]?.pressed) {
              viewPressed = true;
              r3Pressed = !!p.buttons[11]?.pressed;
              l3Pressed = !!p.buttons[10]?.pressed;
              yPressed = !!p.buttons[3]?.pressed;
              xPressed = !!p.buttons[2]?.pressed;
              break;
            }
          }
        }
      }

      const activeChord = viewPressed
        ? r3Pressed
          ? "F8"
          : l3Pressed
          ? "F7"
          : yPressed
          ? "F6"
          : xPressed
          ? "F9"
          : ""
        : "";

      if (activeChord && activeChord !== prevChord) {
        if (activeChord === "F8") setShowEnhanceDrawer((prev) => !prev);
        else if (activeChord === "F7") setShowPerformance((prev) => !prev);
        else if (activeChord === "F6") toggleEnhanceCompare();
        else if (activeChord === "F9") void handleToggleRecording();
      }
      prevChord = activeChord;
    }, 45);

    return () => {
      clearInterval(jitterTimer);
      clearInterval(cutoutTimer);
      clearInterval(padChordTimer);
      window.removeEventListener("keydown", onHotkey);
      Ipc.removeListener("enhance", listener);
      if (enhancedRecorder.isRecording()) {
        void enhancedRecorder.stop().catch(() => undefined);
      }
      steamDeckWebglEnhancer.stop();
      void Ipc.send("enhance", "stopEngine").catch(() => undefined);
    };
  }, [handleEnhanceSettingsChange, handleToggleRecording]);

  useEffect(() => {
    let streamType = "home";
    let serverId = router.query.serverid as string;

    const localFontSize = localStorage.getItem('fontSize');
    if (localFontSize && localFontSize !== '16') {
      document.documentElement.style.fontSize = localFontSize + 'px';
    }

    if (serverId.startsWith(XCLOUD_PREFIX)) {
      streamType = "cloud";
      serverId = serverId.split("_")[1];
    }

    setConsoleId(serverId)

    setStreamingType(streamType)

    let lastMovement = 0;
    const mouseEvent = () => {
      lastMovement = Date.now();
    };
    window.addEventListener("mousemove", mouseEvent);
    window.addEventListener("mousedown", mouseEvent);

    window.addEventListener("touchstart", mouseEvent);
    window.addEventListener("touchmove", mouseEvent);

    const escEvent = (event) => {
      if (event.key === 'Escape') {
        Ipc.send('app', 'exitFullscreen')
      }
    }
    window.addEventListener('keydown', escEvent)

    const mouseInterval = setInterval(() => {
      if (Date.now() - lastMovement >= 5000) {
        setShowActionbar(false)
      } else {
        setShowActionbar(true)
      }
    }, 100);

    let detachTurboInput: (() => void) | undefined;

    if (xPlayer !== undefined) {
      const server_url: any = router.query.server_url;
      const server_username: any = router.query.server_username;
      const server_credential: any = router.query.server_credential;
      if (server_url && server_username && server_credential) {
        // Init with TURN server
        xPlayer.bind({
          turnServer: {
            url: server_url,
            username: server_username,
            credential: server_credential
          }
        });
      } else {
        xPlayer.bind();
      }
      detachTurboInput = attachTurboInputToPlayer(xPlayer);

      console.log("streamType:", streamType);
      console.log("serverId:", serverId);
      console.log("settings:", settings);
      console.log("sessionId:", sessionId);

      xPlayer.setVideoFormat(settings.video_format || "");

      // Set video codec profiles
      // xPlayer.setCodecPreferences('video/H264', { profiles: ['4d'] }) // 4d = high, 42e = mid, 420 = low
      if (settings.codec) {
        if (settings.codec.indexOf("H264") > -1) {
          const codecArr = settings.codec.split("-");
          xPlayer.setCodecPreferences(codecArr[0], {
            profiles: codecArr[1] ? [codecArr[1]] : [],
          });
        } else {
          xPlayer.setCodecPreferences(settings.codec, { profiles: [] });
        }
      }

      // Set gamepad kernal
      const gamepadKernel = resolveGamepadKernel(settings);
      xPlayer.setGamepadKernal(gamepadKernel);

      // Set gamepad mix
      xPlayer.setGamepadMix(gamepadKernel === "Web" && settings.gamepad_mix)

      // Set gamepad index
      xPlayer.setGamepadIndex(settings.gamepad_index);

      // Set vibration
      xPlayer.setVibration(settings.vibration);
      xPlayer.setVibrationMode(gamepadKernel === "Native" ? "Native" : "Webview");
      (xPlayer as any).setSwapTriggerRumble?.(!!settings.swap_trigger_rumble);

      // Set deadzone
      xPlayer.setGamepadDeadZone(settings.dead_zone);

      // Set fsr sharpness
      const _fsrDisplayOptions = window.localStorage.getItem(FSR_DISPLAY_KEY);
      let fsrDisplayOptions = FSR_DEFAULT_OPTIONS

      if (_fsrDisplayOptions) {
        try {
          fsrDisplayOptions = JSON.parse(_fsrDisplayOptions);
        } catch {
          fsrDisplayOptions = DEFAULT_OPTIONS;
        }
      }
      xPlayer.setFsrSharpness(fsrDisplayOptions.sharpness);

      // Set gamepad maping
      if (settings.gamepad_maping) {
        xPlayer.setGamepadMaping(settings.gamepad_maping)
      }

      if (settings.force_trigger_rumble) {
        xPlayer.setForceTriggerRumble(settings.force_trigger_rumble)
      }

      // Set bitrate
      if (streamType === "cloud") {
        if (
          settings.xcloud_bitrate_mode === "Custom" &&
          settings.xcloud_bitrate !== 0
        ) {
          console.log(
            "setVideoBitrate xcloud:",
            settings.xcloud_bitrate + "Mb/s"
          );
          xPlayer.setVideoBitrate(settings.xcloud_bitrate);
        }
      } else {
        if (
          settings.xhome_bitrate_mode === "Custom" &&
          settings.xhome_bitrate !== 0
        ) {
          console.log(
            "setVideoBitrate xhome:",
            settings.xhome_bitrate + "Mb/s"
          );
          xPlayer.setVideoBitrate(settings.xhome_bitrate);
        }
      }

      // Set audio bitrate
      if (
        settings.audio_bitrate_mode === "Custom" &&
        settings.audio_bitrate !== 0
      ) {
        console.log(
          "setAudioBitrate:",
          settings.audio_bitrate + "Mb/s"
        );
        xPlayer.setAudioBitrate(settings.audio_bitrate);
      }

      // Set audio control
      if (settings.enable_audio_control) {
        xPlayer.setAudioControl(true)
      }

      // Set audio rumble
      if (settings.enable_audio_rumble) {
        xPlayer.setAudioRumble(settings.enable_audio_rumble, settings.audio_rumble_threshold)
      }

      // Set polling rate
      if (settings.polling_rate) {
        xPlayer.setPollRate(settings.polling_rate)
      }

      // Set co-op mode
      if (settings.coop) {
        xPlayer.setCoOpMode(true)
      }

      xPlayer.setConnectFailHandler(() => {
        // Not connected
        reportConnectError(
          t(
            "NAT failed. If you are trying to stream remotely, please ensure that you have a public IPV4/6 address and that your router has port forwarding enabled for ports 9002/3074."
          )
        );
      });

      xPlayer.setSdpHandler((client, offer) => {
        Ipc.send("streaming", "sendChatSdp", {
          sessionId: sessionId,
          sdp: offer.sdp,
        })
          .then((sdpResponse) => {
            xPlayer.setRemoteOffer(sdpResponse.sdp);
          })
          .catch((error) => {
            console.log("ChatSDP Exchange error:", error);
            if (connectStateRef.current === "connected") {
              // The stream is already running, a chat renegotiation failure
              // should not tear it down.
              alert("ChatSDP Exchange error:" + JSON.stringify(error));
            } else {
              reportConnectError(
                "ChatSDP Exchange error: " + JSON.stringify(error)
              );
            }
          });
      });
      xPlayer.setSystemUiHandler(handleSystemUiEvent);
      xPlayer.setMessageHandler(handleStreamingMessage);

      xPlayer.createOffer().then((offer: any) => {
        console.log("offer:", offer);
        setLoadingText(t("Configuration obtained successfully, initiating offer..."));
        Ipc.send("streaming", "sendSdp", {
          sessionId: sessionId,
          sdp: offer.sdp,
        })
          .then((sdpResult: any) => {
            setLoadingText(t("Remote offer retrieved successfully..."));
            console.log("sdpResult:", sdpResult);
            xPlayer.setRemoteOffer(sdpResult.sdp);

            // Gather candidates
            const iceCandidates = xPlayer.getIceCandidates();
            const candidates = [];
            for (const candidate in iceCandidates) {
              candidates.push({
                candidate: iceCandidates[candidate].candidate,
                sdpMLineIndex: iceCandidates[candidate].sdpMLineIndex,
                sdpMid: iceCandidates[candidate].sdpMid,
              });
            }

            setLoadingText(t("Ready to send ICE..."));
            Ipc.send("streaming", "sendIce", {
              sessionId: sessionId,
              ice: candidates,
            })
              .then((iceResult: any) => {
                setLoadingText(t("Exchange ICE successfully..."));
                console.log("iceResult:", iceResult);

                xPlayer.setIceCandidates(iceResult);

                // All done. Waiting for the event 'connectionstate' to be triggered
              })
              .catch((error) => {
                console.log("ICE Exchange error:", error);
                // alert("ICE Exchange error:" + JSON.stringify(error));
              });
          })
          .catch((error) => {
            console.log("SDP Exchange error:", error);
            reportConnectError("SDP Exchange error: " + JSON.stringify(error));
          });
      })
        .catch((error) => {
          console.log("Create offer error:", error);
          reportConnectError(
            "Failed to create offer. Error details:\n" + JSON.stringify(error)
          );
        });

      xPlayer.getEventBus().on("connectionstate", (event) => {
        console.log("connectionstate changed:", event);

        // Toggle microphone
        if (connectStateRef.current === "connected" && (event.state === "new" || event.state === "connecting")) {
          return;
        }

        setConnectState(event.state);
        connectStateRef.current = event.state;

        if (event.state === "connected") {
          setLoadingText(t("Connected"));
          applyWifi6LowLatencyMode(enhanceSettingsRef.current.wifi6JitterMode);

          // 连接成功后自动启动增强管线：Windows 启动 C++ D3D11/D3D12 原生引擎，SteamOS (Steam Deck) 自动无缝启用 WebGL2 RDNA2 硬件加速管线！
          if (!settings.fsr) {
            setTimeout(() => {
              void Ipc.send("enhance", "startEngine", enhanceSettingsRef.current)
                .then((res: any) => {
                  if (res?.useWebglFallback || res?.supported === false) {
                    steamDeckWebglEnhancer.start(
                      enhanceSettingsRef.current,
                      (tel) => {
                        setEnhanceTelemetry((prev) => ({
                          ...(prev || ({} as EnhanceTelemetry)),
                          ...tel,
                        }));
                      }
                    );
                  }
                })
                .catch(() => {
                  steamDeckWebglEnhancer.start(
                    enhanceSettingsRef.current,
                    (tel) => {
                      setEnhanceTelemetry((prev) => ({
                        ...(prev || ({} as EnhanceTelemetry)),
                        ...tel,
                      }));
                    }
                  );
                });
            }, 600);
          }

          if (settings.fsr) {
            setTimeout(() => {
              xPlayer && xPlayer.startFSR(() => {
                addToast({
                    title: t('FSR started'),
                    color: 'success'
                  });
              });

              const videoHolder = document.getElementById('videoHolder');
              const canvasContainer = document.getElementById('canvas-container');
              videoHolder.style.visibility = 'hidden';
              canvasContainer.style.visibility = 'visible';
            }, 3000)
          }

          setTimeout(() => {
            setLoading(false);

            if (settings.video_format && settings.video_format.indexOf(':') > -1) {
              resizePlayer(settings.video_format)
            }

            // Start keepalive loop
            if (!keepaliveInterval.current) {
              keepaliveInterval.current = setInterval(() => {
                console.log("sendKeepalive sessionId:", sessionId);
                Ipc.send("streaming", "sendKeepalive", {
                  sessionId,
                })
                  .then((result) => {
                    console.log("StartStream keepalive:", result);
                  })
                  .catch((error) => {
                    console.error(
                      "Failed to send keepalive. Error details:\n" +
                        JSON.stringify(error)
                    );
                  });
              }, 30 * 1000);
            }

            // Refresh video player (若已开启内置原生 DLSS 5 引擎则跳过慢速 CSS SVG 滤镜以降低延迟)
            if (!settings.fsr && !enhanceSettingsRef.current.enabled) {
              setTimeout(() => {
                const _displayOptions = window.localStorage.getItem(DISPLAY_KEY);

                let displayOptions: any = DEFAULT_OPTIONS;
                if (_displayOptions) {
                  try {
                    displayOptions = JSON.parse(_displayOptions);
                  } catch {
                    displayOptions = DEFAULT_OPTIONS;
                  }
                }

                const videoStyle = document.getElementById("video-css");
                console.log('Refresh video player:', displayOptions)
                const filters = getVideoPlayerFilterStyle(displayOptions);
                let videoCss = "";
                if (filters) {
                  videoCss += `filter: ${filters} !important;`;
                }
                let css = "";
                if (videoCss) {
                  css = `#videoHolder video { ${videoCss} }`;
                }

                videoStyle!.textContent = css;
              }, 1000);
            }

            // const xboxTitleId = window._xboxTitleId || ''
            // // inputConfigs
            // Ipc.send("streaming", "inputConfigs", {
            //   xboxTitleId,
            // })
          }, 500);
        } else if (event.state === "closed") {
          console.log(":: We are disconnected!");
        }
      });
    } else if (sessionId === "") {
      setLoadingText(t("Connecting..."));

      const gameNameParam = router.query.gameName
        ? decodeURIComponent(router.query.gameName as string)
        : '';
      const gameImageUrlParam = router.query.gameImageUrl
        ? decodeURIComponent(router.query.gameImageUrl as string)
        : '';

      Ipc.send("streaming", "startStream", {
        type: streamType,
        target: serverId,
        gameName: gameNameParam,
        gameImageUrl: gameImageUrlParam,
      })
        .then((result: string) => {
          console.log("StartStream sessionId:", result);
          setSessionId(result);
        })
        .catch((error) => {
          reportConnectError(
            "Failed to start new stream. Error details:\n" +
              JSON.stringify(error)
          );
        });
    } else {
      if (!streamStateInterval.current) {
        streamStateInterval.current = setInterval(() => {
          Ipc.send("streaming", "getPlayerState", {
            sessionId: sessionId,
          })
            .then((session: {
              errorDetails?: any
              id: string,
              target: string,
              path: string,
              type: string,
              playerState: "pending"|"started"|"failed"|"queued",
              state: string,
              waitingTimes: {
                  estimatedProvisioningTimeInSeconds: number,
                  estimatedAllocationTimeInSeconds: number,
                  estimatedTotalWaitTimeInSeconds: number
              }
          }) => {
              console.log("Player state:", session);
              const formatQueueData = () => {
                const currentDate = new Date()
                currentDate.setSeconds(session.waitingTimes.estimatedTotalWaitTimeInSeconds)
                return format(t("YouConnect", {defaultValue: "You connect in %s"}), moment(currentDate.toISOString()).local().format(t('dateFormat', {defaultValue: 'YYYY/MM/DD HH:mm'})))
              }

              switch (session.playerState) {
                case "pending":
                  // Waiting for console to start
                  break;

                case "started":
                  // Console is ready
                  clearInterval(streamStateInterval.current);

                  // Start xPlayer interface
                  setxPlayer(
                    new xStreamingPlayer("videoHolder", {
                      input_touch: false, // 支持原生触摸, PC端禁用
                      ui_touchenabled: false,
                      input_mousekeyboard: settings.enable_native_mouse_keyboard, // 支持原生键鼠操作
                      input_legacykeyboard: true, // Keep keyboard input
                      // @ts-ignore
                      input_mousekeyboard_config: settings.input_mousekeyboard_maping
                    })
                  );
                  break;

                case "failed": {
                  // Error
                  clearInterval(streamStateInterval.current);
                  streamStateInterval.current = null;

                  const errorMessage = session.errorDetails?.message || "";
                  let errorContent = "";

                  if (errorMessage.includes("WaitingForServerToRegister")) {
                    // Detected the "WaitingForServerToRegister" error. This means the console is not connected to the xbox servers
                    errorContent = t("WaitingForServerToRegister") + "\n";
                  } else if (errorMessage.includes("xboxstreaminghelper.cpp")) {
                    errorContent = t("XboxstreaminghelperErr") + "\n";
                  }

                  errorContent +=
                    "Stream error result: " +
                    session.state +
                    "\nDetails: [" +
                    session.errorDetails?.code +
                    "] " +
                    errorMessage;

                  console.log("Full stream error:", session.errorDetails);
                  reportConnectError(errorContent);
                  break;
                }

                case "queued":
                  setLoadingText(formatQueueData())
                  break;
              }
            })
            .catch((error) => {
              if (streamStateInterval.current) {
                clearInterval(streamStateInterval.current);
                streamStateInterval.current = null;
              }
              reportConnectError(
                "Failed to get player state. Error details:\n" +
                  JSON.stringify(error)
              );
            });
        }, 1000);
      }
    }

    return () => {
      if (detachTurboInput) {
        detachTurboInput();
      }
      if (xPlayer !== undefined) {
        xPlayer.close();
        setxPlayer(null);
      }

      if (keepaliveInterval.current) {
        clearInterval(keepaliveInterval.current);
      }

      if (streamStateInterval.current) {
        clearInterval(streamStateInterval.current);
      }

      if (mouseInterval) clearInterval(mouseInterval);

      window.removeEventListener("mousemove", mouseEvent);
      window.removeEventListener("mousedown", mouseEvent);
      window.removeEventListener('keydown', escEvent)
    };
  }, [
    xPlayer,
    sessionId,
    t,
    router.query,
    settings,
    handleSystemUiEvent,
    handleStreamingMessage,
    reportConnectError,
  ]);

  const getVideoPlayerFilterStyle = (options) => {
    const filters = [];
    const usmMatrix = document.getElementById("filter-usm-matrix");

    const sharpness = options.sharpness || 0; // sharpness
    if (sharpness !== 0) {
      const level = (7 - (sharpness / 2 - 1) * 0.5).toFixed(1); // 5, 5.5, 6, 6.5, 7
      const matrix = `0 -1 0 -1 ${level} -1 0 -1 0`;
      usmMatrix.setAttributeNS(null, "kernelMatrix", matrix);
      filters.push(`url(#filter-usm)`);
    }

    const saturation = options.saturation || 100; // saturation
    if (saturation != 100) {
      filters.push(`saturate(${saturation}%)`);
    }

    const contrast = options.contrast || 100; // contrast
    if (contrast !== 100) {
      filters.push(`contrast(${contrast}%)`);
    }

    const brightness = options.brightness || 100; // brightness
    if (brightness !== 100) {
      filters.push(`brightness(${brightness}%)`);
    }

    return filters.join(" ");
  };

  const refreshPlayer = (options) => {
    const videoStyle = document.getElementById("video-css");
    const filters = getVideoPlayerFilterStyle(options);
    let videoCss = "";
    if (filters) {
      videoCss += `filter: ${filters} !important;`;
    }
    let css = "";
    if (videoCss) {
      css = `#videoHolder video { ${videoCss} }`;
    }

    videoStyle!.textContent = css;
  };

  const resizePlayer = (prefRatio) => {
    let targetWidth = '';
    let targetHeight = '';
    let targetObjectFit = '';
    const tmp = prefRatio.split(':');
    const videoRatio = parseFloat(tmp[0]) / parseFloat(tmp[1]);

    let width = 0;
    let height = 0;

    const $video = document.getElementsByTagName('video')[0];

    // Get window's ratio
    const winWidth = document.documentElement.clientWidth;
    const winHeight = document.documentElement.clientHeight;
    const parentRatio = winWidth / winHeight;

    // Get target width & height
    if (parentRatio > videoRatio) {
      height = winHeight;
      width = height * videoRatio;
    } else {
      width = winWidth;
      height = width / videoRatio;
    }

    // Prevent floating points
    width = Math.ceil(Math.min(winWidth, width));
    height = Math.ceil(Math.min(winHeight, height));

    $video.dataset.width = width.toString();
    $video.dataset.height = height.toString();

    targetWidth = `${width}px`;
    targetHeight = `${height}px`;
    targetObjectFit = prefRatio === '16:9' ? 'contain' : 'fill';

    $video.style.width = targetWidth;
    $video.style.height = targetHeight;
    $video.style.objectFit = targetObjectFit;
  }

  const onDisconnectPowerOff = () => {
    xPlayer && xPlayer.close();
    setLoading(true);
    setLoadingText(t("Disconnecting..."));
    Ipc.send("consoles", "powerOff", consoleId).then(res => {
      console.log('poweroff result:', res)
      onDisconnect();
    }).catch(() => {
      onDisconnect();
    });
  }

  const handleSendText = (text: string) => {
    Ipc.send("consoles", "sendText", {
      consoleId,
      text
    });
  }

  const onDisconnect = () => {
    setLoading(true);
    setShowPerformance(false);
    setShowEnhanceDrawer(false);
    if (enhancedRecorder.isRecording()) {
      void enhancedRecorder.stop().catch(() => undefined);
      setIsRecording(false);
    }
    steamDeckWebglEnhancer.stop();
    void Ipc.send("enhance", "stopEngine").catch(() => undefined);
    setLoadingText(t("Disconnecting..."));
    xPlayer && xPlayer.close();

    const videoHolder = document.getElementById("videoHolder");
    const canvasContainer = document.getElementById("canvas-container");
    videoHolder.style.visibility = 'visible';
    canvasContainer.style.visibility = 'hidden';

    if (streamStateInterval.current) {
      clearInterval(streamStateInterval.current);
    }

    if (keepaliveInterval.current) {
      clearInterval(keepaliveInterval.current);
    }

    setTimeout(() => {
      console.log("stopStream:", sessionId);
      const timer = setTimeout(() => {
        setLoading(false);
          router.back();
      }, 20 * 1000);

      Ipc.send("streaming", "stopStream", {
        sessionId: sessionId,
      })
        .then((result) => {
          console.log("Stream stopped:", result);
          clearTimeout(timer)
          setLoading(false);
          router.back();
        })
        .catch((e) => {
          console.log(e);
          clearTimeout(timer)
          setLoading(false);
          router.back();
        });
    }, 1000);
  };

  const handlePressNexus = () => {
    if (xPlayer && xPlayer.getChannelProcessor("input")) {
      xPlayer.getChannelProcessor("input").pressButtonStart("Nexus");
      setTimeout(() => {
        xPlayer.getChannelProcessor("input").pressButtonEnd("Nexus");
      }, 150);
    }
  };

  const handleLongPressNexus = () => {
    if (xPlayer && xPlayer.getChannelProcessor("input")) {
      xPlayer.getChannelProcessor("input").pressButtonStart("Nexus");
      setTimeout(() => {
        xPlayer.getChannelProcessor("input").pressButtonEnd("Nexus");
      }, 1000);
    }
  };

  const handleToggleMic = () => {
    if (!xPlayer) return
    if(xPlayer.getChannelProcessor('chat').isPaused === true){
      xPlayer.getChannelProcessor('chat').startMic();
      setOpenMicro(true);
    } else {
      xPlayer.getChannelProcessor('chat').stopMic();
      setOpenMicro(false);
    }
  };

  let videoHolderStyle = {}

  if (settings.video_format) {
    if (settings.video_format.indexOf(':') > -1) {
      videoHolderStyle = {
        display: 'flex',
        justifyContent: 'center',
        alignItems: 'center'
      }
    }
  }

  return (
    <>
      {
        showActionbar && (
          <ActionBar
            connectState={connectState}
            type={streamingType}
            openMicro={openMicro}
            onDisconnect={onDisconnect}
            onDisconnectPowerOff={onDisconnectPowerOff}
            onTogglePerformance={() => {
              setShowPerformance(!showPerformance);
            }}
            onDisplay={() => {
              if (settings.fsr) {
                setShowFSRDisplay(true)
              } else {
                setShowDisplay(true)
              }
            }}
            onEnhance={() => setShowEnhanceDrawer(true)}
            onToggleCompare={() => {
              const toggled = !enhanceSettingsRef.current.enabled;
              const next = { ...enhanceSettingsRef.current, enabled: toggled };
              handleEnhanceSettingsChange(next);
              addToast({
                title: toggled ? "🚀 光影精雕 + 真 X2 增强已开启" : "👁️ 已切换至原画直出对比",
                color: toggled ? "success" : "warning",
              });
            }}
            isRecording={isRecording}
            onToggleRecording={handleToggleRecording}
            onAudio={() => setShowAudio(true)}
            onMic={handleToggleMic}
            onText={() => {
              xPlayer.setKeyboardInput(false)
              setShowTextModal(true)
            }}
            onPressNexus={handlePressNexus}
            onLongPressNexus={handleLongPressNexus}
          />
        )
      }

      <FailedModal
        show={showConnectError}
        content={connectErrorContent}
        confirmText={t("Confirm")}
        onCancel={leaveStreamOnError}
      />

      <WarningModal
        show={showWarning}
        onConfirm={() => {
          setShowWarning(false);
          // handleExit('exit')
        }}
        onCancel={() => {
          setShowWarning(false);
        }}
      />
      {showPerformance && (
        <Perform
          xPlayer={xPlayer}
          connectState={connectState}
          enhanceTelemetry={enhanceTelemetry}
          isRecording={isRecording}
        />
      )}

      <EnhanceControlDrawer
        isOpen={showEnhanceDrawer}
        onClose={() => setShowEnhanceDrawer(false)}
        settings={enhanceSettings}
        onChange={handleEnhanceSettingsChange}
        telemetry={enhanceTelemetry}
        isStreaming={connectState === "connected"}
        isRecording={isRecording}
        lastRecordingPath={lastRecordingPath}
        onToggleRecording={handleToggleRecording}
      />

      {showDisplay && (
        <Display
          onClose={() => setShowDisplay(false)}
          onValueChange={(options) => {
            refreshPlayer(options);
          }}
        />
      )}

      {
        showFSRDisplay && (
          <FSRDisplay
            onClose={() => setShowFSRDisplay(false)}
            onValueChange={(options) => {
              const { sharpness } = options
              xPlayer && xPlayer.setFsrSharpnessDynamic(sharpness)
            }}
          />
        )
      }

      {
        showTextModal && (
          <TextModal 
            onClose={() => {
              setShowTextModal(false);
              xPlayer.setKeyboardInput(true);
            }}
            onConfirm={value => {
              let text = value.trim();
              if (!text) return
              if (text.length > 150) {
                text = text.substring(0, 150);
              }
              handleSendText(text);
            }}
          />
        )
      }

      {showSystemMessageModal && (
        <Modal isOpen={showSystemMessageModal} onClose={cancelSystemMessage}>
          <ModalContent>
            <>
              {systemMessageTitle !== "" && (
                <ModalHeader className="flex flex-col gap-1">
                  {systemMessageTitle}
                </ModalHeader>
              )}
              <ModalBody>
                {systemMessageContent !== "" && <p>{systemMessageContent}</p>}
              </ModalBody>
              <ModalFooter className="flex flex-wrap justify-end gap-2">
                {systemMessageButtons.length > 0 ? (
                  systemMessageButtons.map(button => (
                    <Button
                      key={`system-message-btn-${button.index}`}
                      color="primary"
                      onPress={() => {
                        completeSystemMessage(button.index);
                      }}
                    >
                      {button.label}
                    </Button>
                  ))
                ) : (
                  <Button
                    color="primary"
                    onPress={() => {
                      const { defaultIndex } = systemMessageActionRef.current;
                      completeSystemMessage(
                        typeof defaultIndex === "number" ? defaultIndex : 0
                      );
                    }}
                  >
                    {t("Confirm")}
                  </Button>
                )}
              </ModalFooter>
            </>
          </ModalContent>
        </Modal>
      )}

      {showSystemKeyboardModal && (
        <Modal isOpen={showSystemKeyboardModal} onClose={cancelSystemKeyboard}>
          <ModalContent>
            <>
              {systemKeyboardTitle !== "" && (
                <ModalHeader className="flex flex-col gap-1">
                  {systemKeyboardTitle}
                </ModalHeader>
              )}
              <ModalBody>
                {systemKeyboardDescription !== "" && (
                  <p>{systemKeyboardDescription}</p>
                )}
                <Input
                  label={t("Text")}
                  value={systemKeyboardText}
                  onValueChange={value => setSystemKeyboardText(value)}
                  maxLength={systemKeyboardMaxLength}
                  type={resolveSystemKeyboardProps().type}
                  inputMode={resolveSystemKeyboardProps().inputMode}
                />
              </ModalBody>
              <ModalFooter>
                <Button color="default" onPress={cancelSystemKeyboard}>
                  {t("Cancel")}
                </Button>
                <Button color="primary" onPress={completeSystemKeyboard}>
                  {t("Confirm")}
                </Button>
              </ModalFooter>
            </>
          </ModalContent>
        </Modal>
      )}

      {showAudio && (
        <Audio
          volume={volume}
          onClose={() => setShowAudio(false)}
          onValueChange={value => {
            setVolume(value)
            if (xPlayer) {
              xPlayer.setAudioVolumeDirect(value)
            }
          }}
        />
      )}

      {loading && <Loading loadingText={loadingText} />}

      <div id="videoHolder" style={videoHolderStyle}>
        {/* <video src="https://www.w3schools.com/html/mov_bbb.mp4" autoPlay muted loop playsInline></video> */}
      </div>
      
      <div id="canvas-container">
        <canvas id="canvas"></canvas>
      </div>
      

      <svg id="video-filters" style={{ display: "none" }}>
        <defs>
          <filter id="filter-usm">
            <feConvolveMatrix
              id="filter-usm-matrix"
              order="3"
            ></feConvolveMatrix>
          </filter>
        </defs>
      </svg>
    </>
  );
}

export default Stream;

// eslint-disable-next-line react-refresh/only-export-components
export const getStaticProps = makeStaticProperties(["common", "cloud"]);

// eslint-disable-next-line react-refresh/only-export-components
export { getStaticPaths };
