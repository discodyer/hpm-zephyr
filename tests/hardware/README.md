<!-- SPDX-License-Identifier: Apache-2.0 -->

# HPM5361 硬件验收测试

本应用对应工作区 `docs/agent/driver-review.md` 的硬件验收矩阵，面向
`hpm5300evk/hpm5361`。各 profile 单独生成一个 Ztest 固件。优先调用 Zephyr 标准
API；TRGM 和内部电机模块使用模块的 HPM 扩展 API，不引入 SDK middleware。
本轮已在 HPM5300EVK RevD 上验证 11 个 profile：`core`、`dma`、`crypto`、`flash`、
`watchdog`、`can`、`motor`、`gpio`、`uart`、`i2c` 和 `spi`，分别通过
8/8、2/2、6/6、1/1、4/4、2/2、5/5、4/4、4/4、3/3 和 1/1 项，共 40 项测试。
历史候选 profile 的 build-only 记录保留在下文，
不代表这些候选已纳入本轮提交或通过硬件验收。

2026-10-27 `core` 运行耗时 0.923 s。UART 完整日志位于工作区
`build/hw-core-gptmr-fix-uart.log`，最终结果为 `PROJECT EXECUTION SUCCESSFUL`。
日志中的重复启动横幅来自烧录/校验复位；温度阈值测试及其余测试最终均通过。
具体覆盖和限制见[硬件记录](../../../docs/agent/hardware-bringup.md#core-profile-hardware-result-2026-10-27)。

2026-10-28 `dma` profile 的链式传输和循环传输暂停/恢复测试均通过，完整 UART
日志位于工作区 `build/hw-dma-current-uart-final.log`。测试修复了 HDMA 将固定
AHB 时钟误送入 SDK 源/分频配置接口的问题；详情见[硬件记录](../../../docs/agent/hardware-bringup.md#dma-profile-hardware-result-2026-10-28)。

2026-10-27 `crypto` profile 的六项同步 SDP KAT 全部通过，覆盖 AES、CCM
及错误 tag 拒绝、SHA-1/SHA-256/SM3/CRC32 和 SM4。完整 UART 日志位于
`build/hw-crypto-current-uart-final.log`；详情见[硬件记录](../../../docs/agent/hardware-bringup.md#crypto-profile-hardware-result-2026-10-27)。

2026-10-28 `flash` profile 的擦除、256 B 写读、二次擦除和 XIP 映射缓存一致性
测试通过。干净复位后的完整 UART 日志位于工作区
`build/hw-flash-cache-fix-uart-clean.log`；详情见[硬件记录](../../../docs/agent/hardware-bringup.md#flash-profile-hardware-result-2026-10-28)。

2026-10-28 `watchdog` profile 的四项 EWDG0 仅中断测试通过，覆盖超时后等待
2200 ms 再在线程喂狗、ISR 内喂狗、重复超时、停用/重装和 500 ms → 2500 ms
分频重配置。干净复位后 4/4 通过，耗时 8.921 s；实测超时间隔为 500 ms 和 2501 ms，
回调同时校验 ISR 上下文、设备和通道。完整 UART 日志位于
`build/hw-watchdog-final-uart.log`；详情见[硬件记录](../../../docs/agent/hardware-bringup.md#watchdog-profile-hardware-result-2026-10-28)。
驱动使能后先有界轮询计数器的诊断读回，确认计数推进再刷新，修复了重配后无 IRQ
的问题；这不证明其内部时钟域故障的确切成因。
SoC 复位、窗口模式、睡眠/调试暂停、已挂起 IRQ 与停用的竞争，以及 EWDG1/PEWDG
尚未上板验证。

2026-10-28 `can` profile 的 MCAN0 静默内部回环通过 2/2 项测试。两次干净物理
复位均在 0.214 s 内通过；日志为 `build/hw-can-final-uart.log` 和
`build/hw-can-final-repeat-uart.log`。覆盖 16 帧标准 classic/FD/BRS、8/64 B 负载、
显式 TX/RX ISR，以及 20 帧 PTPC0 → TSU SOF 64 位捕获、8 槽递增及两次回绕。
PTPC 从 10 s 开始，每次捕获均位于发送前/接收后的 PTPC 读数之间。
模式为 `LOOPBACK | LISTENONLY`，TXD 保持 recessive；配置仲裁速率 125 kbit/s、
数据速率 1 Mbit/s，但内部回环不能证明外部总线时序。无需修改 CAN 驱动。
外部收发器/电气、仲裁/ACK、错误与 bus-off 恢复、其他实例、共享 MRAM 并发压力、
extended/RTR、32 位 TSU、其他时基及绝对精度仍未验证；详情见
[硬件记录](../../../docs/agent/hardware-bringup.md#can-profile-hardware-result-2026-10-28)。

2026-10-28 `motor` profile 无需外部接线，两次干净物理复位均通过 5/5 项测试，
耗时 0.209/0.208 s。日志为 `build/hw-motor-final-uart.log` 和
`build/hw-motor-final-repeat-uart.log`，每份只有一次启动和成功结果。覆盖 SYNT
推进/冻结/恢复、PLB TYPE_B0 slice15 LOAD/KEEP/恢复、QEO0 四种幅值/中点配置的
四象限及三个通道数值（48 次断言）、MMC0 1 turn/s 开环位移与 SYNT 时间的 2% 一致性、
整块 MMC 停启，以及 GPIOM PA24 FGPIO0 可见性 1→0→1 和恢复读回。
PLB 修复了 SDK helper 漏配 slice15，并通过 TRGM 内部实时比较器观察数据单元。
MMC 的 BR_EN 单独清零没有冻结连续预测结果，本测试使用 MOD_EN 停启整块；
未验证独立 predictor 停止、外部电机路由、ABZ/PWM、闭环和中断。
详情及 QEO 实测数值见[硬件记录](../../../docs/agent/hardware-bringup.md#motor-profile-hardware-result-2026-10-28)。

2026-10-28 `gpio` profile 用一根 PB04 输出 → PB05 输入跳线通过 4/4 项测试，
两次独立物理复位耗时 0.491/0.490 s，每份日志只有一次启动和成功结果：
`build/hw-gpio-final-uart.log`、`build/hw-gpio-final-repeat-uart.log`。
接线为 P1 第 31 脚（GPIO22/PB04）↔ 第 32 脚（GPIO26/PB05），由官方
`HPM5300EVKUGV1.0.pdf` 第 9 页表 2 和 RevC 原理图第 2、9 页交叉确认；实物为
RevD，用户已确认接好。旧 PA24 → PA25 接线已弃用，PA24 控制 LIN 的 12 V
电源使能，且未引到 P1。首版四项在输出自身读回失败，GDB 确认输出输入缓冲未启用；
fixture 为 PB04 显式配置 `input-enable` 后复测通过。
覆盖电平/raw set/clear/mask/toggle、上升/下降沿各 20 次、双边沿 20 次 pin 写入
和 2 次 masked 写入、同值 masked 无额外回调，以及停用时 20 次无回调、重装恢复。
所有回调核验真实 ISR、来源和计数。仅 GPIOB4/5 回环通过；level IRQ、其他端口、
FGPIO、独立电压/频率量测及残留 IF 与另一 pin ISR 并发仍未验证。
详情见[硬件记录](../../../docs/agent/hardware-bringup.md#gpio-profile-hardware-result-2026-10-28)。

2026-10-28 `uart` profile 在 UART1 上通过 4/4 项测试，两次独立物理复位均通过。
新增 P1-5/PB07 TX ↔ P1-3/PB06 RX，保留 P1-32/PB05 RTS ↔ P1-31/PB04 CTS；日志为
`build/hw-uart-final-uart.log` 和 `build/hw-uart-final-repeat-uart.log`。
覆盖 257 B 无流控 DMA、同一 RX buffer 的 97 B + 160 B 分段 offset、RTS/CTS
回环与 `config_get`，以及 RX disable/re-enable 和 buffer release 顺序。驱动使用
HPM5361 RX line idle IRQ，并修复 DMA 启动竞态和 queued RX buffer 释放；详情见
[硬件记录](../../../docs/agent/hardware-bringup.md#uart-profile-hardware-result-2026-10-28)。

2026-10-28 `i2c` profile 在 HPM5300EVK RevD 上两次独立复位均通过 3/3 项，
日志为 `build/hw-i2c-smbus-clean1.uart.log` 和
`build/hw-i2c-smbus-clean2.uart.log`；`west flash --verify` 成功。
接线为 I2C0 PB02/SCL（RevC P1-28）↔ I2C1 PB07/SCL（P1-5），以及
PB03/SDA（P1-27）↔ PB06/SDA（P1-3）。EVK 已有 10 kΩ 上拉，不需外加电阻；
RevD 排针位置须按板上丝印核对。

测试覆盖 I2C DMA 65 B 写、重复 START 后 64 B 读和 target STOP 回调；SMBus byte/word、
32 B block、1 B/32 B block PEC 正确读取、损坏 PEC 拒绝，以及 count=33 拒绝后立即恢复。
block helper 在 SDK sequence read 期间屏蔽 controller IRQ，并按进入时状态恢复；
超长 count 路径读弃一个字节并通过 final frame 的 NACK/STOP 完成总线释放。
详情见[硬件记录](../../../docs/agent/hardware-bringup.md#i2c-profile-hardware-result-2026-10-28)。
相关 Twister build-only 通过 EVK I2C profile 和 EVK/ICB polling 共 3 个配置、35 项测试，
无失败/错误/警告，报告为 `build/twister-i2c-smbus-final-fixed/twister.json`。
polling 场景已将两份 `EXTRA_CONF_FILE` 合为同一列表，防止 XIP 配置被覆盖。

2026-10-28 `spi` profile 在 RevD 上两次独立物理复位均通过 1/1 项；
`west flash --verify` 成功。干净 UART 日志为
`build/hw-spi-confirmed-reset1.uart.log` 和 `build/hw-spi-confirmed-reset2.uart.log`；
OpenOCD 校验日志为 `build/hw-spi-confirmed-flash.log`。同一测试循环覆盖 SPI1
Motorola mode 0-3 的 1 MHz、8-bit 同步回环和异步链式 DMA 回环，每项比较全部
256 B TX/RX 数据。TX 为 96+160 B 两块，RX 为 128+128 B 两块，缓冲 32-byte
对齐；成功回调断言运行在 ISR 中，同步和异步完成后均检查 GPIO CS 已释放。

接线只需 MOSI PA29/P1-19 ↔ MISO PA28/P1-21；SCLK PA27/P1-23 不需跳线，
GPIO CS PB04/P1-31 由固件驱动。旧 I2C、UART 流控及 GPIO 回环跳线应移除。
映射依据为 RevC 原理图第 2、9 页；RevD 实物排针由操作者按丝印确认并连接。
测试初次发现 `spi_config.cs.gpio` 需要同时设置 `cs_is_gpio`；并按 HPM SDK
EVK 初始化复现 PA27 的 loopback/input-enable。同步路径另外修复了缓冲指针误取
局部指针地址的问题，现有 8-bit 模式比较可覆盖该回归。

该结果只验证 MOSI/MISO 跳线回环、同步单线传输和 SPI/DMA 组合路径；不代表
外接 SPI 从设备兼容、外部 CS 时序、其他帧宽、双/四线或示波器电平/时序验收。
SPI 加入后 11 个注册 profile、40 项测试的 Twister build-only 回归通过，
0 失败/错误/警告，报告为
`build/twister-hardware-post-spi/twister.json`。

2026-10-28 GPIO 验收后回归：八个已注册 Twister 场景全部构建成功，
8/8 build-only、32 项测试、0 失败/错误/警告，报告为
`build/twister-hardware-post-gpio-final/twister.json`。

UART 场景单独 build-only 构建成功，报告为
`build/twister-hpm-uart-final-build/twister.json`；四项测试均被识别且无构建
失败、错误或警告。随后九 profile 全量 build-only 回归通过 36 项测试，报告为
`build/twister-hardware-post-uart/twister.json`。

2026-10-28 motor 验收后回归：当时七个已注册 Twister 场景全部构建成功，
7/7 build-only、28 项测试、0 失败/错误/警告，报告为
`build/twister-hardware-post-motor-final/twister.json`。包含 PLB 的 EVK `peripherals`
兼容配置也通过，1/1 build-only、16 项测试、0 失败/错误/警告，报告为
`build/twister-plb-api-compat-final/twister.json`；兼容构建不建立额外硬件验收。

2026-10-28 CAN 验收后回归：当时六个已注册 Twister 场景全部构建成功，
6/6 build-only、23 项测试、0 失败/错误/警告，报告为
`build/twister-hardware-post-can/twister.json`。

2026-10-28 watchdog 修复后回归：当时五个已注册 Twister 场景全部构建成功，
5/5 build-only、21 项测试、0 失败/错误/警告，报告为
`build/twister-hardware-post-watchdog-final/twister.json`。

2026-10-28 flash 修复后回归：当时四个已注册 Twister 场景全部构建成功，
4/4 build-only、0 失败/错误/警告，报告为
`build/twister-hardware-post-flash-pmp/twister.json`。

历史 2026-10-27 build-only 验证：当时注册的 23 个 Twister 配置全部构建成功，
共发现 48 个测试项，0 失败、0 错误、0 警告；默认 core 构建、23 份生成 DTS 的
封装/引脚检查、checkpatch、DTS 格式及 module/manifest 检查通过。构建日志位于工作区
`build/twister-hardware-final.log`，报告为 `build/twister-hardware-final/twister.json`。
当时 core 上板验证后的 build-only 复跑也通过：23/23 构建、48 项、0 失败/错误/警告；
Twister 记录位于 `build/twister-hardware-post-core/twister.log`，报告位于
`build/twister-hardware-post-core/twister.json`。
基线为 Zephyr `f78c4deb4523`、SDK `ccc5ec467a047f3ea85cd2fa4565e91db7135a38`。
该历史矩阵包含尚未纳入本轮提交的候选文件，不能用当前 11-profile 范围直接复现。

## 构建与运行

从 west 工作区根目录执行：

```sh
export HPM_SDK_DIR=/workdir/zephyr-workspace/hpm_sdk
export ZEPHYR_EXTRA_MODULES=/workdir/zephyr-workspace/hpm-zephyr

# 默认构建 core，不需要外接回环线。
west build -p always -b hpm5300evk/hpm5361 hpm-zephyr/tests/hardware \
  -d build/hw-core

# 例如 UART1 DMA 回环；其他 profile 同时替换 conf 和 overlay。
west build -p always -b hpm5300evk/hpm5361 hpm-zephyr/tests/hardware \
  -d build/hw-uart -- \
  -DEXTRA_CONF_FILE=profiles/uart.conf \
  -DDTC_OVERLAY_FILE=profiles/uart.overlay

# SPI1 MOSI/MISO loopback with synchronous and linked-block DMA tests.
west build -p always -b hpm5300evk/hpm5361 hpm-zephyr/tests/hardware \
  -d build/hw-spi -- \
  -DEXTRA_CONF_FILE=profiles/spi.conf \
  -DDTC_OVERLAY_FILE=profiles/spi.overlay

# 当前 testcase.yaml 中全部已注册 profile 的构建检查，不连接开发板。
west twister -T hpm-zephyr/tests/hardware -p hpm5300evk/hpm5361 \
  --build-only -j6 -O build/twister-hardware
```

准备好下表中的接线后，在另一终端先打开日志，再烧录选择的固件：

```sh
python3 -m serial.tools.miniterm --raw --dtr 0 --rts 0 /dev/ttyHPM 115200
```

```sh
west flash -d build/hw-uart --openocd /usr/local/bin/openocd --verify
```

RevD 当前使用 BOOT OFF/OFF、FT2232 interface 0 JTAG、interface 1 UART0。
`/dev/ttyHPM` 始终是 PA00/PA01 上的 115200 8N1 控制台；UART 测试使用 UART1。
所有 profile 保留 PA04–PA07 JTAG，保持 XIP、D-cache 和 PMP 数据执行保护开启。
测试失败后用 JTAG 复位再测，尤其是 DMA 故障后。
应用提供等待超时及停止/注销清理；驱动自身阻塞或死锁由外部测试超时判定失败。

不要把原 `tests/hpm5300evk` 的全部 build-only 配置直接作为硬件验收固件：其中有
控制台重配置及与 JTAG 重合的旧引脚配置。本应用独立管理接线和资源。

## Profile、接线和断言

下表列出 11 个已上板验收 profile，具体范围及限制见前述运行记录。

引脚名称是 MCU pad，不是排针序号；按实际 RevD 原理图确认连接点及板载负载。
每次只连接当前 profile 所需线路，数字输入使用 3.3 V 电平。

| Profile | fixture | 接线/设备 | 自动断言与输出 |
|---|---|---|---|
| `core` | `hpm5300evk` | 无外接线 | GPTMR 20 ms alarm IRQ、时钟 off/on 恢复；RNG 自检/非重复数据/真实 ISR 非阻塞预算；CRC32 分段已知答案；TSNS 温度及阈值 IRQ；MBX A/B 双向各 32 字；OTP UUID 区重复只读；PTPC 比较 IRQ 与 TRGM 内部捕获 |
| `dma` | `hpm5300evk` | 无 | 3×256 B 链式拷贝、前后 32 B 哨兵、完成 IRQ；单块循环 DMA 暂停后 IRQ 静止、恢复后继续、数据一致 |
| `crypto` | `hpm5300evk` | 无 | 复用已知答案向量：AES-128/256 ECB/CBC/CTR、SHA-1/SHA-256/SM3、CRC32、SM4 ECB/CBC/CTR、CCM 加解密及错误 tag 拒绝 |
| `gpio` | `hpm_gpio_loop` | PB04 → PB05，P1-31 ↔ P1-32；按 RevD 丝印核对 | 四项通过：电平及 raw set/clear/mask/toggle；上升/下降沿各 20 次，反向不触发；双边沿 20 次及 masked 写入 2 次边沿、同值无额外回调；停用时 20 次无回调、重装恢复，核 ISR/来源/次数 |
| `uart` | `hpm_uart_loop` | PB07 TX → PB06 RX（P1-5 ↔ P1-3）；PB05 RTS → PB04 CTS（P1-32 ↔ P1-31） | 257 B 无流控 DMA、97 B + 160 B 分段 offset、RTS/CTS 回环与配置读回、RX disable/re-enable；校验 TX/RX 事件、buffer、offset/length 和数据 |
| `i2c` | `hpm_i2c_pair` | PB02 SCL ↔ PB07 SCL（RevC P1-28 ↔ P1-5）；PB03 SDA ↔ PB06 SDA（P1-27 ↔ P1-3）；按 RevD 丝印核对，板载已有 10 kΩ 上拉 | I2C0 DMA controller → I2C1 target `0x52`；65 B 写、重复 START 后 64 B 读、STOP 回调；SMBus byte/word、32 B block、1/32 B block PEC、错误 PEC 拒绝、count=33 拒绝及后续恢复 |
| `spi` | `hpm_spi_loop` | PA29/MOSI ↔ PA28/MISO（P1-19 ↔ P1-21）；保留 PA27/SCLK（P1-23），GPIO PB04/P1-31 CS 由固件控制；按 RevD 丝印核对 | SPI1 1 MHz，Motorola mode 0-3；每种模式 256 B sync 与 async linked-block DMA 回环，TX/RX 各两块不同分割；全量比较、ISR completion 和 CS inactive 检查 |
| `can` | `hpm5300evk` | MCAN0 静默内部 loopback，无外部 CAN 总线 | 16 帧标准 classic/FD/BRS、8/64 B 数据及 TX/RX ISR；另 20 帧 PTPC0→TSU SOF 64 位捕获，有效/无丢失/递增、8 槽及两次回绕、PTPC 发送前后时间包络 |
| `flash` | `hpm_flash_scratch` | 板载 XPI NOR，允许擦写下述专用测试扇区 | 全扇区擦除、256 B 写读、二次擦除、先填充 XIP 映射 cache 再验证写/擦后的映射一致性 |
| `watchdog` | `hpm5300evk` | 无 | EWDG0 仅中断：每 100 ms 喂狗三次；500 ms 超时后延迟 2200 ms 再喂狗并验证重复超时；ISR 内喂狗；停用后无 IRQ、重装完整周期；500 ms → 2500 ms 分频重配；容差分别 ±80/160 ms |
| `motor` | `hpm5300evk` | 无外部接线，不配置 PWM/pinctrl 输出 | SYNT 推进/冻结/恢复；PLB TYPE_B0 slice15 LOAD/KEEP 和实时比较器；QEO0 四配置×四象限×三通道数值；MMC0 1 turn/s 位移/SYNT 时间一致性及整块停启；GPIOM PA24 可见性切换/恢复读回 |

PA24 是 LIN 12 V 电源使能，未引到 P1。GPIO profile 仅驱动 PB04；motor profile
只切换 PA24 的输入可见性，不配置该引脚输出。

## Flash、cache 与验收边界

`flash` profile 缩小 `slot0_partition`，仅擦写相对 NOR 偏移 `0xef000` 开始的
4096 B 专用分区，写入 256 B 已知数据后再次擦除。编译时检查扇区大小、对齐和与
固件分区不重叠；运行会清除该扇区原有内容。其余 profile 不启用 flash driver。
该 profile 已在 HPM5300EVK RevD 上通过；OpenOCD 烧录成功本身不能替代 Zephyr
flash API 测试。当前 flash API 未提供 JEDEC-ID 查询接口，器件识别/几何信息、
跨页边界和延迟压力仍需另外核对。

所有 profile 保持缓存及 PMP 保护。DMA、UART、I2C、SDP 测试覆盖普通 RAM 数据，
DMA 测试显式执行 cache flush/invalidate，避免用全局关闭 D-cache 掩盖问题。
HPM5361 固定未缓存 DLM 的 CPU 地址与 DMA 总线别名的
可达性、cache 维护遗漏都可能使测试失败，必须作为移植缺陷调查。

XPI flash driver 的 ROM API helper 位于可执行 ILM，BootROM
`0x20000000–0x2001ffff` 由 PMP 显式授予 RWX；DLM 保持数据用途，PMP 数据执行
保护仍开启。写入/擦除后驱动执行 D-cache 区间失效和 I-cache 全量失效；读路径只
失效对应 D-cache 区间，避免 Andes VA CCTL 对 XIP scratch 地址产生访问异常。

OTP 测试只读取 UUID words 88–91 两次，不打印标识、不烧写、不锁定、不 reload。
看门狗测试只验证 IRQ，不自动执行 SoC reset。motor 测试不配置外部 PWM/pinctrl
输出，GPIOM 测试只修改输入可见性并恢复，不驱动 PA24。

## 仍需补充的验收路径

下列项目仍留在驱动审核矩阵；本应用成功不能自动勾选它们：

| 模块 | 本轮尚未覆盖的硬件验收 |
|---|---|
| clock/pinctrl/GPIO/counter | 全时钟树、PIOC/PY、上拉/开漏电气特性、独立基准频率、counter top/wrap |
| UART | CTS 阻塞/背压、双 buffer 连续 RX、RS-485 DE 时序及地址匹配 |
| I2C/SMBus | 10-bit、zero-count response、block process call、timeout/故障注入、总线恢复及重复 START 电气时序 |
| DMA | 错误 IRQ、暂停边界竞争、压力/并发；循环完成 IRQ 密集度也需记录 |
| Watchdog | SoC reset、窗口模式、睡眠/调试暂停、挂起 IRQ 与停用的竞争、其他实例 |
| CAN/PTPC | 实体收发器和总线、bus-off、共享 MRAM 并发；TSU 与绝对时间关联、PTPC PPS/频率调整及双通道共享 IRQ |
| MBX/GPIOM | 跨核通信/FIFO、跨控制器访问、锁定持久性 |
| QEO/MMC/PLB/SYNT/TRGM | QEO 外部 PWM/ABZ 及波形、闭环 MMC、独立 predictor 停止、PLB filter、SYNT comparator/debug-stop、外部电机路由和中断 |
| SDP/RNG/CRC/TSNS | cache/并发/故障注入压力、随机源统计质量、CRC 多项式矩阵、温度校准 |
| flash/OTP | NOR JEDEC/几何、跨页及边界/延迟压力；OTP 烧写/lock/reload/region 仅在专用可消耗芯片另行验收 |

generic 9-bit UART、DMA P2P、CPU-core-only/multistage watchdog、TRGM 独立事件
回调、HPM5361 PLB ABZ 和 SDP 真正 IRQ 异步不伪造正向通过测试。
其他外设及能力限制仍由工作区 `docs/agent/driver-review.md` 跟踪；RTC、
SEC/SECMON/KEYM 按当前优先级延后。

PLB 的 `SW_INJECT` 只回读软件注入值，不能用作实时计数器读回；模块接口返回
`-ENOTSUP` 并保留输出参数。硬件数据单元操作通过 TRGM 内部比较器输出核对。

## 结果记录

保存 profile、源码/Zephyr/SDK SHA、板版本、接线/仪器、完整 UART 日志和结论。
Ztest 汇总应为 `PROJECT EXECUTION SUCCESSFUL`，且每个断言通过；超时、异常、缺少
fixture、缺少协议模拟器都不能记为硬件通过。Twister `--build-only` 只证明编译链接。
仪器项目另附波形和测量值，再更新工作区的硬件验收记录。

`testcase.yaml` 用 fixture 隔离接线要求，未设置 `build_only: true`，以后可交给具备
相应 fixture 的 Twister 硬件设备池；不要给未接线的普通 EVK 一次性声明所有 fixture。
