<!-- SPDX-License-Identifier: Apache-2.0 -->

# HPM5361 硬件验收测试

本应用对应工作区 `docs/agent/driver-review.md` 的硬件验收矩阵，面向
`hpm5300evk/hpm5361`。各 profile 单独生成一个 Ztest 固件。优先调用 Zephyr 标准
API；TRGM 和内部电机模块使用模块的 HPM 扩展 API，不引入 SDK middleware。
本轮已在 HPM5300EVK RevD 上验证 30 个 profile 实现形成的 32 个测试配置：
`core`、`counter_capture`、`dma`、`crypto`、`flash`、`watchdog`、`can`、`ptpc`、
`motor`、`gpio`、`uart`、`uart_address`、`uart_rs485`、`lin`、`i2c`、
`i2c_fast`、`i2c_fast_plus`、`spi`、
`analog`、`front_end`、`pwm`、`qei`、`qei_pd`、`qei_ud`、`qei_single`、
`qei_uvw`、`usb_device`、`usb_endpoints`、`usb_endpoints_fs`、`usb_host`、
`usb_host_hub` 和 `usb_host_hid_report`，共 150 项测试。三个 I2C 配置各
16/16，分别在 100 kHz、400 kHz 和 1 MHz 下通过；其余配置沿用各自记录中的
上板验收结果。两个 endpoint 配置共用同一份
测试源码。历史候选 profile 的 build-only 记录保留在下文，不代表这些候选已纳入
本轮提交或通过硬件验收。

2026-10-27 `core` 运行耗时 0.923 s。UART 完整日志位于工作区
`build/hw-core-gptmr-fix-uart.log`，最终结果为 `PROJECT EXECUTION SUCCESSFUL`。
日志中的重复启动横幅来自烧录/校验复位；温度阈值测试及其余测试最终均通过。
2026-11-04 `uart` 扩展为 5 项，新增连续双 buffer DMA RX。首缓冲区设置为 97 B，
回调在 `UART_RX_BUF_REQUEST` 后提交第二块 320 B 缓冲区；257 B 自回环数据跨两个
buffer 连续接收，逐事件校验各 buffer 的 RX_RDY offset、累计内容、释放顺序和
`UART_RX_DISABLED`。正式固件两次独立复位均 5/5 通过，每次耗时 0.119 s。
首轮诊断中新用例已通过，但既有 RX disable/re-enable 用例因测试未重置新增的
per-buffer consumed 计数而失败；修正测试状态后全部通过。失败日志保留为
`build/hw-uart-dual-rx-run1.uart.log`。正式 build/flash 为
`build/hw-uart-dual-rx-final-build.log`、`build/hw-uart-dual-rx-fix2.flash.log`；
UART/reset 证据为 `build/hw-uart-dual-rx-fix2-run{1,2}.{uart,reset}.log`。

2026-11-04 `uart` 随后扩展为 6 项，新增 CTS 硬件背压。测试不启用 RX DMA，
发送 257 B 后等待 40 ms，确认异步 TX 未完成；随后通过 `uart_poll_in()` 排空
接收 FIFO，验证 TX 自动恢复、257 B 顺序完整、TX_DONE 到达且无 overrun/framing
错误。该路径真实经过 PB05 RTS → PB04 CTS 回环和 auto-RTS/auto-CTS。两次独立复位
均为 6/6，每次耗时 0.182 s。build/flash 为
`build/hw-uart-cts-final-build.log`、`build/hw-uart-cts-flash.log`，UART/reset
证据为 `build/hw-uart-cts-run{1,2}.{uart,reset}.log`。

2026-11-04 新增独立 `uart_rs485` profile，验证自动 DE 时序。UART1 配置为
`UART_CFG_FLOW_CTRL_RS485` 后连续发送 64 B；PB05 DE 输出回环到 PB04 GPIO，
PB07 TX 输出回环到 PB06 GPIO，两个边沿均由 `k_cycle_get_32()` 打时间戳。测试
确认 64 B 突发只有一次 DE 上升/下降、DE 在 TX 首边沿前拉高、在 TX 末边沿后释放、
DE 脉冲覆盖完整 TX 突发，释放延迟不超过 500 µs；TX 突发约 5.56 ms。正式固件
两次独立复位均 1/1 通过，每次耗时 0.006 s。build/flash 为
`build/hw-uart-rs485-build.log`、`build/hw-uart-rs485-flash.log`，UART/reset
证据为 `build/hw-uart-rs485-run{1,2}.{uart,reset}.log`。

2026-11-04 新增独立 `uart_address` profile，验证 UART 九位地址匹配。UART1
配置 address0 `0x55` 和 address1 `0x77`，UART2 通过 TXEN_9BIT 发送地址标记帧；
错误地址 `0x66` 被接收端硬件过滤，两个正确地址均产生 address-match 和
address-match-idle 事件，并完整接收地址字节与 8 B 数据。测试同时验证标志
读后清除、非法配置返回 `-EINVAL`、地址匹配关闭路径及无 UART 错误。正式固件
两次独立复位均 1/1 通过，每次耗时 0.019 s。build/flash 为
`build/hw-uart-address-build.log`、`build/hw-uart-address-flash.log`，UART/reset
证据为 `build/hw-uart-address-run{1,2}.{uart,reset}.log`。

2026-11-04 `core` 扩展为 10 项，新增 GPTMR top/reload/flags 与 32-bit wrap。
正式固件在烧录后两次独立复位均 10/10 通过，每次耗时 0.897 s，单次启动 banner
且以 `PROJECT EXECUTION SUCCESSFUL` 结束。top 测试验证 2 ms 周期 callback、
`COUNTER_TOP_CFG_DONT_RESET` 保留计数值、`RESET_WHEN_LATE` 复位、活跃 alarm
时返回 `-EBUSY`、`set_value(top+1)` 返回 `-EINVAL`，以及 `set_value()` 精确读回。
wrap 测试将计数器置于 `UINT32_MAX` 前约 5 ms 并实测跨零。首轮诊断同时发现
`counter_set_value()` 调用 SDK helper 后读回少 1，驱动改为直接写 `CNTUPTVAL`。
根因修复前日志为 `build/hw-core-counter-top-run1.uart.log`；正式 build/flash 为
`build/hw-core-counter-top-final-build.log`、`build/hw-core-counter-top-fix3.flash.log`，
正式 UART/reset 为 `build/hw-core-counter-top-fix3-run{1,2}.{uart,reset}.log`。

