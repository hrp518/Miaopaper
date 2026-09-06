# MiaoPaper 工程笔记 / 开发教训

> 这些是我(开发助手)在迭代 MiaoPaper(TLSR8258 e-paper 阅读器)过程中踩过的坑和
> 确认过的事实。**遇到类似问题先查这里,避免重复走弯路。**

## 硬件可靠性(最重要的一条)

**MiaoPaper 是商用硬件,按钮/GPIO pad 唤醒是可靠的。不存在"pad 唤醒不可靠/易乱触发"这个选项。**
- 证据:按钮在阅读模式翻页很稳(不会乱翻页),说明按钮引脚电平干净、无毛刺;
- 因此如果**深睡电流没降/按键唤不醒**,一定是**固件代码的 bug**,不是硬件问题。
- 不要再拿"pad 唤醒不可靠"当借口;直接去审计固件的睡眠实现。

## EPD 主控(Firmware/src/epd_bw_213.c)

- 主控是 **SSD1680**(GDEQ0213B74 2.13" 250x128)。对照 SSD1680.pdf:
  - `0x10,0x01` = **Enter Deep Sleep Mode 1**(深睡);
  - `0x11`/`0x44`/`0x45`/`0x20`/`0x22`/`0x21`/`0x24`/`0x26` 都是 SSD1680 标准命令。
- **退深睡必须发 HWRESET**(数据手册:"To Exit Deep Sleep mode, send HWRESET")。
  固件局部刷新只发 `0x00`(WakeOnly)退不出深睡 → 会静默失败。所以场景切换必须
  走**全刷(HW reset)**,固件已这么做(epd.c set_EPD_wait_flush 强制全刷)。
- 深睡后电流 ~1-2uA,EPD 不是电流大头。

## 睡眠 / 低功耗

- 锁屏后已做:**Flash 0xB9 深断电** + **按钮 GPIO pad 唤醒**(cpu_set_gpio_wakeup +
  PM_WAKEUP_PAD)。
- **注意:改 pad 唤醒时把 `bls_pm_setAppWakeupLowPower(1s 轮询)` 关了。要确认 BLE 栈
  是否真的进入了 `deep retention`(而非 suspend/不睡)。若栈需要 app 定时器作为深睡
  唤醒源,只留 PM_WAKEUP_PAD 可能让栈不进深睡 → 电流反而没降。** 排查时检查栈的
  睡眠模式/唤醒源是否被正确用上。

### 休眠诊断日志(src/sleep_log.c, 2026-09)

上面"是否真进 deep retention"的疑问现在可以直接实测验证:
- 外置 Flash 扇区 **0x7FD000**(GBK 字库上限 0x700000 之上、阅读进度 0x7FE000 之下)
  存 16 字节/条的环形日志(256 条,写满保留最近 32 条)。
- 事件:BOOT(冷启动=真掉电/看门狗/全深睡)、LOCK、SLP(即将 blt_pm_proc,含上次
  SUSPEND_ENTER 的模式字节)、WAKE(retention 唤醒,含 pad 标志+睡前模式字节)、
  DISC(断连 reason)、UNLK。
- 判读:**有 WAKE = 一定发生过 deep retention**(只有 retention 唤醒才重跑
  user_init_deepRetn);WAKE 的 c=0x07 = 栈用的 LOW32K retention;c=0x00 = 只到
  suspend;只有 SLP 且 a 恒为 0xFF = 栈压根没睡;突然出现 BOOT = full deep sleep
  或断电/复位。
- 读出:BLE 终端发 **0xF6**(最近 80 条,新→旧),**0xF7** 清空;或 0xF4 裸读
  0x7FD000。
- 电流对照(手册值):MCU deep retention 32K=1.4µA,全深睡=0.4µA;SSD1680 深睡
  1=1µA typ;外置 Flash 0xB9 ~1µA。**锁屏整机理想 ~4-6µA;量到 ~30µA → 面板停在
  sleep 模式没进深睡;量到 mA 级 → MCU 没睡(拿日志对质)。**

### 实测结案(2026-09,日志证据)

- **锁屏+蓝牙连接 = 保持唤醒**(设计如此,用户确认)。连接态日志零记录是正常的
  (SLP/WAKE 有防写穿门控)。
- **锁屏+未连接,只靠 `blt_pm_proc`(掩码含 DEEPSLEEP_RETENTION_ADV/CONN)睡的是
  普通 suspend,不是 retention!** 判据:唤醒后无 W a=1(retention 唤醒必重跑
  user_init_deepRetn)。原因:广播关掉+无连接 → 链路层 IDLE → 两个 retention
  掩码位都不适用,栈回落 suspend。**"锁屏关广播"把 retention 路径堵死了。**
- **节流勿用 clock_time()**:16MHz 32 位 tick 每 268 秒回绕,int32 比较在
  [134s,268s] 真实间隔上误判为"不足 5s"→ 记录成片丢失(踩过)。用墙上秒。

### 方案B:锁屏未连接时绕过栈的原始 deep retention(已实现)

- 睡:`app.c` allow_sleep 且 `LOCK && !connected` → 直接
  `cpu_sleep_wakeup(DEEPSLEEP_MODE_RET_SRAM_LOW32K, PM_WAKEUP_PAD, 0)`;
  入睡前 `slp_last_sleep_mode=0x07` + `slp_raw_sleep_arm()`(retention RAM 标记)。
- 醒:retention 唤醒重跑 `user_init_deepRetn`;`slp_raw_sleep_consume()==1` 时
  **不能走 `blc_ll_recoverDeepRetention()`**(库没为栈外入睡保存 LL 上下文),
  改 `init_ble()` 整栈重初始化,然后锁屏态把广播关回(隐身)。RAM 业务状态
  (锁屏/时钟/设置)原样保留;cpu_sleep_wakeup 内部带系统计时器恢复,时钟不丢。
- 预期日志签名:`S(a=07,c=0F) → [空白] → W a=1 c=07 → U`。
- **实测失败(2026-09):raw 唤醒后解锁蓝牙扫不到** —— `init_ble()` 在 retention
  唤醒态下重建 BLE 栈不完整(具体缺什么未知,库闭源无法比对 recoverDeepRetention
  的完整恢复内容)。已默认回退方案A。
- **方案A(现为默认):锁屏保持广播**,掩码 DEEPSLEEP_RETENTION_ADV 在有 ADV 活动
  时生效 → 标准栈路径,retention 有保证,~5-10µA,手机可随时连。要点:
  锁屏分支**不可**再调 `bls_pm_setWakeupSource(PM_WAKEUP_PAD)`(剥掉 timer 唤醒
  会复现"锁屏即断连"),让它用栈默认 timer+pad。按钮 pad 唤醒寄存器(cpu_set_gpio_
  wakeup + afe 0x22/0x23/0x26/0x28/0x29)两方案通用,保留。
- 方案B代码保留,`slp_raw_sleep_enabled`(RAM)控制,BLE **0xFB dat=1/0**
  切换。raw 唤醒重建完成后写 N(a=1) 日志记录分支,便于将来排查。
- **B 修复(反汇编 llt_8258.a 得出)**:`blc_ll_recoverDeepRetention` 不是大块
  上下文恢复,而是轻量"睡醒对账"—— 清 bltPm 陈旧标志、处理 pad 早唤醒、补写
  LL 寄存器 0x800f01/0x800f1c/0x800f20/0x800744(重数据靠 retention RAM 硬件
  保活)。第一版 B 只做 init_ble 重建、跳过对账 → 库内部状态停留在睡前,
  调度器楔死 → 解锁后广播失联。**修复:raw 唤醒 = init_ble() + recover() 两步**。
  另注意:本 SDK 里 app 的 `blt_pm_proc()` 本来就只是"设掩码"一行(Telink 标准
  写法),真正的睡眠决策在 blt_sdk_main_loop 内部调度器,IDLE 态只给 suspend。

### 终版方案 B'：锁屏广播保留 + 间隔拉长(2026-09 定案)

- **"关广播 + 绕过栈裸调 cpu_sleep_wakeup"彻底放弃**。反汇编 blt_brx_sleep 确认:
  库的 retention 入睡是与 ADV 事件绑定的一整套编排(PHY 保存/blt_state/
  0x800744 锚点/early-wakeup),裸调驱动要么被降级要么唤醒后库状态与硬件脱节。
  sleep 驱动入口还查 func_before_suspend、对 wakeup_src 分档,裸调参数很难凑对。
- **B'(当前固件)**:锁屏**不关广播**,只把间隔从 1s 拉到 **2s**(ble_adv_slow_
  for_lock)→ 掩码 DEEPSLEEP_RETENTION_ADV 在有 ADV 活动时生效,栈在广播间隔间
  进 deep retention,整机 ~5µA;按键 pad 唤醒秒醒;解锁 ble_adv_restore_fast 还原
  1s。手机 2-4s 可发现设备。这是库原生支持路径,唤醒走标准 recoverDeepRetention,
  稳定。
