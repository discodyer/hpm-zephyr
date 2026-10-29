<!-- SPDX-License-Identifier: Apache-2.0 -->

# HPM5361 硬件验收测试

本应用对应工作区 `docs/agent/driver-review.md` 的硬件验收矩阵，面向
`hpm5300evk/hpm5361`。各 profile 单独生成一个 Ztest 固件。优先调用 Zephyr 标准
API；TRGM 和内部电机模块使用模块的 HPM 扩展 API，不引入 SDK middleware。
本轮已在 HPM5300EVK RevD 上验证 15 个 profile：`core`、`dma`、`crypto`、`flash`、
`watchdog`、`can`、`motor`、`gpio`、`uart`、`i2c`、`spi`、`analog`、`front_end`、
`pwm` 和 `qei`，分别通过 8/8、2/2、6/6、1/1、4/4、2/2、5/5、4/4、4/4、3/3、1/1、6/6、
3/3、3/3 和 3/3 项，共 55 项测试。
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

2026-10-28 `analog` profile 在 HPM5300EVK RevD 上通过 6/6 项测试，两次独立
物理复位均通过；`west flash --verify` 成功。日志为
`build/hw-analog-final-reset1.uart.log` 和
`build/hw-analog-final-reset2.uart.log`。接线为 PB08/DAC0.OUT（J7-20）→
PB05/ADC0.IN13（P1-32），映射已与 EVK 接口资料和板级 pinmux 核对，实物位置由操作者确认。

覆盖 ADC0 12-bit 同步/异步序列读取、双通道、TRGM 上升沿触发和抢占 DMA，
以及 DAC0 直接输出、step endpoint 和内部 buffer 输出；DAC 电平由 ADC0 以
160 code 容差校验。驱动按 ADC 时钟源选择同步模式，等待整条 sequence 完成后读取
DMA 数据；DAC step/buffer 路径限制输出时钟低于 1 MHz，并在启动 DMA 前 flush buffer。
本结果只验证上述单一环回，不包含模拟精度、噪声、线性、外部波形时序或其他 ADC/DAC
实例。analog 加入后 12 个 profile 的 Twister build-only 回归通过，0 失败/错误/警告，
报告为 `build/twister-hardware-analog-final/twister.json`。

2026-10-29 `front_end` profile 在 HPM5300EVK RevD 上两次独立物理复位均通过
3/3 项测试，各耗时 4.502 s；`west flash --verify` 成功。最终 UART 日志为
`build/hw-front-end-final-reset1.uart.log` 和
`build/hw-front-end-final-reset2.uart.log`，烧录记录为
`build/hw-front-end-final-flash.log`。接线见下文模拟前端回环节，由操作者确认。

OPAMP0 follower 的 DAC0 输入为 768、1536、2304、3072；两次 ADC0.IN15 回读分别为
774/1546/2313/3084 和 776/1546/2316/3080，均满足既定 ±160 code 容差。ACMP1
检查 8 次下降沿、8 次上升沿、反向边沿不回调、停用时 16 次跨阈值无回调及重装恢复，
共 17 次 ISR 回调；另检查双边沿 16 次、pending 读清、重绑立即在线程派发及后续 ISR
恢复，共 18 次回调。硬件边沿检查 ISR 上下文、设备、user data 和精确次数；pending
重绑检查回调在 API 返回前执行。驱动无需修改。

加入 front_end 后 13 个 profile、49 个测试项的 Twister build-only 回归通过，
0 失败/错误，Twister 汇总及编译器无警告；原始构建日志仍有既存 CMake 空库和未使用
`TC_NAME` 提示。报告为 `build/twister-hardware-front-end-final/twister.json`。
测试保持 XIP、cache 和 PMP；本结果不建立校准精度、噪声或传播延迟验收。

2026-10-29 `pwm` profile 最终两次独立物理复位均通过 3/3、0 fail/skip，各耗时
3.178 s；`west flash --verify` 成功。最终日志为
`build/hw-pwm-final-reset1.uart.log`、`build/hw-pwm-final-reset2.uart.log` 和
`build/hw-pwm-final-flash.log`。两次连续捕获均为 160 MHz、回绕上取整 1678 ms，
实际运行 1929 ms 并收到 1928 次回调。初次版本通过 3/3、3.146 s，随后补充了
blocking timeout 清理/恢复及 output/capture 独占检查，以上最终日志对应补充后的版本。
标准捕获现在自行启动独立的满 28-bit 计数器，将硬件时间戳的原始差值右移 4 位后
返回 Zephyr 周期单位，并阻止同 block 输出重配置破坏捕获时基。

周期 0.5/1/2 ms、占空比 25/50/75%、输出与捕获各两种极性及 PERIOD/PULSE/BOTH
共 108 种组合通过；预设容差为目标的 1%，最低 32 cycles。另验证微秒/纳秒 blocking
API 换算。连续 1 ms/25% 捕获检查每次回调的周期、脉宽、设备、通道、user data 和 ISR
上下文，跨越完整 28-bit 回绕，并检查 busy、停用和重装。0/100% 正反极性使用 PB11
各采样 20 次确认恒定电平，且不产生捕获回调。随后故意触发 10 ms blocking timeout，
确认 callback 清除和恢复；UART 的 `W: pwm capture timed out` 为该用例的预期输出。

