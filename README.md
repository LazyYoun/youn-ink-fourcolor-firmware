# Youn Ink Four Color

本仓库提供面向 ZECTRIX Note4C 的公开参考固件与开发工具。当前固件基于 ESP-IDF，目标硬件为 ESP32-S3 与 4.2 英寸 400 × 300 电子墨水屏，默认支持黑、白、红、黄四色 BWRY 面板，同时保留 1bpp 黑白面板配置。

> [!IMPORTANT]
> Note4C 与黑白版 Note4 使用的屏幕和固件不同，请勿混刷。本仓库是二次开发起点，不包含 Note4 的官方成品固件或开箱即用的 AI 体验。

## 当前公开范围

已接入当前固件主流程的功能包括：

- RawDraw 直接帧缓冲 UI，默认开放相册与设置页面；
- Wi-Fi Station、SoftAP 配网及网络状态显示；
- AP/LAN HTTP 图片上传、本地图片存储、相册浏览与定时轮播；
- 1bpp 黑白图片与 2bpp BWRY 四色图片显示；
- 物理按键、RTC/SNTP 时间、电池与充电状态、深度休眠；
- 四色屏刷新合并、差异检测及异步刷新任务。

仓库中还保留了对话、天气、新闻、日历、电子书等页面渲染器和流式处理组件。它们并非全部出现在默认导航中，也不代表相应数据源或端到端服务已经在公开仓库中接通。

## 公开仓库边界

当前公开快照不包含完整的 Python AI 后端、Web 管理前端或生产部署配置：

- `server/` 目前只包含 `mock_client.py`，用于连接另行部署的兼容 WebSocket 服务；
- 仓库中没有可独立启动的 `llmserve.py`、`push_image.py` 或对应服务端依赖文件；
- 仓库中没有 `frontend/`、根目录 `package.json` 或完整管理后台源码；
- 语音识别、LLM、TTS、云端同步及 OTA 服务端不属于当前公开快照可独立复现的功能。

请只根据仓库中实际存在的代码和文档判断公开能力，不要将实验性 renderer、接口占位或模拟客户端视为完整产品功能。

## 目录结构

```text
.
├── docs/                         图片转换和局域网推图协议文档
├── firmware/                     ESP-IDF 固件、板级适配、RawDraw UI 与开发工具
├── server/
│   └── mock_client.py            外部兼容服务的协议模拟客户端
├── CONTRIBUTING.md               贡献与验证要求
├── LICENSE                       仓库根目录许可证
└── SECURITY.md                   安全与隐私说明
```

## 构建固件

### 前置条件

- Git；
- Python 3；
- ESP-IDF。组件清单声明最低版本为 `>=5.4.0`；近期合并的修复 PR 报告使用 ESP-IDF v6.0.0 编译通过，其他版本需自行验证；
- 支持数据传输的 USB 线仅在烧录或真机验证时需要。

### 基础编译

先加载 ESP-IDF 环境，再从 `firmware/` 目录构建：

```bash
cd firmware
source /path/to/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
```

Windows 用户请使用 ESP-IDF 提供的 PowerShell/Command Prompt 环境，或先运行对应的 `export.ps1`。

`idf.py build` 通过只证明代码可以编译，不等同于固件已经在 Note4C 真机上验证。

### 发布打包脚本的限制

`firmware/build.sh` 和 `firmware/scripts/release.py` 面向完整发布环境。当前脚本需要 `firmware/main/boards/zectrix-s3-epaper-4.2/config.json`，该文件不在公开快照中，因此不能把 `build.sh` 作为公开仓库的开箱即用构建入口。

请勿自行猜测或提交内部板型、OTA 地址、Wi-Fi 凭据及生产部署配置。若只需要验证公开源码，请使用上面的基础 `idf.py build` 路径。

## 固件配置

Kconfig 提供两种面板配置：

```text
ZECTRIX_EPD_PANEL_4COLOR_SSD2683  四色 BWRY SSD2683 面板
ZECTRIX_EPD_PANEL_1BPP            1bpp 黑白面板
```

Note4C 默认使用四色配置。修改面板类型后必须重新完整编译，并在烧录前再次确认设备型号。

## 图片与设备管理

设备端可以启动 SoftAP 或局域网 HTTP 服务接收图片。相关实现和协议说明位于：

- `firmware/main/ui/renderers/rawdraw/ap_transfer_server.cc`；
- `docs/LAN_PHOTO_PUSH_API.md`；
- `docs/inkscreen_image_converter.js`；
- `firmware/tools/`。

设备进入 SoftAP 模式后，通常可通过以下地址打开本地页面：

```text
http://192.168.4.1
```

局域网地址由设备获得的 IP 决定。不要把设备的本地 HTTP 服务直接暴露到公网。

## 开发约定

- 四色屏整屏刷新时间较长，交互设计应优先考虑低频更新、刷新合并和明确的等待状态；
- RawDraw 内部使用语义颜色与主题 token，业务页面不要直接散落裸颜色常量；
- 修改刷新、配网、电源或存储逻辑时，应明确区分编译验证、模拟验证和 Note4C 真机验证；
- 不要提交 `.env`、Wi-Fi 凭据、API Key、`firmware/build/`、`firmware/sdkconfig`、发布包或设备私有数据。

提交修改前请阅读 [CONTRIBUTING.md](CONTRIBUTING.md)。安全与隐私相关问题请阅读 [SECURITY.md](SECURITY.md)。

## 许可证与第三方组件

根目录源码、`firmware/` 及其第三方组件可能分别带有许可证和版权声明。使用、修改或再分发前，请同时检查：

- 根目录 `LICENSE`；
- `firmware/LICENSE`；
- 各组件目录中的 `LICENSE`、`license.txt`、`idf_component.yml` 和第三方 NOTICE。

保留原始版权和许可证声明，不要用根目录许可证覆盖第三方组件自己的许可条件。
