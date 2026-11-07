# 音频播放：如何自己驱动（不经过 `rmm`）

本文记录「让相机喇叭出声」的完整可控路径，以及路上踩过的所有坑。
所有结论都是真机实测，不是推断。

---

## 一、最小可用配方

```c
/* 1. 音频通路总电源（cpld_periph 的第一对 GPIO） */
int fd = open("/dev/cpld_periph", O_RDWR);
ioctl(fd, 0x7020, 0);                       // 开。0x7021 是关，但见 §三

/* 2. ADEC 通道 */
ADEC_ATTR_G726_S g = { .enG726bps = 4 };
ADEC_CHN_ATTR_S  a = { PT_G726, 20, ADEC_MODE_PACK, &g };
HI_MPI_ADEC_CreateChn(0, &a);               // u32BufSize 合法范围很窄，20 可用

/* 3. 解码结果直送 AO */
MPP_CHN_S src = { HI_ID_ADEC, 0, 0 };       // HI_ID_ADEC = 24
MPP_CHN_S dst = { HI_ID_AO,   0, 0 };       // HI_ID_AO   = 22
HI_MPI_SYS_Bind(&src, &dst);

/* 4. 送流：每包 4 字节头 + 80 字节 G.726 */
unsigned char pkt[84];
pkt[0] = 0; pkt[1] = 1; pkt[2] = 0x28; pkt[3] = 0;
memcpy(pkt + 4, g726_80bytes, 80);

AUDIO_STREAM_S st = { 0 };
st.pStream = pkt;
st.u32Len  = 84;                            // 含 4 字节头
HI_MPI_ADEC_SendStream(0, &st, HI_TRUE);
usleep(23000);                              // 20ms 一包，23ms 留余量

/* 5. 结束 */
ioctl(fd, 0x7021, 0);
```

实测 `SendCnt = GetCnt = PutCnt = 116`、`SendFail = 0`，人耳确认出声。

参考实现：`src/tests/p36_cpld.c`（完整体）、`src/tests/p35_adec_rmm.c`（不含 CPLD）。

---

## 二、那个 `0xA0188040` 到底是什么意思

如果你自己试着往 ADEC 塞 G.726，几乎一定会撞上 `0xA0188040`。
它**不是**"包格式错误"这种专用错误码，而是：

> 「解码回调返回了非零」，库把它统一包装成 `0xA0188040`，真实错误码只打到调试流。

解码器读长度的方式和你想的不一样：

```c
/* DecodeG726Frm */
len3   = in[2];                 // 第 3 个字节
total  = (len3 + 2) * 2;        // 40 -> 84
if (total <= available) { 解码; 消费 total 字节; }
```

所以 **4 字节头 `{0, 1, 40, 0}` 是必需的**，其中 `byte[2] = 载荷长度 / 2`。
送裸 G.726 时 `byte[2]` 是音频数据本身，算出来的长度乱掉，解码直接失败。

另外：

| 参数 | 坑 |
|---|---|
| `ADEC_CHN_ATTR.u32BufSize` | 范围很窄，`2048` 直接 ILLEGAL_PARAM；`20` / `30` 可用 |
| `ADEC_ATTR_G726_S.enG726bps` | 用 `4`（与 `rmm` 一致）。该字段更像"提示"，实测不匹配也能解 |
| 先 `SYS_Init` | `ADEC_CreateChn` / `SYS_Bind` 都要求本进程先 `HI_MPI_SYS_Init()`，否则分别返回 `0xA0188003` / `0xA0028003` |

---

## 三、`0x7020` / `0x7021`：不只是喇叭功放

`cpld_periph` 的 GPIO 分配：

| 命令 | 作用 |
|---|---|
| `0x7020` / `0x7021` | 音频通路总开关（功放 **+ 麦克风偏置**） |
| `0x7022` / `0x7023` | 蓝色 LED 开 / 关 |
| `0x7024` / `0x7025` | 黄色 LED 开 / 关 |

**重点：`0x7021` 会把麦克风偏置供电一起切断。** 表现为唤醒词完全失效——
KWS 不再有任何命中（只剩开机瞬态的饱和误触发），但 `/proc/umap/ai` 看不出异常。