- 锁屏分支不要动栈的唤醒源(剥 timer 会复现"锁屏即断连")。
- **唤醒源必须显式 = `bls_pm_setWakeupSource(PM_WAKEUP_PAD | PM_WAKEUP_TIMER)`
  (2026-09 定案)**:两个位缺一不可 —— 库默认只有 timer(无 pad),深睡中按键
  完全唤不醒("有时候咋按都不开",实测日志:每 268.4s tick 回绕才醒一次,
  醒着的 42s 窗口里按键才有效);只给 pad 不给 timer 则锁屏即断连(更早踩坑)。
  cpu_set_gpio_wakeup + afe 0x26/0x28/0x29 的 pad 硬件寄存器是另一层,两处都要。

### 超级省电模式(super_sleep,2026-09)

- 设置菜单 Super: On 后,锁屏+未连接+屏稳 → 全深睡 `DEEPSLEEP_MODE(0x80)`,
  ~2.5µA(MCU 0.4 + EPD 1 + Flash 1)。**时钟不走**:醒来=入睡时刻,连网页校准。
- 唤醒 = 冷启动(驱动走 soft_reboot_dly13ms 路径)→ init_ble 全量重建 → 蓝牙
  必然可用,无 retention 的栈状态残留问题 —— 这是全深睡比裸 retention 靠谱的
  地方。被拒返回则兜底 blt_pm_proc(B')。
- 状态持久化:`super_sleep.c` stash 记录(ext 0x7FC000,追加+压缩)存墙上秒;
  冷启动 `ss_boot_restore()` 恢复 current_unix_time + 判定"深睡唤醒" → 直接回
  锁屏 + 10s 操作窗(lock_hold_until),超时重睡;噪声误醒代价 ~50µA·s。
- 入睡顺序:save_settings_to_flash → ss_stash → adv off → flash 0xB9 → 深睡。
- **全深睡唤醒后外部 Flash 仍在 0xB9(关键坑)**:0xB9 要收到 0xAB/重新上电才
  解除,而全深睡唤醒=软重启,RAM 里 `flash_powered_down` 标志被清零 → 固件
  以为 Flash 醒着,此后所有外部 Flash 读写都是哑弹(读回全 0):stash 恢复
  失败→唤醒落时钟界面、睡眠日志全变坏记录。修复:冷启动
  `ext_flash_boot_resync()` 无条件补发 0xAB(对醒着的芯片无害)。
- **analog 0x44 bit3(pad 唤醒状态)是粘滞位,软重启不清**:残留为 1 时,
  0x80 入睡被驱动短路成"立即软重启" → 每 10s(操作窗时长)重启循环 + 锁屏
  原地 GC。修复:入睡入口 `analog_write(0x44, read & ~BIT(3))` 清位;清不掉
  则本轮退回 B'。若实测清不掉(只读位),需要换 blc_pm_procGpioPadEarlyWakeup
  或 pm_long_sleep_wakeup 路径。
- **全深睡必须传真实 wakeup_tick + 唤醒源含 TIMER(最终根因,2026-09)**:
  `cpu_sleep_wakeup(0x80, PM_WAKEUP_PAD, 0)` 里 tick=0 且无 TIMER 位 → 驱动
  不装 32k 闹钟 → 入睡瞬间闹钟到期 → 无限软重启循环(锁屏每 10s 原地 GC)。
  正确姿势:`cpu_sleep_wakeup(DEEPSLEEP_MODE, PM_WAKEUP_PAD|PM_WAKEUP_TIMER,
  clock_time() + 周期)`。tick 宽度 268.4s 封顶 → 维护周期取 180s:定时醒来的
  冷启动做"维护"(时钟 +180s、刷新 stash、不渲染直接回睡,屏幕无感);
  pad 醒来才渲染锁屏 + 10s 操作窗。外置 Flash stash 是追加写,磨损可忽略。
- **0x80 纯 pad 唤醒循环的真因(反汇编实锤,2026-09 终版)**:
  `cpu_sleep_wakeup_32k_rc` 入口 0x1e4-0x1f6:`analog_read(0x44)` 的
  **bit3(IO pad 唤醒状态,粘滞锁存)为 1 且 mode==0x80 → 直接 soft_reboot
  (不睡)**;bit3=0 → 0x3ac sleep_start 正常入睡;0x07 retention 不受影响
  (→0x1f8),这就是为什么 retention 一直正常而全深睡循环。
  **修复:每次冷启动钩子里 `analog_write(0x44, read & ~BIT(3))` 清锁存位**
  (驱动自身在 0x38a 处也写 0x44=0x0F,证明该寄存器可写)。
  另:wakeup_tick 传真实未来时刻(驱动上限 0xE0000000 ticks = 234.9s,
  超限/为 0 都会拒睡或立即醒 —— 这就是"234 秒"的出处)。
  应用侧形态:`cpu_sleep_wakeup(0x80, PM_WAKEUP_PAD, clock_time()+230s)`,
  无 PM_WAKEUP_TIMER。定时唤醒(静默维护)是否存在由 0xF6 的 B 记录频率判定。
- 调试代码已删(128K 体积):epd_autodetect/epd_spi_autodetect/button_scan/
  battery_scan 的扫描(保留 PB5 差分电量测量)/cmd 0xB2-B9、E4、E9-EC、EF、
  F3-F5、F8、0x46-0x49。保留:0xF6/0xF7 睡眠日志、0xF3 已删。

## OTA(网页)

- **擦除命令必须是 5 字节 `[0x01][4字节BE地址]`**。设备端 `custom_otaWrite` 只在
  `data_len>=5` 才解析地址;**4 字节擦除命令地址不解码(恒 0)→ 擦除被静默跳过**,
  残留数据越积越多 → 校验永远是"设备 CRC 更高"的确定性失败。
- 0x02 bank 写是 5 字节(`'02'+8位hex`),所以能写;擦除(0x01)之前写成 4 字节是 bug。
- 网页只显示**纯 ASCII** 日志行,含中文会被过滤(FREEGPIO/BTNLVL 等日志须用英文)。

## 网页 OTA 块大小

- 原版 ATC 网页单次 0x03 写 240 字节(走协商后 MTU 247,浏览器实际接受 ~244B;
  maxWriteValueLength()=20 是谎报)。上传后可加 case 6 校验 + 失败自动重传
  (换块大小避损坏点)。

## 决策记录:0x80 全深睡是唯一目标(2026-09-05,项目所有者拍板)

- **"回退 B' retention"方案被永久否决**。任何后续调试、任何助手/工具/报告,
  不得再以"放弃 0x80、改用 B'(~5µA)也能接受"作为建议或选项。
  0x80(0.4µA 级)必须做成;做不成就继续查,不降级。
- 背景:super sleep(0x80)自引入以来**从未成功睡进去过一次**。表现为
  "入睡 → 同周期冷启动 → 10s 观察窗 → 再试"的复位循环。
- 静态审计结论(2026-09-05,全反汇编 grep 核实):
  - 0x21~0x2a/0x26 的写入者整个二进制只有三处:cpu_wakeup_init(开机清
    per-port 使能)、cpu_set_gpio_wakeup(布防)、cpu_sleep_wakeup(入睡写
    0x26=src&0xFF)。BLE 栈/RF/ADV 路径的命中全是无关立即数。
  - 按键脚(PB4/PC0/PC4)默认 1M 上拉(gpio_default_8258.h),运行时
    btn_reconfig_gpio 每 10ms 补 10K;模拟输入使能 gpio_set_input_en 写
    areg_gpio_pb/pc_ie ✓;Level_Low=0,驱动 pol==0 → 置位极性位。
  - 即静态层面一切"应该空闲",但实机 0x44[3] 在入睡前恒为 1(SLPLOG
    S a=80 c=88/8A 两种极性写法下不变),驱动守卫(0x1e4 处 bit3+0x80 →
    13ms 后写 0x6f=0x20 复位)必然触发。
  - 已排除:看门狗(B c=00:0x72[0]/TMR_STA/0x44[6] 全 0,且 wd_stop 后
    循环依旧)、滤波位(0x26[3] 无关)、init_ble 伪造 W(已修:
    拆出 apply_rf_power)、驱动极性写法(手动 afe 块已删,仅剩
    cpu_set_gpio_wakeup)。
- **运行时取证(SLP_T_PADCFG=9 / SLP_T_PADPROBE=10 记录,入睡前写入)**:
  - PADCFG×3:a/b/c = {0x26,0x21,0x22} / {0x23,0x24,0x27} / {0x28,0x29,0x2a}
    (实测寄存器原始值,对照代码以为的配置)。
  - PADPROBE:a=位图(bit0 原配置下 W1C→3ms→bit3 重锁存;bit1 全撤使能仍
    重锁;bit2 仅 PB4;bit3 仅 PC0;bit4 仅 PC4),b=三键数字电平
    (bit0=F/PB4,bit1=L/PC4,bit2=R/PC0,1=松开),c=探测后 0x44 原始值。
  - 判读:bit1=1 → 锁存与引脚电平无关(0x26/检测器本身问题);
    仅某脚 =1 → 该脚电平×极性语义即定(数字电平见 b);
    全 0 → 入睡路径内另有置位者(再看 S c)。

## 2026-09-06 凌晨:两个最终根因与修复

1. **负半周封睡眠(lock_hold_until=0 陷阱)**:锁屏分支
   `(int32_t)(clock_time() - lock_hold_until) < 0` 在逗留期从未设置(=0)的
   手动锁屏下,开机 134~268s(tick 有符号值为负的半周期)里恒真 → 睡眠被
   整体封死。实测对照:锁屏时 uptime 在正半周的会话 1 秒内入睡;在负半周
   的 26~129 秒无任何尝试,00:25:57 那次解锁时刻恰为负半周结束点。
   修复:先判 `lock_hold_until != 0`(与 lock_hint_tick 同款防法)。
2. **驱动守卫误报 → 直入 0x80**:运行取证(SLP_T_PADPROBE)证明引脚电平
   干净(五态 bisect 全不锁存、三键松开),但 cpu_sleep_wakeup(0x80) 的
   守卫(0x1e4:0x44[3] → soft reboot)仍每次触发 —— 触发源是驱动自身在
   "W1C 之后、检查点之前"的寄存器写入副作用(0x2b/0x2c/0x07/0x7f/0x20/
   0x1f 等)。布防了 pad 唤醒脚的应用走 cpu_sleep_wakeup 永远进不去
   0x80。修复:direct_deep_sleep() 逐条复刻驱动 DEEPSLEEP 序列
   (pm_32k_rc 反汇编 0x54~0x1de)并删守卫,末尾调 SDK 公开的
   sleep_start();wakeup_src=PAD|BIT(3) → 0x26=0x18(直写路径不过守卫,
   16us 滤波可安全启用)。守卫删除的正当性:入睡前 probe 已证电平干净。

## 2026-09-06 02:00:start_suspend 实锤 + 两个假唤醒源

- **start_suspend 真身**(最终 ELF .vectors 段 0x20):就是"写 0x81 到 0x6f"
  (bit7=断电入睡硬件触发) + NOP 等待。直写路径的 sleep_start 调用序列本身
  正确,芯片一直在真的断电。
- **深睡唤醒的复位会清 0x35~0x39 并置 0x72[0]** → 此前"0x35 魔数判别法"
  把"睡了被假唤醒"误判成"没睡"(c=01 两种情况同值,判别法作废)。
- 两个假唤醒源(已修):
  1. 32k 比较值:驱动公式(literal 0xFFFFB1E0 + tick)在 tick_cur 较大时
     算出"≈现在" → 入睡 ~1s 定时假唤醒(实测复位周期恒 ~1s)。
     改为 tick_32k_cur + 0x60000000(≈13.7h @32k,永不命中)。
  2. 数字域 0x6e 复位默认 0x1f(I2C/SPI/USB host 唤醒全开)。按键走模拟域
     (0x26/0x27~0x2a),与之无关 → 入睡前写 0。

## 2026-09-06 02:45:0x80 维护睡眠定型(铁证)

- **0x80 从第一版起就能真睡**(32k 差值实锤:0x3a~0x3c 存入睡 tick,开机求差)。
  此前所有"没睡成/1s 复位/ERR wakeup"全是误判 —— 被"时钟恢复到入睡时刻"
  (stash)骗了:S/B 同秒 ≠ 没睡,只是日志时钟冻结。
- watchdog 在 0x80 深睡中不走(19.5s 睡眠 @10s 狗未复位)。无需停狗。
- **0x80 + 按键(pad)唤醒在这颗 8258 上不可用**:布防即致 0x44[3] → 驱动
  守卫拒睡(实验:撤防+纯定时 20s → 每次真睡 19.5s 并定时醒)。
- **定型方案 = 0x80 维护睡眠**:cpu_sleep_wakeup(0x80, PM_WAKEUP_TIMER,
  +60s)。维护醒=冷启动,广播保持 ON(不再关),15s 连接窗口内手机连上即
  自动解锁(main_loop maintenance+connected 检测);无连接 15s 后重睡。
  时钟靠 stash 恢复到入睡时刻(会慢,连接后校准)。
- 按键唤醒需求 → 只能关 super 走 B' retention(pad 布防只在非 super 执行)。
- 保留诊断:type-9 PADCFG/type-10 PADPROBE(取证书写在 0x28/0x29 会毛刺
  0x44[3])、type-11 WAITGATE(未睡门)、type-12 ELAPSED(真实断电秒数,
  用 0x3a~0x3c,仅 POR 清)。
