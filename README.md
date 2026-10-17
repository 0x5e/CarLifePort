# CarLifePort

CarLife 车机端（web 版）。服务端运行车机侧协议栈，浏览器用 WebCodecs 解码显示。

分层参考 [playport](https://github.com/youcci/playport)：车机协议在服务端，浏览器只负责解码显示。
CarLife 协议栈使用百度的开源车机库 [CarLifeVehicleLib](./vendor/carlife)（Apache-2.0）。

浏览器端直接使用 playport 的前端（`vendor/playport/web`，不做任何修改），本项目只实现它所需的
后端接口，因此仓库中没有自己的 `web/` 目录。

## 架构

```
手机 ──USB(AOA)──► carlife-link ──Unix socket──► carlifed ──WebSocket──► 浏览器
      CarLife 协议   link/           /tmp/carlife-link.sock  server/     playport 前端
```

三段职责：

| 组件 | 语言 | 职责 |
|---|---|---|
| `link` | C / C++ | USB 传输（AOA 握手、USB 帧与六条本地 TCP 通道互转）与 CarLife 车机协议栈（链接 `libcarlifevehicle`） |
| `server` | Go | 会话状态机、H.264 拆包、WebSocket 与 HTTP 接口（对齐 playport 服务端） |
| `vendor/playport/web` | TypeScript | WebCodecs 解码、Web Audio 播放、输入上行（原样使用，不修改） |

`carlife-link` 是一个进程，内部两个子系统各占一半：`bridge`（USB 传输，`link/aoa.c` 与
`link/bridge.c`）与 `stack`（协议栈，`link/main.cpp`）。两者之间不是进程边界，而是六个本地
TCP 通道——与库自身在 ADB 模式下使用的方式一致：

```
手机 ◄─USB(AOA)─► bridge ◄─TCP 127.0.0.1─► CarLifeVehicleLib ◄─► stack
```

车辆侧各通道固定端口（与库的 `SOCKET_*_LOCALHOST_PORT` 对齐，由 `bridge` 监听、库连接）：

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
make all     # 构建全部组件
make sdk     # 只构建原生库（CarLife SDK）
make clean   # 删除 dist/ 与 .build/
```

构建是增量的：产物只在自己的输入更新时才重建，改一个文件就只重编那一个；只有 `make clean` 会强制
全量重建。可运行产物全部在 `dist/`，中间产物在 `.build/`，两者都不进入版本库。

| 目标 | 内容 | 产物 |
|---|---|---|
| `make all` | 下列四项，一次运行内完成 | — |
| `make sdk` | 原生库：protobuf 2.5.0 + CarLife 协议栈 | `dist/libcarlifevehicle.dylib` |
| `make link` | 链接进程（USB 传输 + 车机协议栈） | `dist/carlife-link` |
| `make server` | Go 后端 | `dist/carlifed` |
| `make web` | 前端（playport 原样构建后复制） | `dist/web/` |
| `make clean` | 删除 `dist/` 与 `.build/` | — |

首次全量构建可以加 `-j`（`make -j8 all`）并行编译原生库的 69 个源文件。

全部产物放在同一目录：`carlifed` 按自身所在目录查找链接进程与前端，因此整个 `dist/`
可以移动到其他位置运行。原生库使用 `@rpath` 安装名，链接进程使用 `@loader_path` rpath 定位同级
目录中的 dylib，二进制中不包含构建路径。

原生库的 69 个 C++ 源文件各自编译成 `.build/obj` 下的目标文件，按源文件与头文件的时间戳增量重建
（编译时用 `-MMD` 记录每个目标文件包含了哪些头文件）。protobuf 安装进 `.build/pb`，只在库文件缺失
或打补丁后才重新 configure 与编译；它是打了补丁的上游源码包、不随本仓库改动，补丁内容为 3 行，给
`platform_macros.h` 补上 `__aarch64__` 分支（上游版本不支持 Apple Silicon），补丁脚本为
`scripts/patch-protobuf.py`，其余源文件原样编译。

`make clean` 保留 `vendor/playport/web/node_modules`：它是前端依赖缓存，删除后下一次构建需要重新
联网安装。

## 运行

```sh
dist/carlifed
```

一条命令启动全部组件。`carlifed` 启动后把 `carlife-link` 作为子进程拉起，它在退出时被重启
（重新插拔手机即由此触发：USB 读失败使链接进程退出）。

两个进程的日志输出到同一终端，格式与级别见「日志」。Ctrl-C 停止全部。

浏览器打开 `carlifed` 打印的任意一个地址。默认监听 `0.0.0.0:8080`，启动时列出每张网卡的地址
（带网卡名），Docker、虚拟机创建的 `bridge` 网卡不在其中：

```
17:08:35.984 INFO  [carlifed]   service started
17:08:35.984 INFO  [carlifed]   viewer URL   https://127.0.0.1:8080/?token=0 (lo0)
17:08:35.984 INFO  [carlifed]   viewer URL   https://192.168.40.2:8080/?token=0 (en0)
```

服务使用 **HTTPS**，证书为启动时生成的自签名证书（缓存在用户缓存目录，生成后复用，
与 playport 一致）。TLS 不可省略：playport 前端使用 WebCodecs 解码，而浏览器只在安全上下文中
提供 WebCodecs，通过局域网 IP 以 http 访问时不会提供解码器，localhost 除外。自签名证书会触发
一次浏览器安全警告，接受即可，本机使用不需要证书颁发机构。

浏览器在显示警告页之前会中止一次连接，服务端因此收到一次 `unknown_ca` 告警；这类握手错误
不打印（与 playport 一致，启动日志已说明需要接受一次警告），其它握手错误照常打印。证书按生成
时的网卡地址签发，更换网络后某个地址不在证书中时会再次出现警告，与 playport 行为一致。

URL 中的 `token` 是 playport 前端要求的访问令牌，固定为 `0`：它是前端接口的一部分，不是可配置项，
也不构成访问控制。令牌在首次打开后存入 localStorage，其后可以不带 `?token=` 访问。

### 配置

`carlifed` 的配置只通过 `CL_` 前缀的环境变量传入，没有命令行参数，`dist/carlifed --help`
列出全部项，默认值定义在 `server/main.go`。

| 环境变量 | 默认值 | 说明 |
|---|---|---|
| `CL_VIDEO_MODE` | `1280x720@60` | 请求手机的视频模式，格式为 `<宽>x<高>@<帧率>` |
| `CL_WEB_ADDR` | `0.0.0.0:8080` | HTTPS 监听地址（默认对局域网可访问） |
| `CL_WEB_DIR` | `<carlifed 目录>/web` | 前端静态资源目录 |
| `CL_CHANNEL` | `20062100` | 上报给手机的渠道号（见「车机鉴权」） |
| `CL_LOG_LEVEL` | `info` | 日志级别：`debug` / `info` / `warn` / `error` |
| `CL_LINK` | `<carlifed 目录>/carlife-link` | 要拉起的链接进程可执行文件 |

`CL_LINK` / `CL_WEB_DIR` 按给定值使用，相对路径相对于当前工作目录。视频模式与监听地址以外的行为
不可配置：服务总是使用自签名证书的 HTTPS，总是拉起并重启链接进程，链接进程监听的 Unix socket
固定为 `/tmp/carlife-link.sock`；访问令牌固定为 `0`，车机名与车机 id 都取本机（hostname 与 host uuid）。

链接进程由 `carlifed` 组装参数拉起（`--socket` / `--channel` / `--cuid`），其全部命令行参数如下：

| 参数 | 默认值 | 说明 |
|---|---|---|
| `--socket <path>` | 无（必填） | 链接进程监听的 Unix socket 路径 |
| `--channel <name>` | 无（必填） | 上报给手机的渠道号 |
| `--cuid <id>` | 无（必填） | 上报给手机的车机 id |
| `--host <address>` | `127.0.0.1` | 六条本地通道端口的监听地址 |
| `--vid` / `--pid` | 自动挑选 | 指定目标设备的 VID / PID |
| `--list` | 关 | 只枚举 USB 设备，不操作任何设备 |
| `-q` / `--quiet` | 关 | 不打印协议栈的运行日志 |
| `-v` / `--verbose` | 关 | 额外输出协议栈内部日志（默认丢弃） |

三个必填参数的取值只来自 `carlifed`（渠道号是 `CL_CHANNEL`，车机 id 取本机 host uuid，socket 路径
是内部常量），链接进程不重复定义；缺任一个都会直接报错退出，`--list` 不需要它们。

周期性的转发统计默认打印但标记为 debug，由 `carlifed` 按级别过滤。

### 日志

两个进程的输出汇入同一终端，格式为 `时间 级别 [模块] 内容`：

```
17:08:35.984 INFO  [carlifed]   service started
17:08:36.415 INFO  [bridge]     waiting for a phone
17:08:36.422 INFO  [stack]      listening on /tmp/carlife-link.sock
17:08:37.987 INFO  [link]       connected to carlife-link: /tmp/carlife-link.sock
17:08:41.203 INFO  [session]    phone ready, video 1280x720@60, starting encoder
17:08:41.208 DEBUG [session]    video: 59.8 fps received (299 frames, 3612.4 KB/s) in the last 5s, requested 60 fps
```

输出到终端时带颜色：时间为灰色，级别按严重程度着色（`DEBUG` 白、`INFO` 绿、`WARN` 黄、`ERROR` 红），
模块与内容保持终端默认颜色。stderr 不是终端时（重定向到文件或管道）不上色，避免转义序列混入日志文件。

模块对应到实现：

| 模块 | 来源 |
|---|---|
| `carlifed` | 启动、关闭与配置（`server/main.go`） |
| `supervisor` | 链接进程的拉起和重启 |
| `link` | 与链接进程的 Unix socket 连接（`server/linkclient.go`） |
| `session` | CarLife 会话状态机：握手、视频、音频、按键 |
| `viewer` | 浏览器连接与 HTTP/WebSocket 接口 |
| `bridge` / `stack` | 链接进程的两半：USB 传输与协议栈，其输出由 `carlifed` 转发 |

链接进程在一个流里写两个子系统的日志，因此每行都在级别之后写出所属子系统（`bridge` 或
`stack`），`carlifed` 读回来放进模块列。不属于这两者的行（例如动态链接器自身的报错）记为 `link`。

级别由 `CL_LOG_LEVEL` 选择（默认 `info`）：

| 级别 | 内容 |
|---|---|
| `debug` | 以上全部，外加周期性遥测（实测帧率、USB 传输的每通道转发统计、手机的播放进度）、逐次下发的按键与触摸，以及协议栈转交后端后重复记下的那一份事件日志 |
| `info` | 状态变化：握手推进、会话开始与结束、客户端接入、链接进程重建 |
| `warn` | 可以继续运行但需要留意：令牌不符、握手停滞、手机拒绝认证、车机对手机的鉴权校验未通过、帧率被限制、设备就绪前的握手重试、被丢弃的帧或连接、未识别的通道 tag |
| `error` | 失败并中断：USB 与本地 socket 的读写失败、内存与线程创建失败、绑定或监听失败、六条通道建立失败、链接进程无法启动 |

级别在源头标记，而不是由 `carlifed` 按内容识别：链接进程在自身时间戳后写入 `DEBUG` / `WARN` /
`ERROR` 标记（`aoa_log_debug`、`aoa_log_warn`、`aoa_log_error`、`log_line_debug`、`log_line_warn`、
`log_line_error`；不写标记即 `info`），`carlifed` 读回后放进自己的级别列，低于 `CL_LOG_LEVEL` 的行
在转发时丢弃。因此这些内容在 `CL_LOG_LEVEL=debug` 下可见，而链接进程单独运行时也总能看到。

判据是「这件事之后还能不能继续跑」：会重试的一步、被丢弃的帧或连接、不认识的事件标为 `warn`；
操作被放弃、或者链路要重建、进程要退出时标为 `error`。所以一行文本里出现 `failed` 并不等于
`error`——设备就绪前反复重试的 `51/52/53 failed` 是 `warn`，而 `USB receive loop ended (error)`
是 `error`。

标记的还有两类。一类是重复：协议栈每收到一个事件都在本地记一行，同时把事件当帧转给 `carlifed`，
后者再记一行同义的日志，于是默认级别下同一件事出现两次。凡属这种情况，协议栈那行一律标为
`DEBUG`：每个事件只在真正解释它的那一侧（`session`）留下一条 `info`，链接进程单独运行时仍能看到
原始回调。协议栈独有的信息不在其列——蓝牙配对、导航播报、媒体信息、身份上报、六个通道的参数等
仍按 `info` 输出。手机自己重发的事件同理：未上报新车机版本的车型每次开始播放都会收到两遍音频
参数（手机分两处发送），重复的那一遍记 `DEBUG`。

另一类是逐次下发的输入命令：硬按键与触摸（action / down / up / move / click / long press）每发出
一次就记一行 `DEBUG`。它们频率高、内容重复，只在下行排查时才有用。

### 设备接入时机

| 时机 | 行为 |
|---|---|
| 启动前已插好 | 链接进程直接握手，走完整 51/52/53，然后才连接库 |
| 启动后再插 | 链接进程在拿到设备前一直轮询等待，插上即握手；`carlifed` 在此之前一直重试连接它的 socket |
| 运行中重新插拔 | USB 读失败 → 链接进程退出 → `carlifed` 重建 → 重新握手出新会话 |

三种情况下浏览器都不需要刷新：`carlifed` 常驻，WebSocket 断开后自动重连；会话重建后前端
会拿到新的编解码参数并重建解码器。

等待期间的重试与等待提示每段只打印一次（`link` 的 `waiting for carlife-link`、`bridge` 的
`no phone yet, waiting` 与 `device not ready, retrying`）：重试间隔只有 1–2 秒，逐次打印会把
日志刷满，而这段时间里状态没有变化。与设备的连接建立、断开与失败原因仍然逐次记录。

accessory 模式在手机侧是粘性的，跨重新插拔保留。手机仍处于上一次连接的 accessory 模式时，
重新握手不会触发系统重新拉起 CarLife App（仅当从常规模式完成一次全新握手时，系统才广播
`USB_ACCESSORY_ATTACHED`）。这种情况需要先手动复位手机的 USB 模式：

```sh
adb shell svc usb setFunctions     # 空参数 = charging，清除 accessory 模式
```

重新插拔要重建进程而不是原地重连：CarLifeVehicleLib 只在 `connectionSetup()` 中建立六条通道，
而 `createCmdSocket()` 直接覆盖指针、不关闭旧 socket，同一进程内重复调用会留下悬空 fd 和阻塞在
旧 fd 上的接收线程；手机侧每次 accessory 附着也都是一个新会话。

`carlife-link` 因此不调用 `CCarLifeLib::connectionSetup()`，改为直接调用
`CConnectManager::createXSocket()` 系列。`connectionSetup()` 会先执行六条 `adb forward`，而本项目
不使用 adb（USB 由链接进程的传输半边管理），两者都有实际危害：adb server 会被重新拉起并占用手机，
使传输层的下一次 claim 报 `LIBUSB_ERROR_ACCESS`；`popen()` 拉起的 adb 子进程会继承链接进程的监听
socket，进程退出后 socket 仍被持有，`carlifed` 会连接到一个无人 accept 的 socket 且不重连。
链接进程自身的 socket 也加了 `FD_CLOEXEC` 兜底。

### 单独运行某个组件

```sh
dist/carlife-link --list      # 只枚举 USB 设备，不操作任何设备
dist/carlife-link --socket /tmp/carlife-link.sock --channel 20062100 --cuid "$(uuidgen)"
                              # 完整的链接进程；三个参数必填，cuid 手工调试时随便给一个唯一值
dist/carlife-link --socket /tmp/carlife-link.sock --channel 20062100 --cuid "$(uuidgen)" -v
                              # 同上，并保留协议栈内部日志
```

链接进程接受上表的全部参数（见「配置」），正常情况下由 `carlifed` 组装，只有手工调试时才直接运行。
`carlifed` 总是拉起链接进程，因此后端无法单独运行；手工跑完整链路时要自己给出 socket 路径，
`carlifed` 用的是 `/tmp/carlife-link.sock`。

实际帧率由手机决定，后端以 debug 级别每 5 秒打印一行实测值（`video: 29.8 fps received ...`），
手机限制帧率时以 info 级别打印（`phone limited the frame rate to 30`）。

### 车机鉴权

指南第 17 章把「安全认证」拆成方向相反的两段，两段都已实现，且互不依赖：

```
车机 --MSG_CMD_HU_AUTHEN_REQUEST(0x00018048, randomValue)--> 手机   ┐
手机 --MSG_CMD_MD_AUTHEN_RESPONSE(0x00010049, encryptValue)--> 车机  │ 17.1 车机验证手机
车机 --MSG_CMD_HU_AUTHEN_RESULT(0x0001804A, authenResult)--> 手机   ┘
手机 --MSG_CMD_MD_AUTHEN_RESULT(0x0001004B, authenResult)--> 车机    17.2 手机验证车机
```

**17.1（车机验证手机）** 由 `carlifed` 在协议版本匹配后发起，手机确认前每 30 秒重发一次。
`randomValue` 是 16 个十六进制字符的随机串 + `_` + 版本号（`link/main.cpp` 的 `kAuthenVersion`，
当前 `1.1.0`）。后缀不能省：手机端按 `_` 切出版本段、逐段与自身版本比较，没有 `_` 会在内部抛异常
并被外层 `catch` 吞掉，**整条 `MSG_CMD_MD_AUTHEN_RESPONSE` 都不会发出**。要求低于自身版本时
（`1.1.0` 对 8.9.0）手机把收到的字符串原样签回，车机因此可以直接与发出的值比对；要求高于自身版本
时手机会改写成 `<随机串>_00.00.00` 再签回，比对必然失败——这是 `kAuthenVersion` 不能随手调高的原因。

校验在 `link/md_authen.c`：用手机端内置私钥对应公钥对 `encryptValue` 做 RSA 公钥运算
（1024 位、e=65537、PKCS#1 v1.5），比对还原出的明文。公钥不在开源车机库里，取自手机端 APK
（逆向报告 `CARLIFE_AUTH.md` 1.1 节；该 apk 内没有公钥文件，公钥只存在于车机端）。校验的两步都走
libcrypto：`EVP_DecodeUpdate` 解码协议里的 Base64，`EVP_PKEY_verify_recover` 配 `RSA_PKCS1_PADDING`
一次完成公钥运算与 PKCS#1 去填充——块内填充不合法或块不小于模数都直接失败。公钥在源码里就是那份 PEM
字符串（与逆向项目 `keys/` 下的文件逐字节相同），由 `PEM_read_bio_PUBKEY` 读入；全程只加载公钥，
不涉及私钥材料。构建 link 因此需要 OpenSSL 3（Homebrew `openssl@3`，由 `pkg-config libcrypto` 定位）。
失败时指南要求车机主动断开，这里只上报 `authenResult=false` 并记一条 `warn`：校验代码自身出错
不该表现成「手机完全连不上」，先让链路跑起来、日志留下证据。

**17.2（手机验证车机）** 依赖的是渠道号，见下一节；手机把结论放在 `MSG_CMD_MD_AUTHEN_RESULT`
回给车机，`carlifed` 收到后记 `phone authenticated this head unit` 或 `phone rejected this head
unit`。身份上报在协议版本匹配时立即发出（真机抓包顺序如此），早于 17.1 完成；手机端对两者的处理
互不依赖，指南里的先后顺序不影响结果。

### 车机身份上报

真车机在发完协议版本后紧接着上报两条身份信息，顺序如下（取自真机抓包）：

```
车机 --MSG_CMD_HU_PROTOCOL_VERSION(0x00018001)--> 手机
车机 --MSG_CMD_STATISTIC_INFO(0x00018027)--> 手机    含渠道号 channel
车机 --MSG_CMD_HU_INFO(0x00018003)--> 手机
车机 --MSG_CMD_VIDEO_ENCODER_INIT(0x00018007)--> 手机
```

开源库自带的测试程序不发送这两条。它们除统计用途外还有认证作用：指南 17.2 指出手机端依据
`MSG_CMD_STATISTIC_INFO` 中的渠道号判断车机是否合法，校验通过后才回 `MSG_CMD_MD_AUTHEN_RESULT`
并启动车厂相关的定制服务；而手机的 `MSG_CMD_MD_FEATURE_CONFIG_REQUEST(0x00010051)` 只在
「双方握手认证通过后」才发送。真机已验证这条链：渠道号合法后日志出现
`replied to phone feature config request`，音频传输方式随之协商完成，音频不再零帧。

CarLife Android app 侧的校验规则（逆向所得，`CARLIFE_AUTH.md` 2.2 节）为：去空白后非空、匹配
`2(\d){7}`（`2` 加恰好 7 位数字），且不等于后装市场渠道 `20000000` 与预装市场渠道 `20000001`，
不满足即判定车机不合法。这条规则不是白名单——除那两个值外任意 `2xxxxxxxx` 都通过；而渠道号同时
决定该厂商的功能定制项，所以默认值取映射表里的奔驰值 `20062100`
（`VehicleChannel.VEHICLE_CHANNEL_DAIMLER`），可由 `CL_CHANNEL` 覆盖。奔驰是唯一会让 app 额外校验
自身安装包签名的渠道（`SignatureVerification.packageSignatureCheck`），官方包必然通过；重新打包的包
本来也过不了 app 在收到统计信息时无条件执行的那道签名检查，与渠道号无关。
`cuid` 按指南要求每台车机唯一（重复会使手机主动断开连接），取本机 host uuid（macOS 的硬件 UUID、
Linux 的 `/etc/machine-id`），不提供配置项。

该上报必须发送，且渠道号必须合法：它是手机上功能定制链的触发条件，缺失或不合法会让音频通道零帧。

## 与 playport 前端的接口对齐

`server/` 实现 playport 服务端的接口契约，前端因此可以直接使用：

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
| `touch` 的 contacts | 取 id 最小的触点，按按下状态映射 DOWN / MOVE / UP；空列表即 UP，坐标沿用最后位置（CarLife 只支持单点） |
| `knob.select` / `home` / `back` | 硬按键 ok(0x14) / main(0x1E) / back(0x0E) |
| `knob.x` 方向 | 硬按键 move left(0x15) / move right(0x16) |
| `knob.y` 方向 | 硬按键 selector previous(0x07) / selector next(0x06) |
| `media` 3 / 4 / 5 | 硬按键 media start(0x1F) 与 media stop(0x20) 交替 / seek add(0x10) / seek sub(0x0F) |
| `telephony` 1 / 2 | 硬按键 phone call(0x02) 或 phone end(0x03)，按手机上报的通话状态选择 / mute(0x13) |
| `siri` | 硬按键 VR start(0x21) 与 VR stop(0x22) 交替 |
| `keyframe` | 丢弃：CarLife 没有「请求关键帧」命令 |
| `night` | 丢弃：CarPlay 专属，CarLife 无对应 |
| `ping` | 回 `{"type":"pong"}` |
| 二进制帧（麦克风上行） | 丢弃：CarLife 的语音上行由手机发起 |

实现要点：

- **方向键分两组。** `selector previous/next` 的含义是「在当前小区域中切换焦点」（等效于旋转旋钮），
  `move left/right` 的含义是「跳到左右相邻的小区域」。`move up/down` 协议中同样存在，但手机在列表
  界面上不响应（实测 0x17/0x18 无反应，0x07/0x06 可使焦点上下移动），而 selector 没有横向版本，
  因此纵轴使用 selector、横轴使用 move。
- **`home` 使用 `main(0x1E)`。** `KEYCODE_Home(0x01)` 的含义是「按下手机的 Home 键」，会把 CarLife
  切到后台；「回到 CarLife 主界面」对应 `KEYCODE_MAIN`，开源车机端的 Home 按钮发送的也是它
  （`ControlTestWindow`）。
- **`media` 3 不能使用 `KEYCODE_Media(0x09)`。** 该键值的含义是「进入手机端 CarLife 的音乐播放界面」，
  按下只会切换界面（实测如此）；播放/暂停使用 `media start(0x1F)` / `media stop(0x20)`。上一首/下一首
  使用 `seek sub/add`，开源车机端把 Android 的 `KEYCODE_MEDIA_PREVIOUS/NEXT` 映射到这两条
  （`MediaButtonReceiver`），不使用 `selector`。
- **`siri` 只覆盖开关。** `VR start/stop` 可打开与关闭语音识别界面，但识别本身需要麦克风：功能定制项中
  上报的是 `VOICE_MIC=0`（使用车机麦克风），手机据此等待 VR 通道上的 PCM，而服务端不发送（`vr ↑0`），
  语音输入仍不通。要打通需改报 `VOICE_MIC=1`（使用手机麦克风）或实现 VR 通道上行。

前端的 contacts 只在手指按下期间存在：`pointerup` 把该触点从列表中直接删除，而不是补发一条 `down`
为假的记录（见 `input.ts` 的 `scheduleFlush`），因此抬手一定表现为该槽位在消息中消失。抬手事件本身
不带坐标，只能沿用该槽位最后一次上报的位置；以原点填充会被手机解释为触点跳到左上角，从而把手势判定
为离开控件并取消这次按压，表现为按钮按下与抬起的视觉都在、功能不生效。服务端因此记录每个触点最后
发送的设备像素坐标，抬手时按原位置发送 UP；拖动过的手指抬手也落在拖动后的位置。

音频流名经 wire 传给前端并直接作为音量滑块标签，因此使用英文 `Media` / `Navigation`。

## 已知限制

- 只实现有线（AOA）通路，无线 CarLife 未实现。
- 触摸为单点，CarLife 协议本身不支持多指：前端的多点触点中只有 id 最小的那个生效。
- 麦克风上行未接通：CarLife 的语音通道由手机发起，服务端不向浏览器请求麦克风。
- 视频只有 H.264；音频按 16 位 PCM 处理。
- 帧率由手机决定：请求 60 fps 时手机可能仍按 30 fps 编码，后端只能报告实测值。
- 双向鉴权已在真机上走通：17.1 的 RSA 验签与 17.2 的渠道号都被对端接受，「未认证的车机端」提示不再
  出现。车机验证失败时按指南本应断开，这里选择只上报不断开（见「车机鉴权」）。
- 手机侧 `encryptValue` 还有一条 `"2.0;"` 前缀的 native 分支（`libencryption.so`，用 APK 签名派生
  校验码，不用 RSA），本实现不产生该前缀，因此不受影响；若将来把 `kAuthenVersion` 换成 `2.0;…`
  形式，校验会失败。
- 重新插拔需要重建 `carlife-link`，`carlifed` 会自动处理；手工单独运行链路时需要自行重启。
- accessory 模式在手机侧粘性保留，在手动复位前无法通过重新握手拉起 CarLife App
  （`adb shell svc usb setFunctions`，见「设备接入时机」）。
- 手机侧 CarLife App 由系统在 accessory 附着时拉起，比链路就绪慢数秒。`carlifed` 每 5 秒重发一次
  协议版本直到会话建立，用于覆盖这段延迟；一直无法连接手机时会持续重发。
