# 小净开机动画：主工程 agent 审查 / 集成交接

日期：2026-10-31。状态：已接入生产构建，ESP-IDF 编译、COM8 烧录校验及正常重启串口验证通过；屏幕视觉残留验收待用户现场确认。

## 主工程审查与接入记录

原 `boot_animation_draft/` 作为交接原件保留。生产实现位于
`components/display_service/display_boot_animation.c/.h`，由组件 CMake 编译。

审查发现并修复：

1. 原 stop 在 `active=false` 时直接返回，遗漏了 start 失败后可能仍在队列中的 DMA。
   生产版以保留的 IO 所有权判断排空责任，即使没有 worker 也必须 drain 后才能释放。
2. 原 display start/prepare 错误路径忽略 hardware_stop 结果；停止失败后仍可能重新进入启动。
   现在失败资源保持 STOPPING 状态，只允许重试清理；join/drain/lock 失败均停止后续销毁。
3. 原 LVGL 清理未在互斥锁内等待 flush 结束，可能先释放 callback context/DMA。
   现在锁定、停止刷新 timer、确认 flush 完成、排空 SPI、解除 callback 后再移除 display。

PSRAM 帧分配失败：不用动画，填充静态深色背景后进入正常交接；worker 分配失败：
保留已完成的静态首帧。显示 prepare 失败不再阻止独立洗衣控制初始化。
SPI 故障且不能安全清理时仍禁止 LVGL 接管该 transport。

已核对：LVGL 8.4 字体 plain 4bpp，所有动画文字均有字形；RGB565 高字节先传，
方向与现有主 UI 一致；不新增 GPIO、音频 DMA 分配顺序或洗衣输出。
真实初始化里程碑为 5/30/60/85，不等待联网。用户后续确认希望完整播放：
正常交接改用 `display_boot_animation_finish()`，从显示首帧起至少播放3秒。
等待通过 FreeRTOS semaphore 阻塞启动任务，动画及独立安全/执行器任务继续运行。
异常清理仍调用 `display_boot_animation_stop()`，立即请求退出，不等待3秒。

### 背光与开机白屏（当前接线未改）

LED 背光目前直连3.3V，因此上电即发光；应用显示初始化前无法用软件隐藏
面板未初始化的白色画面。后续可用合适的背光开关电路，让复位/上电时默认关闭，
首帧传输完成后点亮，按需用 LEDC 做亮度渐变。
必须先核对模块 LED 引脚是背光供电还是已有使能输入，以及是否暴露独立 LEDK。
LED 正极供电且模块共地时应按模块电路设计供电侧开关，不可切断整个 LCD 的公共地。
GPIO 只控制开关，不直接承担背光负载；MOS 栅极应有确保复位默认关闭的外部偏置。
本次没有选择/分配背光 GPIO，也没有改变 LEDC、供电或现有接线。

参考：Espressif LCD 背光设计说明
https://docs.espressif.com/projects/esp-iot-solution/en/latest/display/lcd/lcd_guide.html

下文保留原交接设计和真机验收清单。正常启动可通过日志核对交接顺序；
冷启动、屏幕残留、页面切换以及故障注入必须区分已实测与尚未执行。

### 完整播放版验证（最新）

用户反馈原版约340ms就交接，观感过快。已按电脑预览中3秒启动画面的节奏调整：
从首帧点亮起至少完整播放3000ms，正常交接才等待，错误清理不等待。

- `idf.py build size` 通过，无本次 C 编译警告。BIN 4,155,648 字节，应用分区剩余
  300,800 字节；DIRAM 229,291 / 341,760（67.09%）。COM8 app-flash 校验通过。
- 重启日志：2039ms Started；5029ms Presentation finished after 3000ms；
  5099ms Stopped and drained；5459ms 不透明主界面整屏重绘提交、3页运行。
- 6429ms bootstrap 完成，开机播报成功，未观察到新增 panic/WDT。
  既有 Core dump/GPIO15/16/ESP-SR/ADC 校准告警仍保留。
- 日志：`tmp/boot_animation_full_duration.log`。LCD 背光当前仍直连3.3V，
  复位到应用显示初始化之前的亮白屏未通过本次软件时长调整消除。

### 首次集成验证（立即交接版，保留基线）

- COM8，ESP32-S3 rev0.2，app-flash 成功，Hash verified。
- EN/RTS 正常重启日志：2033ms 动画启动；2373ms worker 停止、SPI 排空；
  2733ms 不透明 overview 整屏重绘提交，display running（3 pages）。
- 3633ms bootstrap 完成：internal free 16,231，largest 8,192，PSRAM free 7,148,340。
  启动 PSRAM 帧已释放，保留现有音频 DMA 预留先于 LVGL 的分配顺序。
