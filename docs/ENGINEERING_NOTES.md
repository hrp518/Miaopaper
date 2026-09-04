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
