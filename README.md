# CarLifePort

CarLife 车机端（web 版）。服务端跑车机侧协议栈，浏览器用 WebCodecs 当屏幕。

参考 [playport](https://github.com/youcci/playport) 的分层方式：车机协议在服务端，浏览器只负责解码显示。
CarLife 协议栈用的是百度的开源车机库 [CarLifeVehicleLib](./vendor/carlife)（Apache-2.0）。

浏览器端**直接原样使用 playport 的前端**（`vendor/playport/web`，不改动一行），本项目只
实现它期望的那套后端接口。因此仓库里没有自己的 `web/` 目录。

## 架构

```
手机 ──USB(AOA)──► carlife-usb-bridge ──TCP(127.0.0.1)──► carlife-host ──Unix socket──► carlifed ──WebSocket──► 浏览器
      CarLife 协议      tools/usb-bridge      7200/8200/9200       host/            server/        playport 前端
                                            9201/9202/9300
```

四段职责：

| 组件 | 语言 | 职责 |
|---|---|---|
| `tools/usb-bridge` | C | AOA 握手、USB 帧与六条本地 TCP 通道互转 |
| `host` | C++ | 链接 `libcarlifevehicle`，跑协议栈，把回调数据转成帧 |
| `server` | Go | 会话状态机、H.264 拆包、WebSocket 与 HTTP 接口（对齐 playport 服务端） |
| `vendor/playport/web` | TypeScript | WebCodecs 解码、Web Audio 播放、输入上行（原样使用，不修改） |

车辆侧各通道固定端口（与库的 `SOCKET_*_LOCALHOST_PORT` 对齐）：

| 通道 | 车机侧 | 手机侧 |
|---|---|---|
| cmd | 7200 | 7240 |
| video | 8200 | 8240 |
| audio | 9200 | 9240 |
| tts | 9201 | 9241 |
| vr | 9202 | 9242 |
| touch | 9300 | 9340 |

## 构建

```sh
./scripts/build.sh          # 全部构建；已存在的原生库自动跳过
./scripts/build.sh --force  # 连原生库一起重建
./scripts/build.sh --native # 只构建原生库（host/Makefile 内部调用）
```

按顺序构建五段，产物均不进入版本库：

| 步骤 | 产物 |
|---|---|
| 原生库（protobuf 2.5.0 + CarLife 协议栈） | `.build/lib/libcarlifevehicle.dylib` |
| USB 桥接层 | `tools/usb-bridge/carlife-usb-bridge` |
| 车机协议栈宿主 | `host/carlife-host` |
| Go 后端 | `server/carlifed` |
| 前端（playport 原样构建） | `vendor/playport/web/dist/` |

原生库要编译 protobuf 与 69 个 C++ 源文件，首次构建需数分钟；产物已存在时跳过。
`build.sh` 会给 protobuf 2.5.0 打一个 3 行补丁（补上 `__aarch64__` 分支，
原版不认识 Apple Silicon），其余源文件原样编译。

## 运行

```sh
./scripts/run.sh
```

脚本依次做三件事：检查桌面版微信是否在运行（它独占 USB 设备，检测到会直接中止并
提示）→ 复位手机 USB 模式（清 accessory 残留、停掉 adb server）→ 启动
`carlife-usb-bridge`、`carlife-host`、`carlifed`。

三个进程的日志带 `[bridge]` / `[host]` / `[server]` 前缀输出到同一终端，Ctrl-C 停止全部。

然后浏览器打开脚本打印的任意一个地址。默认监听 `0.0.0.0:8080`，启动时会像 vite
一样把每张网卡的地址都打出来（带网卡名）：

```
  viewer URL   https://127.0.0.1:8080/?token=0
  viewer URL   https://192.168.40.2:8080/?token=0 (en0)
  certificate  .../self-signed.crt (self-signed: accept the browser warning once)
```

服务默认走 **HTTPS**，用的是启动时自动生成的自签证书（缓存在用户缓存目录，**生成一次
之后一直复用**，与 playport 一致）。之所以不能省掉 TLS：playport 前端用 WebCodecs 解码，
而 WebCodecs 只在安全上下文里可用，局域网 IP 走 http 时浏览器根本不给解码器，只有
localhost 例外。自签证书浏览器会报「不安全」，接受一次即可，本机自用不需要证书颁发机构。

浏览器在弹警告页之前会先中止一次连接，服务端会收到一个 `unknown_ca` 告警。这类握手错误
**不打印**（playport 也不打印，启动时那句提示已经说明要接受一次警告）；其它握手错误照常
打印。注意证书是按生成时的网卡地址签的，之后换网络导致某个地址不在证书里时，浏览器会再
要一次警告 —— 与 playport 行为一致。

地址里的 `token` 是 playport 前端要求的访问令牌，缺了它前端不会建立连接。
首次打开后令牌会存在 localStorage，之后不带 `?token=` 直接访问也可以；但令牌变了以后
localStorage 里还是旧值，需要用带新 `?token=` 的地址再打开一次。

### 服务端配置（环境变量）

监听地址、请求帧率与访问令牌由环境变量控制，不再是命令行参数；默认值就在 `server/main.go` 里，
`run.sh` 不重复声明。三者对 `./scripts/run.sh` 与直接运行 `server/carlifed` 都生效：

| 环境变量 | 默认值 | 说明 |
|---|---|---|
| `ADDR` | `0.0.0.0:8080` | HTTP 监听地址 |
| `FPS` | `60` | 请求手机用的帧率（手机可能上限更低，见后文） |
| `TOKEN` | `0` | 浏览器访问令牌 |

其余选项（`--host-socket` / `--web` / `--device-name` / `--width` / `--height` / `--tls`）
仍是命令行参数，`carlifed -h` 可查。

默认对外可访问（局域网内其它机器也能打开这个页面，上面就是手机画面和控制通道，
保护只有这一层 token）。要限制成本机访问：

```sh
ADDR=127.0.0.1:8080 ./scripts/run.sh      # 或 ADDR=127.0.0.1:8080 server/carlifed
```

只在本机用、又不想看证书告警时，可以退回明文 http（仅 localhost 有安全上下文，
所以只有本机能用）：

```sh
ADDR=127.0.0.1:8080 server/carlifed --tls=false
```

### 手机什么时候插都行

| 时机 | 行为 |
|---|---|
| 启动前已插好 | 桥接层直接握手（accessory 残留已清），走完整 51/52/53 |
| 启动后再插 | 桥接层轮询等待设备，插上即握手；宿主一直等 `127.0.0.1:7200`，不会超时退出 |
| 运行中重新插拔 | 桥接层退出 → `run.sh` 重建 bridge 与 host → 重新握手出新会话 |

三种情况下浏览器都不用刷新：`carlifed` 常驻，WebSocket 断开后自动重连；会话重建后
前端会拿到新的编解码参数并重建解码器。

重新插拔之所以要重建进程而不是原地重连：CarLifeVehicleLib 只在 `connectionSetup()`
里建立六条通道，而 `createCmdSocket()` 是直接覆盖指针、不关闭旧 socket，同一进程内
重复调用会留下悬空 fd 和阻塞在旧 fd 上的接收线程；手机侧每次 accessory 附着也都是
一个新会话。

`carlife-host` 因此不调用 `CCarLifeLib::connectionSetup()`，改为直接调
`CConnectManager::createXSocket()` 系列。`connectionSetup()` 会先跑六条
`adb forward`，而本项目不用 adb（USB 归桥接层管），这两点都有实际危害：adb server
会被重新拉起并占住手机，使桥接层下次 claim 报 `LIBUSB_ERROR_ACCESS`；`popen()` 拉起
的 adb 子进程还会继承宿主的监听 socket，宿主退出后 socket 仍被持有，`carlifed` 会连上
一个没人 accept 的僵尸 socket 且永不重连。宿主自身的 socket 也加了 `FD_CLOEXEC` 兜底。

### 单独运行某个组件

```sh
tools/usb-bridge/carlife-usb-bridge --list   # 枚举 USB 设备，并显示挑中的是哪个
tools/usb-bridge/carlife-usb-bridge          # 只跑桥接层
host/carlife-host -v                         # 只跑宿主；默认静音，-v 保留库日志
server/carlifed --host-socket /tmp/carlife-host.sock \
                --web vendor/playport/web/dist \
                --width 1280 --height 720
```

上面这条 `carlifed` 用的是默认的 `ADDR` / `FPS` / `TOKEN`；要改就加前缀，例如
`ADDR=127.0.0.1:8080 server/carlifed ...`。

帧率默认 60，用环境变量覆盖：`FPS=30 ./scripts/run.sh`。实际帧率由手机
决定，后端每 5 秒打印一行实测值（`video: 29.8 fps received ...`），手机把帧率压低时也
会打印（`phone limited the frame rate to 30`）。

### 车机鉴权（当前停用）

协议里有这条流程，手机对没走完的车机会提示「未认证的车机端」并在几分钟后断开：

```
车机 --MSG_CMD_HU_AUTHEN_REQUEST(randomValue)--> 手机
手机 --MSG_CMD_MD_AUTHEN_RESPONSE(encryptValue)--> 车机
车机 --MSG_CMD_HU_AUTHEN_RESULT(authenResult)--> 手机
手机 --MSG_CMD_MD_AUTHEN_RESULT(authenResult)--> 车机
```

曾经由 `carlifed` 在协议版本匹配后发起（未确认前每 30 秒重发一次），但真机上
**一发就立刻被判未认证**，比不问还糟：手机要校验的是 `encryptValue` 里的证书，开源车机库
没有百度签发的证书，自己上报的 `authenResult` 骗不过去。所以两处调用点先注释掉了
（`server/session.go` 的 `requestAuthentication()`，恢复时把注释放开即可）。
`carlife-host` 侧的鉴权收发能力保留未动（两个回调 + `CMD_AUTHEN_REQUEST` 分支）：
Go 侧不发这条命令，宿主就不会自己发起鉴权。（注意这只针对鉴权；身份上报是另一回事，
见下一节。）

### 车机身份上报

真车机在发完协议版本后还会紧接着上报两条身份信息，顺序如下（取自真车机抓包）：

```
车机 --MSG_CMD_HU_PROTOCOL_VERSION(0x00018001)--> 手机
车机 --MSG_CMD_STATISTIC_INFO(0x00018027)--> 手机    含渠道号 channel
车机 --MSG_CMD_HU_INFO(0x00018003)--> 手机
车机 --MSG_CMD_VIDEO_ENCODER_INIT(0x00018007)--> 手机
```

开源库自带的测试程序这两条都没发。**但它们不只是统计信息**：指南 17.2 写明手机端靠
`MSG_CMD_STATISTIC_INFO` 里的渠道号判断车机是否合法，校验通过才回 `MSG_CMD_MD_AUTHEN_RESULT`
并**启动车厂相关的定制服务**；而手机的 `MSG_CMD_MD_FEATURE_CONFIG_REQUEST(0x00010051)` 只在
「双方握手认证通过后」才发。真机日志里始终没有 `replied to phone feature config request`
这一行，正是这条链没走通的表现：我们回了功能定制项（含音频传输方式 `AUDIO_TRANSMISSION_MODE`），
手机却从来没来问过。

渠道号取百度签发的真值拿不到，这里用开源车机端自己的默认值
`CommonParams.VEHICLE_CHANNEL_AFTER_MARKET = "20000000"`（后装市场渠道），`--channel` 可覆盖。
`cuid` 指南要求每台车机唯一（重复会让手机主动断开连接），默认 `CarLifePort`，`--cuid` 可覆盖。

上一轮曾把整段用 `#if 0` 停掉做排除（当时判断「发了也没用」），现在恢复：在「未上报任何身份」的
状态下手机既不主动断开、也不是没声音的原因，却静默丢掉了功能定制这条链。恢复后若手机一连上就
断开，说明手机拒绝了这个渠道号，退回停用状态即可。

## 与 playport 前端的接口对齐

`server/` 实现的正是 playport 服务端的那套契约，前端因此可以直接用：

| 接口 | 内容 |
|---|---|
| `GET /ws?token=` | 令牌不符直接按策略违规关闭。接入后先发 `hello`，再回放已缓存的 video/audio 配置，然后才把该 viewer 计入广播 |
| `GET /api/status` | deviceName / sessionActive / phone / viewers / display |
| `GET /api/display` | width / height / fps / uiScale / hevc / orientation / presets |
| `POST /api/display?token=` | 改分辨率与帧率，返回 `{"ok":true}` 或 `{"error":...}`；HEVC 与非法参数会被拒绝 |

控制消息沿用 `WebHub.kt` 的 `ClientMessage` / `ServerMessage` 定义。CarLife 没有自己的旋钮，
按下表映射（键值取指南表 10.6-1，与开源车机端 `CommonParams.KEYCODE_*` 一致）：

| 前端消息 | CarLife 侧 |
|---|---|
| `touch` 的 contacts | 取 id 最小的那个触点，按按下状态映射 DOWN / MOVE / UP；空列表即 UP，坐标沿用最后位置（CarLife 只支持单点） |
| `knob.select` / `home` / `back` | 硬按键 ok(0x14) / main(0x1E) / back(0x0E) |
| `knob.x` 方向 | 硬按键 move left(0x15) / move right(0x16) |
| `knob.y` 方向 | 硬按键 selector previous(0x07) / selector next(0x06) |
| `media` 3 / 4 / 5 | 硬按键 media start(0x1F) 与 media stop(0x20) 交替 / seek add(0x10) / seek sub(0x0F) |
| `telephony` 1 / 2 | 硬按键 phone call(0x02) 或 phone end(0x03)，按手机上报的通话状态选一个 / mute(0x13) |
| `siri` | 硬按键 VR start(0x21) 与 VR stop(0x22) 交替 |
| `keyframe` | 丢弃：CarLife 没有「请求关键帧」命令 |
| `night` | 丢弃：CarPlay 专属，CarLife 无对应 |
| `ping` | 回 `{"type":"pong"}` |
| 二进制帧（麦克风上行） | 丢弃：CarLife 的语音上行由手机发起 |

几个容易踩错的地方：

- **方向键分两族**。`selector previous/next` 是「在当前小区域里换焦点」（等于转旋钮），
  `move left/right` 是「跳到左右相邻的小区域」。`move up/down` 协议里也有，但手机在列表界面上不响应
  （实测发 0x17/0x18 无反应，发 0x07/0x06 焦点会上下移动），而 selector 没有横向版本，所以纵轴用
  selector、横轴用 move。
- **`home` 用 `main(0x1E)`**。`KEYCODE_Home(0x01)` 的含义是「按下手机的 Home 键」，会把 CarLife 切到
  后台；「回到 CarLife 主界面」是 `KEYCODE_MAIN`，开源车机端的 Home 按钮发的也是它
  （`ControlTestWindow`）。
- **`media` 3 不能用 `KEYCODE_Media(0x09)`**。那一条的含义是「进入手机端 CarLife 的音乐播放界面」，
  按下去只跳界面（实测如此）；播放/暂停要用 `media start(0x1F)` / `media stop(0x20)`。上一首/下一首用
  `seek sub/add`，开源车机端把 Android 的 `KEYCODE_MEDIA_PREVIOUS/NEXT` 映射到这两条
  （`MediaButtonReceiver`），不要用 `selector`。
- **`siri` 只解决开关**。`VR start/stop` 能打开和关闭语音识别界面，但识别本身要麦克风：功能定制项里
  我们报的是 `VOICE_MIC=0`（用车机麦克风），手机据此等 VR 通道上的 PCM，而我们不发（`vr ↑0`），
  语音输入仍不通。要打通得改报 `VOICE_MIC=1`（用手机麦克风）或实现 VR 通道上行。

前端的 contacts 只在手指按下期间存在：`pointerup` 把该触点从列表里直接删掉，而不是补发一条
`down` 为假的记录（见 `input.ts` 的 `scheduleFlush`），所以抬手一定表现为「这个槽位在消息里
消失了」。抬手事件本身不带坐标，只能沿用该槽位最后一次上报的位置。填原点等于告诉手机手指跳到了
左上角，手机会把手势判为离开控件、取消这次按压，于是按钮按下与抬起的视觉都在、功能不生效。
服务端因此记住每个触点最后发送的设备像素坐标，抬手时按原位置发 UP；拖动过的手指抬手也落在
拖动后的位置。

音频流名走 wire 传到前端直接作为音量滑块标签，所以用英文 `Media` / `Navigation`。

## 已知限制

- 只实现有线（AOA）通路，无线 CarLife 未实现。
- 触摸是单点，CarLife 协议本身不支持多指：前端的多点触点只有 id 最小的那个生效。
- 麦克风上行未接通：CarLife 的语音通道由手机发起，服务端不向浏览器请求麦克风。
- 视频只有 H.264；音频按 16 位 PCM 处理。
- 帧率由手机决定：请求 60 fps 时手机可能仍按 30 fps 编码，后端只能报告实测值。
- 车机主动发起的鉴权（17.1）仍停用：一发就会被手机立刻判定未认证，比不问还糟，因为手机要校验
  `encryptValue` 里的证书，开源库没有百度签发的证书。身份上报（17.2）已恢复，渠道号用开源车机端
  的默认值；手机是否接受这个值只能上机验证。手机侧的认证判定要彻底过掉，只能拿到百度签发的真值、
  逆向手机端 APK，或换用官方认证过的车机端实现。
- 重新插拔要重建 `carlife-host`，`run.sh` 会自动做；手工单独运行时需自己重启。
- 手机侧 CarLife App 是系统在 accessory 附着时拉起的，比链路就绪慢几秒。`carlifed`
  每 5 秒重发一次协议版本直到会话建立，用来覆盖这段延迟；一直连不上手机时会持续重发。