具体覆盖和限制见[硬件记录](../../../docs/agent/hardware-bringup.md#core-profile-hardware-result-2026-10-27)。

2026-11-06 新增独立 `counter_capture` profile，使用 Zephyr 标准
`counter_capture_configure()`、`counter_enable_capture()` 和
`counter_disable_capture()` API。唯一跳线为 PB07/P1-5 GPIO 输出 →
PB06/P1-3 GPTMR0_CAPT_0 输入，无需电源或地。七个用例覆盖参数/状态拒绝、
rising/falling/both-edge continuous、rising/falling/both-edge single-shot、
捕获 tick 区间、设备/通道/flags/callback 上下文、无重复回调以及禁用后不再捕获。

首次上板仅 API 校验用例通过，6 个功能用例均等待 CAPF 超时。检查确认
`hw_fixture` 没有驱动实例，`pinctrl-0` 不会自动应用，测试也未把 PB07 配置为
GPIO 输出；这与既有 GPIO/QEI 夹具的初始化方式不一致。测试补充
`pinctrl_apply_state()` 和 `gpio_pin_configure_dt(GPIO_OUTPUT_INACTIVE)` 后，
最终源码经 `west flash --openocd /usr/local/bin/openocd --verify` 烧录校验，
两次独立复位均为 7/7、0 fail/skip，每次 0.258 s，并均以
`PROJECT EXECUTION SUCCESSFUL` 结束。

构建/烧录证据为 `build/hw-gptmr-capture-final.{build,flash}.log`、
`build/hw-gptmr-capture-twister-final.log`，UART/reset 证据为
`build/hw-gptmr-capture-final-run{1,2}.{uart,reset}.log`；首轮 1/7 诊断日志
`build/hw-gptmr-capture-run1.uart.log` 保留但不计入验收。GPTMR 双边沿只有一个
共享 CAPF 标志且没有事件 FIFO；若多个边沿在 ISR 执行前合并，驱动只能依据
`CAPPOS`/`CAPNEG` 变化报告单个可恢复事件。其他捕获通道/实例、输出比较、
64-bit 捕获和 IRQ 延迟压力仍未验证。当前 32 配置/150 项的 build-only 报告为
`build/twister-hardware-counter-capture-final/twister.json`。

2026-10-29 已在 Zephyr v4.5.0 上重新执行七个无需接线的 profile：
`core` 8/8、`dma` 2/2、`crypto` 6/6、`flash` 1/1、`watchdog` 4/4、
`can` 2/2 和 `motor` 5/5，共 28 项全部通过，且每个镜像均通过
`west flash --verify`。日志位于工作区
`build/hw-{core,dma,crypto,flash,watchdog,can,motor}-zephyr-4.5.0*`。
其余 profile 的旧结果仍属于先前 `f78c4deb4523` 基线，需在 v4.5.0 上重跑。

2026-10-29 已在 Zephyr v4.5.0 上用 P1-31（PB04/GPIO22）↔ P1-32
（PB05/GPIO26）跳线复测 `gpio`：烧录后 4/4 通过（0.491 s），两次独立复位
均 4/4 通过（0.491/0.490 s），`west flash --verify` 成功。日志为
`build/hw-gpio-zephyr-4.5.0-run{1,2,3}*.uart.log`。

2026-10-29 已在 Zephyr v4.5.0 上用两根跳线复测 `uart`：
P1-31/PB04 CTS↔P1-32/PB05 RTS，以及 P1-5/PB07 TX↔P1-3/PB06 RX。
烧录首轮和三次独立复位均 4/4 通过（每次 0.094 s），`west flash --verify`
成功。日志为 `build/hw-uart-zephyr-4.5.0-run{1,2,3,4}*.uart.log`。

2026-10-29 已在 Zephyr v4.5.0 上用两根跳线复测 `i2c`：
P1-28/PB02 SCL↔P1-5/PB07 SCL，P1-27/PB03 SDA↔P1-3/PB06 SDA。
烧录首轮和两次独立复位均 3/3 通过（每次 0.037 s），`west flash --verify`
成功。日志为 `build/hw-i2c-zephyr-4.5.0-run{1,2,3}*.uart.log`。

2026-11-05 先将 `i2c` 扩展到 7 项。沿用上述两根跳线，以 I2C1 target 地址
`0x155` 验证 10-bit 单写、单读和写后 repeated START 读，并增加卡死 SDA
总线恢复：测试先注销 I2C1 target，将 PB07 切为 GPIO 上升沿监测、PB06 切为
开漏输出并拉低 SDA；I2C0 的 `i2c_recover_bus()` 产生 9 个 SCL 时钟，回调在
第 9 个上升沿释放 SDA。测试精确断言 9 个边沿、回调来源/上下文、两条线回高，
随后恢复 I2C1 pinctrl 和目标并完成普通写，证明控制器已重新初始化。

10-bit repeated START 的首个实现暴露 SDK 地址阶段会重新发送完整 10-bit 地址，
target 因此拒绝。驱动改为手工发送短读地址头，再调用 SDK sequence receive
读取数据；恢复路径在 `RESET_ON` 后重新调用 `i2c_init_master()`。最终源码
重新构建并经 `west flash --verify` 后，三次独立复位均为 7/7（每次 0.045 s）。
证据为 `build/hw-i2c-recovery-final.build.log`、
`build/hw-i2c-recovery-final.flash.log` 和
`build/hw-i2c-recovery-final-run{1,2,3}.{uart,reset}.log`。

随后将 `i2c` 扩展到 8 项，增加 SCL 被持续拉低的恢复拒绝路径。测试同时拉低
PB07/SCL 和 PB06/SDA，确认 `i2c_recover_bus()` 立即返回 `-EBUSY` 且不产生
恢复时钟；释放 SCL 后再次调用恢复，验证第 9 个上升沿释放 SDA、两条线回高，
随后完成普通 target 写入。最终镜像经 `west flash --verify` 后，三次独立复位
均为 8/8（每次 0.047 s）。证据为
`build/hw-i2c-scl-recovery-final.{build,flash}.log` 和
`build/hw-i2c-scl-recovery-final-run{1,2,3}.{uart,reset}.log`；全量
build-only 报告为 `build/twister-i2c-scl-full-final/twister.json`。

随后将 `i2c` 扩展到 9 项，增加 SMBus Block Process Call。测试以命令 `0xa0`
发送 8 B，并在目标端返回 6 B 响应和 PEC `0x6e`；校验目标写入、读回调和 STOP，
再破坏响应 PEC 确认返回 `-EIO` 且不发布接收长度，恢复 PEC 后重试成功。最终镜像
经 `west flash --verify` 后，三次独立复位均为 9/9（每次 0.053 s）。证据为
`build/hw-i2c-block-pcall-final.{build,flash}.log` 和
`build/hw-i2c-block-pcall-final-run{1,2,3}.{uart,reset}.log`；全量
build-only 报告为 `build/twister-i2c-block-pcall-final/twister.json`，共
29 个配置、99 项测试，0 失败/错误/告警。

随后将 `i2c` 扩展到 10 项，增加零计数响应。无 PEC block read、PEC block read
和 PEC Block Process Call 均验证返回 `-ENODATA` 且不发布输出长度，随后立即执行
有效读或 process call 证明总线已恢复。诊断发现旧驱动会把空块当作成功；修复后
又发现无 PEC 分支只发出 STOP 却未等待完成并超时。由于 HPM 控制器不能在 ACK
count 字节后直接停止，无 PEC 路径现读取 1 个丢弃字节并 NACK/STOP 释放总线。
最终镜像经 `west flash --verify` 后，三次独立复位均为 10/10（每次 0.057 s）。
证据为 `build/hw-i2c-zero-count-final5.{build,flash}.log` 和
`build/hw-i2c-zero-count-final5-run{1,2,3}.{uart,reset}.log`；全量
build-only 报告为 `build/twister-i2c-zero-count-final/twister.json`，共
29 个配置、100 项测试，0 失败/错误/告警。

2026-11-05 将同一套 I2C 用例扩展为 16 项，并新增 400 kHz Fast 与 1 MHz
Fast Plus 配置；原有 100 kHz 配置作为标准速率回归。三个配置均显式设置控制器
和目标端时序，各经 `west flash --verify` 后完成三次独立复位，均为 16/16：
标准模式每次 0.087-0.089 s，Fast 每次 0.050 s，Fast Plus 每次
0.042-0.044 s。证据为
`build/hw-i2c-{standard,fast,fast-plus}-isrfinal.{build,flash}.log` 和
`build/hw-i2c-{standard,fast,fast-plus}-isrfinal-run{1,2,3}.{uart,reset}.log`。

速率矩阵发现两个真实的目标模式 ISR 缺陷。100 kHz 下，仅因 ADDRESS_HIT
置位就排空 RX FIFO 会在 BYTERECV 尚未置位时读到幻影 `0x00`，污染命令 cursor
和块长度；现在只有本轮状态快照包含 BYTERECV 时才排空，Fast Plus 中
ADDRESS_HIT/BYTERECV 合并的场景仍由同一个 ISR 后续分支处理。Fast Plus
quick 命令还可能把 ADDRESS_HIT 与真实 STOP 合并在一次中断中；ISR 现保留地址
命中的 STOP 锁存，并在分派 target stop 回调前重新检查 `BUS_BUSY`，只丢弃
repeated START 带来的陈旧 STOP。总线恢复临时以 100 kHz 产生时钟，结束后恢复
配置速率。

最终全量 build-only 回归构建 31 个配置、发现 138 项测试，0 失败/错误/告警：
`build/twister-i2c-fast-isrfinal/twister.json`。其中 `i2c`、`i2c_fast`、
`i2c_fast_plus` 各 16 项。Zephyr 100 列 clang-format、`git diff --check` 和
checkpatch 均无问题。

2026-10-29 已在 Zephyr v4.5.0 上复测 `spi`。第一次接错线时同步 mode 0
首字节为 `tx=5a rx=00`；按 P1-19/PA29 MOSI↔P1-21/PA28 MISO 更正后，
烧录首轮和三次独立复位均 1/1 通过（每次 0.018 s），覆盖 mode 0–3 的同步和
linked-DMA 回环。`west flash --verify` 成功。有效日志为
`build/hw-spi-zephyr-4.5.0-fixedwire-run{1,2,3,4}*.uart.log`；接线错误时的
日志只保留作诊断，不计入验收。

2026-11-05 将 `spi` 扩展为 6 项。最终固件经 `west flash --verify` 后完成
两次独立复位，均为 6/6、0 fail/skip，每次耗时 0.027 s，并以
`PROJECT EXECUTION SUCCESSFUL` 结束。接线仍只需 PA29/MOSI（P1-19）↔
PA28/MISO（P1-21）；PA27/SCLK（P1-23）、PA26/native CS0（P1-24）和
PB04/GPIO CS（P1-31）均不需跳线。新增覆盖包括 16/24/32-bit 同步传输、
16/32-bit 双块异步链接 DMA、`SPI_HOLD_ON_CS` 保持与释放、native CS 传输、
4/12/40-bit 位宽拒绝及拒绝后的 8-bit 恢复。

首轮上板发现两个驱动缺陷：宽字位双块异步传输被限制为 workqueue 回退，违反
测试对 DMA ISR 完成路径的断言；native CS 使用临时 `spi_config` 后，
`spi_release()` 通过上下文中的旧指针强制释放 GPIO CS，访问了已失效的栈对象
并触发 load access fault。驱动现允许 8/16/32-bit 链接 DMA，并让 release 使用
调用者传入的有效配置。失败证据保留为 `build/hw-spi-extended-run1.uart.log`
和 `build/hw-spi-extended-run1.reset.log`，不计入验收；正式 build-only 报告为
`build/twister-spi-extended-fix1-20261105/twister.json`，烧录记录为
`build/hw-spi-extended-fix1.flash.log`，UART/reset 证据为
`build/hw-spi-extended-fix1-run{2,3}.{uart,reset}.log`。扩展后的完整
31 配置 build-only 回归发现 143 项测试，0 失败/错误/告警：
`build/twister-hardware-spi-extended-final/twister.json`。

本轮没有逻辑分析仪波形，因此 native CS 的电气时序、setup/hold 精度和外部从设备
兼容性仍不在验收范围内；半双工、双/四线以及 `SPI_LOCK_ON` 也未覆盖。

2026-10-29 已在 Zephyr v4.5.0 上用 J7-20/PB08 DAC0.OUT 到
P1-32/PB05 ADC0.IN13 复测 `analog`。烧录首轮和两次独立复位均 6/6 通过
（每次 0.172 s），`west flash --verify` 成功。日志为
`build/hw-analog-zephyr-4.5.0-run{1,2,3}*.uart.log`。

2026-10-29 已在 Zephyr v4.5.0 上用 J7-20/PB08→P1-31/PB04 和
P1-8/PB08→P1-40/PB11 复测 `front_end`。烧录首轮和两次独立复位均 3/3 通过
（每次 4.502 s），`west flash --verify` 成功。日志为
`build/hw-front-end-zephyr-4.5.0-run{1,2,3}*.uart.log`。

2026-10-30 已在 Zephyr v4.5.0 上完成 `pwm` 稳定性修复和复测。此前的偶发
失败源于 PWM 输出重配时只更新 STA shadow，没有真正复位计数器；旧 CNT 大于
新的 RLD 时，首个捕获边沿可能延迟约 100 ms。驱动现改为调用
`pwm_timer_reset()`。修正后烧录首轮 3/3 通过（3.095 s），随后 12 次独立
复位全部 3/3 通过。日志为 `build/hw-pwm-fix3-flash-*.uart.log` 和
`build/hw-pwm-fix3-stress-*.uart.log`。

2026-10-30 已在 Zephyr v4.5.0 上用 J7-20/PB08→P1-31/PB04（QEI1_A）和
P1-40/PB11→P1-32/PB05（QEI1_B）复测 `qei`。烧录首轮和两次独立复位均
3/3 通过（每次 0.158 s），`west flash --verify` 成功。日志为
`build/hw-qei-zephyr-4.5.0-run{1,2,3,4}*.uart.log`。

2026-11-03 新增 `qei_pd` profile，并以同一组 QEI1_A/B 跳线验证 pulse/direction
模式。首版用例按“每个完整脉冲计 1 count、方向高为正向”建模，三项均失败；实测该
SDK 配置同时启用 A 相上升沿和下降沿，且当前板级路径中方向低递增、方向高递减。
用例据此明确覆盖方向低 10 个脉冲 `+20`、方向高 10 个脉冲 `-20`、CPR 64 的 32
脉冲回绕，以及运行时 CPR 64→128。修正后烧录首轮和两次独立复位均 4/4 通过
（每次 0.270 s），`west flash --verify` 成功。证据为
`build/hw-qei-pd-fixed-run{1,2}.uart.log`、
`build/hw-qei-pd-fixed-reset{1,2}.log`、`build/hw-qei-pd-build2.log` 和
`build/hw-qei-pd-flash2.log`；首版失败日志 `build/hw-qei-pd-run2.uart.log`
仅保留作诊断，不计入验收。

2026-11-03 新增 `qei_ud` profile，沿用同一组 QEI1_A/B 跳线验证独立的 up/down
脉冲输入，并保持 QEI1_Z 断开。四个用例覆盖 10 个 up 脉冲 `+20`、10 个 down
脉冲 `-20`、CPR 64 下完整 up/down 一圈回到 0，以及运行时 CPR 64→128 后完整
上下各一圈回到 0。当前 SDK UD 配置同样启用 A/B 双沿，因此每个完整 up 或 down
脉冲周期均计 2 counts。修正后的四用例镜像在完整烧录后经两次独立复位均 4/4
通过（每次 0.233 s），`west flash --verify` 成功。证据为
`build/hw-qei-ud-flash4.log`、`build/hw-qei-ud-run{2,3}.uart.log`、
`build/hw-qei-ud-reset{2,3}.log` 和 `build/hw-qei-ud-build1.log`。首次烧录误用
不含 `hpm_xpi` 的 Zephyr SDK OpenOCD，记录为 `build/hw-qei-ud-flash1.log` 并
排除；UD 验收不需要修改驱动或 SDK。

2026-11-04 新增 `qei_uvw` profile，使用 QEI1 的 U/V/W 三根独立信号跳线。
三个正式用例覆盖六扇区的正向 UVW 相序、GPIO 与 QEI A/B 回读、Q32 扇区中心
POSITION、编码器 count/角度换算、正反各两圈 `PHASE_CNT` 往返，以及运行时
counts-per-revolution 64→128 重配。首次正式运行因 PB07/W 回读缺少 pad
`input-enable` 而只有 1/3，正反相序用例已证明 UVW 解码本身正常；补齐 overlay
后重新构建并烧录，三次独立复位均 3/3 通过（每次 0.113 s），每轮仅有一个启动
banner，均以 `PROJECT EXECUTION SUCCESSFUL` 结束。构建、烧录和 UART 证据为
`build/hw-qei-uvw-build-formal4.log`、`build/hw-qei-uvw-flash-formal4.log` 和
`build/hw-qei-uvw-run-formal4-{1,2,3}.uart.log`；对应复位记录为
`build/hw-qei-uvw-run-formal4-{1,2,3}.reset.log`。旧 1/3 日志保留作诊断，
不计入验收。

2026-11-04 新增 `qei_single` profile，只保留 PB08 到 QEI1_A 一根脉冲跳线，
并断开 QEI1_B/Z。三个正式用例覆盖 10 个脉冲产生 `+20` counts、32 个脉冲完成
CPR 64 一圈，以及运行时 CPR 64→128 后 64 个脉冲完成 128 counts 一圈；同时检查
`QEI_CFG` 只启用 A 相双沿。首次烧录日志前部混入上一 UVW 镜像输出，且烧录/校验
过程产生多个启动 banner，因此不作为干净验收记录；随后三次独立 JTAG 复位均
3/3 通过（每次 0.138 s），每轮仅一个启动 banner，均以
`PROJECT EXECUTION SUCCESSFUL` 结束。构建、烧录和 UART 证据为
`build/hw-qei-single-build1.log`、`build/hw-qei-single-flash1.log` 和
`build/hw-qei-single-reset-run{1,2,3}.uart.log`；对应复位记录为
`build/hw-qei-single-reset-run{1,2,3}.log`。单相模式没有方向输入，因此不声明反向
计数验收。

2026-11-04 新增 `ptpc` profile，同时启用 PTPC0/PTPC1，无需外部接线。首轮诊断
发现驱动只使能了定时器，却没有执行硬件要求的 `INIT_TIMER` 脉冲，两个计数器均
停在 `0.000000000`，因此 PPS IRQ 不产生。驱动改为在 `ptpc_init()` 后调用
`ptpc_init_timer()`；同时按手册修正 PPS API 的 4 位控制值语义，并绕过 SDK 对
合法最大值 15（数字模式 16.384 kHz）的错误拒绝。正式四用例覆盖两通道独立设置
时间和正负调整、`1.05/0.95` 速率调整、PPS 控制值 0/1/2/4/15、事件锁存/清除，
以及 PTPC0/PTPC1 共享 IRQ 39 的独立 compare 回调。烧录首轮和三次独立复位均
4/4 通过（每次 7.162 s），每轮仅有一个启动 banner，均以
`PROJECT EXECUTION SUCCESSFUL` 结束。实测控制值 0/1/2/4 在 1101 ms 内分别收到
1/2/4/15 次边沿；`1.05/0.95` 速率测得约
`+49956~49967/-50015~50017 ppm`。构建、烧录和 UART
证据为 `build/hw-ptpc-build-final.log`、`build/hw-ptpc-flash-final.log` 和
`build/hw-ptpc-reset-final-run{1,2,3}.uart.log`；对应复位记录为
`build/hw-ptpc-reset-final-run{1,2,3}.reset.log`。首轮诊断日志
`build/hw-ptpc-diagnostic{1,2,3}.uart.log` 保留作缺陷定位，不计入验收。

2026-10-30 已在 Zephyr v4.5.0 上复测 `usb_device`。首次混合记录在虚拟机
挂载前等待 DTR 超时，已排除；随后三次“复位后立即启动 CDC peer”均 1/1 通过
（1.181/1.393/1.187 s），高速、MPS 512、27 次交换，每方向 11334 字节校验。
日志为 `build/hw-usb-device-zephyr-4.5.0-clean{1,2,3}-*.{uart,peer}.log`。

2026-10-30 已在 Zephyr v4.5.0 上复测 `usb_endpoints` 和
`usb_endpoints_fs`。每个配置在独立复位后均 1/1 通过两次：HS 为
3.431/2.307 s、MPS 512；FS 为 3.633/6.733 s、MPS 64。每次均覆盖 15 组
非控制 IN/OUT、405 次交换和每方向 170010 字节。日志为
`build/hw-usb-endpoints{,-fs}-zephyr-4.5.0-*.{uart,peer}.log`。首次
VMware 挂载后的瞬时 `no langid` 错误，以及一次 VMware 未暴露设备的 HS
复位，均排除在验收结果之外。

2026-10-30 已在 Zephyr v4.5.0 上通过 `usb_host`。使用带 OTG 标识的 USB-C 转
USB-A 适配器，有线 boot keyboard（VID:PID `1c4f:00b7`，low speed）在 J1
USB_OTG 上完成枚举；HID SET_PROTOCOL/SET_IDLE 和 GET_PROTOCOL 回读通过，
随后收到 `a` 键按下及全释放的 8 字节 interrupt-IN 报告。测试 1/1 通过，
耗时 21.606 s，日志为
`build/hw-usb-host-wired-keyboard-keypress-090344.uart.log`。同一适配器还
成功枚举 `0079:0126` 无线手柄，证明并非单一键盘偶然匹配。
随后一次独立物理复位再次 1/1 通过，耗时 0.795 s；日志为
`build/hw-usb-host-wired-keyboard-retest2-092603.uart.log`。提交前又执行
一次烧录、校验和物理复位复测，1/1 通过（16.891 s），日志为
`build/hw-usb-host-wired-keyboard-retest3-093111.uart.log`。

2026-11-03 新增 `usb_host_hid_report` profile，并沿用上述 OTG 适配器和有线
boot keyboard 接线。测试完整读取 65 字节 HID Report Descriptor，发送
`SET_PROTOCOL(report)` 后读回 `GET_PROTOCOL=1`，并验证按键非零报告
`00 00 04 00 00 00 00 00` 和全零释放报告。烧录后首轮与一次独立物理复位均
1/1 通过，耗时 7.614/6.058 s；`west flash --verify` 成功。证据为
`build/hw-usb-host-hid-report-run{1,2}.uart.log`、
`build/hw-usb-host-hid-report-run1.flash.log` 和
`build/hw-usb-host-hid-report-build.log`。首次串口捕获因 OpenOCD 访问同一
FT2232 时串口重枚举而中断，记录在 `...-capture-attempt.uart.log`，不计入验收。

2026-11-04 新增 `usb_host_hub` profile，使用 GenesysLogic `05e3:0610` USB 2.1
High-Speed、Single-TT、四端口自供电 Hub，下游 Port 1 接 VID:PID `2a7a:8a57`
低速 boot keyboard。验证过程中发现并修复两个 HPM UHC 问题：连接事件在 USB2
High-Speed chirp 前把设备误报为 Full-Speed，以及驱动把非 Halted qTD 的粘滞
`XACTERR` 误判为 `-EIO` 并丢弃已成功收到的数据。修复后在连接阶段完成 port
reset、按 reset 后 PSPD 上报真实速度，并且只在 qTD `Halted=1` 时判定失败。
正式用例同时断言键盘确有 Hub 父设备、Hub 为 High-Speed、键盘为 Low-Speed 且
层级为 2。烧录首轮和三次独立复位均 1/1 通过，耗时分别为 0.972/0.726/0.598/
0.689 s；每轮仅有一个启动 banner，均以 `PROJECT EXECUTION SUCCESSFUL` 结束。
构建和烧录证据为 `build/hw-usb-host-hub-formal-build-final.log` 与
`build/hw-usb-host-hub-final-flash.log`；UART/reset 证据为
`build/hw-usb-host-hub-final-{flash,reset1,reset2,reset3}.uart.log` 和
`build/hw-usb-host-hub-final-reset{1,2,3}.log`。诊断日志
`build/hw-usb-host-hub-token-run1.uart.log` 保留作 qTD 失败定位，不计入验收。

2026-11-04 新增 `qei_uvw` 后，对注册后的全部 profile 重新执行 Twister build-only
回归：23/23 配置构建成功，发现 73 项测试，0 失败/错误，无编译器或 Twister
汇总警告；报告为 `build/twister-qei-uvw-registered/twister.json`。

2026-11-04 新增 `qei_single` 后再次执行 Twister build-only 回归：24/24 配置构建
成功，JSON 中合计 76 项测试，0 失败/错误，无编译器或 Twister 汇总警告；报告为
`build/twister-qei-single-registered/twister.json`。

2026-11-04 新增 `ptpc` 后再次执行 Twister build-only 回归：25/25 配置构建成功，
JSON 中合计 80 项测试，0 失败/错误，无编译器或 Twister 汇总警告；报告为
`build/twister-ptpc-registered/twister.json`。

2026-11-04 新增 `usb_host_hub` 后再次执行 Twister build-only 回归：26/26 配置
构建成功，JSON 中合计 81 项测试，0 失败/错误，无编译器或 Twister 汇总警告；
报告为 `build/twister-usb-host-hub-registered/twister.json`。

截至该轮扩展，当时 26 个注册配置均已在 v4.5.0 上构建通过，共注册 81 项测试，
均已通过硬件验收（81/81）；当前 32 个配置的后续结果以上文汇总为准。Host 验收
仍需操作者按键；RDC、SEI 协议和 USB
suspend/remote wakeup 验收现由用户暂缓，详见下文。PWM 接线可拆除，Host
测试可保留 OTG 适配器和有线 boot keyboard。

2026-11-03 的构建更新在 UHC control 路径增加 buffer 容量校验：当 `wLength`
超过 IN buffer tailroom 或 OUT buffer 数据长度时立即返回错误，不再静默缩短
qTD 数据阶段；`usb_host` 配置同时加入 undersized control IN buffer 断言。
该变体已通过 HPM5361 构建，但尚未重新执行需要键盘交互的 Host 硬件验收，
因此此前 1/1 的 Host control/interrupt 结果仍作为硬件基线。

RAM 模式尚未通过 v4.5.0 硬件验收：Zephyr 镜像把代码链接在 DLM
`0x80000`，入口为 `0x80018`，而 SDK RAM 链接脚本把代码放在 ILM、仅把数据
放在 DLM。`west flash --verify` 能写入并校验 21064 B，但随后串口运行的是
NOR 中旧镜像；改用 `halt` 后 DLM 写入报 `abstractcs=0x8000304`。ILM/DLM
链接布局、可执行映射和 runner 启动流程均需继续调查，日志为
`build/hpm5300evk-bringup-ram-zephyr-4.5.0*`。

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
2026-10-29 的初始轮仅验证 QEI1 AB 输入。2026-11-03 增加第四个用例 `test_periodic_speed_from_standstill`：验证首次采样基线、
静止有效零速、正反向 RPM 计数差分，以及模式重配后的测速状态复位。
`west flash --verify` 成功；两次独立物理复位均为 4/4、0 fail/skip，各耗时
0.251 s；其中周期测速用例各耗时 0.093 s。证据为 `build/hw-qei-periodic-build.log`、
`build/hw-qei-periodic-flash.log`、`build/hw-qei-periodic-run{1,2}.uart.log`、
`build/hw-qei-periodic-reset{1,2}.log` 和
`build/hw-qei-periodic-capture{1,2}.stdout.log`。此前 3/3 的 QEI 硬件结果继续作为
历史记录保留；其他测速配置、其他模式、模拟精度及外部时序仍待验收。
接线见下文 QEI 回环节，完整记录见
[硬件记录](../../../docs/agent/hardware-bringup.md#qei-profile-hardware-result-2026-10-29)。

2026-11-03 增加第五个用例 `test_z_index_calibration_and_event`，并用
P1-5/PB07 GPIO 输出连接 P1-3/PB06 QEI1_Z。CPR 64、PHIDX 12 下，正向 Z
在 A 上升沿捕获后将 PHCNT 重载为 12、ZCNT 加 1；反向 Z 在 A 下降沿捕获后
将 PHCNT 重载为 11、ZCNT 减 1，两方向均产生 Z 事件及有效 ISR 回调。
最终 5/5 固件在两次独立复位后均为 5/5、0 fail/skip，每次耗时 0.263 s。
证据为 `build/hw-qei-z-index-build12.log`、`build/hw-qei-z-index-flash12.log`、
`build/hw-qei-z-index-run{12,13}.uart.log` 和
`build/hw-qei-z-index-reset{11,12}.log`。QEI profile 当前为 5/5，完整记录见
[硬件记录](../../../docs/agent/hardware-bringup.md#qei-profile-hardware-result-2026-10-29)。

加入 QEI 后 15 个已注册 profile 的 Twister build-only 回归全部通过，发现 55 项
测试，0 失败/错误，编译器和 Twister 汇总无警告；原始 CMake 空库及 `TC_NAME`
提示仍存在。报告为 `build/twister-hardware-qei-final/twister.json`。

2026-10-29 `usb_device` 在 RevD 上两次独立物理复位均通过 1/1 项，
耗时 5.916/5.163 s；`west flash --verify` 成功。日志为
`build/hw-usb-device-final-reset1.uart.log`、`build/hw-usb-device-final-reset2.uart.log`，
主机对应 `.peer.log`，烧录记录为 `build/hw-usb-device-final-flash.log`。
两轮均高速枚举，bulk IN `0x82`、OUT `0x01` 的 MPS 为 512。每轮三次遍历
1/63/64/65/511/512/513/1024/1025 B，共 27 帧、双向各 11334 B，逐字节校验并逐帧 ACK。
主机还读回 115200 8N1 line coding 并设置 DTR/RTS。

UDC 现在声明 `addr_before_status`，让 Zephyr 在 EP0 status ACK 前向 SDK 提交地址；
USBADRA 硬件在该 ACK 后才生效，与 SDK 及 STM32 UDC 的同类处理一致。
测试使用中断 FIFO 收发和有界等待，按实际写入量续传；直接轮询发送会在当前
512 B buffer 满时丢字节，总 FIFO 容量不能保证突发轮询发送无损。
日志中的枚举阶段 `Spurious resume event` 和主动关闭时取消空 OUT request 的警告
保留记录；两轮未收到 stack/UDC error 消息。

构建回归为 `build/twister-hardware-usb-device-final/twister.json` 的原 15 个 profile
及 `build/twister-hardware-usb-device-irq-final/twister.json` 的最终 USB 版本，
合计 16 个 profile、56 项测试，0 失败/错误及编译器或 Twister 汇总警告。
首份报告中的 USB 仍是早期测试版本，不能作为最终 USB 源码的构建证据；既存 CMake
空库和未使用 `TC_NAME` 提示仍存在。接线、主机运行方法及覆盖限制见下文。

2026-10-29 `usb_endpoints`（HS）和 `usb_endpoints_fs`（强制 FS）各两次独立
物理复位均通过 1/1。HS 耗时 26.456/6.868 s，FS 耗时 52.452/18.264 s，含等待
VMware 附加设备的时间。固件与主机分别确认实际速度及 MPS：HS 512、FS 64。
每轮同时启用全部 30 个非控制端点，依次执行三轮、九种长度、15 对端点的
405 次双向交换，双向各 170010 B。各端点每个方向的 27 次完成回调逐一断言；
最后 EP15 OUT 另收一个主机 ACK。UDC 的 pending/reset/completion 遍历已改为
32 个方向槽，修复原先只处理 EP0～EP7 的错误。

日志为 `build/hw-usb-endpoints-{hs,fs}-reset{1,2}.uart.log` 及对应 `.peer.log`；
烧录记录为 `build/hw-usb-endpoints-hs-final-flash.log`、
`build/hw-usb-endpoints-fs-flash.log`。未接入 VMware 的 FS 尝试保存在
`build/hw-usb-endpoints-fs-no-vmware.*`，不计入通过。18 配置/58 项的最终
Twister build-only 报告为 `build/twister-usb-endpoints-commit/twister.json`。
完整容量依据和电源管理诊断见[USB 验证记录](../../../docs/agent/usb-validation.md)。

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
该历史记录基线为 Zephyr `f78c4deb4523`、SDK
`ccc5ec467a047f3ea85cd2fa4565e91db7135a38`；当前 Zephyr 4.5.0 的重编译
记录见工作区 `build/twister-zephyr-4.5.0-recheck-hardware/twister.json`。
该历史矩阵包含尚未纳入提交的候选文件，不能用当前 32 个配置的矩阵直接复现。

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

下表列出 30 个 profile 实现形成的 32 个已上板验收配置，具体范围及限制见前述运行记录。

引脚名称是 MCU pad，不是排针序号；按实际 RevD 原理图确认连接点及板载负载。
每次只连接当前 profile 所需线路，数字输入使用 3.3 V 电平。

| Profile | fixture | 接线/设备 | 自动断言与输出 |
|---|---|---|---|
| `core` | `hpm5300evk` | 无外接线 | GPTMR 20 ms alarm IRQ、时钟 off/on 恢复、2 ms top 周期 callback、`DONT_RESET`/`RESET_WHEN_LATE`、活跃 alarm 拒绝、精确 `set_value` 和跨 `UINT32_MAX` 回绕；RNG 自检/非重复数据/真实 ISR 非阻塞预算；CRC32 分段已知答案；TSNS 温度及阈值 IRQ；MBX A/B 双向各 32 字；OTP UUID 区重复只读；PTPC 比较 IRQ 与 TRGM 内部捕获 |
| `counter_capture` | `hpm_gptmr_capture` | PB07 GPIO/P1-5 → PB06 GPTMR0_CAPT_0/P1-3；一根跳线，无需电源/地 | 参数及状态错误拒绝；rising/falling/both-edge continuous 与 single-shot；tick 区间、设备/通道/flags/callback 上下文、无重复回调、disable 后不再捕获；两次独立复位均 7/7 |
| `dma` | `hpm5300evk` | 无 | 3×256 B 链式拷贝、前后 32 B 哨兵、完成 IRQ；单块循环 DMA 暂停后 IRQ 静止、恢复后继续、数据一致 |
| `crypto` | `hpm5300evk` | 无 | 复用已知答案向量：AES-128/256 ECB/CBC/CTR、SHA-1/SHA-256/SM3、CRC32、SM4 ECB/CBC/CTR、CCM 加解密及错误 tag 拒绝 |
| `gpio` | `hpm_gpio_loop` | PB04 → PB05，P1-31 ↔ P1-32；按 RevD 丝印核对 | 四项通过：电平及 raw set/clear/mask/toggle；上升/下降沿各 20 次，反向不触发；双边沿 20 次及 masked 写入 2 次边沿、同值无额外回调；停用时 20 次无回调、重装恢复，核 ISR/来源/次数 |
| `uart` | `hpm_uart_loop` | PB07 TX → PB06 RX（P1-5 ↔ P1-3）；PB05 RTS → PB04 CTS（P1-32 ↔ P1-31） | 257 B 无流控 DMA、97 B + 160 B 分段 offset、连续双 buffer、RTS/CTS 回环、真实 RX FIFO/auto-RTS CTS 背压、RX disable/re-enable；校验 TX/RX 事件、buffer、offset/length、数据、释放顺序及排空后的自动恢复 |
| `uart_address` | `hpm_uart_address_pair` | UART1 PB07 TX/P1-5 → UART2 PB09 RX/P1-10；UART2 PB08 TX/P1-8 → UART1 PB06 RX/P1-3；另接 GND | HPM UART 9-bit 地址匹配：address0 `0x55`、address1 `0x77`、非匹配 `0x66` 抑制、address-match/match-idle 标志读清、地址与 8 B 数据完整性和 UART 错误检查；两次独立复位均 1/1 |
| `uart_rs485` | `hpm_uart_rs485_de` | UART1 PB05 DE/P1-32 → PB04/P1-31 GPIO；PB07 TX/P1-5 → PB06/P1-3 GPIO | 64 B 连续发送期间自动 DE 仅上升/下降一次；DE 上升不晚于 TX 首边沿、下降不早于 TX 末边沿、覆盖完整突发，释放延迟 ≤500 µs；两次独立复位均 1/1 |
| `lin` | `hpm_lin_uart_pair` | UART1 PB07 TX/P1-5 ↔ UART2 PB09 RX/P1-10；UART1 PB06 RX/P1-3 ↔ UART2 PB08 TX/P1-8；另接 GND | 标准 `lin` API 的 commander 写/读、classic/enhanced checksum、responder read/response、PID 过滤、接收超时、wakeup、configure/start/stop 错误语义；两次独立复位均 6/6 |
| `i2c` | `hpm_i2c_pair` | PB02 SCL ↔ PB07 SCL（RevC P1-28 ↔ P1-5）；PB03 SDA ↔ PB06 SDA（P1-27 ↔ P1-3）；按 RevD 丝印核对，板载已有 10 kΩ 上拉 | 100 kHz 标准速率完整 16 项：I2C0 controller → I2C1 target 的 7-bit DMA、重复 START、10-bit、地址/数据阶段 NACK、SCL/SDA 恢复、SMBus byte/word/block/PEC/process call/零计数及故障后恢复 |
| `i2c_fast` | `hpm_i2c_pair` | 与 `i2c` 完全相同，无需改线 | 400 kHz Fast 模式运行同一套 16 项，控制器和目标端均配置为 Fast timing |
| `i2c_fast_plus` | `hpm_i2c_pair` | 与 `i2c` 完全相同，无需改线 | 1 MHz Fast Plus 模式运行同一套 16 项，控制器和目标端均配置为 Fast Plus timing；覆盖 ADDRESS_HIT/BYTERECV 与 ADDRESS_HIT/STOP 合并事件 |
| `spi` | `hpm_spi_loop` | PA29/MOSI ↔ PA28/MISO（P1-19 ↔ P1-21）；PA27/SCLK（P1-23）、PA26/native CS0（P1-24）和 PB04/GPIO CS（P1-31）不接跳线；按 RevD 丝印核对 | SPI1 1 MHz，Motorola mode 0-3 的 8-bit sync/async 链接 DMA；16/24/32-bit sync；16/32-bit 双块 async 链接 DMA；GPIO CS hold/release；native CS；4/12/40-bit 拒绝及 8-bit 恢复 |
| `analog` | `hpm_analog_loop` | PB08/DAC0.OUT（J7-20）→ PB05/ADC0.IN13（P1-32）；按 RevD 丝印核对 | ADC 同步/异步、双通道、抢占 DMA、TRGM 上升沿触发；DAC direct/step/buffer 输出由 ADC 回读 |
| `front_end` | `hpm_front_end_loop` | J7-20/PB08 → P1-31/PB04；P1-8/PB08 → P1-40/PB11，见下文 | OPAMP0 follower 四电平回读；ACMP1 单/双边沿精确次数及 ISR 来源、禁用/重装、pending 读清及同步 callback 派发 |
| `pwm` | `hpm_pwm_loop` | P1-31/PB04 → J7-20/PB08；P1-8/PB08 → P1-40/PB11，复用前端接线 | PWM0.P4 输出、PWM1.P4 捕获；108 组周期/占空比/极性/类型，单位换算、连续回绕、停用/恢复、busy/timeout，以及 GPIO 恒定电平 |
| `qei` | `hpm_qei_loop` | PB08 → PB04，PB11 → PB05，PB07 → PB06；见下文 | QEI1 AB 相位/圈数/角度、Z/index 正反向校准与事件、双比较器 IRQ/轮询/pending、复位及 CPR 重配；静止零速基线及正反向 RPM 测速通过 |
| `qei_uvw` | `hpm_qei_uvw_loop` | PB08 U → PB04/QEI1_A，PB11 V → PB05/QEI1_B，PB07 W → PB06/QEI1_Z；见下文 | 六扇区正向/反向相序、GPIO 与 QEI A/B 回读、Q32 扇区中心 POSITION、count/角度换算、正反两圈 `PHASE_CNT` 往返及运行时 CPR 64→128 重配 |
| `qei_pd` | `hpm_qei_pd_loop` | PB08 pulse → PB04/QEI1_A，PB11 direction → PB05/QEI1_B；见下文 | PD 每脉冲周期双沿计数、方向低 `+2`/方向高 `-2`、CPR 64 回绕及运行时 CPR 64→128 重配 |
| `qei_ud` | `hpm_qei_ud_loop` | PB08 up → PB04/QEI1_A，PB11 down → PB05/QEI1_B；见下文 | UD 每个完整 up/down 脉冲周期双沿各计 2 counts、10 脉冲 `+20`/`-20`、CPR 64 完整上下回零及运行时 CPR 64→128 重配 |
| `qei_single` | `hpm_qei_single_loop` | 仅 PB08 pulse → PB04/QEI1_A；断开 QEI1_B/Z；见下文 | 单相 A 双沿、10 脉冲 `+20` counts、CPR 64 完整一圈、运行时 CPR 64→128 重配及 `QEI_CFG` 仅启用 A 相 |
| `usb_device` | `hpm_usb_device` | J1 USB_OTG 数据线连接 PC，保留 J8 USB_DEBUG | 高速枚举、CDC line coding/DTR 和 27 帧双向数据校验，主机脚本逐帧 ACK |
| `usb_endpoints` / `usb_endpoints_fs` | `hpm_usb_device` | 同上，主机运行 `usb_endpoint_peer.py` | HS/FS 实际速度与 MPS、EP1～EP15 全部 IN/OUT、各 405 次应用层交换及逐端点完成回调 |
| `usb_host` | `hpm_usb_host_keyboard` | J1 USB_OTG 接 OTG 适配器，再接有线 boot keyboard；保留 J8 USB_DEBUG | 设备枚举与 VID/PID、HID boot protocol 控制请求、8 字节 interrupt-IN 的 `a` 按下/释放报告 |
| `usb_host_hub` | `hpm_usb_host_hub` | J1 USB_OTG → OTG 适配器 → 自供电 GenesysLogic `05e3:0610` HS Hub；Hub Port 1 → 低速 boot keyboard | 断言 HS Host 经 Single-TT Hub 枚举 LS 键盘、Hub/键盘父子层级，以及 SET_PROTOCOL/SET_IDLE、GET_PROTOCOL 和 interrupt-IN 按下/释放报告 |
| `usb_host_hid_report` | `hpm_usb_host_hid_report` | 同 `usb_host`；使用 boot-capable 有线键盘 | 完整 HID Report Descriptor、`SET_PROTOCOL(report)` 与 `GET_PROTOCOL` 读回、非零按键和全零释放 interrupt-IN 报告 |
| `can` | `hpm5300evk` | MCAN0 静默内部 loopback，无外部 CAN 总线 | 16 帧标准 classic/FD/BRS、8/64 B 数据及 TX/RX ISR；另 20 帧 PTPC0→TSU SOF 64 位捕获，有效/无丢失/递增、8 槽及两次回绕、PTPC 发送前后时间包络 |
| `ptpc` | `hpm5300evk` | 无外部接线，同时启用 PTPC0/PTPC1 | 两通道初始化进度、时间设置及正负调整、`1.05/0.95` 速率调整、PPS 控制值 0/1/2/4/15、事件锁存/清除，以及共享 IRQ 39 两通道 compare 分发 |
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

### LIN UART 双节点接线与结果

`lin` profile 使用标准 Zephyr `lin` API，将 UART1 和 UART2 分别实例化为
commander 与 responder，并在 TTL 侧交叉接线：

| UART1 信号 | 连接到 | UART2 信号 |
|---|---|---|
| PB07 / TX，P1-5 | ↔ | PB09 / RX，P1-10 |
| PB06 / RX，P1-3 | ↔ | PB08 / TX，P1-8 |

两板同一块 EVK，因此再连接任一 GND，例如 P1-30 或 P1-34。不得连接 3.3 V。
固件在发送 break 时临时将 PB07/PB08 切为 GPIO，随后恢复 UART mux。该测试覆盖
classic/enhanced checksum、commander 写/读、responder read/response、PID 过滤、
接收超时、wakeup 生成以及 configure/start/stop 错误语义。

2026-11-04 正式固件在烧录后两次独立复位均为 6/6 通过（每次 0.143 s），
覆盖 commander 写/读、classic/enhanced checksum、responder read/response、
PID 过滤、接收超时、wakeup 生成及 configure/start/stop 错误语义。首版诊断发现
`lin_response()` 在 ISR 中无法把 9 字节帧一次性写入 UART FIFO；驱动改为
TX-slot 中断逐字节入队，并在最后一字节后启用 TX-idle 完成检测。失败日志保留为
`build/hw-lin-run1.uart.log` 和 `build/hw-lin-diag{1,2}.uart.log`，不计入验收。
正式证据为 `build/hw-lin-final-build.log`、`build/hw-lin-final-flash.log`、
`build/hw-lin-final-run{1,2}.uart.log` 和对应 `.reset.log`。

该 TTL 回环只验证 UART 实现的 LIN 帧状态机，不验证板载 12 V LIN 收发器、
总线电气特性或外部 LIN 节点。板载 J9-5 LIN 接口需要在另一项 fixture 中启用
`lin-transceiver-gpio`、PA13/PA24 控制及 `rx-header-echo` 后单独验收。
自动波特率同步和 bus conflict detection 当前明确不支持。

## QEI 数字回环

`qei` 使用三根独立跳线：

| GPIO 输出 | QEI1 输入 |
|---|---|
| J7-20 / PB08 | P1-31 / PB04 / QEI1_A |
| P1-40 / PB11 | P1-32 / PB05 / QEI1_B |
| P1-5 / PB07 | P1-3 / PB06 / QEI1_Z |

从 PWM fixture 切换时保留第一根，将第二根的 P1-8/PB08 端移到 P1-32/PB05。
早期 AB-only 用例不接 Z；当前 5/5 profile 还必须连接第三根 Z 跳线并启用索引校准。
P1-32 与 P1-31 正对，丝印 GPIO26；P1-30、P1-34 为 GND。仅接信号脚，无需电源或地。
映射与官方 RevC 原理图第 2、6、9 页核对，RevD 实物由操作者确认。
不能保留把 PB08 与 PB11 短接的旧模拟前端/PWM 接法，因为本测试独立驱动 A/B。
普通 GPIO 生成低速边沿，不能代替外部编码器、电气/滤波/最高频率、Z 脉宽/时序和
模拟解码精度验收。

### Pulse/direction 回环接线

`qei_pd` 只使用两根跳线，并必须断开 `qei` 的 Z 跳线：

| GPIO 输出/方向 | QEI1 输入 |
|---|---|
| J7-20 / PB08：pulse | P1-31 / PB04 / QEI1_A |
| P1-40 / PB11：direction | P1-32 / PB05 / QEI1_B |

PB07/P1-5 与 PB06/P1-3 之间的 QEI1_Z 跳线保持断开。当前 HPM SDK 的 PD 配置同时
启用 A 相上升沿和下降沿，因此一个完整 pulse 周期计 2 counts。实测本板路径中
direction 为低时递增、为高时递减；用例固定这两个方向语义，避免把“每脉冲 1
count”或手册中的通用方向描述误当作当前实现行为。该测试不覆盖外部 PD 信号频率、
滤波/延迟、毛刺、Z 与 PD 组合或另一 QEIV2 实例。

### Up/down 回环接线

`qei_ud` 使用与 `qei_pd` 相同的两根跳线，但两个 GPIO 都作为独立脉冲输入；
QEI1_Z 必须保持断开：

| GPIO 输出/方向 | QEI1 输入 |
|---|---|
| J7-20 / PB08：up pulse | P1-31 / PB04 / QEI1_A |
| P1-40 / PB11：down pulse | P1-32 / PB05 / QEI1_B |

测试一次只驱动 up 或 down 中一路，另一路保持低电平。当前 HPM SDK 的 UD 配置
启用两路输入的双沿，因此每个完整 up/down 脉冲周期均计 2 counts。该测试不覆盖
两路同时翻转、外部编码器频率、滤波/延迟、毛刺、与其他模式组合或另一 QEIV2 实例。

### Single-phase 回环接线

`qei_single` 只使用一根跳线；QEI1_B 和 QEI1_Z 必须保持断开：

| GPIO 输出/方向 | QEI1 输入 |
|---|---|
| J7-20 / PB08：pulse | P1-31 / PB04 / QEI1_A |

当前 HPM SDK 的单相配置启用 A 相上升沿和下降沿，因此一个完整 pulse 周期计
2 counts。测试覆盖 10 个脉冲 `+20` counts、CPR 64 下 32 个脉冲完成一圈，以及
运行时 CPR 64→128 后 64 个脉冲完成 128 counts 一圈。单相模式没有独立方向输入，
因此不测试反向计数；该测试也不覆盖外部编码器频率、滤波/延迟、毛刺、sincos
模拟输入或另一 QEIV2 实例。

### UVW 回环接线

`qei_uvw` 复用 `qei` 的三根独立信号跳线，三个 GPIO 分别驱动 U/V/W 相；
QEI1_Z 在本模式中作为 W 相输入，不再作为索引脉冲：

| GPIO 输出 | QEI1 输入 |
|---|---|
| J7-20 / PB08：U | P1-31 / PB04 / QEI1_A |
| P1-40 / PB11：V | P1-32 / PB05 / QEI1_B |
| P1-5 / PB07：W | P1-3 / PB06 / QEI1_Z |

测试依次输出正向六扇区序列 `101`、`100`、`110`、`010`、`011`、`001`，
并反向返回。HPM QEIV2 的 UVW POSITION 使用 Q32 格式的 60° 扇区中心；
`counts-per-revolution` 只保留为速度换算参数，不选择 UVW 扇区表。该测试不覆盖
外部高速 UVW、非法相态、滤波/输入延迟或 QEIV2 的其他实例。

## USB Device 主机连接与运行

使用一根支持数据的 USB-C 线连接 J1 `USB_OTG` 到运行主机脚本的电脑，保持 J8
`USB_DEBUG` 供电/调试连接。无需新增跳线，现有 QEI fixture 可保留。
VID:PID 为 `1209:0001`，产品名 `HPM5361 hardware acceptance`，序列号
`HPM5361-HW-ACCEPT`。该标识仅用于本地测试，不用于发布产品。

构建时将上文 profile 参数替换为 `usb_device`。烧录并复位后运行主机脚本；
板端等待配置最多 60 s，随后等待 DTR 最多 30 s，各帧等待最多 10 s。
每次重测需先复位固件，再及时启动主机脚本。

```sh
python3 -m pip install pyserial
python3 hpm-zephyr/tests/hardware/scripts/usb_cdc_peer.py /dev/ttyACM0
```

脚本也可按 VID/PID/序列号自动发现 CDC port。`/dev/ttyHPM` 始终是 FT2232
调试控制台，不是 CDC port。若容器的私有 `/dev` 不出现新增 ttyACM，但已挂载
`/dev/bus/usb`，可使用 PyUSB；本工作区实际验证命令如下：

```sh
sudo -n /opt/python/venv/bin/python3 \
  hpm-zephyr/tests/hardware/scripts/usb_cdc_peer.py --backend usb --wait 75
```

该解释器需安装 `pyusb`，系统需提供 libusb。USB 节点访问权限不足时可配置主机
udev 权限或使用上述 sudo 命令。脚本只临时解绑匹配设备的 CDC 接口，并在退出时
释放接口、恢复内核绑定；多个匹配设备需用 `--bus` 和 `--address` 指定。
VMware 环境中每次烧录后必须通知操作者，将新设备手动连接给虚拟机，保留 FT2232。
本轮复位重测会自动重新连接，但仍应检查实际枚举结果。

`usb_device` 仅验证高速 CDC 的低编号端点；后续 endpoint 矩阵单独验证 HS/FS
的全部非控制端点。应用帧长度覆盖 64/512 B 附近及多包长度，不能据此断言确切
USB 分包。其他类、suspend/resume、remote wakeup、Hub/非 boot HID/等时
Host 路径和运行时 OTG
切换仍待验证。
SDK 板级说明指出 RevC 及更早版本的 R37 1 kΩ 可能阻碍 Device 枚举，需要换为
10 Ω；本次为已修正的 RevD，未修改电阻。

### USB 端点容量与 HS/FS 矩阵

HPM5300 用户手册第 51.1 节明确支持 **16 个双向端点，即 16 IN + 16 OUT**，
编号 EP0～EP15，包含控制 EP0；应用可用的非控制端点为 15 IN + 15 OUT。
SDK 的 `USB_SOC_DCD_MAX_ENDPOINT_COUNT=16`、32 个 QH 及 `ENDPTCTRL[16]`
与此一致。该上限描述 Device 资源，不能解读为 Host 可连接设备端点的总上限。
中文手册个别段落残留“16 dQH”，部分英文位域残留“N=0..7”，与功能简介、
完整寄存器表及 SDK 不一致；本轮已实际验证 EP15 OUT `0x0f` / IN `0x8f`。

同一 vendor interface 同时声明 15 对 bulk 端点。更换构建 profile 为
`usb_endpoints` 或 `usb_endpoints_fs`，两者均使用 `profiles/usb_endpoints.overlay`。
FS 配置同时关闭 UDC 高速支持并选择 `CONFIG_USBD_MAX_SPEED_FULL`，由 PFSC
强制物理全速，主机脚本会拒绝错误的速度或 MPS。每次烧录后通知 VMware 操作者
连接 `HPM5361 USB endpoint acceptance`，VID:PID `1209:0002`，序列号
`HPM5361-USB-ENDPOINT`，然后运行：

```sh
sudo -n /opt/python/venv/bin/python3 \
  hpm-zephyr/tests/hardware/scripts/usb_endpoint_peer.py --speed high --wait 80
```

FS 使用 `--speed full`。每轮遍历 1/63/64/65/511/512/513/1024/1025 B，主机验证
逐字节 XOR 回复后，最后向 EP15 OUT 发送额外 `0x5a` ACK。普通数据每个方向
170010 B，不含这个 ACK。不将该测试解释为并发满带宽、精确分包、独立电气规范、
STALL/取消或在途请求跨 bus reset 的验收。

### Host 与电源管理验收

Host boot protocol 已用 USB-C 转 USB-A OTG 适配器和有线 boot keyboard 在
RevD 上两次独立复位通过。`usb_host_hid_report` 随后在同一边界读取完整
Report Descriptor、选择 Report Protocol 并验证通用按键按下/释放报告，也已
两次独立复位通过。boot protocol 与 report protocol 当前针对的都是
`1c4f:00b7` boot-capable 键盘，不能据此宣称已支持任意非 boot HID 设备。
`usb_host_hub` 进一步通过 GenesysLogic `05e3:0610` 自供电 HS Hub，验证了 Hub
下游低速键盘的 Single-TT 分拆事务和 HID interrupt-IN 路径。
J1 的 CC1/CC2 在 RevC 原理图中没有板载主机上拉（R30/R31 DNP），因此必须使用
带 OTG/CC 处理的适配器；普通 C-to-C 线不会让该板进入 Host 连接状态。J1 的
VBUS 由板载 AP2151 开关提供，无需外部注入 5 V。

这些 profile 尚未覆盖多层/非 TT Hub、任意非 boot HID 类、等时音频或 Host
侧挂起/恢复。设备端点容量同样不能用于推断 Host 侧 Hub 的端点容量。

挂起/远程唤醒草稿已尝试上板，尚未通过。当前容器 `/sys` 为只读，操作者在 VM
容器外只为 `1209:0003` 配置自动挂起；guest 连续 17 s 报告 suspended 时，固件在
退出和关时钟前读到 `PORTSC=0x18000205`（SUSP=0）、`USBINTR=0x147`，且
FRINDEX 在约 100 ms 内从 `0x31a1` 推进至 `0x3519`。当前 VMware 链路没有使
物理 USB 总线进入挂起，不能据 guest 状态判定通过，也不能据此断言驱动故障。
诊断日志为 `build/hw-usb-power-diagnostic.uart.log` 和 `.peer.log`；退出后
guest 设备节点已删除。

2026-11-04 起该测试正式挂起。当前环境为 WSL2 + Docker，USB 设备通过 USB/IP
和 `vhci_hcd` 接入；USB/IP 只转发 URB submit/unlink，不转发物理 port
suspend/resume，guest 中看到的 suspend 是虚拟端口状态。待其他硬件测试完成后，
使用原生 Linux 主机或 Linux SBC 直接连接 J1 再继续，不能通过 WSL2、VMware
或 USB/IP 结果声明 suspend、host resume 或 remote wakeup 通过。Power 测试及
远程唤醒驱动改动继续作为未验收草稿，并保存在独立 stash 中。

## 暂缓的硬件验收

以下草稿和待验证项不计入注册 profile，也不声明通过或失败。待具备对应设备后，
再注册 Twister fixture、补充接线记录并执行烧录和独立复位。

这些草稿按功能分别保存在本地 stash，不污染当前工作树。恢复和检查时使用完整
SHA，不要依赖会随 stash 增减而变化的 `stash@{n}`：

```sh
git stash list --format='%gd %H %gs'
git stash show --include-untracked --stat <完整 SHA>
git stash apply <完整 SHA>
```

| 项目 | 本地 stash（完整 SHA） | 草稿/待验证范围 | 暂缓原因与恢复条件 |
|---|---|---|---|
| RDC | `ea832bda7b355fccca0dd707b84004299c588d54`<br>`wip: rdc resolver hardware fixture` | `rdc.conf`、`rdc.overlay`、`src/rdc.c`；激励、I/Q ADC 流、ACC IRQ 和角度 | 用户当前没有与 PA28 激励同步的旋变信号模拟器；具备可提供 I/Q 反馈的 resolver simulator 后恢复 |
| SEI SSI 及其他协议 | `872680374c91ac9c8f8ab16c75a0dc0a13fafe77`<br>`wip: sei encoder protocol fixtures` | `sei*.conf`、`sei.overlay`、`src/sei.c`、`src/sei_protocol.c`；SSI、Tamagawa、BiSS-C、EnDat、Nikon consumer 和协议 fixture | 缺少对应编码器或协议模拟器及 CRC/时序验证条件；取得真实或可编程模拟器后恢复 |
| USB suspend/wakeup | `7037b39b2edbb3488b0d3b41f0773f29f2f85074`<br>`wip: usb suspend and remote wake acceptance` | `usb_power` 草稿、peer 脚本和 FPR 驱动改动 | 当前 WSL2 + Docker 通过 USB/IP/`vhci_hcd` 接入，不能传播物理 suspend/resume；迁移到原生 Linux 主机或 Linux SBC 后恢复 |

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

OTP 驱动按项目约束永久只读，只读取 UUID words 88–91 两次，不打印标识、不烧写、
不锁定、不 reload；即使启用 `CONFIG_OTP_PROGRAM`，标准 `otp_program()` 也返回
`-ENOSYS`。
看门狗测试只验证 IRQ，不自动执行 SoC reset。motor 测试不配置外部 PWM/pinctrl
输出，GPIOM 测试只修改输入可见性并恢复，不驱动 PA24。

## 仍需补充的验收路径

下列项目仍留在驱动审核矩阵；本应用成功不能自动勾选它们：

| 模块 | 本轮尚未覆盖的硬件验收 |
|---|---|
| clock/pinctrl/GPIO/counter | 全时钟树、PIOC/PY、上拉/开漏电气特性、独立基准频率、GPTMR output compare、其他 capture 通道/实例、64-bit capture 和双边沿高延迟压力 |
| UART | 外部 RS-485 收发器电气、总线冲突和远端 DE/RTS 时序仍需独立验收 |
| LIN | 板载 LIN 收发器、J9-5 外部 peer、真实总线边沿/唤醒脉冲时序、自动同步及冲突检测仍需独立验收 |
| I2C/SMBus | timeout/故障注入、外部器件兼容及重复 START 电气时序 |
| SPI | native CS 波形/setup-hold 电气测量、外部从设备时序、半双工、双/四线及 `SPI_LOCK_ON` 仍待验收 |
| DMA | 错误 IRQ、暂停边界竞争、压力/并发；循环完成 IRQ 密集度也需记录 |
| Watchdog | SoC reset、窗口模式、睡眠/调试暂停、挂起 IRQ 与停用的竞争、其他实例 |
| CAN/PTPC | 实体收发器和总线、bus-off、共享 MRAM 并发；TSU 与外部绝对时间关联、PPS 引脚电气/占空比、PPS 控制值 5–14 及最大值 IRQ 压力 |
| MBX/GPIOM | 跨核通信/FIFO、跨控制器访问、锁定持久性 |
| QEO/MMC/PLB/SYNT/TRGM | QEO 外部 PWM/ABZ 及波形、闭环 MMC、独立 predictor 停止、PLB filter、SYNT comparator/debug-stop、外部电机路由和中断 |
| RDC/SEI | RDC resolver simulator 和 SEI SSI/Tamagawa/BiSS-C/EnDat/Nikon 编码器 fixture；草稿已按功能 stash，当前由用户暂缓，不能以 build-only 记为硬件通过 |
| SDP/RNG/CRC/TSNS | cache/并发/故障注入压力、随机源统计质量、CRC 多项式矩阵、温度校准 |
| OPAMP/ACMP | OPAMP1、其他拓扑和增益矩阵、校准精度/噪声；ACMP0、双通道并发、阈值精度、滤波/迟滞、反相、输出脚及传播延迟 |
| QEIV2 | sincos、UVW 外部高速/非法相态/滤波、PD/UD 外部高速/毛刺/滤波、外部 Z 脉宽/时序、HOME/FAULT/看门狗、其他实例、模拟精度及外部高速时序 |
| USB | 其他类、并发/STALL/取消/在途 reset、suspend/resume、remote wakeup、多层/非 TT Host Hub、非 boot HID、等时传输/PM、VBUS/ID/OC/PWR 策略及运行时 OTG 切换；suspend/wakeup 草稿已 stash |
| PWM | 其他输出/捕获实例组合、绝对频率精度、互补/死区/fault、参考输出、超长周期及高速/长 IRQ 延迟下的丢样 |
| flash/OTP | NOR JEDEC/几何、跨页及边界/延迟压力；OTP 的 program/lock/region 在当前范围明确不实现 |

generic 9-bit UART、DMA P2P、CPU-core-only/multistage watchdog、TRGM 独立事件
回调、HPM5361 PLB ABZ 和 SDP 真正 IRQ 异步不伪造正向通过测试。
其他外设及能力限制仍由工作区 `docs/agent/driver-review.md` 跟踪；RTC 延后。
SEC/SECMON/KEYM 没有匹配的 Zephyr 子系统 API，按当前范围不新增自定义驱动；
其不可逆操作也不会提供写接口。

PLB 的 `SW_INJECT` 只回读软件注入值，不能用作实时计数器读回；模块接口返回
`-ENOTSUP` 并保留输出参数。硬件数据单元操作通过 TRGM 内部比较器输出核对。

## 结果记录

保存 profile、源码/Zephyr/SDK SHA、板版本、接线/仪器、完整 UART 日志和结论。
Ztest 汇总应为 `PROJECT EXECUTION SUCCESSFUL`，且每个断言通过；超时、异常、缺少
fixture、缺少协议模拟器都不能记为硬件通过。Twister `--build-only` 只证明编译链接。
仪器项目另附波形和测量值，再更新工作区的硬件验收记录。

`testcase.yaml` 用 fixture 隔离接线要求，未设置 `build_only: true`，以后可交给具备
相应 fixture 的 Twister 硬件设备池；不要给未接线的普通 EVK 一次性声明所有 fixture。
