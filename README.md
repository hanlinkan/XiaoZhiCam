# XiaoZhiCam

**把 AI 语音助手塞进一台 2014 年的 IP 摄像头。**

小蚁 `YHS-113`（海思 Hi3518A）：单核 240 MHz ARM926EJ-S，无 FPU、无 NEON，运行期空闲内存约 2 MB。
这台设备从设计上就没考虑过语音交互 —— 没有麦克风孔位、没有按键、音频通路被原厂录像进程独占。
本项目把它改造成能接 [小智](https://xiaozhi.me) 服务端的完整语音助手：喊一声就行，能对话，能看图。

**不刷机、不拆机、不改固件。** 文件拷进 SD 卡插上就能用，拔卡即恢复原厂相机。

> 状态：完整闭环已在真机跑通 —— 采集 → 上行 → 服务端 → TTS → 播放全链路可用，
> 本地唤醒已接入，MCP 识图已打通，空闲断连会自动重连。

---

## 三分钟上手

**1. 准备**

- 小蚁 720P 一代夜视版（YHS-113 / YHS-113-IR，型号 22CN）
- 原厂固件 `1.8.7.0C`（telnet 登录后 `cat /home/version` 可查）
- 相机**已经用米家 App 配好 WiFi** —— 语音助手要联网，没网就没反应
- 一张 FAT32 的 SD 卡（卡里的录像不影响，不用清空）。**卡必须有 MBR 分区表、
  第一个分区是 FAT32** —— 原厂 `init.sh` 挂的是 `/dev/hd1`，整盘直格式化（无分区表）
  的卡不会生成这个设备节点，钩子永远不触发。Windows 格式化、SD Card Formatter
  出来的都是这种结构；Linux 下别直接 `mkfs.vfat /dev/sdX`。



**2. 拷贝**

把 `sdcard/` 目录**里面的东西**放到 SD 卡**根目录**：

```bash
cp -r sdcard/* /你的/SD卡挂载点/
```

Windows 上就是打开 `sdcard` 文件夹，把里面的 `test`、`hack` 全选复制到卡根。

拷完卡根应该直接看到 `test/` 和 `hack/`，**不要多套一层**（不要变成 `卡根/sdcard/test/`）。

> 从 Releases 下载 zip 的话更省事：解压出来就是卡的结构，全选复制到卡根即可。

```
SD卡根/
├── test/equip_test.sh      ← 原厂 init.sh 的出厂测试钩子，靠它接管启动
├── hack/bin/p8s            ← 语音助手主程序
├── hack/bin/rmm            ← 打过补丁的原厂录像进程（见「三个关键参数」）
├── hack/init_norm.sh       ← 裁剪过的正常启动流程
├── hack/launch_p8.sh
├── hack/autostart.sh
├── hack/voice/*.pcm        ← 验证码播报用的数字语音
└── README.md               ← 本文件
```

**3. 开机**

断电 → 插卡 → 上电。约 1 分钟后，相机会念出一串 6 位数字。

---

## 首次激活

新相机还没绑定你的小智账号，要激活一次：

1. **听验证码** —— 开机约 1 分钟后，相机会用中文念出 6 位验证码，连念两遍。
   没听清不要紧：SD 卡根目录会生成 `ACTIVATION.txt`，里面是同一个验证码
   （拔卡插到电脑上看）。
2. **去绑定** —— 浏览器打开 <https://xiaozhi.me> → 登录 → 控制台 → 设备 →
   添加设备 → 输入验证码。
3. **重启** —— 拔插电源，就能对话了。绑定成功后 `ACTIVATION.txt` 会自动消失。

验证码只念一次：没绑定时程序会被服务端拒绝并重启，每次都念 14 秒会把人逼疯。
想再听一遍，删掉 `hack/.announced` 再重启。

**怎么确认成功**

- 开机听到中文念 6 位数字 → 引导、音频通路、网络、激活查询全通了
- 绑定后指示灯变黄常亮 → 已连上服务端，待命中
- SD 卡根出现 `client.log` → 语音链路日志，说话后能看到 `listen` / `stt` / `tts`
- 想进相机看看：`telnet 相机IP`，免密 root

---

## 出问题怎么办

按这个顺序查：

1. **卡根路径对不对** —— `test/equip_test.sh` 必须正好在这个位置。多套一层
   （`卡根/sdcard/test/...`）就不会触发。
2. **看 `boot.log`** —— 记录了引导过程：有没有拿到 root、`init.sh` 钩子在第几行、
   `init_norm.sh` 是怎么生成的。这个文件最能说明问题。
3. **看 `client.log`** —— `audio devices ready` 说明音频起来了；一直 `reconnected`
   通常是 WiFi 或服务端问题。`[kws] HIT` 表示本地已经认出唤醒词，没跟进
   `wake accepted` 见下一条。
4. **刚上电喊不醒** —— 唤醒门控默认要求进程启动满 **2 分钟**（躲开机阶段的误报），
   这期间喊"你好小智"，日志里会有 `HIT` 但不会出现 `wake accepted`，
   对话不会开始。想改这个时间见下一节。
5. **相机没反应、telnet 也连不上** —— 拔卡重启确认相机本身还好，再插卡重试。

反馈问题时请附上 `boot.log` 和 `client.log`。

> ⚠️ 这个包会开启 telnet（root 免密），同网段任何人都能登录你的相机。
> 介意的话删掉 `test/equip_test.sh` 里 `telnetd -l /bin/sh` 那一行。

---

## 唤醒门控配置

卡上 `hack/wake.conf` 控制上电后多久才接受唤醒：

| `WAKE_GATE_MS` | 行为 |
|---|---|
| `120000`（默认） | 2 分钟后可唤醒 |
| `0` | 关闭门控，上电立即可唤醒 |
| `60000` | 1 分钟可用，但开机期偶发误唤醒 |
| `180000` | 旧版的 3 分钟，最保守 |

门控期内 KWS 照常检测、照常打 `[kws] HIT`，只是不进对话。早期硬编码 180 秒是给
afe 电平归一化 / 底噪估计的热机期留的保守余量；缩到 60 秒后真机出现过「没说唤醒词、
直接问一句话就被当成唤醒」——因为误报放行的同时恰好有人说话，服务端会把它当成一次
提问回答。所以默认折中取 2 分钟。

改完**重启 `p8s`** 生效（断电重启，或 telnet 里 `kill` 一次让守护拉起）；
启动日志会打印 `[kws] wake gate = 120000 ms (...)`。
临时调试不用改文件：`WAKE_GATE_MS=0 ./bin/p8s`，环境变量优先级更高。

---

## 卸载与还原

拔掉 SD 卡，重新上电，就是原厂相机。

不刷机、不动分区表、不刷 kernel/rootfs，**也不改 `/home/rmm`**：patch 版 rmm
直接从 SD 卡启动，拔卡后原厂 rmm 照常工作。

flash 上只留两处很小的痕迹（首次启动时写入，之后不再碰）：

- `/home/forbidden.g726` 变成指向内存盘的符号链接（约 21 字节）
- 同目录下 `forbidden.g726.orig`，原厂提示音备份（9 KB）

播放对话时**不写 flash**：段文件都在内存盘上，只改内存盘里的符号链接。

### 为什么别人的相机插卡就能用

原厂 `/home/init.sh` 第 37 行有个出厂测试钩子：

```sh
if [ -f "/home/hd1/test/equip_test.sh" ]; then
        /home/hd1/test/equip_test.sh
        exit
fi
```

它排在 `cd /home; ./rmm &` **之前**，而且 `exit` 掉了整个脚本。所以插着这张卡开机时，
原厂流程根本走不到启动 `rmm` 那一步，flash 上的原版 `rmm` 完全不参与 —— 这就是
「别人的相机一个字节都不用改 flash」的原因。只有被手工改过（比如把 `/home/rmm`
换成指向 SD 的符号链接）的机器，才需要先把原版写回去。

**彻底还原一台被改过的机器**。需要一份原版 `rmm`：早期版本的卡上带
`hack/bin/rmm_orig`，本仓库因版权原因不分发；没有的话从你自己的相机备份里取。

```sh
cp <原版rmm路径> /home/rmm.new && sync
chmod 755 /home/rmm.new && rm /home/rmm && mv /home/rmm.new /home/rmm && sync
md5sum /home/rmm          # 期望 ebc2650d4f06d04a070730a8bce0dbc9
rm /home/forbidden.g726 && mv /home/forbidden.g726.orig /home/forbidden.g726
```

### 卡模式为什么关掉 `watch_process`

原厂 `/home/wp_cmd` 挂着 `p2pserver` / `goolink` / `mp4record` / `cloud` /
`record_event` / `p2p_tnp` 六项「缺了就拉起」的规则，`watch_process` 每 10 秒扫一次。
这台相机总内存只有 34 MB、运行期空闲 1–2 MB，`cloud` 一项就要 ~0.9 MB —— 被拉起来
语音必崩。

所以 `init_norm.sh` **不启动 `watch_process`**，改由 `autostart.sh` 里的 guard 循环
接管它真正有价值的两件事：`rmm` 掉了就重启整机、持续清掉云/P2P/录像进程。
对「已经手工精简过 `wp_cmd` 的机器」看不出区别，但**对没改过的相机是必需的**。

同理，`rmm` 启动时卡上文件若缺失/损坏，会退回 flash 上的原版 `rmm`，不会陷入重启循环。

---

## 为什么这事不好办

难点不在"写个语音客户端"，而在**这台设备的音频被原厂进程锁死了**：

| 障碍 | 表现 | 后果 |
|---|---|---|
| 原厂 `rmm` 独占音频 | AI/AO 是全局单例，它先 Init 了 | 自己初始化直接失败 |
| kill 掉 `rmm` | 硬件看门狗 10 秒复位整机 | 设备变砖重启循环 |
| 采样率被锁 | 8 kHz / mono / 320 点 / 40 ms，改不了 | 协议要 16 kHz，必须双向重采样 |
| 无 FPU | 浮点拖入软浮点库，实时余量被吃光 | Opus、重采样、KWS 全得定点 |

既然 `rmm` 会周期性播放自己的提示音文件（`/home/forbidden.g726`），那就**借它的嘴说话**：

```
TTS PCM(8k) → G.726 编码 → 切成 80 字节/包 → 写成段文件
                                              ↓
                        符号链接覆盖 /home/forbidden.g726
                                              ↓
                      mqueue 0x1614 通知 rmm "该放提示音了"
                                              ↓
                              rmm 用自己的 ADEC/AO 通路播出来
```

**一行代码都不碰 `rmm` 的 ADEC/AO/Bind。** 原厂进程以为自己在放"禁止录像"的提示音，
实际上放的是大模型的回答。这是整个项目能成立的关键。

---

## 数据链路

```
        麦克风                                     喇叭
          │                                          ▲
          ▼                                          │
   AI 采集 8k/40ms                          rmm ADEC 通路
          │                                          ▲
          ▼                                          │
    VQE / AENC                                 段文件 + mqueue
          │                                          ▲
          ▼                                          │
   Opus 16k 编码 ──► WebSocket ──► 服务端 ──► TTS ──► G.726 编码
```

上行是标准 Opus over WebSocket；下行绕不开 `rmm`，只能借道。

---

## 现在能做到什么

- 喊「你好小智」本地唤醒，不依赖网络
- 完整对话：8 kHz 采集 → Opus 上行 → 服务端 → G.726 下行 → 相机喇叭
- 让相机看图回答（复用原厂抓拍通道，走 MCP 视觉接口）
- 服务端空闲断线后自动重连，不用重启相机

---

## 实测数字

在 YHS-113 上跑出来的（单核 240 MHz ARM926EJ-S，无 FPU/NEON，运行期空闲内存 1.6–2 MB）：

| 项 | 数值 |
|---|---|
| 采集 | 8 kHz / mono / 320 点 / 40 ms（原厂锁定，改不了） |
| 上行编码 | Opus 16 kHz / mono / 16 kbps，complexity 1 |
| 下行播放 | G.726 16 kbps（2-bit ADPCM），80 字节/包 |
| 唤醒引擎开销 | 单核 11–12% |
| 主程序体积 | 1.38 MB（strip 后，含 Opus + mbedTLS + KWS） |
| 起播缓冲 | 2.4 s（攒够才开口，换来全程不断音） |

---

## 三个关键参数

播放链路的三个数字都是真机上量出来的，改之前请先看理由：

| 参数 | 值 | 为什么 |
|---|---|---|
| 段长 `PB_SEG_PKTS` | 16 包 = 640 ms | 段音频必须长于实际触发间隔（实测中位 521 ms）。段比间隔短，ADEC 队列就会见底，AO 欠载强制 DMA 重同步 —— 那是"咔哒"，不是空白 |
| 收尾补静音 `PB_TAIL_PKTS` | 75 包 = 3 s | `rmm` 播完一个文件会写 DAC 待机寄存器，此刻 AO 里 1.2 s 缓冲没播出的部分被静默丢弃 —— 这就是"最后几个字没了"。补到 3 s 让 `rmm` 停留足够久，缓冲自然排空 |
| 起播水位 `PB_STARTUP_RING` | 19200 样本 = 2.4 s | TTS 供给 1×、播放消耗 1×，端到端没有余量。先攒够再起播，后面攒包才不用等数据 |

副作用：每轮回答结束后会多播约 3 秒静音，这段时间唤醒它响应会慢几秒。

**关于 `hack/bin/rmm`** —— 原厂 `rmm` 在 `rmm_msg_handle` 里有一道闸门：

```asm
0x380d4  cmp  lr, #19        ; 连续播放次数
0x380dc  bgt  0x380f0        ; 超过 19 次就跳过 send_ao_beep
```

语音助手每 640 ms 触发一次，10 秒的回答就 16 次，长回答必然超过 —— 原厂版会播到后面
卡顿、丢句。补丁把那处条件跳转改成了空指令（另有一处时间常量 1000000 → 20000），
与原厂仅差 6 个字节。

它从 SD 卡启动（`/tmp/hd1/hack/bin/rmm`），**不替换**相机内的 `/home/rmm`，
所以拔卡后原厂行为完全不变。

---

## 仓库结构

```
sdcard/               ★ 卡上的全部内容，拷到 SD 卡根目录就生效
  test/                原厂 init.sh 的引导钩子
  hack/                主程序、启动脚本、语音素材
README.md             ★ 本文件（打包时会一并放进 zip）

src/tests/
  p8_client.c         ★ 主程序源码（3118 行，设备上以 p8s 运行）
  p0…p36_*.c          硬件探针：开发过程中逐个摸清音频/codec/看门狗的小程序
  wd_*.c              看门狗接管实验
  Makefile            全部目标的构建规则
src/kws/              唤醒词引擎（独立可测）
  ref/                参考实现，移植时用来验证逐位一致
  mf/                 TFLite Micro audio microfrontend（Apache-2.0 子集）
  test/               主机侧工装：kws_host + verify + 训练脚本
```

> 主程序放在 `tests/` 下是历史原因：它是从一串硬件探针逐步长出来的，
> 探针编号 `p0`…`p27` 对应当初的排查顺序。

---

## 构建

主程序依赖厂商 SDK，需自备：

```bash
cd src/tests
make p8_client \
  TOOLCHAIN_DIR=/opt/arm-hisiv100nptl-linux/arm-hisiv100-linux \
  MPP_SDK=/opt/mpp \
  OPUS_DIR=/opt/opus-1.3.1 \
  MBEDTLS_DIR=/opt/mbedtls-2.28.8
```

| 变量 | 应指向 |
|---|---|
| `TOOLCHAIN_DIR` | 海思工具链中**包含 `bin/` 的那一层**（`.../arm-hisiv100nptl-linux/arm-hisiv100-linux`） |
| `MPP_SDK` | **包含 `hi3518v100_mpp_1.0.A.0/` 的父目录**，不是 SDK 目录本身 |
| `OPUS_DIR` | 已按 `--enable-fixed-point` 配置并编译过的 Opus 源码树 |
| `MBEDTLS_DIR` | 已编译过的 mbedTLS 2.28.8 源码树 |

SDK 版本串必须与设备严格匹配：测试机自报 `Hi3518_MPP_V1.0.A.0`，
用 V200 或 V1.0.B.0 都会 `SYS_Init` 失败并污染驱动状态。

唤醒词组件不依赖任何 SDK，主机上就能编和测：

```bash
cd src/kws/test
make                                      # 构建 kws_host
make ref && make verify                   # 逐位一致性校验（需自备两段 16 kHz WAV）
```

> 主程序 `p8_client.c` 没有自动化测试 —— 它需要 MPP SDK 和目标硬件，只能在真机上验证。

---

## 仓库里没有什么

以下内容**刻意排除**，都不能再分发：

| 缺失项 | 原因 |
|---|---|
| **海思 MPP SDK**（约 87 MB） | 专有、NDA 授权，版权属海思 |
| **海思交叉工具链**（约 550 MB） | 同上 |
| **原厂固件与内核模块**（`*.ko`） | 版权属小蚁 / 海思 |
| **训练数据**（约 8.8 GB） | 数据集许可禁止转分发 |
| **录音素材** | 第三方与人声，隐私问题 |
| **完整反汇编 / 函数地址表** | 不公开 |

---

## 可以直接抄的设计

即使你永远不碰 Hi3518：

1. **设备被原厂进程锁死时，别硬抢 —— 借道。** 找到一个"它本来就会做的事"（放提示音），
   把自己的数据伪装成它的输入。比逆向着去解锁一个 NDA 保护的闭源驱动省力得多。
2. **解码不要放在播放线程。** 解码耗时有 10–40 ms 抖动；和 20 ms 送帧循环共线程，
   每一次慢帧都是可听的断音。解码放网络线程，PCM 走无锁 SPSC 环。
3. **绝不让音频设备饿死 —— 欠载补静音。** 欠载会强制 DMA 重同步，那是"咔"，不是空白。
4. **绝对时间调度。** `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` 按固定网格步进；
   相对 sleep 会累积漂移，直到缓冲深度振荡。

---

## 许可与第三方组件

原创部分（`src/`、`sdcard/hack/` 下的脚本、本文件）采用 [MIT](LICENSE)。

### 随仓库分发的二进制

| 文件 | 来源 | 许可 |
|---|---|---|
| `sdcard/hack/bin/p8s` | 本项目原创（`src/tests/p8_client.c` 的交叉编译产物） | MIT |
| `sdcard/hack/bin/rmm` | **小蚁原厂固件的修改版**，与原厂仅差 6 字节 | **不适用 MIT**。版权属原厂，仅为互操作性研究提供，使用者须自行评估合规性 |

`sdcard/hack/bin/rmm` 从 SD 卡启动，不替换相机内的 `/home/rmm`，拔卡即恢复原厂行为。

### vendored 源码

| 组件 | 位置 | 许可 |
|---|---|---|
| TFLite Micro audio microfrontend | `src/kws/mf/` | Apache-2.0（TensorFlow 仓库 `tensorflow/lite/experimental/microfrontend/lib` 子集，未改算法） |
| kiss_fft (int16 变体) | `src/kws/mf/.../kiss_fft_int16.c` | BSD-3-Clause |
| mel 滤波器组 / 窗函数系数 | `src/kws/mf/` | Apache-2.0（由 Google 语音团队生成） |

`src/kws/ref/` 是参考实现，只用于逐位对拍，不参与固件构建。

### 构建期外部依赖（不在本仓库）

| 组件 | 许可 | 用途 |
|---|---|---|
| Opus 1.3.1 | BSD-3-Clause | 音频编解码，需 `--enable-fixed-point` |
| mbedTLS 2.28.8 | Apache-2.0 | TLS 传输，静态链接 |
| [microWakeWord](https://github.com/kahrendt/microWakeWord) | Apache-2.0 | 模型架构与训练流水线 |

### 商标

"Xiaoyi"、"HiSilicon"、"Hi3518" 等为其各自所有者的商标，仅用于事实性描述与兼容性说明。

---

## 技术文档

| 文档 | 内容 |
|---|---|
| [`docs/audio-playback.md`](docs/audio-playback.md) | **如何自己驱动喇叭发声**（不经过 `rmm`）：CPLD 功放开关、ADEC 送流的包格式与 `0xA0188040` 的真相、两条无效路径、`/proc/umap` 字段的正确读法 |

配套的验证程序都在 `src/tests/` 下（`p36_cpld.c` 是最小可用示例，
`make p36_cpld MPP_SDK=/opt/mpp` 即可构建，不参与 `make all`）。

## 免责声明

本项目是对**作者自有设备**的独立互操作性研究，与小米/小蚁、海思、小智项目**无隶属、
背书或支持关系**。仓库不分发厂商 SDK、固件镜像、签名密钥。

代码中的设备 MAC、客户端 ID、服务端地址均为**示例值**，使用前请改成你自己的。
设备固件改动可能导致硬件永久损坏，风险自负。建议先在闲置的相机上试。
