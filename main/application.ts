import { app as ElectronApp, BrowserWindow, dialog, powerSaveBlocker } from "electron";
import serve from "electron-serve";
import Store from "electron-store";
import Debug from "debug";
import { createWindow, xboxWorker } from "./helpers";
import Authentication from "./authentication";
import MsalAuthentication from "./MsalAuthentication";
import Ipc from "./ipc";
import { defaultSettings } from '../renderer/context/userContext.defaults'
import DiscordRpcHelper from './helpers/discordrpc'

import xboxWebApi from "xbox-webapi";
import xCloudApi from "./helpers/xcloudapi";

import pkg from "../package.json";

interface startupFlags {
  fullscreen: boolean;
  autoConnect: string;
}

export default class Application {
  private _log;
  public _store = new Store();
  private _startupFlags: startupFlags = {
    fullscreen: false,
    autoConnect: "",
  };

  public _isProduction: boolean = process.env.NODE_ENV === "production";
  private _isCi: boolean = process.env.CI !== undefined;
  private _isMac: boolean = process.platform === "darwin";
  private _isWindows: boolean = process.platform === "win32";
  private _isQuitting: boolean = false;

  public _mainWindow;
  public _ipc: Ipc;

  public _authentication: Authentication;
  public _msalAuthentication: MsalAuthentication;

  public _discordRpc: DiscordRpcHelper;

  public streamingTokens: any

  public webToken: any