加入 PWM 后 14 个 profile 的 Twister build-only 回归通过，发现 52 项测试，
0 失败/错误及编译器警告，Twister 汇总无警告；原始 CMake 既存空库和 `TC_NAME`
提示仍存在。报告为 `build/twister-hardware-pwm-final/twister.json`。另完成
`CONFIG_PWM_CAPTURE=n` 构建，记录为 `build/hw-pwm-no-capture.build.log` 和
`build/hw-pwm-no-capture-final.build.log`。首次回归的测试局部输出变量初始化告警已修复，
失败记录保留在 `build/twister-hardware-pwm-uninitialized.log`，不计入最终通过结果。

2026-10-29 `qei` profile 在 RevD 上两次独立物理复位均通过 3/3 项，
各耗时 0.158 s；`west flash --verify` 成功。日志为
`build/hw-qei-final-reset1.uart.log`、`build/hw-qei-final-reset2.uart.log` 和
`build/hw-qei-final-flash.log`。GPIO 生成 AB Gray 序列，逐边沿核对相位计数、圈数和
角度：CPR 64 下 512 个正反向边沿穿越正负两圈并回零；另验证比较值 8/16 的
两路中断、精确回调次数/ISR/设备/user data、屏蔽、轮询、逐位清除及重绑定。
解绑时同时保留已由 ISR 锁存的事件和被屏蔽的硬件事件。运行时复位将 CPR
从 64 改为 128，重配比较源，再检查 512 个正反向边沿。

