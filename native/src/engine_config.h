#pragma once
// XStreaming Desktop Enhanced - 原生超低延迟神经渲染与 X2 补帧引擎配置定义 (v2 专治串流60帧容器内30帧游戏补帧 + 全链路降延迟版)

#include <cstdint>
#include <vector>

struct ExcludeRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct EnhanceConfig {
    // 总开关与暂停控制
    bool enabled = true;
    bool paused = false;

    // 视口裁剪偏移（相对于目标 HWND 客户区）
    int viewportOffsetX = 0;
    int viewportOffsetY = 0;
    int viewportWidth = 0;
    int viewportHeight = 0;

    // ==================== 1. 超低延迟 X2 插帧与 30-in-60 智能去重配置 ====================
    bool fgEnabled = true;
    // 智能识别并剔除 Xbox 60fps 串流容器中的 30fps 编码重复帧（解决“串流60帧但游戏30帧导致不插帧”的核心开关，默认必开！）
    bool dedup30In60 = true;
    // 重复帧抗噪灵敏度 (1 ~ 10，默认 5：精准过滤 H.264 压缩噪点同时保留微幅视角移动)
    int dedupSensitivity = 5;
    // 0 = 零等待预测 X2 (Zero-Wait Extrapolation，0ms 真实帧扣押)
    // 1 = 双向光流遮挡修补精插 X2 (Bidirectional Midpoint，默认推荐：专治 30帧转视角卡顿，严格 50% 几何中点丝滑 60帧)
    int fgMode = 1;
    float fgSceneCutThreshold = 0.18f; // 切镜防果冻阈值 (0.05 ~ 0.40)
    float fgUiProtection = 0.75f;      // 全彩静态 UI / 小地图 / 血条 / 准星 / 字幕防闪烁锁定强度 (0.0 ~ 1.0)

    // ==================== 2. 内置 DLSS 5 (nvngx_dlssnr.dll) 单层/双层叠加配置 ====================
    bool dlss5Enabled = true;
    int dlss5Passes = 1;               // 1 = 标准单层 DLSS 5, 2 = 双层叠加 DLSS 5 (Pass 1 输出直连 Pass 2 二次精雕)
    // 0 = 异步残差极低延迟模式 (Async Residual，光度自检防残影 + 后台 D3D12 队列独立推理)
    // 1 = 逐帧同步零残影模式 (Sync Every-Frame，默认推荐：RTX 40 系仅需 ~5ms，零残影零天空闪烁)
    int dlss5LatencyMode = 1;
    uint32_t dlss5InputScale = 75;     // 神经工作集缩放百分比 (50 ~ 100)
    int dlss5Style = 0;                // 第 1 层风格 (0: 自然均衡, 1: 鲜明通透, 2: 电影光影)
    float dlss5Intensity = 1.00f;      // 第 1 层神经强度
    int dlss5Pass2Style = 0;           // 第 2 层叠加风格
    float dlss5Pass2Intensity = 0.85f; // 第 2 层叠加神经强度
    float dlss5LocalTone = 1.00f;
    float dlss5LocalStructure = 1.00f;
    float dlss5SkinStructure = 0.00f;
    bool dlss5AutoMask = false;        // 对齐 Magpie 默认关闭内部掩码，防止暗场与高反差车牌产生误判斑块
    bool dlss5UiCorrection = false;    // 对齐 Magpie 默认关闭无 Alpha 掩码的 UI 猜测修正

    // Oklab 感知色彩空间残差精细控制（对齐 Magpie 1:1 中性直通默认参数，100% 释放自然/鲜明/电影风格滤镜效果！）
    float residualStrength = 1.00f;
    float shadowStructure = 1.00f;
    float highlightGlow = 1.00f;
    float saturation = 1.00f;
    float darkProtection = 0.00f;      // 默认 0.0（依靠清零深度缓冲根治暗斑，不破坏暗部车漆与光影风格；可按需微调）
    float highlightProtection = 0.00f;
    int antiFlicker = 1;

    // ==================== 3. 硬件级超低延迟锐化器配置 ====================
    // 0: 关闭, 1: AMD FidelityFX CAS, 2: AMD FSR RCAS
    int sharpenMode = 1;
    float sharpenStrength = 0.55f;
    bool sharpenDenoise = true;

    // ==================== 4. 低延迟呈现、录屏兼容与实时分屏对比 ====================
    bool allowTearing = true;
    // false (默认): 采用 DXGI Output Duplication + WDA_EXCLUDEFROMCAPTURE，零 DWM 遮挡节流，确保 X2 插帧 100% 满帧丝滑；
    // true (F9 内置录屏时自动开启，也可手动开启供 OBS 采集): 切换为 WGC 单窗口捕获 + WDA_NONE + Alpha 254 防 DWM 遮挡
    bool obsCaptureCompat = false;
    float splitCompare = 0.0f;

    // 需要从覆盖层镂空避让的 React 交互面板矩形区域
    std::vector<ExcludeRect> excludeRects;
};