- 开机 B 播报完成，源 PCM 无削波、err=0；本次记录未出现新的 panic/WDT。
- 最终固件二进制精确包含新版“稍等，我想一想哦”的 WAV 一份，旧思考 WAV 不在镜像中。
  源/输出 SHA-256、单声道16位16kHz及2400ms长度校验通过。
- 日志：`tmp/boot_animation_and_thinking_r2.log`，由监控工具过滤并遮盖本地凭据。
- 仍有既存 Core dump 校验、共享 GPIO15/16、ESP-SR CPU/cache 建议及浊度 ADC 校准告警。
- 未执行：断电冷启动、无 Wi-Fi/禁语音配置矩阵、资源分配/SPI/join 故障注入；
  未目视确认 LCD 颜色、流畅度、三页切换及10秒后是否无残留，需现场反馈。

用户已审核电脑端动效方向。下文为保留的草案交接设计，当前生产接入与验证状态以上述审查记录为准；sdkconfig 未手动修改。

## 文件

- `components/display_service/boot_animation_draft/display_boot_animation.c`
- `components/display_service/boot_animation_draft/display_boot_animation.h`
- 本文档。

原型：`C:/Users/rihao23/.codex/visualizations/2026/10/31/01a19049-934d-7330-beb5-fdab03d8f906/xiaojing-boot.html`。

## 已核对的工程约束

- ESP32-S3 N16R8、IDF 5.5.4、LVGL 8.4.0、esp_lvgl_port 2.8.0~1。
- 320×240 横屏、ILI9341、SPI2 40MHz、BGR、RGB565；不改接线。
- 当前 `hardware_prepare()` 提前点亮 LCD，却没有填充显存；`hardware_build_ui()` 延迟到音频 DMA 预留后启动，存在明显空白窗口。
- 背光接 3.3V 常亮，不能通过软件背光控制消除 ROM / bootloader 阶段白屏。仅能从应用初始化显示后改善，实机时间需测量。
- 保持现有音频 DMA / LVGL / 无线分配顺序；不要为了动画提前启动完整 LVGL。

## 实现选择

启动阶段独占 panel/io，纯整数光栅化青色洗衣机、水波、品牌和进度条；复用 `s_rt.dma_reserve` 的两行 INTERNAL DMA 缓冲。PSRAM 帧缓冲 153600 字节；PSRAM 任务栈 4096 字节，TCB 及少量系统结构仍消耗内部 RAM。复用当前 `telemetry_font` 的未压缩 4bpp 字形，既不新增字体也不创建 LVGL 对象。

草案是低负载嵌入式适配版：像素图元、无抗锯齿渐变/圆角/扫光，约 80ms 延时加实际传输时间一帧。电脑预览的淡入及精细曲线不保证逐像素复现；需真机审核颜色、流畅度和方向。进度条只表示已完成的初始化里程碑，不显示百分比，不用时钟伪造完成状态。

`telemetry_font` 的读取仍会访问 LVGL 的字体描述缓存，因此动画 worker 必须在 `lvgl_port_init()` 前退出；不能与任何 LVGL 字体绘制并行。

## 集成步骤（由主工程 agent 审查后执行）

1. 将 c/h 从 `boot_animation_draft/` 移到 display_service 目录内，h 可放 private include。CMake 的 SRCS 增加该 c 文件；增加所需私有 include 路径。通过现有组件依赖使用 esp_lcd / freertos / lvgl；编译检查依赖声明是否需显式增加 esp_lcd。

2. `display_service.c` 包含新头文件。在 `hardware_prepare()` 中，panel reset/init 成功后显式 `esp_lcd_panel_disp_on_off(panel, false)`；将原来的 `...disp_on_off(..., true)` 移除。保持 DMA 预留分配位置不变。分配成功后调用：

```c
err = display_boot_animation_start(s_rt.panel, s_rt.io,
                                    s_rt.dma_reserve,
                                    LCD_DMA_RESERVE_BYTES);
```

start 在显示关闭时绘完第一帧并确认完成才发 display ON，避免主动展示未填充显存。start 内已设置与现有 LVGL 相同的 swap_xy/mirror。不要重复操作正在被 worker 使用的 panel。

3. 显示属于观察层，动画分配失败不应使洗衣安全控制服务启动失败。建议记录警告后进入“静态深色屏幕 / 原 UI”回退。start 失败时 worker 未运行，但 DMA 可能有失败后的未完成传输；需要调用 `esp_lcd_panel_io_tx_param(io, 0x00, NULL, 0)` 确认排空后才释放/复用 DMA。排空失败保留 transport 资源并使显示路径失败，不能继续 LVGL。

4. `hardware_build_ui()` 的第一步（必须在 `lvgl_port_init()` 之前）调用 `display_boot_animation_finish()` 并检查返回值。它等待正常3秒展示完成后调用 stop；stop 成功后才允许 free(dma_reserve)、lvgl_port_add_disp、安装 LVGL flush callback。若初始化本来超过3秒则不再额外等待；静态回退和动画传输错误不强制等待。