  constructor() {
    console.log(
      __filename + "[constructor()] Starting XStreaming v" + pkg.version
    );
    this._log = Debug("xstreaming");

    const settings: any = this._store.get('settings', defaultSettings)

    if (process.platform === 'win32') {
      // Windows 专属防黑屏与超低延迟硬件解码管线（完美兼容 NVIDIA RTX + AMD Radeon 独显/核显）：
      // 1. 严禁在 Windows 下启用 Linux 专用 DefaultANGLEVulkan/VaapiVideoDecoder（会导致 D3D11 硬解视频流黑屏！）
      // 2. 显式禁用 DirectComposition NV12 MPO 硬件叠加层，彻底根治 AMD Adrenalin 驱动常见的串流画面全黑 Bug！
      // 3. 当用户在设置中勾选「使用Vulkan渲染 (兼容渲染模式)」时，额外关闭 zero-copy 启用拷贝回读安全硬解模式
      ElectronApp.commandLine.appendSwitch('use-angle', 'd3d11')
      ElectronApp.commandLine.appendSwitch('ignore-gpu-blocklist')
      ElectronApp.commandLine.appendSwitch('enable-gpu-rasterization')
      ElectronApp.commandLine.appendSwitch('enable-oop-rasterization')
      ElectronApp.commandLine.appendSwitch('disable-direct-composition-video-overlays')
      if (!settings.use_vulkan) {
        ElectronApp.commandLine.appendSwitch('enable-accelerated-video-decode')
        ElectronApp.commandLine.appendSwitch('enable-zero-copy')
      } else {
        // 用户开启「防黑屏兼容渲染模式 (原Vulkan选项)」：彻底绕过问题显卡驱动 DXVA/NV12 硬解黑屏，交由多核解码直传 D3D11 纹理，100% 杜绝黑屏！
        ElectronApp.commandLine.appendSwitch('disable-accelerated-video-decode')
        ElectronApp.commandLine.appendSwitch('disable-zero-copy')
      }
      ElectronApp.commandLine.appendSwitch('disable-gpu-vsync')
      ElectronApp.commandLine.appendSwitch('disable-frame-rate-limit')
      ElectronApp.commandLine.appendSwitch('disable-renderer-backgrounding')
      ElectronApp.commandLine.appendSwitch('disable-background-timer-throttling')
      ElectronApp.commandLine.appendSwitch('webrtc-max-cpu-consumption-percentage', '100')
      // 突破 Chromium VCMTiming 默认 8ms 最小解码节拍限制，压缩解码队列深度至 2 帧
      ElectronApp.commandLine.appendSwitch(
        'force-fieldtrials',
        'WebRTC-ZeroPlayoutDelay/min_pacing:0ms,max_decode_queue_size:2/'
      )
      ElectronApp.commandLine.appendSwitch(
        'enable-features',
        settings.use_vulkan
          ? 'CanvasOopRasterization,WebRtcHideLocalIpsWithMdns'
          : 'D3D11VideoDecoder,CanvasOopRasterization,PlatformHEVCDecoderSupport,WebRtcHideLocalIpsWithMdns'
      )
      ElectronApp.commandLine.appendSwitch(
        'disable-features',
        'DirectCompositionUseNV12DecodeSwapChain,DirectCompositionVideoOverlays,Vulkan'
      )
    } else if (settings.use_vulkan) {
      ElectronApp.commandLine.appendSwitch('use-vulkan')
      ElectronApp.commandLine.appendSwitch('enable-features', 'Vulkan,VulkanFromANGLE,DefaultANGLEVulkan,VaapiIgnoreDriverChecks,VaapiVideoDecoder,PlatformHEVCDecoderSupport,CanvasOopRasterization,WebRtcHideLocalIpsWithMdns')
      ElectronApp.commandLine.appendSwitch('enable-gpu-rasterization')
      ElectronApp.commandLine.appendSwitch('enable-oop-rasterization')
      ElectronApp.commandLine.appendSwitch('enable-accelerated-video-decode')
      ElectronApp.commandLine.appendSwitch('disable-frame-rate-limit')
      ElectronApp.commandLine.appendSwitch('disable-renderer-backgrounding')
      ElectronApp.commandLine.appendSwitch('disable-background-timer-throttling')
      ElectronApp.commandLine.appendSwitch('force-fieldtrials', 'WebRTC-ZeroPlayoutDelay/min_pacing:0ms,max_decode_queue_size:2/')
      ElectronApp.commandLine.appendSwitch('ozone-platform-hint', 'x11')
      ElectronApp.commandLine.appendSwitch('ignore-gpu-blocklist')
      ElectronApp.commandLine.appendSwitch('no-sandbox');
      ElectronApp.commandLine.appendSwitch('enable-zero-copy');
    } else {
      // SteamOS 3.x (Steam Deck LCD 60Hz / OLED 90Hz) & Linux 默认专属 VA-API RDNA2 硬件加速与 0ms WebRTC 低延迟管线
      ElectronApp.commandLine.appendSwitch('ignore-gpu-blocklist')
      ElectronApp.commandLine.appendSwitch('enable-gpu-rasterization')
      ElectronApp.commandLine.appendSwitch('enable-oop-rasterization')
      ElectronApp.commandLine.appendSwitch('enable-accelerated-video-decode')
      ElectronApp.commandLine.appendSwitch('enable-zero-copy')
      ElectronApp.commandLine.appendSwitch('disable-frame-rate-limit')
      ElectronApp.commandLine.appendSwitch('disable-renderer-backgrounding')
      ElectronApp.commandLine.appendSwitch('disable-background-timer-throttling')
      ElectronApp.commandLine.appendSwitch('webrtc-max-cpu-consumption-percentage', '100')
      ElectronApp.commandLine.appendSwitch('force-fieldtrials', 'WebRTC-ZeroPlayoutDelay/min_pacing:0ms,max_decode_queue_size:2/')
      ElectronApp.commandLine.appendSwitch(
        'enable-features',
        'VaapiVideoDecoder,VaapiIgnoreDriverChecks,CanvasOopRasterization,PlatformHEVCDecoderSupport,WebRtcHideLocalIpsWithMdns'
      )
      ElectronApp.commandLine.appendSwitch('ozone-platform-hint', 'x11')
      if (process.platform === 'linux') {
        ElectronApp.commandLine.appendSwitch('no-sandbox')
      }
    }

    this.readStartupFlags(settings);
    this.loadApplicationDefaults();

    this._ipc = new Ipc(this);
    this._authentication = new Authentication(this);
    this._msalAuthentication = new MsalAuthentication(this);
    this._discordRpc = new DiscordRpcHelper();

    if (settings.discord_rpc !== false) {
      this._discordRpc.enable()
    }

    this._ipc.startUp();

    // Prevent display from sleeping
    const id = powerSaveBlocker.start('prevent-display-sleep')
    console.log('Prevent sleep state:' + powerSaveBlocker.isStarted(id))
  }

  log(namespace = "application", ...args) {
    this._log.extend(namespace)(...args);
  }

  getStartupFlags() {
    return this._startupFlags;
  }

