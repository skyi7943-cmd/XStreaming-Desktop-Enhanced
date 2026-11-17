import { useState, useEffect } from "react";
import { useTranslation } from "next-i18next";
import { useSettings } from "../context/userContext";

function Perform({ xPlayer, connectState, enhanceTelemetry, isRecording }: any) {
  const { t } = useTranslation("cloud");
  const { settings } = useSettings();
  const [performance, setPerformance] = useState<any>({});
  const [isLight, setIslight] = useState(false);

  useEffect(() => {
    const localTheme = localStorage.getItem("theme");
    if (localTheme === "xbox-light") {
      setIslight(true);
    }

    const perfInterval = setInterval(() => {
      if (xPlayer && connectState === "connected") {
        xPlayer.getStreamState &&
          xPlayer.getStreamState().then((perf: any) => {
            setPerformance(perf);
          });
      }
    }, 1000);

    return () => {
      clearInterval(perfInterval);
    };
  }, [xPlayer, connectState]);

  let resolutionText = "";
  if (performance.resolution) {
    resolutionText = performance.resolution;
    if (settings.resolution === 1081) {
      resolutionText = resolutionText + "(HQ)";
    }
  }

  const enhActive = !!enhanceTelemetry?.running;
  const dupCount = enhanceTelemetry?.dupSkipped || 0;
  const srcFpsLabel =
    enhActive && dupCount >= 5 && enhanceTelemetry?.srcFps
      ? `真${enhanceTelemetry.srcFps.toFixed(0)}(容器${performance.fps || 60})`
      : `${performance.fps || "--"}`;
  const outFpsText =
    enhActive && enhanceTelemetry?.outFps
      ? `${enhanceTelemetry.outFps.toFixed(0)} FPS${enhanceTelemetry.fgActive ? " (真X2)" : ""}`
      : null;

  // 100% 硬件实测真实增加延迟拆解（含单层/双层 DLSS 5 推理/竞争耗时 + GPU 管线上屏耗时 + 插帧半周期扣押耗时）
  const gpuPipeMs = Number(enhanceTelemetry?.gpuPipeMs ?? 0);
  const fgHoldMs = Number(enhanceTelemetry?.fgHoldMs ?? 0);
  const totalAddedMs = Number(
    enhanceTelemetry?.totalAddedMs ?? gpuPipeMs + fgHoldMs
  );
  const dlss5Ms = Number(enhanceTelemetry?.dlss5Ms ?? 0);
  const dlss5HostMs = Number(enhanceTelemetry?.dlss5HostMs ?? 0);
  const dlss5On = !!enhanceTelemetry?.dlss5Active;
  const dlss5Passes = Number(enhanceTelemetry?.dlss5Passes ?? 1);
  const dlss5Sync = !!enhanceTelemetry?.dlss5Sync;
  const dlssTag = dlss5Passes >= 2 ? "DLSS5×2" : "DLSS5";

  const dlssDetailText = dlss5On
    ? `${dlss5Ms.toFixed(1)}ms(${dlss5Sync ? "同步阻塞" : `异步·主链+${dlss5HostMs.toFixed(1)}ms`})`
    : "关(+0ms)";
  const fgHoldText = enhanceTelemetry?.fgActive
    ? fgHoldMs > 0.05
      ? `+${fgHoldMs.toFixed(1)}ms(内插)`
      : "0.0ms(零扣押)"
    : "关";

  return (
    <>
      {settings.performance_style ? (
        <div id="performances-x" className="flex flex-row justify-center w-full">
          <span className={isLight ? "performance-x-wrap-light" : "performance-x-wrap"}>
            {isRecording && (
              <span className="text-xs text-red-400 font-bold">🔴录屏中 | &nbsp;</span>
            )}
            <span className="text-xs">{resolutionText || "--"} | &nbsp;</span>
            <span className="text-xs">
              {t("RTT")}: {performance.rtt || "--"} | &nbsp;
            </span>
            <span className="text-xs">
              {t("JIT")}:{" "}
              {enhanceTelemetry?.webrtcJitterMs !== undefined
                ? `${enhanceTelemetry.webrtcJitterMs}ms`
                : performance.jit || "--"}{" "}
              | &nbsp;
            </span>
            <span className="text-xs">
              {t("FPS")}: {srcFpsLabel}
              {outFpsText ? ` → ${outFpsText}` : ""} | &nbsp;
            </span>
            {enhActive && (
              <span className="text-xs text-emerald-400 font-semibold">
                真实增延: +{totalAddedMs.toFixed(1)}ms [管线{gpuPipeMs.toFixed(1)}ms | {dlssTag}:{dlssDetailText} | 补帧缓冲:{fgHoldText}] | &nbsp;
              </span>
            )}
            <span className="text-xs">
              {t("FD")}: {performance.fl || "--"} | &nbsp;
            </span>
            <span className="text-xs">
              {t("PL")}: {performance.pl || "--"} | &nbsp;
            </span>
            <span className="text-xs">
              {t("Bitrate")}: {performance.br || "--"} | &nbsp;
            </span>
            <span className="text-xs">
              {t("DT")}: {performance.decode || "--"}
            </span>
          </span>
        </div>
      ) : (
        <div id="performances">
          {isRecording && (
            <div className="px-1 text-xs text-red-400 font-bold">
              🔴 增强画面+原声录制中 (F9停止)
            </div>
          )}
          <div className="px-1 text-sm">
            {t("Resolution")}: {resolutionText || "--"}
          </div>
          <div className="px-1 text-sm">
            {t("RTT")}: {performance.rtt || "--"}
          </div>
          <div className="px-1 text-sm">
            {t("JIT")}:{" "}
            {enhanceTelemetry?.webrtcJitterMs !== undefined
              ? `${enhanceTelemetry.webrtcJitterMs}ms`
              : performance.jit || "--"}
          </div>
          <div className="px-1 text-sm">
            {t("FPS")}: {srcFpsLabel}
            {outFpsText ? ` → ${outFpsText}` : ""}
          </div>
          {enhActive && (
            <>
              <div className="px-1 text-sm text-amber-300 font-bold">
                真实总增延: +{totalAddedMs.toFixed(1)} ms
              </div>
              <div className="px-1 text-xs text-emerald-400">
                ├ 捕获+渲染上屏: +{gpuPipeMs.toFixed(2)} ms
              </div>
              <div className="px-1 text-xs text-purple-300">
                ├ {dlssTag} 神经推理: {dlssDetailText}
              </div>
              <div className="px-1 text-xs text-sky-300">
                └ X2 补帧帧序缓冲: {fgHoldText}
              </div>
            </>
          )}
          <div className="px-1 text-sm">
            {t("FD")}: {performance.fl || "--"}
          </div>
          <div className="px-1 text-sm">
            {t("PL")}: {performance.pl || "--"}
          </div>
          <div className="px-1 text-sm">
            {t("Bitrate")}: {performance.br || "--"}
          </div>
          <div className="px-1 text-sm">
            {t("DT")}: {performance.decode || "--"}
          </div>
        </div>
      )}
    </>
  );
}

export default Perform;