5. `hardware_stop()` 的第一步也调用 stop。失败则立即返回，保留 DMA / panel / io 及生命周期为可重试状态。不要让现有 start 错误路径忽略 stop 超时后继续销毁 transport；审查 `display_service_start()`、prepare 失败和 bootstrap rollback 的整个链路。应用停止动画超时不能通过强制删除任务规避。

6. 可在 display_service 内封装 `display_service_set_boot_stage(uint8_t)`，由 bootstrap 线程依次提交实际完成的里程碑：display prepare 5、HAL 与安全服务完成 30、执行器/业务服务完成 60、音频 DMA 预留后 85。当前 voice 分支会在较早位置调用 display_service_start；该处发生交接后后续 stage 不再改变屏幕。无语音配置走末尾 start 分支，同样必须交接。不要将 Wi-Fi 获得 IP 作为显示启动的等待条件。

7. 如果实测 prepare 之前仍有明显白屏，再评估将“只准备显示”提前到 bootstrap 开头。先确认 NVS 和安全初始化依赖以及内部堆分配压力，不要直接搬动现有初始化顺序。

## 无残留交接协议（必审）

顺序必须是：请求 worker 停止 → 等待 worker 完成（2s join 超时）→ SPI 排空成功 → 释放启动帧 → 建立 LVGL → 装载不透明主页面 → 整屏 invalidation + refresh。

在 `hardware_build_ui()` 的 LVGL lock 内，三页建好后进行以下处理：

```c
lv_obj_t *home = s_rt.pages[DISPLAY_PAGE_OVERVIEW];
lv_obj_set_style_bg_color(home, lv_color_hex(CLR_BG_DEEP), 0);
lv_obj_set_style_bg_opa(home, LV_OPA_COVER, 0);
lv_scr_load(home);                /* 无淡出旧屏动画 */
lv_obj_invalidate(home);          /* 覆盖完整 320x240，而非仅刷新卡片 */
lv_refr_now(s_rt.display);        /* 仍在 LVGL lock 内 */
```

普通 LVGL flush 的最后一个色块可能仍在 SPI 队列中：不要在其后调用 raw panel 清屏、改方向或 NOP 排空。后续传输由 esp_lvgl_port 的 flush callback 管理。若要求记录“主屏已完整可见”时刻，使用 LVGL flush 完成路径做状态记录；不要抢占/替换 esp_lvgl_port 的 callback。

worker 停止后不会再写 panel 或字体缓存。启动画面不创建 LVGL 对象、timer、anim，因此不存在旧动画对象留在 top/sys layer 或定时回写的问题。主页面必须是整屏不透明背景；不透明卡片并不足以清除卡片间隙的启动像素。

IDF 5.5.4 的 `esp_lcd_panel_io_spi.c::panel_io_spi_tx_param` 在轮询命令前回收所有排队颜色事务。草案以 ILI9341 NOP(0x00) 作为每条 DMA 色块的完成屏障，确认后才复用缓冲。它依赖已核对的 SPI 驱动实现；切换 IDF、总线或 panel 驱动必须重新审查。底层驱动 drain 使用 portMAX_DELAY；2s 只约束 join，不代表底层 SPI drain 有 2s 硬超时。

## 主工程验证清单

- 先审查 c/h API、4bpp 字形位序、RGB565 字节序、swap/mirror 与接管资源所有权。
- `idf.py build size` 已通过：BIN 4,155,408 字节，应用分区剩余 301,040 字节，
  DIRAM 229,283 / 341,760 字节（67.09%）。无 C 编译警告；保留已有 CPU 配置旧名替换提示。
  未执行 commit/push。
- 检查字体覆盖（小净 / 让洁净，自然发生 / 正在启动）；现有字体为 plain 4bpp，禁止未适配就换成压缩字体。
- 记录 prepare、动画 start/stop、display running 启动时间；确认音频 DMA、BLE/Wi-Fi、Edge/ESP-SR 启动无堆碎片退化。
- 真机冷启动、EN 复位、无 Wi-Fi、禁用语音配置分别检查；动画不等待网络、不触发真实输出。
- 观察主屏四角、滚筒原位置、标题和底部进度原位置；主屏显示后等待至少 10 秒，切换三页，确认无残留/回闪/异常重绘。
- 重复 stop、start/stop、分配失败、SPI 出错、join 超时后重试：不释放在用 DMA、不让两名 writer 并存、不留下运行任务。
- 长时间监测 WDT / Guru / heap；项目历史上有 LVGL 弧线绘制 WDT 记录，本草案启动图元不走 LVGL 圆弧绘制。

验收后再考虑优化帧率与淡入。不要在审查前将“电脑预览通过”描述为“真机动画通过”。