  resetAutoConnect() {
    this._startupFlags.autoConnect = "";
  }

  readStartupFlags(settings: any) {
    this.log(
      "application",
      __filename + "[readStartupFlags()] Program args detected:",
      process.argv
    );

    for (const arg in process.argv) {
      if (process.argv[arg].includes("--fullscreen")) {
        this.log(
          "application",
          __filename +
          "[readStartupFlags()] --fullscreen switch found. Setting fullscreen to true"
        );
        this._startupFlags.fullscreen = true;
      }

      if (process.argv[arg].includes("--auto-connect=") || settings.xhome_auto_connect_server_id) {

        let key = '';
        if (process.argv[arg].includes("--auto-connect=")) {
          key = process.argv[arg]?.substring(15);
        } else {
          key = settings.xhome_auto_connect_server_id;
        }
        this.log(
          "application",
          __filename +
          "[readStartupFlags()] --auto-connect switch found. Setting autoConnect to",
          key
        );
        this._startupFlags.autoConnect = key;
      }
    }

    this.log(
      "application",
      __filename + "[readStartupFlags()] End result of startupFlags:",
      this._startupFlags
    );
  }

  loadApplicationDefaults() {
    if (this._isProduction === true && this._isCi === false) {
      serve({ directory: "app" });
    } else if (this._isCi === true) {
      const random = Math.random() * 100;
      ElectronApp.setPath(
        "userData",
        `${ElectronApp.getPath("userData")} (${random})`
      );
      ElectronApp.setPath(
        "sessionData",
        `${ElectronApp.getPath("userData")} (${random})`
      );
      this._store.delete("user");
      this._store.delete("auth");

      serve({ directory: "app" });
    } else {
      ElectronApp.setPath(
        "userData",
        `${ElectronApp.getPath("userData")} (development)`
      );
    }

    ElectronApp.whenReady()
      .then(() => {
        this.log(
          "electron",
          __filename +
          "[loadApplicationDefaults()] Electron has been fully loaded. Ready to open windows"
        );

        this.openMainWindow();
        this._authentication.startWebviewHooks();
      })
      .catch((error) => {
        this.log(
          "electron",
          __filename +
          "[loadApplicationDefaults()] Electron has failed to load:",
          error
        );
      });

    ElectronApp.on("window-all-closed", () => {
      if (this._isMac === true) {
        this.log(
          "electron",
          __filename +
          "[loadApplicationDefaults()] Electron detected that all windows are closed. Running in background..."
        );
      } else {
        this.log(
          "electron",
          __filename +
          "[loadApplicationDefaults()] Electron detected that all windows are closed. Quitting app..."
        );
        ElectronApp.quit();
      }
    });

    ElectronApp.on("activate", () => {
      this._mainWindow !== undefined
        ? this._mainWindow.show()
        : this.openMainWindow();
    });
    ElectronApp.on("before-quit", () => {
      this._isQuitting = true;
      this._ipc?._channels?.enhance?._manager?.stop();
      this._discordRpc && this._discordRpc.destroy();
    });
  }

  _webApi: xboxWebApi;
  _xHomeApi: xCloudApi;
  _xCloudApi: xCloudApi;
  _xboxWorker: xboxWorker;