驱动修复共享 AHB 时钟初始化和数字模式的计数/角度读回：使用同一个 READ 快照的
PH/Z，按 CPR 换算角度；更换回调不再丢弃 pending 事件。比较器保留 SDK 原有的
`SPDCMP=0` 参与条件；移除该条件的诊断版本在比较值停留时挂起，不能作为修复。
仅 QEI1 AB 输入通过硬件验证，Z/索引校准、其他模式、速度、模拟精度及外部时序仍待验收。
接线见下文 QEI 回环节，完整记录见
[硬件记录](../../../docs/agent/hardware-bringup.md#qei-profile-hardware-result-2026-10-29)。

加入 QEI 后 15 个已注册 profile 的 Twister build-only 回归全部通过，发现 55 项
测试，0 失败/错误，编译器和 Twister 汇总无警告；原始 CMake 空库及 `TC_NAME`
提示仍存在。报告为 `build/twister-hardware-qei-final/twister.json`。

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
该历史矩阵包含尚未纳入提交的候选文件，不能用当前 15-profile 范围直接复现。

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

下表列出 14 个已上板验收 profile，具体范围及限制见前述运行记录。

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
| `analog` | `hpm_analog_loop` | PB08/DAC0.OUT（J7-20）→ PB05/ADC0.IN13（P1-32）；按 RevD 丝印核对 | ADC 同步/异步、双通道、抢占 DMA、TRGM 上升沿触发；DAC direct/step/buffer 输出由 ADC 回读 |
| `front_end` | `hpm_front_end_loop` | J7-20/PB08 → P1-31/PB04；P1-8/PB08 → P1-40/PB11，见下文 | OPAMP0 follower 四电平回读；ACMP1 单/双边沿精确次数及 ISR 来源、禁用/重装、pending 读清及同步 callback 派发 |
| `pwm` | `hpm_pwm_loop` | P1-31/PB04 → J7-20/PB08；P1-8/PB08 → P1-40/PB11，复用前端接线 | PWM0.P4 输出、PWM1.P4 捕获；108 组周期/占空比/极性/类型，单位换算、连续回绕、停用/恢复、busy/timeout，以及 GPIO 恒定电平 |
| `can` | `hpm5300evk` | MCAN0 静默内部 loopback，无外部 CAN 总线 | 16 帧标准 classic/FD/BRS、8/64 B 数据及 TX/RX ISR；另 20 帧 PTPC0→TSU SOF 64 位捕获，有效/无丢失/递增、8 槽及两次回绕、PTPC 发送前后时间包络 |
| `flash` | `hpm_flash_scratch` | 板载 XPI NOR，允许擦写下述专用测试扇区 | 全扇区擦除、256 B 写读、二次擦除、先填充 XIP 映射 cache 再验证写/擦后的映射一致性 |
| `watchdog` | `hpm5300evk` | 无 | EWDG0 仅中断：每 100 ms 喂狗三次；500 ms 超时后延迟 2200 ms 再喂狗并验证重复超时；ISR 内喂狗；停用后无 IRQ、重装完整周期；500 ms → 2500 ms 分频重配；容差分别 ±80/160 ms |
| `motor` | `hpm5300evk` | 无外部接线，不配置 PWM/pinctrl 输出 | SYNT 推进/冻结/恢复；PLB TYPE_B0 slice15 LOAD/KEEP 和实时比较器；QEO0 四配置×四象限×三通道数值；MMC0 1 turn/s 位移/SYNT 时间一致性及整块停启；GPIOM PA24 可见性切换/恢复读回 |

PA24 是 LIN 12 V 电源使能，未引到 P1。GPIO profile 仅驱动 PB04；motor profile
只切换 PA24 的输入可见性，不配置该引脚输出。

### 模拟前端回环接线

`front_end` profile 验证 OPAMP0 跟随器和 ACMP1 阈值中断。先移除 `analog`
的 PB08 → PB05 跳线，再连接以下两根信号线：

- J7-20（PB08/DAC0.OUT）↔ P1-31（PB04/OPAMP0.INP0，GPIO22）。
- P1-8（PB08/DAC0.OUT，TXD）↔ P1-40（PB11/ACMP1.INN4，GPIO29）。

J7-20 和 P1-8 是同一个 PB08 信号，因此每个针脚只需一根杜邦线。P1-10 是
P1-8 旁边的 RXD；P1-39 是 P1-40 旁边的 GND。不要把这些邻针接入回环，
不需要连接电源脚。引脚映射依据官方 EVK RevC 原理图第 2、6、9 页；本次硬件为
RevD，接线由操作者按实物核对。

OPAMP0.OUT 与 ADC0.IN15 共用 PB00，固件通过该节点回读，不需要第三根跳线。
ACMP1 的正输入使用其内部 8-bit DAC 阈值 128，负输入连接外部 DAC0/PB08。
这种回环不建立 OPAMP 增益矩阵、模拟精度、比较器传播延迟或独立电压校准验收。

### PWM 回环接线与边界

`pwm` profile 直接保留上一轮 `front_end` 两根跳线：P1-31/PB04 现在作为
PWM0.P4 输出，经 J7-20/PB08 进入 PWM1.P4 捕获；P1-8 同属 PB08，再连接
P1-40/PB11，后者仅配置为 GPIO 输入以读取 0/100% 电平。DAC0、OPAMP 和 ACMP
节点保持 disabled，完整复位后不会驱动该网络。原理图依据仍为官方 RevC 第 2、6、9 页，
实际 RevD 接线沿用操作者上一轮确认。无需增加跳线或连接电源、地脚。

测试替换了候选草稿中的 PA08（JTAG TRST）和 PA13（LIN SLP_N），不启用这些脚。
输出与捕获分属 PWM0/PWM1，但共用实际 160 MHz AHB 时钟；DTS 的共享时钟单元不应
解读为独立 24 MHz 时源。本测试可验证周期比值和驱动语义，不证明绝对频率精度。

标准捕获启用时自行启动 28-bit 自由计数器，不需要先设置虚拟 PWM 输出。一个 PWM
block 在本次启动中配置过任何输出（含 0%）、参考或 fault 输出后，不能切换为标准
捕获；活动捕获也会拒绝修改同 block 输出或原始捕获配置。该保守限制防止共享计数器
被重装。被测周期必须小于 `2^28 / pwm_get_cycles_per_sec()`，不支持区分多个回绕。
0.5/1/2 ms 的测试不覆盖高速捕获、长时间屏蔽 IRQ 或硬件无 FIFO 时的覆盖丢样。
互补输出、死区、fault、参考输出以及外部示波器测量仍需独立验收。

## QEI 数字回环

`qei` 使用两根独立跳线：

| GPIO 输出 | QEI1 输入 |
|---|---|
| J7-20 / PB08 | P1-31 / PB04 / QEI1_A |
| P1-40 / PB11 | P1-32 / PB05 / QEI1_B |

从 PWM fixture 切换时保留第一根，将第二根的 P1-8/PB08 端移到 P1-32/PB05。
P1-32 与 P1-31 正对，丝印 GPIO26；P1-30、P1-34 为 GND。仅接信号脚，无需电源或地。
映射与官方 RevC 原理图第 2、6、9 页核对，RevD 实物由操作者确认。
不能保留把 PB08 与 PB11 短接的旧模拟前端/PWM 接法，因为本测试独立驱动 A/B。
Z 不接、不配置 pinctrl，关闭索引校准，以相位回绕更新圈数。普通 GPIO 生成低速
边沿，不能代替外部编码器、电气/滤波/最高频率和模拟解码精度验收。

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
| OPAMP/ACMP | OPAMP1、其他拓扑和增益矩阵、校准精度/噪声；ACMP0、双通道并发、阈值精度、滤波/迟滞、反相、输出脚及传播延迟 |
| QEIV2 | Z/索引校准、PD/UD/single/UVW/sincos、速度、HOME/FAULT/看门狗、其他实例、模拟精度及外部高速时序 |
| PWM | 其他输出/捕获实例组合、绝对频率精度、互补/死区/fault、参考输出、超长周期及高速/长 IRQ 延迟下的丢样 |
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