更麻烦的是：**CPLD 状态能扛过软重启**，`reboot` 救不回来，只有 `0x7020` 能恢复。

所以约定：**播放结束后不要调 `0x7021`**，或者在启动时无条件 `0x7020` 兜底。
（见 `src/tests/cpld_sw.c`，可单独开关这一路电源。）

---

## 四、两条曾走过但无效的路

### 1. DAC 寄存器 `0x20050074`

网上/逆向资料里常见 `0x0c802424`（放音）/ `0x06802424`（待机）的说法。
实测在本文所述的机型上：

- 真实播放期间该寄存器**全程保持 `0x06802424` 不变**
- 强行写成 `0x4F4F2424`（最大音量）也不影响是否出声

它更像"原始固件里那句命令在本机没生效"，不要把它当成出声的必要条件。

### 2. 直接用 `HI_MPI_AO_SendFrame` 送 PCM

数据通路本身是好的，可以用 `HI_MPI_AO_QueryChnStat` 验证：

```c
AO_CHN_STATE_S st;
HI_MPI_AO_QueryChnStat(0, 0, &st);
/* 送 20 帧（320 点，40ms/帧）后：busy 1 -> 21，1 秒后回到 1 */
```

如果 `busy` 从这个变化对得上，说明帧进了环形缓冲、DMA 也在按时钟消费——
**此时若仍无声音，问题一定在模拟侧（功放没开），而不是数据。**

帧参数必须与设备属性严格一致，否则驱动直接拒收：
`enBitwidth` 相同、`u32Len == 设备字节数`（8k/mono/320 点 → 640）、
`u32Len < 0x2001`、通道 state 为 enable/pause。

---

## 五、两个 `/proc` 字段的真实含义（容易误读）

### `/proc/umap/ao` 的 `BufEmp`

**不是缓冲水位，是欠载计数器** —— DMA 发现队列空、重复上一帧的次数。

- 空闲时稳定上涨（约 12 次/秒）
- **播放期间会冻结** → 这是"确有数据在播"的客观判据

判断 AO 是否真的在消费数据，用这个比看 `BufEmp` 的绝对数值靠谱得多。

### `/proc/umap/adec` 的 `SendCnt`

原始固件播放提示音时它**恒为 0**（走的是另一条通道实例）。
所以不要拿它做"播放进度"判断，会永远等不到。

但如果**你自己**调 `HI_MPI_ADEC_SendStream`，它每包 +1 ——
这种场景下用它做进度判断是完全可靠的。

---

## 六、和原厂播放共存

原厂 `rmm` 把 `ADEC chn0 → AO0` 绑好、AO 也 enable 好之后，用户态再送流是可行的
（本文配方就是这么跑的）。需要注意：

- `ADEC_CreateChn` 可能返回 ILLEGAL_PARAM（通道已存在），**复用即可**，不必重建
- 但如果你的进程没调过 `SYS_Init`，`SYS_Bind` 会失败；这时可以直接跳过 Bind
  （原厂已经绑过），只做送流
- 一旦在**同一个进程**里调 `SYS_Init`，可能打乱原厂已初始化好的 MPP/AI 状态
  （实测会导致麦克风采集"电平正常但唤醒词失效"）
  → 若需要自己初始化，**把播放逻辑放进独立子进程**

---

## 七、相关工具

| 文件 | 用途 |
|---|---|
| `src/tests/p36_cpld.c` | 完整播放（CPLD 电源 + ADEC 直送），首选入口 |
| `src/tests/p35_adec_rmm.c` | 同上但不控 CPLD，用于单独验证包格式 |
| `src/tests/cpld_sw.c` | `cpld_sw on/off` 单独开关音频电源 |
| `src/tests/p32_ao_state.c` | `QueryChnStat` 观察环形队列占用 |
| `src/tests/p31_ao_beep.c` | AO 直送节奏音（验证数据通路） |
| `src/tests/p22_ao.c` | `UnBind(ADEC→AO)` + 声学回环自检 |
| `src/tests/p10_acodec.c` | acodec 寄存器读写（需先停掉占用 `/dev/acodec` 的进程） |