  authenticationCompleted(streamingTokens, webToken) {
    this.log(
      "electron",
      __filename + "[authenticationCompleted()] authenticationCompleted called"
    );
    this.log(
      "electron",
      __filename + "[authenticationCompleted()] streamingTokens:",
      streamingTokens
    );

    this.streamingTokens = streamingTokens
    this.webToken = webToken

    this._webApi = new xboxWebApi({
      userToken: webToken.data.Token,
      uhs: webToken.data.DisplayClaims.xui[0].uhs,
    });

    this._authentication._isAuthenticating = false;
    this._authentication._isAuthenticated = true;

    this._msalAuthentication._isAuthenticating = false;
    this._msalAuthentication._isAuthenticated = true;

    this._webApi
      .getProvider("profile")
      .get(
        "/users/me/profile/settings?settings=GameDisplayName,GameDisplayPicRaw,Gamerscore,Gamertag"
      )
      .then((result) => {
        if (result.profileUsers.length > 0) {
          for (const setting in result.profileUsers[0].settings) {
            if (result.profileUsers[0].settings[setting].id === "Gamertag") {
              this._store.set(
                "user.gamertag",
                result.profileUsers[0].settings[setting].value
              );
            } else if (
              result.profileUsers[0].settings[setting].id ===
              "GameDisplayPicRaw"
            ) {
              this._store.set(
                "user.gamerpic",
                result.profileUsers[0].settings[setting].value
              );
            } else if (
              result.profileUsers[0].settings[setting].id === "Gamerscore"
            ) {
              this._store.set(
                "user.gamerscore",
                result.profileUsers[0].settings[setting].value
              );
            }
          }
        }

        // Run workers
        this._xboxWorker = new xboxWorker(this);
        this._ipc.onUserLoaded();
        this._discordRpc.setIdle();
      })
      .catch((error) => {
        this.log(
          "electron",
          __filename +
          "[authenticationCompleted()] Failed to retrieve user profile:",
          error
        );
        dialog.showMessageBox({
          message:
            "Error: Failed to retrieve user profile:" + JSON.stringify(error),
          type: "error",
        });
      });
  }

  openMainWindow() {
    this.log(
      "electron",
      __filename + "[openMainWindow()] Creating new main window"
    );

    const settings: any = this._store.get('settings', defaultSettings)
    console.log('application.ts settings:', settings)

    const windowOptions: any = {
      title: "XStreaming",
      backgroundColor: "rgb(26, 27, 30)",
    };

    const isGamescope =
      process.platform === "linux" &&
      (process.env.SteamDeck === "1" ||
        Boolean(process.env.GAMESCOPE_WAYLAND_DISPLAY) ||
        process.env.XDG_CURRENT_DESKTOP === "gamescope" ||
        process.env.XDG_SESSION_DESKTOP === "gamescope");

    if (settings.fullscreen || isGamescope) {
      windowOptions.fullscreen = true;
    }

    this._mainWindow = createWindow("main", {
      width: 1280,
      height: 800,
      ...windowOptions,
    });

    this._mainWindow.webContents.on('before-input-event', (event, input) => {
      if (input.type === 'keyDown') {
        const isFullScreen = this._mainWindow.isFullScreen();

        if (input.key === 'F11' || (input.alt && input.key === 'Enter')) {
          this._mainWindow.setFullScreen(!isFullScreen);
          event.preventDefault();
        }

        if (input.key === 'Escape' && isFullScreen) {
          this._mainWindow.setFullScreen(false);
          event.preventDefault();
        }
      }
    });

    // this._mainWindow.openDevTools();

    // 始终关闭渲染进程计时器节流，确保手柄 500Hz+ 轮询与 WebRTC 解码零延迟响应
    this._mainWindow.webContents.setBackgroundThrottling(false);

    this._mainWindow.on("show", () => {
      this.log(
        "electron",
        __filename + "[openMainWindow()] Showing Main window."
      );
    });

    this._mainWindow.on("close", (event) => {
      if (this._isMac === true && this._isQuitting === false) {
        event.preventDefault();
        this.log(
          "electron",
          __filename + "[openMainWindow()] Main windows has been hidden"
        );
        this._mainWindow.hide();
      } else {
        this._ipc?._channels?.enhance?._manager?.stop();
        this.log(
          "electron",
          __filename + "[openMainWindow()] Main windows has been closed"
        );
        this._mainWindow = undefined;
      }
    });


    const locale = settings.locale || 'en'

    if (this._isProduction === true && this._isCi === false) {
      this._mainWindow.loadURL(`app://./${locale}/home`);
    } else {
      const port = process.argv[2] || 3000;
      this._mainWindow.loadURL(`http://localhost:${port}/${locale}/home`);

      // if(this._isCi !== true){
      //     this._mainWindow.webContents.openDevTools()
      //     this.openGPUWindow()
      // }
    }
  }

  _gpuWindow;

  openGPUWindow() {
    this._gpuWindow = new BrowserWindow({
      width: 800,
      height: 600,
    });

    // Load chrome://gpu
    this._gpuWindow.loadURL("chrome://gpu");

    // Open DevTools
    this._gpuWindow.webContents.openDevTools();
  }

  quit() {
    ElectronApp.quit();
  }

  restart() {
    this.quit();
    ElectronApp.relaunch();
  }
}

new Application();
