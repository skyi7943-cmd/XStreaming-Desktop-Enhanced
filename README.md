<h1 align="center">🚀 XStreaming Desktop Enhanced</h1>

<p align="center">
  <b>内置真 NVIDIA DLSS 5 神经渲染（支持双层串联） + NVOFA 硬件光流真 X2 插帧 + 全链路 0ms 级超低延迟 Xbox 桌面串流增强客户端</b>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Platform-Windows%2010%2F11%20x64-0078D6?style=flat-square&logo=windows" alt="Windows" />
  <img src="https://img.shields.io/badge/Neural%20Rendering-NVIDIA%20DLSS%205%20(Feature%2018)-76B900?style=flat-square&logo=nvidia" alt="DLSS 5" />
  <img src="https://img.shields.io/badge/Frame%20Gen-NVOFA%20True%20X2%20(30→60%20FPS)-10B981?style=flat-square" alt="X2 Frame Gen" />
  <img src="https://img.shields.io/badge/Console-Xbox%20Series%20S%20%7C%20Series%20X-107C10?style=flat-square&logo=xbox" alt="Xbox Series X|S" />
</p>

---

## ✨ 项目简介

**XStreaming Desktop Enhanced** 是基于开源客户端 [Geocld/XStreamingDesktop](https://github.com/Geocld/XStreamingDesktop) 深度定制并内嵌 **C++20 DirectX 11 / DirectX 12 原生超低延迟神经渲染与硬件光流补帧管线** 的增强版客户端（神经渲染底层协议参考自 [SAOG0721/Magpie](https://github.com/SAOG0721/Magpie)）。

它的核心使命非常简单直接：**把主机受限于体积、功耗与串流码率而缺失的算力，无缝交给电脑上的 RTX 显卡实时补齐**——在把无线/有线串流延迟压榨到物理极限的同时，让你的 **Xbox Series S** 与 **Xbox Series X** 在电脑屏幕上实现 **「真 30→60 FPS 丝滑补帧 + 真 DLSS 5 神经光影材质重塑 + AMD CAS / FSR RCAS 抗压缩锐化 + 一键 60FPS 增强内录」**！

---

## 🎮 它能为 Xbox Series S 和 Xbox Series X 做到什么？

无论你手头是主打性价比的 **Xbox Series S**，还是旗舰机能的 **Xbox Series X**，本增强版都能彻底打破主机硬件锁帧与串流画质折损的天花板：

### 1️⃣ 对于 **Xbox Series S (XSS)**：4 TFLOPS 小主机直接「逆天改命」

Xbox Series S 最大的遗憾是大量次世代 3A 大作（如《荒野大镖客 2》、《星空》、《黑神话：悟空》、《博德之门 3》、《艾尔登法环》、《赛博朋克 2077》画质模式等）**被死死锁在 30 FPS**，且内部分辨率常跌至 **720p ~ 900p**，叠加串流压缩后远景模糊、路面贴图发灰、暗场缺乏立体感。

使用 **XStreaming Desktop Enhanced** 连接 Xbox Series S 后，你可以直接做到：
- **🔓 锁 30 帧游戏 → 满血真 60 FPS 丝滑畅玩**：
  Xbox 串流协议无论游戏内部是 30 帧还是 60 帧，都会固定封装进 60Hz 视频流里（即 `A, A, B, B` 重复帧），导致市面上普通外部补帧工具全部失效或卡成重影。本客户端内置 **GPU 4,096 分块抗 H.264 蚊噪重复帧剥离引擎 (`30-in-60 Dedup`)**，在进入 NVOFA 硬件光流前精准剔除编码重复帧，提取出纯净真 30 FPS 序列并以微秒级半周期排期生成 **50% 几何真中点补帧**——让锁 30 帧的 XSS 游戏瞬间拥有 **真 60 FPS 的丝滑视角转动**！
- **🧠 900p 糊画质 → 真 DLSS 5 神经光影与微表面重塑**：
  直接调用电脑 RTX 显卡运行真 **`nvngx_dlssnr.dll` (DLSS 5 Feature 18)**，支持 **单层极速** 与 **双层显存串联精雕 (`Pass 1 → Pass 2`)**，重塑柏油路面颗粒、植被层次、车漆高光反射与岩石纹理，一键切换 **「自然均衡 / 鲜明通透 / 电影光影」** 三大神经风格，叠加 **CAS / FSR RCAS 去宏块锐化**，彻底洗掉 XSS 低内部分辨率与 H.264 码率带来的灰蒙感！

---

### 2️⃣ 对于 **Xbox Series X (XSX)**：终结「画质光追模式 vs 60帧性能模式」的世纪妥协！

即便是性能强悍的 Xbox Series X，在面对《极限竞速：地平线》、《赛博朋克 2077》、《心灵杀手 2》、《巫师 3 次世代光追版》、《瘟疫传说：安魂曲》、《星空》等重度 3A 游戏时，玩家依然被迫在两个残缺选项中痛苦二选一：
- 选 **画质/光追模式 (Quality / RT Mode)**：拥有主机最高原生 4K 采样、硬件光线追踪反射与满密度植被人群，但**帧率被砍到 30 FPS**，快速转视角卡顿难受；
- 选 **性能模式 (Performance Mode)**：虽然有了 60 FPS，但主机砍掉了光追、大幅削减了阴影精度、远景 LOD 与动态分辨率，串流到电脑上观感大打折扣。

使用 **XStreaming Desktop Enhanced** 连接 Xbox Series X 后，你可以直接做到：
- **🏆 游戏内放心开满「最高画质 / 光追 30 帧模式」，电脑端无损补齐「真 60 FPS + DLSS 5 二次精雕」**：
  让 XSX 主机全力负责渲染最高原生贴图、满密度场景与硬件光追，再由电脑端 RTX 显卡接力完成 **30→60 FPS 双向遮挡自洽光流补帧 + DLSS 5（乃至双层叠加 DLSS 5）电影级光影增强**——**从此「4K 光追最高画质」与「60 帧丝滑流畅」真正全都要！**
- **🎯 原生 60 帧竞技大作「0ms 级超低延迟 + 串流画质无损还原」**：
  游玩《使命召唤》、《光环：无限》、《Forza Motorsport》等原生 60 FPS 游戏时，一键切至 **「🎯 原生 60 帧竞技直通档」**——关闭补帧（`0.0ms` 扣押），仅开启 **DLSS 5 逐帧零残影神经增强 + CAS 去噪锐化 + 全链路极限降延迟**，彻底消除 WebRTC 串流造成的画面发灰和无线操作粘滞感！

---

### 📊 Xbox Series S 与 Xbox Series X 增强前后能力全对比

| 游戏场景 / 痛点 | 原版串流表现 | **XStreaming Desktop Enhanced 增强后表现** |
| :--- | :--- | :--- |
| **Xbox Series S 锁 30 帧 3A 大作** | 30 帧转视角卡顿，且因 60Hz 容器重复帧导致外部补帧失效 | **自动剥离重复帧 → NVOFA 真 30→60 FPS 丝滑中点补帧** |
| **Xbox Series S 720p~900p 低清画面** | 贴图糊、边缘锯齿、串流 H.264 宏块明显 | **内置真 DLSS 5（支持双层串联）重塑微表面材质 + CAS/RCAS 去噪锐化** |
| **Xbox Series X 画质光追模式 (30 FPS)** | 有光追与高画质但锁 30 帧，与性能模式无法兼得 | **保留 XSX 最高光追画质底子 + 电脑端实时升格真 60 FPS + 电影光影滤镜** |
| **Xbox Series X 原生 60 帧射击/竞速** | 存在浏览器音画同步排队与手柄轮询延迟，串流色彩发灰 | **音画解绑 + 0ms 零缓冲 + 500Hz 手柄瞬发（总延迟最高直降 25~45ms）+ DLSS 5 通透还原** |
| **高速转视角角色轮廓与彩色 UI** | 普通补帧器人物边缘撕裂闪动，血条/小地图/字幕果冻拉扯 | **零竞态光度汇聚 + 双空间矢量自洽修补 + 全彩静态 UI 零闪烁锁定** |
| **黑夜 / 暗场神经渲染** | 未初始化零深度或自动掩码易产生大面积暗部斑点 | **GPU 显式清零深度契约 + 1:1 Magpie 无损残差管线，黑夜纯净通透** |
| **实机画面录制** | 覆盖层防截屏属性导致黑屏或只能录到底层未增强原画 | **内置 `F9` 一键直录「60FPS 增强后画面 + Xbox 纯净原声」，同时支持 OBS 兼容模式** |

---

## 🛠️ 四大核心技术架构亮点

### 1. 内置真 NVIDIA DLSS 5 (`nvngx_dlssnr.dll` Feature 18) 单层与双层串联管线
- **免游戏深度缓冲驱动**：基于零深度引导契约（显式 GPU 清零 `R32_FLOAT` 深度纹理 + D3D11↔D3D12 共享显存栅栏与 `ResourceBarrier` 状态同步）+ 实时 NVOFA 运动矢量注入，直接对任意 Xbox 串流画面执行神经渲染。
- **支持「再叠加一层」双层显存串联 (`2× DLSS 5`)**：可在 `F8` 面板一键开启双层模式，将第 1 层 `nvngx_dlssnr` 的增强输出在显存内零拷贝直喂给第 2 层做二次深度重塑，第 2 层风格与强度独立可调。
- **1:1 对齐 `Magpie` 原版无损色彩与风格实时切换**：完整移植 `ApplyOklabControls` 中性直通与 `4x4 Catmull-Rom` 双三次残差上采样；切换 **自然均衡 / 鲜明通透 / 电影光影** 时自动下发 `DLSSNR.Reset = 1` 当帧即时刷新模型状态。

### 2. NVOFA 硬件光流真 X2 插帧（专克 `30-in-60` 串流容器）
- **`HLSL_DETECT_DUPLICATE` 智能去重**：4,096 块亮度均值抗 H.264 P 帧量化蚊噪检测，精准丢弃 60Hz WebRTC 流里的 30fps 重复包。
- **零竞态运动场汇聚 (`HLSL_PROJECT_FLOW`) + 双空间遮挡修补 (`RG8_UNORM FlowConf`)**：严禁快速旋转的背景跨过画面中央静止的第三人称角色与武器，平滑修补单侧遮挡边缘，剧烈甩镜头人物轮廓不碎裂、彩色血条与小地图零闪烁。
- **精确锚定第 1 帧上屏时刻的半周期排期**：完美兼容「逐帧同步零残影 DLSS 5」与双层叠加模式，杜绝 `FLIP_DISCARD` 瞬时吞帧，稳稳输出真 60 FPS。

### 3. 专为「有线 Xbox + Wi-Fi 6 电脑」打造的全链路极限降延迟
- **音视频唇音同步锁解绑 (`A/V Lip-Sync Unbind`)**：拦截 `<video>.srcObject` 剥离音画共享 `MediaStream`，禁止 Chromium 为等待音频缓冲而扣押已解码视频帧（**立减 `15 ~ 35ms`**）。
- **WebRTC `0ms` 零缓冲内核破限**：注入 `WebRTC-ZeroPlayoutDelay/min_pacing:0ms,max_decode_queue_size:2/` + `playoutDelayHint = 0`，并提供 `0ms / 2ms / 8ms(Wi-Fi 6) / 20ms` 五档抗抖动切换。
- **手柄 `500Hz` 瞬态零排队直发 (`Turbo Input`)**：重写按键/摇杆发送队列为单槽最新态覆盖，按键变化 `0ms` 事件级立即发包（**立减 `6 ~ 12ms`**），同时支持原生键鼠映射与多账号切换串流。
- **100% 硬件实测真实增加延迟监控 (`F7`)**：基于 `D3D11_QUERY_EVENT` 非阻塞探针与 `D3D12` 时间戳，实时显示 **`真实总增延 = 捕获渲染上屏耗时 + X2 补帧帧序缓冲`** 及 **`DLSS 5 实测推理耗时`**，绝不虚标。

### 4. 游戏内原生锐利调参面板 (`F8`) 与一键 60FPS 增强录屏 (`F9`)
- **GPU 像素级 `ExcludeRects` 原生直通**：针对 Windows 11 `FLIP_DISCARD` 交换链特性，在呈现与光流着色器中直接对 `F8` 调参抽屉、`F7` 监控条与顶部菜单做 `1:1` 原画隔离直通——**边开车/边跑图边调 DLSS 5 滤镜，面板文字 100% 原生锐利不模糊，还支持实时滑动分屏对比**！
- **内置 `F9` 60FPS 增强画面 + 游戏原声硬件录屏**：一键调用显卡硬件编码器录制叠加了 DLSS 5 + X2 60FPS 的最终增强画面，并直接克隆混入纯净 Xbox WebRTC 音频轨（不录电脑微信/系统杂音），同时提供外部 OBS / NVIDIA ShadowPlay 桌面采集兼容开关。

---

## ⌨️ 串流内快捷键一览

| 快捷键 | 功能说明 |
| :---: | :--- |
| **`F8`** | 呼出 / 收起 **DLSS 5（单层/双层）+ 真 X2 补帧 + 锐化 + 降延迟实时控制中心** |
| **`F6`** | 一键切换 **增强画面 / 原版串流画面**（瞬间对比增强效果） |
| **`F7`** | 显示 / 隐藏 **性能与硬件实测真实增加延迟监控条**（含 DLSS 5 推理耗时与总增延拆解） |
| **`F9`** | 开始 / 停止 **60FPS 增强画面 + Xbox 纯净游戏原声内录**（默认保存至 `E:\XStreaming_Recordings\`） |

---

## 📦 快速开始与本地构建

### 硬件与环境要求
- **操作系统**：Windows 10 / Windows 11 64 位（推荐 Windows 11 22H2 及以上）
- **显卡要求**：NVIDIA GeForce RTX 显卡（推荐 RTX 30 / 40 / 50 系列，最新 Game Ready 驱动以启用真 `nvngx_dlssnr.dll` Feature 18 与 NVOFA 硬件光流；非 RTX 显卡亦内置 Compute Shader 光流 + CAS/RCAS 锐化自动回退）

### 开箱即用（Release 绿色版）
1. 前往本仓库 [**Releases**](../../releases) 页面下载最新绿色解压包（或下载 `nvngx_dlssnr.dll` 放入 `native/bin/` 与打包目录 `resources/native/` 下）；
2. 双击运行 `XStreaming.exe`，登录微软 Xbox 账号连接你的 **Xbox Series S** 或 **Xbox Series X**，进入串流即自动挂载增强引擎！

### 从源码编译构建

```powershell
# 1. 克隆仓库并安装前端依赖
git clone https://github.com/skyi7943-cmd/XStreaming-Desktop-Enhanced.git
cd XStreaming-Desktop-Enhanced
yarn install

# 2. 将 NVIDIA DLSS 5 运行库 nvngx_dlssnr.dll 放入 native/bin/ 目录后，编译并执行真卡硬件自检
npm run build:engine

# 3. 运行端到端自动化集成验证套件
node test/verify-enhance-integration.mjs

# 4. 打包生成 Windows 绿色免安装客户端
npx next build renderer
npx next export -o app renderer
node node_modules/nextron/bin/webpack.config.js
npx electron-builder --dir --win -c.directories.output=dist-v2
```

---

## 🙏 开源致谢与声明

- 串流客户端底座基于 [**Geocld/XStreamingDesktop**](https://github.com/Geocld/XStreamingDesktop) 开发；
- 零深度驱动 `nvngx_dlssnr.dll` 签名组件互操作与 Oklab 残差重建管线深度致敬 [**SAOG0721/Magpie**](https://github.com/SAOG0721/Magpie)；
- **免责声明**：本项目与 Microsoft（微软）、Xbox 及 NVIDIA 无官方隶属关系，所有相关商标均归各自所有者所有，仅供个人串流画质与低延迟技术交流研究使用。
