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
`build-native.sh` 会给 protobuf 2.5.0 打一个 3 行补丁（补上 `__aarch64__` 分支，
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
  viewer URL   http://127.0.0.1:8080/?token=carlife
  viewer URL   http://192.168.40.2:8080/?token=carlife (en0)
```

地址里的 `token` 是 playport 前端要求的访问令牌，缺了它前端不会建立连接。
首次打开后令牌会存在 localStorage，之后不带 `?token=` 直接访问也可以。

默认对外可访问（局域网内其它机器也能打开这个页面，上面就是手机画面和控制通道，
保护只有这一层 token）。要限制成本机访问：

```sh
ADDR=127.0.0.1:8080 ./scripts/run.sh      # 或 carlifed --addr 127.0.0.1:8080
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
server/carlifed --addr 127.0.0.1:8080 \
                --host-socket /tmp/carlife-host.sock \
                --web vendor/playport/web/dist \
                --token carlife \
                --width 1280 --height 720 --fps 60
```

`run.sh` 里帧率默认 60，可用环境变量覆盖：`FPS=30 ./scripts/run.sh`。实际帧率由手机
决定，后端每 5 秒打印一行实测值（`video: 29.8 fps received ...`），手机把帧率压低时也
会打印（`phone limited the frame rate to 30`）。

### 车机鉴权

手机会要求车机走完鉴权流程，没走完就会提示「未认证的车机端」并在几分钟后断开：

```
车机 --MSG_CMD_HU_AUTHEN_REQUEST(randomValue)--> 手机
手机 --MSG_CMD_MD_AUTHEN_RESPONSE(encryptValue)--> 车机
车机 --MSG_CMD_HU_AUTHEN_RESULT(authenResult)--> 手机
手机 --MSG_CMD_MD_AUTHEN_RESULT(authenResult)--> 车机
```

`carlifed` 在协议版本匹配成功后发起该流程，并在未收到手机确认前每 30 秒重发一次；
`carlife-host` 收到手机响应后直接上报通过（开源车机库没有百度证书，`encryptValue`
无法校验，而这条流程里上报结果的一方本来就是车机）。日志里会看到
`phone authenticated this head unit`。

### 车机身份上报

真车机在发完协议版本后还会紧接着上报两条身份信息，顺序如下（取自真车机抓包）：

```
车机 --MSG_CMD_HU_PROTOCOL_VERSION(0x00018001)--> 手机
车机 --MSG_CMD_STATISTIC_INFO(0x00018027)--> 手机    含渠道号 channel
车机 --MSG_CMD_HU_INFO(0x00018003)--> 手机
车机 --MSG_CMD_VIDEO_ENCODER_INIT(0x00018007)--> 手机
```

开源库自带的测试程序这两条都没发（`cmdHuAuthenRequest` 在它里面也是注释掉的），
本项目已补上。`MSG_CMD_STATISTIC_INFO` 里的 `channel` 就是百度发给前装厂商的**渠道号**，
CarLife 车机端 V2.0 SDK 明确写了它在进入 ESTABLISHED 之前会被校验、失败触发
`onConnectionAuthenFailed()` —— 所以「未认证的车机端」很可能就是这条没上报或渠道号无效。

默认值只是占位（`CarLife`），拿到真值后这样覆盖：

```sh
CHANNEL=xxxxx ./scripts/run.sh          # 或直接 carlife-host --channel xxxxx
```

配套还有 `--cuid`（车机 ID，默认 `CarLifePort`）。日志里会看到
`sent statistics info (channel=...)` 与 `sent head unit info (...)`。

需要说明的是：我们拿不到百度签发的渠道号，也没有公开的逆向结果可用，所以**这一步
只能消除「没上报」这个原因**，无法保证手机一定判定为已认证。

## 与 playport 前端的接口对齐

`server/` 实现的正是 playport 服务端的那套契约，前端因此可以直接用：

| 接口 | 内容 |
|---|---|
| `GET /ws?token=` | 令牌不符直接按策略违规关闭。接入后先发 `hello`，再回放已缓存的 video/audio 配置，然后才把该 viewer 计入广播 |
| `GET /api/status` | deviceName / sessionActive / phone / viewers / display |
| `GET /api/display` | width / height / fps / uiScale / hevc / orientation / presets |
| `POST /api/display?token=` | 改分辨率与帧率，返回 `{"ok":true}` 或 `{"error":...}`；HEVC 与非法参数会被拒绝 |

控制消息沿用 `WebHub.kt` 的 `ClientMessage` / `ServerMessage` 定义。CarLife 没有旋钮、
Siri 和夜间模式，按下表映射：

| 前端消息 | CarLife 侧 |
|---|---|
| `touch` 的 contacts | 取 id 最小的那个触点，按按下状态映射 DOWN / MOVE / UP；空列表即 UP（CarLife 只支持单点） |
| `knob.select` / `home` / `back` | 硬按键 ok(0x14) / home(0x01) / back(0x0E) |
| `knob.x` / `knob.y` 方向 | 硬按键 left / right / up / down |
| `media` 3 / 4 / 5 | 硬按键 media(0x09) / next(0x06) / prev(0x07) |
| `keyframe` | 丢弃：CarLife 没有「请求关键帧」命令 |
| `night` / `telephony` / `siri` | 丢弃：CarPlay 专属，CarLife 无对应 |
| `ping` | 回 `{"type":"pong"}` |
| 二进制帧（麦克风上行） | 丢弃：CarLife 的语音上行由手机发起 |

音频流名走 wire 传到前端直接作为音量滑块标签，所以用英文 `Media` / `Navigation`。

## 已知限制

- 只实现有线（AOA）通路，无线 CarLife 未实现。
- 触摸是单点，CarLife 协议本身不支持多指：前端的多点触点只有 id 最小的那个生效。
- 麦克风上行未接通：CarLife 的语音通道由手机发起，服务端不向浏览器请求麦克风。
- 视频只有 H.264；音频按 16 位 PCM 处理。
- 帧率由手机决定：请求 60 fps 时手机可能仍按 30 fps 编码，后端只能报告实测值。
- 车机鉴权由车机端自行上报通过，开源库没有百度证书，无法校验手机返回的
  `encryptValue`。`MSG_CMD_STATISTIC_INFO` 里的渠道号同理：没有百度签发的值，只能用占位
  符。若手机仍判定未认证（跑一段时间后提示「未认证的车机端」并断开），说明卡在证书/渠道
  号这一层，只能拿到真值或换用官方认证过的车机端实现。作为调试用途，能连上的那段时间
  足够验证画面与操控。
- 重新插拔要重建 `carlife-host`，`run.sh` 会自动做；手工单独运行时需自己重启。
- 手机侧 CarLife App 是系统在 accessory 附着时拉起的，比链路就绪慢几秒。`carlifed`
  每 5 秒重发一次协议版本直到会话建立，用来覆盖这段延迟；一直连不上手机时会持续重发。
