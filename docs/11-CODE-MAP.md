> อัปเดตล่าสุด: 4 พ.ย. 2569 จาก commit `1ed5ac2`

# 11 · แผนที่โค้ดและคำอธิบายโค้ด — firmware + ml

> **อ้างอิงโค้ด:** `main` @ `1ed5ac2` (4 พ.ย. 2569) · FW-6/FW-7 (เดิม `fw/fw67-integrate`), OLED, dashboard (FW-8) และการปิด microSD **อยู่ใน `main` แล้วทั้งหมด** · เขียนครั้งแรก 1 พ.ย. 2569
> เอกสารนี้เล่าว่า**โค้ดทำอะไรและอยู่ตรงไหน** ส่วนตัวเลขที่ยังไม่ได้วัดให้ดูป้ายในคอมเมนต์ของโค้ดต้นทาง
> สิ่งที่ยืนยันจากโค้ดใน repo ไม่ได้ (เช่นค่าที่มาจาก sdkconfig หรือค่าเริ่มต้นของ ESP-IDF) ติดป้าย **[ยังไม่ยืนยัน]**
> ถ้าเอกสารนี้ขัดกับโค้ด ให้ยึดโค้ด แล้วแก้เอกสารนี้

**สารบัญ**
1. ภาพรวมทั้งระบบในหน้าเดียว
2. เฟิร์มแวร์: task, ลำดับบูต และช่องทางสื่อสารระหว่าง task
3. หนึ่งวินาทีของ sampler (เส้นทางของหนึ่งแถว CSV)
4. เส้นทาง CO (แยกจาก ML)
5. ML บนบอร์ด (FW-7) + FSM (FW-6)
6. หน้าจอ OLED + dashboard (อ่านอย่างเดียว)
7. แผนที่ไฟล์เฟิร์มแวร์ทีละไฟล์
8. host test และเครื่องมือฝั่ง PC
9. ML pipeline ฝั่ง PC (R0–R6)
10. แผนที่ไฟล์ `ml/` ทีละไฟล์
11. "อยากแก้ X ต้องเปิดไฟล์ไหน"
12. เอกสารเก่าที่ไม่ตรงกับโค้ดแล้ว

---

## 1. ภาพรวมทั้งระบบในหน้าเดียว

```
 เซนเซอร์                     ESP32-S3 (firmware/)                         PC (ml/ + firmware/tools/)
 ─────────                    ─────────────────────                        ──────────────────────────
 PMS7003 ─UART1─┐
 AHT21  ─I²C───┤→ [sampler 1 Hz] ─row queue→ [storage] ─USB serial "AGCSV,"/"AGML,"→ serial_logger.py
 ENS160 ─I²C───┘     │  ↑   │                   │                              → data/raw/usb_train/AG_*_usb.csv
                     │  │   │ D² ─ml_mailbox─┐  └─(SD ปิดเป็นค่าเริ่มต้น)            → EV_*_usb.csv (MARK)
                     │  │peek                ▼                                         │
 ZE07-CO ─UART2─→ [co] ─mailbox→ [io] FSM → LED เขียว/เหลือง/แดง + buzzer              ▼
                                  ↑  │                                ml.qa → ml.events → ml.baseline
 ปุ่ม MARK/MODE/ACK ─ISR→ ────────┘  │ mark queue → sampler                             │
                                     │                                                   ▼
          shown mailbox ┌────────────┘                          ag_mahal_pm_th.h (ค่าโมเดลที่ freeze แล้ว)
                        ▼                                       → copy เข้า firmware/components/mahal/model/
 sampler ─row mailbox→ [oled] → จอ SSD1306 (I²C บัสเดียวกับ sampler)
 sampler/io ─web mailbox→ [web] → [httpd] → มือถือ/โน้ตบุ๊ก (HTTP + WebSocket ผ่าน Wi-Fi)
```

แนวคิดหลักของโปรเจกต์ (CLAUDE.md §1) อยู่ใน **3 จุด** ในโค้ด:
- **บันทึกข้อมูลดิบทุกวินาที:** `tasks_sampler.c` → `tasks_storage.c` → `serial_logger.py`
- **ตัดสินว่าผิดปกติด้วย Mahalanobis D² (หลายตัวแปรพร้อมกัน) ไม่ใช่เกณฑ์ทีละตัว:** ฝั่ง PC คือ `ml/baseline.py` · บนบอร์ดคือ `components/mahal/`
- **เส้นทาง CO ตัดสินด้วยเกณฑ์ ppm ตายตัว แยกจาก ML:** `components/ze07/co_safety.c` + `tasks_co.c`

จอ OLED และ dashboard เป็น**ผู้อ่าน**เท่านั้น ไม่ตัดสินอะไรเอง และไม่มี task ไหนรอมัน (§6)

---

## 2. เฟิร์มแวร์: task, ลำดับบูต และช่องทางสื่อสารระหว่าง task

### 2.1 ตาราง task

ค่า priority และ stack อยู่ที่ [task_config.h](../firmware/main/task_config.h) ซึ่งเขียนเหตุผลไว้ทุกค่า และ [tasks_io.c](../firmware/main/tasks_io.c) (สำหรับ io: `IO_STACK_BYTES` · `IO_TASK_PRIORITY`)

| task | ไฟล์ | prio | core | stack | หน้าที่ | ห้ามทำอะไร |
|---|---|---|---|---|---|---|
| `co` | `tasks_co.c` | **11** | 1 | 3072 | อ่าน ZE07 ผ่าน UART2 · ตัดสิน CO ALARM/FAULT · publish snapshot · ลงทะเบียน task watchdog | ห้ามรอ task อื่น · ไม่ใช้เวลาจริง/SD/Wi-Fi/โมเดล |
| `sampler` | `tasks_sampler.c` | **10** | 1 | 5120 | รอบละ 1 s: อ่าน PMS/AHT21/ENS160 + peek ค่า CO → สร้างแถว → คำนวณ D² → ส่งเข้าคิว + ส่งสำเนาให้จอ/dashboard | ห้าม block (ส่งคิวด้วย timeout 0 · mailbox ใช้ overwrite) |
| `io` | `tasks_io.c` | 5 | ไม่ pin | **5120** | debounce ปุ่ม · FSM · ขับ LED/buzzer (เป็น**เจ้าของ GPIO ขาออกแต่ผู้เดียว**) · publish สิ่งที่แสดงให้จอ/dashboard | — |
| `oled` | `tasks_oled.c` | 4 | 1 | 4096 | วาดจอสถานะ · ส่งทีละชิ้นหลัง sampler ใช้บัสเสร็จ (§6.1) | ห้ามตัดสินสถานะเอง · ห้ามถือบัส I²C นานกว่าหนึ่งชิ้น |
| `storage` | `tasks_storage.c` | 3 | ไม่ pin | 6144 | พิมพ์ `AGCSV,`/`AGEV,`/`AGML,` ออก serial · เขียน SD เฉพาะเมื่อ `STORAGE_SD_ENABLE=1` | เป็นงานช้า จึงให้ priority ต่ำกว่า sampler |
| `console` | `console.c` | 2 | ไม่ pin | 3072 | คำสั่ง `settime` / `help` ทาง UART0 | ⚠️ บนบอร์ดจริงพอร์ต UART ใช้ไม่ได้ (CLAUDE.md §10) |
| `web` | `tasks_web.c` | 2 | ไม่ pin | 4096 | ทุก 1 s: รวม snapshot → ประวัติ 10 นาที → JSON → ส่ง WebSocket (§6.2) | ไม่ส่งคำสั่งเข้าเครื่อง · แถว CSV สำคัญกว่าจึงต่ำกว่า storage |
| `httpd` (esp_http_server) | `tasks_web.c` (`httpd_config_t`) | 2 | ไม่ pin (`tskNO_AFFINITY`) | 6144 | ตอบ HTTP / WebSocket ตาม §6.2 | เหตุผลเดียวกับ web |
| (ระบบ) Wi-Fi/SNTP | `time_sync.c` | ของ IDF | 0 **[ยังไม่ยืนยัน]** (ค่าเริ่มต้นของ IDF ตามคอมเมนต์ `task_config.h` · ดู `CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0` ใน sdkconfig) | — | ตั้งเวลาจาก SNTP · ต่อ Wi-Fi ใหม่ทุก 10 s | — |

**ทำไม co > sampler:** CO เป็นเส้นทางความปลอดภัย latency จากเฟรมถึงการตัดสินจึงต้องไม่ขึ้นกับงานของ sampler · งานต่อเฟรมของ CO สั้นมาก (parse 9 ไบต์ + เทียบเกณฑ์) จึงแทบไม่กินเวลาของ sampler
**ทำไม pin ไว้ core 1:** Wi-Fi และ lwIP มี priority สูงกว่าและอยู่ core 0 (`sdkconfig.defaults` pin lwIP tcpip ไว้ core 0 ด้วย `CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0`) · pin ไว้ core 1 เพื่อไม่ให้ถูกแทรก (เป็นสมมติฐาน ยืนยันด้วย jitter ที่วัดจาก GPIO4)
**ทำไม oled (4) สูงกว่า storage (3):** lock ของบัส I²C เป็น binary semaphore ไม่มี priority inheritance · ถ้าจอต่ำกว่า storage แล้ว storage ค้าง จอจะถือ lock ค้างและ sampler ต้องรอ (priority inversion) — คอมเมนต์ใน `task_config.h`

### 2.2 ลำดับบูต — [main.c](../firmware/main/main.c) `app_main()`

```
1. log reset reason · ขนาด flash · ขนาด PSRAM
2. สร้าง mark_queue (ความยาว MARK_QUEUE_LEN = 8)
3. co_mailbox_init()          ← ต้องมาก่อน io เพราะ io อ่าน mailbox นี้
   ml_mailbox_init()          ← ก่อน io ด้วยเหตุผลเดียวกัน · error แค่ log
   web_mailbox_init()         ← ก่อน io/sampler ที่เป็นผู้เขียน · error แค่ log
4. io_init(mark_queue)        → ตั้ง GPIO, ISR ปุ่ม, health/shown mailbox, สร้าง io task
   io_set_mode(NORMAL) · io_set_indication(IO_IND_WAIT_TIME)
5. co_start()                 → UART2 + co task       🔴 CO เริ่มทำงานตั้งแต่ตรงนี้
6. nvs_init()                 → Wi-Fi ต้องใช้ NVS
7. time_sync_start()          → Wi-Fi STA + SNTP (ถ้ามี secrets.h) · ไม่ abort แม้ Wi-Fi พัง
8. console_start()
9. storage_start()            → คิว + storage task (SPI + mount เฉพาะเมื่อ STORAGE_SD_ENABLE=1)
10. oled_mailbox_init()       ← ก่อน sampler ที่เป็นผู้เขียน
11. sampler_start()           → I²C (I2C_NUM_0 · 100 kHz) + UART1 + sampler task
12. oled_start(sampler_i2c_bus())  → เพิ่มจอบนบัสของ sampler + oled task
13. web_start()               → HTTP server + web task (ต้องมี Wi-Fi · time_sync_wifi_started())
```

**ทำไมเรียงแบบนี้:** CO ต้องเริ่มก่อนทุกอย่างที่อาจพังได้ (NVS, Wi-Fi, SD, เซนเซอร์อื่น) · ข้อ 7–13 ถ้า error จะ**แค่ log ไม่ abort** เพราะถ้า abort บอร์ดจะรีบูตวน และเส้นทาง CO จะดับไปด้วย (คอมเมนต์ FW-5b ใน `main.c`) · จอและ dashboard มาท้ายสุดเพราะเป็นของเสริม

### 2.3 ช่องทางสื่อสารระหว่าง task

กฎ CLAUDE.md §2 ห้ามแชร์ตัวแปรด้วย `volatile` อย่างเดียว ทุกช่องทางจึงเป็น **FreeRTOS queue** ซึ่งก๊อปข้อมูลทั้งก้อนภายใต้ critical section ของตัว queue ผู้อ่านจึงไม่มีทางเห็นข้อมูลครึ่งเก่าครึ่งใหม่ · ข้อยกเว้นเดียวคือประวัติของ dashboard ที่ใช้ mutex (ท้ายตาราง)

| ช่องทาง | ชนิด | ผู้เขียน → ผู้อ่าน | ข้อมูล | ไฟล์ |
|---|---|---|---|---|
| `s_event_queue` | queue 16 ช่อง (`int`) | ISR ปุ่ม + `io_set_indication()` + `io_notify_co()` → io | เลข GPIO (ค่าบวก) หรือคำสั่ง (ค่าลบ `COMMAND_*`) | `tasks_io.c` |
| `mark_queue` | queue 8 ช่อง | io → sampler | `io_mark_event_t` (กด MARK ที่ยืนยันแล้ว) | สร้างใน `main.c` |
| `s_row_q` (storage) | queue แบบ static 64 ช่อง | sampler → storage | `storage_row_t` = `csv_sample_t` + ผล ML ของรอบเดียวกัน | `tasks_storage.c` |
| `s_ev_q` | queue แบบ static 4 ช่อง | sampler → storage | `storage_event_t` (เวลาเริ่ม/จบ MARK) | `tasks_storage.c` |
| CO mailbox (`s_mailbox`) | queue ยาว 1 · `xQueueOverwrite`/`xQueuePeek` | co → io, sampler, oled, web | `co_status_t` | `tasks_co.c` |
| `ml_mailbox` | queue ยาว 1 แบบ static · overwrite/peek | sampler → io, oled, web | `ag_ml_result_t` | `ml_mailbox.c` |
| `s_health_mailbox` | queue ยาว 1 แบบ static · overwrite/peek | sampler → io | `io_sensor_health_t` (เวลาที่อ่านสำเร็จครั้งล่าสุดของแต่ละเซนเซอร์) | `tasks_io.c` |
| `s_shown_mailbox` | queue ยาว 1 แบบ static · overwrite/peek (`io_peek_shown`) | io → oled | `io_shown_t` (สิ่งที่ LED/buzzer แสดงอยู่) | `tasks_io.c` |
| แถวของจอ (`s_row_q` ใน `tasks_oled.c`) | queue ยาว 1 แบบ static · overwrite / **`xQueueReceive`** | sampler → oled | `oled_row_t` · การรับคือสัญญาณ "บัส I²C ว่างแล้ว" | `tasks_oled.c` |
| `s_row` (web) | queue ยาว 1 แบบ static · overwrite/peek | sampler → web | `web_row_t` | `web_mailbox.c` |
| `s_show` (web) | queue ยาว 1 แบบ static · overwrite/peek | io → web | `web_show_t` | `web_mailbox.c` |
| `s_hist_lock` | mutex (`xSemaphoreCreateMutex`) | web (เขียน) ↔ httpd (อ่าน `/history.json`) | ประวัติ 600 รายการ `web_hist_t` | `tasks_web.c` |

**"mailbox" คืออะไร:** queue ยาว 1 ช่องที่ผู้เขียนใช้ `xQueueOverwrite` (เขียนทับ ไม่มีวันรอ) และผู้อ่านใช้ `xQueuePeek` (อ่านโดยไม่เอาออก ไม่มีวันรอ) ใช้กับข้อมูลที่เป็น**สถานะ** ซึ่งสนใจแค่ค่าล่าสุด เช่นค่า CO ตอนนี้ · ส่วนข้อมูลที่เป็น**เหตุการณ์**และห้ามหาย เช่นแถว CSV และการกด MARK ใช้ queue ปกติ · แถวของจอเป็นกรณีพิเศษ: ผู้อ่านใช้ `xQueueReceive` เพื่อให้ตื่นครั้งเดียวต่อแถวใหม่

**ISR ปุ่ม** (`button_isr` ใน `tasks_io.c`) เป็น `IRAM_ATTR` ทำแค่ `xQueueSendFromISR(เลขขา)` · ลงทะเบียนด้วย `gpio_isr_handler_add` ใน `io_init()` (ขา 42, 47, 1 · pull-up ภายใน · `GPIO_INTR_NEGEDGE`) · ถ้าคิวเต็มจะทิ้ง edge นั้นไปเลย ไม่รอและไม่ log · ส่วน debounce 50 ms ทำใน io task

**callback อื่น:** `on_event` (Wi-Fi/IP event · `esp_event_handler_register`) · `on_sntp_sync` (SNTP) · timer ต่อ Wi-Fi ใหม่ (`esp_timer_create`) — ทั้งหมดอยู่ใน `time_sync.c` · `ws_send_all` ถูกส่งเข้า httpd ด้วย `httpd_queue_work` (`tasks_web.c`)

---

## 3. หนึ่งวินาทีของ sampler (เส้นทางของหนึ่งแถว CSV)

`sampler_task()` ใน [tasks_sampler.c](../firmware/main/tasks_sampler.c) เริ่มด้วย `ml_self_test()` และ `aht21_boot()` แล้ววนรอบละ 1000 ms ด้วย `xTaskDelayUntil`

```
GPIO4 = 1                         ← ขาดีบัก ใช้วัดคาบ/jitter ด้วย logic analyzer
t0 = esp_timer_get_time()
gettimeofday() + time_sync_is_valid()  → time_valid
handle_marks()      รับ MARK จาก mark_queue: ครั้งแรก = เริ่ม · ครั้งที่สอง = จบ → ส่ง EV row เข้า s_ev_q
row = {0}
pms_step()          ดึงเฟรมจาก UART1 ให้หมด → เก็บเฟรมล่าสุด · ใช้ซ้ำได้ถ้าอายุ ≤ 2500 ms · ช่วง 30 s แรก = ว่าง
aht21_step()        รอบคี่ = trigger · รอบคู่ = อ่าน 7 ไบต์ + CRC  ⇒ วัดทุก 2 s (DS §4.4 ห้าม ≤ 1 s)
ens160_step()       ส่งค่า T/RH ไปชดเชย (ทุก 2 s) → อ่าน DATA 6 ไบต์ + MISR · ว่างช่วง warm-up/start-up/invalid
co_step()           peek CO mailbox → co_ppm (ว่างช่วงอุ่นเครื่อง 5 นาที, ช่วง FAULT, และเมื่อเฟรมเก่ากว่า 2.5 s)
publish_health()    เวลาที่อ่านแต่ละเซนเซอร์สำเร็จ → io (ใช้ตัดสิน SYS FAULT)
ml_step()           D² + นับ persistence → ml_mailbox
ถ้า time_valid:  record_row()  ปัดเวลาเป็นวินาที · ตรวจวินาทีซ้ำ/ข้าม/ถอยหลัง · ใส่ label · ส่งเข้า s_row_q (timeout 0)
ไม่งั้น:         ไม่เขียน CSV (D1) · เตือนทุก 10 s
oled_publish_row()   สำเนาแถวให้จอ (overwrite) = บอกจอว่างานบัส I²C ของรอบนี้เสร็จแล้ว
web_row_publish()    สำเนาแถวให้ dashboard (overwrite)
update_indication()  ไฟเขียวบอกสถานะ logger: ยังไม่มีเวลา / กำลังบันทึก / marked
summary()            ทุก 60 s: jitter · แถวที่หาย · ตัวนับ error · stack HWM · heap · สรุป ML
GPIO4 = 0
xTaskDelayUntil(1000 ms)   (หรือ re-align ให้ตื่นหลังขอบวินาที 5 ms ถ้าเฟสเลื่อนเกิน 200 ms)
```

**จุดที่มักสงสัย**
- **ทำไมไม่แยก task ต่อเซนเซอร์:** งานต่อรอบเป็นแค่ poll ที่ไม่มีการรอ (ENS160 ใช้ I²C ราว 1.85 ms ต่อรอบ วัดเมื่อ 16 ต.ค.) แยก task จะเพิ่มทั้ง stack และ latency โดยไม่ได้อะไรกลับมา (คอมเมนต์ FW-5a)
- **ทำไม AHT21 ไม่รอ 80 ms:** trigger ในรอบหนึ่งแล้วอ่านในรอบถัดไป ห่างกัน 1 s ซึ่งเกิน 80 ms ไปมาก จึงไม่ต้องรอแบบ busy-wait ใน loop
- **ทำไมเวลาของแถวคือ "เวลาระบบปัดเป็นวินาที":** loop ล็อกเฟสให้ตื่นหลังขอบวินาทีเล็กน้อย ทำให้การปัดอยู่ห่างจุด .5 · jitter ระดับ ms จึงไม่ทำให้เกิดวินาทีซ้ำหรือข้าม (`ALIGN_TOLERANCE_US` · `ALIGN_AFTER_BOUNDARY_US` ต้นไฟล์)
- **ค่าว่างไม่ใช่ 0:** `csv_sample_t` มีธง `has_*` ของแต่ละกลุ่มค่า · `csv_format_row()` เขียนช่องว่างเมื่อธงเป็น false (CLAUDE.md §7)
- **MARK เก็บสถานะใน sampler ไม่ใช่ io:** เพราะ sampler เท่านั้นที่รู้ว่าการกดครั้งนั้นตรงกับแถวไหน · EV row ใช้เวลาของแถว marked แรกและแถว marked **สุดท้าย** (`mark_last`) ส่วน `ml.events draft` บวก 1 s ให้เป็นช่วงครึ่งเปิด
- **บัส I²C ใช้ร่วมกับจอ:** sampler สร้างบัส (`sampler_i2c_bus()`) แล้วจอมาเกาะทีหลัง · การป้องกันไม่ให้จอแย่งบัสอยู่ใน §6.1

**storage task** ([tasks_storage.c](../firmware/main/tasks_storage.c)):
1. รับแถวจากคิว แล้ว `format_row()` หนึ่งครั้ง
2. พิมพ์ `AGCSV,<row>` ออก serial (serial CSV mirror · `STORAGE_SERIAL_MIRROR=1`)
3. พิมพ์ `AGML,<timestamp>,<d2>,<status>,<streak>` ต่อท้ายทุกแถว (`STORAGE_SERIAL_ML=1`) · ไม่อยู่ในสัญญา CSV §7 · `serial_logger.py` เก็บไว้แค่ใน `serial_*.log`
4. **microSD ปิดเป็นค่าเริ่มต้น** (`STORAGE_SD_ENABLE 0` · ทีมตัดสิน 3 พ.ย. 2569 ตามคอมเมนต์ในไฟล์): ไม่ตั้ง SPI ไม่ mount ไม่ลอง remount ⇒ ทุกแถวออกทาง serial เท่านั้น
5. ถ้า build ด้วย `STORAGE_SD_ENABLE=1` (พฤติกรรม FW-4): เขียน SD (`fsync` ทุก 10 แถว · เปลี่ยนไฟล์ทุกวัน) · ถ้า SD หายจะลอง mount ใหม่ทุก 5 s · ระหว่างนั้นถ้าคิวใกล้เต็ม (`MIRROR_HIGH_WATER` = 60) จะดึงแถวออกไปพิมพ์ทาง serial อย่างเดียว

---

## 4. เส้นทาง CO (แยกจาก ML)

```
ZE07 ─UART2 (GPIO5 RX, 9600)─→ tasks_co.c read_uart() → parse_buffer() → ze07_parse()   (components/ze07/ze07_parse.c)
                                                       → co_safety_on_frame()/on_bad_frame()/tick()  (co_safety.c · pure C)
                                                       → publish(): xQueueOverwrite(co_status_t) → io_notify_co()
io task: co_peek_status() → co_alert_eval() (co_alert.c) → ถ้า co.active จะ "ทับ" output อื่นทั้งหมด
sampler: co_peek_status() → co_status_csv_value() → คอลัมน์ co_ppm
```

| ไฟล์ | หน้าที่ |
|---|---|
| `components/ze07/include/ze07.h` · `ze07_parse.c` | เฟรม 9 ไบต์ตาม manual V1.7 Table 4 (0xFF 0x04 …) · ตรวจ checksum · ค่าเป็นจำนวนเต็มหน่วย 0.1 ppm |
| `components/ze07/include/co_safety.h` · `co_safety.c` | **ตรรกะตัดสิน CO** · ALARM ทันทีเมื่อค่าเดี่ยว ≥ 90 ppm หรือค่าเฉลี่ย 1 ชม. ≥ 30 ppm · ค่าเฉลี่ย 8 ชม. ≥ 9 ppm = ธง "elevated" เท่านั้น ไม่มีไฟ/เสียง · hysteresis −10 % ค้าง 60 s · FAULT เมื่อเงียบ 6 s · อุ่นเครื่อง 5 นาที · ค่าเฉลี่ยใช้ถัง 480 ช่อง ช่องละ 1 นาที |
| `components/ze07/include/co_alert.h` · `co_alert.c` | แปลงสถานะเป็นไฟและเสียง: ALARM = แดง 5 Hz + buzzer · FAULT = แดงกะพริบคู่ทุก 2 s + chirp ทุก 60 s · ACK ตอน ALARM ปิดเสียง 5 นาที **แต่ไม่ล้างสถานะ ALARM** (ไฟแดงยังกะพริบ · มีเงื่อนไข ALARM ใหม่จะดังทันที) · ACK ตอน FAULT ปิด chirp จนกว่า FAULT จะหาย |
| `main/tasks_co.c` | task + UART2 (RX อย่างเดียว · GPIO6 ไม่ได้ตั้ง) + mailbox + task watchdog (`esp_task_wdt_add`) · GPIO7 = ขาดีบัก latency (`BOARD_PIN_DBG_CO`) · log สรุปทุก 60 s · `ZE07_ENABLE=0` = ไม่ build เส้นทาง CO เลย |

**ทำไม FAULT ต้องดังด้วย:** ถ้าเซนเซอร์เงียบไป แล้วเครื่องยังแสดงว่า "ปกติ" ผู้ใช้จะเข้าใจผิด · ผู้อ่าน (io) ยังตรวจ `co_status_stale()` เองอีกชั้น ถ้า CO task หยุด publish ก็จะแสดง FAULT เช่นกัน
**ทำไมค่าเฉลี่ยหารด้วยจำนวนตัวอย่างที่มีจริง:** ช่วงแรกหลังบูต ค่าเฉลี่ยจะได้**สูงกว่าหรือเท่ากับ**การนับช่องที่หายเป็น 0 ppm จึงเตือนเกินได้ แต่ไม่มีทางเตือนขาด
⚠️ ALARM ทดสอบด้วย host test เท่านั้น (`test_co_safety.c`) · `ze07_sim.py` มีไว้ทดสอบบนบอร์ดแต่ยังไม่ได้ทำ (CLAUDE.md §10) · ข้อความ §8: **อุปกรณ์นี้ไม่ใช่เครื่องเตือนภัย CO**

---

## 5. ML บนบอร์ด (FW-7) + FSM (FW-6)

### 5.1 Mahalanobis บนบอร์ด — `components/mahal/`

สูตรเดียวกับฝั่ง Python ทุกขั้น (`ml/baseline.py` · `ml/BASELINE.md` §3 · ที่มาของสูตรอยู่ใน `docs/05-MULTIVARIATE.md` §2)

```
t_j = log1p(x)  สำหรับ PM/TVOC   (ข้อมูลเบ้ขวา)   ·   t_j = x  สำหรับค่าอื่น
z_j = (t_j − mean_j) / std_j      ← mean/std มาจากชุด train เท่านั้น 🔴 ห้ามคำนวณใหม่บนบอร์ด
d_j = z_j − center_j
D²  = dᵀ · P · d                  P = Σ*⁻¹ · Σ* = covariance แบบ Ledoit–Wolf ของ z ชุด train (กลับเมทริกซ์บน PC ครั้งเดียว)
persistence: D² ≥ threshold ติดกัน ≥ K แถว → PERSISTENT
```

- **ทำไมต้องใช้ D² แทน 3σ ทีละตัว:** D² วัดว่า "แถวนี้ห่างจากรูปแบบปกติแค่ไหน **เมื่อนับความสัมพันธ์ระหว่างตัวแปรด้วย**" · กรณีทำอาหาร PM, T, RH ขึ้นพร้อมกัน ซึ่งตรงกับ covariance ของช่วงปกติ D² จึงไม่สูงมาก · กรณีปัดฝุ่น PM ขึ้นแต่ T/RH นิ่ง ซึ่งขัดกับ covariance D² จึงพุ่ง
- **ทำไม pm_th (pm2_5, temp_c, humidity_pct):** ฟีเจอร์ชุดนี้ใช้ได้ตั้งแต่ก่อน ENS160/ZE07 พร้อม · ชุด `full` เพิ่ม tvoc_ppb และ co_ppm · ไม่ใช้ pm1_0/pm10 เพราะมาจากเซนเซอร์ตัวเดียวกับ pm2_5 และไม่ใช้ eco2 เพราะคำนวณมาจาก tvoc (`FEATURE_SETS` ใน `baseline.py`)
- **ค่าว่าง:** ถ้าฟีเจอร์ที่เลือกตัวใดว่าง → `AG_ML_MISSING` และ persistence นับใหม่ **ไม่เติมค่าแทน** (no imputation)
- **ช่องว่างเวลา:** ถ้าสองตัวอย่างห่างกันเกิน 1.5 s จะไม่นับว่าติดกัน (`AG_MAHAL_GAP_RESET_US`)
- **self-test ตอนบูต:** `ml_self_test()` ต้น sampler task รัน golden vectors ที่ฝังมาใน header ด้วยเลขคณิต float32 ของบอร์ด · ถ้าไม่ผ่านจะได้ `AG_ML_MODEL_FAULT` ตลอดจนกว่าจะรีบูต · log `mahal self-test PASS: model=...` บอกว่าบอร์ดรันโมเดลไหนอยู่
- **header โมเดล** `model/ag_mahal_pm_th.h` **สร้างอัตโนมัติจาก `ml.baseline` ห้ามแก้ด้วยมือ** · ตัวปัจจุบันเป็นโมเดลจากข้อมูลจริง (`data=real` · `ml3-v1+pm_th+e6f29d2be5b3`) · `AG_MAHAL_THRESHOLD_D2` = 17.92 · `AG_MAHAL_PERSISTENCE_K` = 1 · ตรงกับ `data/processed/ml-real/pm_th/ag_mahal_pm_th.h` · วิธีเปลี่ยนเขียนไว้ใน `model/ag_mahal_model.h`
- `ml_step()` คำนวณใน sampler เพราะใช้แค่ log1pf หนึ่งครั้งกับ quadratic form 3×3 ซึ่งเป็นเวลาคงที่ ไม่มี I/O · เวลาที่ใช้จริงดูได้จาก `compute avg=… max=…` ในบรรทัดสรุป `ml …` ของ sampler ทุก 60 s

### 5.2 FSM — `components/fsm/ag_fsm.c` + `main/io_compose.h`

io task ตัดสินว่าจะแสดงอะไร โดย**เลือกบรรทัดแรกที่เป็นจริง**:

| ลำดับ | สถานะ | เงื่อนไข | ไฟ/เสียง |
|---|---|---|---|
| 1 | **CO** | `co_alert` active | รูปแบบของ CO (§4) — 🔴 ไม่มีอะไรบังได้ |
| 2 | ML_ALARM | PERSISTENT ติดกัน ≥ 30 แถว (`AG_FSM_ML_ALARM_STREAK`) | แดง 5 Hz + beep 3 ครั้งทุก 10 s |
| 3 | SYS_FAULT | เซนเซอร์อ่านไม่สำเร็จนานเกินกำหนด · ML self-test พัง · ผล ML เก่ากว่า 3 s (`AG_FSM_ML_STALE_US`) | เหลืองกะพริบคู่ทุก 2 s ไม่มีเสียง |
| 4 | ML_WATCH | PERSISTENT | เหลือง 1 Hz |
| 5 | NORMAL | อื่นๆ | เขียวติดค้าง / สถานะ logger (§2) |

- ลดระดับได้เมื่อ BELOW ติดต่อกัน 30 s และลดทีละขั้น (ALARM → WATCH → NORMAL) ไฟจึงไม่กะพริบไปมาเมื่อ D² แกว่งรอบ threshold
- **ปุ่ม:** ACK ไปที่ CO ก่อน (`io_route_ack`) ถ้า CO ไม่ได้ใช้ ACK ค่อยไปปิดเสียง ML ALARM · กด MODE ค้าง 3 s = demo 60 s ที่วนแสดง NORMAL → WATCH → ALARM → FAULT · **FSM ไม่เขียนคอลัมน์ใดลง CSV** demo จึงไม่ทำให้ข้อมูลเสีย
- `io_set_mode()` รับแค่ `NORMAL` (คืน `ESP_OK` และไม่ทำอะไร) · `WATCH`/`ALARM` คืน `ESP_ERR_NOT_SUPPORTED` ⇒ task อื่นบังคับสถานะ WATCH/ALARM ไม่ได้ · ปุ่ม MODE แบบวนสถานะและ "ACK → NORMAL" ของ FW-1 ไม่มีแล้ว
- `io_compose.h` เป็นฟังก์ชัน pure C ที่รวม output ของ CO กับ FSM เข้าด้วยกัน แยกไว้เพื่อให้ host test (`test_io_compose.c`) ทดสอบด้วยโค้ดชุดเดียวกับที่รันบนบอร์ด
- io task ส่งสิ่งที่แสดงอยู่ต่อให้จอ (`io_shown_t`) และ dashboard (`web_show_t`) ทุกรอบ ⇒ จอ/หน้าเว็บไม่มีทางแสดงสถานะต่างจาก LED
- ผลขึ้นบอร์ดของเฟิร์มแวร์ชุดนี้บันทึกใน `docs/bringup/2026-11-03.md`

---

## 6. หน้าจอ OLED + dashboard (อ่านอย่างเดียว)

ทั้งสองอย่างเป็น**กระจกสะท้อน**สิ่งที่ sampler, co และ io ตัดสินไปแล้ว: ไม่ตัดสินสถานะเอง ไม่เขียน CSV/serial และไม่มี task ไหนรอ · ถ้าพังจะแค่ log (CLAUDE.md §2: เส้นทาง CO และ logger ไม่ขึ้นกับมัน) · **dashboard ไม่ใช่ CO alarm** (CLAUDE.md §8)

### 6.1 จอ OLED — `main/tasks_oled.c` + `components/oled_ui/`

| ส่วน | หน้าที่ |
|---|---|
| `components/oled_ui/` (`oled_ui.c` · `oled_font.c`) | pure C (ไม่มี FreeRTOS, I²C, float, malloc): แปลงสถานะเป็นข้อความ (`oled_ui_format`) · วาดลง frame buffer 128×64 · ฟอนต์ 5×7 · คำสั่ง init ของชิป (`oled_init_cmds`) · หาชิ้นที่เปลี่ยน (`oled_fb_next_dirty`) |
| `main/tasks_oled.c` | task + I²C + mailbox แถว · อ่าน `io_peek_shown()` · `co_peek_status()` · `ml_mailbox_peek()` และแถวล่าสุดจาก sampler |

**3 ชั้นที่กันไม่ให้จอขวาง sampler บนบัส I²C เดียวกัน** (คอมเมนต์ `tasks_oled.h`):
1. ส่งทีละครึ่งหน้า (`OLED_CHUNK_COLS` = 64 คอลัมน์ · 66 ไบต์ต่อชิ้น) ⇒ ถือ lock ของบัสไม่เกินหนึ่งชิ้น
2. ส่งเฉพาะหลัง sampler ใช้บัสเสร็จ: `oled_publish_row()` ปลุก task จอ แล้วจอส่งชิ้นที่เปลี่ยนได้จนถึง 700 ms หลังต้นรอบ (`OLED_WINDOW_END_US`) · ถ้าไม่มีแถวใหม่ 1.5 s (`OLED_NO_ROW_WAIT_MS`) ยังอัปเดตในช่วงสั้น เพื่อแสดงว่า sampler หยุด
3. priority 4 บน core 1 (§2.1)

- **บูต:** test pattern 3 s (`OLED_PATTERN_US`) → ข้อความ CLAUDE.md §8 ภาษาอังกฤษ + รุ่นโมเดล 8 s (`OLED_BOOT_TEXT_US` · `oled_ui_boot_lines`) → หน้าสถานะ
- **หน้าสถานะ:** สถานะที่ LED แสดง (`oled_show_t`) · CO · PM2.5 · T/RH · TVOC/eCO2 · เวลา + marked · สถานะ ML + D² · ค่าว่างแสดง `--` (ตาม `oled_ui_in_t`)
- **จอหาย/ไม่ตอบ:** probe ใหม่ทุก 30 s (`OLED_PROBE_RETRY_US`) · error ติดกัน 3 ครั้ง (`OLED_ERRORS_TO_DROP`) = ถือว่าจอหาย
- **ตั้งค่าจอ:** `OLED_CHIP` · `OLED_ROTATE_180` · `OLED_I2C_ADDR` (0x3C) ต้นไฟล์ `tasks_oled.c` · ชิปเป็น SSD1306 หรือ SH1106 **[ยังไม่ยืนยัน]** (โค้ดระบุว่ายังไม่ยืนยัน · ค่าในโค้ดคือ `OLED_CHIP_SSD1306`)
- `-DOLED_ENABLE=0` = ไม่มี task ไม่มี mailbox (`oled_publish_row()` ไม่ทำอะไร) · host test `test_oled_ui.c`

### 6.2 Dashboard (FW-8) — `main/tasks_web.c` + `components/webui/` + `main/web/index.html`

เริ่มเฉพาะเมื่อ Wi-Fi เริ่มแล้ว (`time_sync_wifi_started()`) ⇒ ไม่มี `secrets.h` = ไม่มี dashboard · ใช้ Wi-Fi STA เดียวกับ SNTP (hotspot มือถือ) · การออกแบบ: `docs/12-DASHBOARD-DESIGN.md`

| endpoint | handler | ส่งอะไร |
|---|---|---|
| `GET /` | `index_get` | หน้า `index.html` ที่ฝังใน firmware (`EMBED_TXTFILES` → `index_html_start`/`index_html_end`) |
| `GET /history.json` | `history_get` | ประวัติ 10 นาที (`WEB_HIST_LEN` = 600) เป็น array ของ `[uptime_s, pm2_5, temp_c, humidity_pct, d2, shown]` · ก๊อปประวัติภายใต้ `s_hist_lock` แล้วส่งเป็นชิ้น `WEB_HIST_CHUNK` = 1024 B |
| `/ws` (WebSocket) | `ws_handler` | หลัง handshake client ได้ JSON 1 ข้อความ/วินาที · ข้อความที่ client ส่งมาถูกอ่านแล้วทิ้ง (ไม่มีคำสั่ง) |

- **web task ทุก 1 s** (`WEB_PERIOD_MS`): `build_snapshot()` (peek `web_row_t`, `web_show_t`, ML, CO) → `web_hist_push()` (ภายใต้ mutex) → `web_json_live()` → `httpd_queue_work(ws_send_all)` ส่งให้ทุก client ที่เป็น WebSocket · ถ้าคิวของ httpd เต็มจะทิ้งวินาทีนั้น
- **รูปแบบ JSON** (`components/webui/web_json.c` · `WEB_JSON_VERSION` = 1): **ไม่ใช่สัญญา CSV §7** ไม่ลงไฟล์ข้อมูล แต่ใช้กติกาค่าเดียวกัน: ค่าว่าง = `null` (ไม่ใช่ 0/-1) · T/RH/CO ปัดด้วย `csv_round_tenths` ให้ตัวเลขตรงกับแถว CSV
- **หน้าเว็บ** (`index.html`): โหลด `/history.json` ครั้งแรก แล้วเปิด `/ws` · แสดงแถบสถานะ · ค่าเซนเซอร์ · การ์ด Mahalanobis D² · การ์ด CO (เส้นทางแยกจาก ML) · กราฟ 10 นาทีของ PM2.5, D², อุณหภูมิ, ความชื้น
- **server:** `max_open_sockets` = 4 (`WEB_MAX_SOCKETS`) + `lru_purge_enable` · พอร์ตใช้ค่าจาก `HTTPD_DEFAULT_CONFIG()` **[ยังไม่ยืนยัน]** (ไม่ได้ตั้งในโค้ด · ดูบรรทัด `dashboard on port …` ใน log ตอนบูต) · 4 sockets เทียบกับ `CONFIG_LWIP_MAX_SOCKETS` **[ยังไม่ยืนยัน]** (โค้ดติดป้ายให้ตรวจเอง)
- **build:** ต้องเปิด `CONFIG_HTTPD_WS_SUPPORT` (อยู่ใน `sdkconfig.defaults` · CMake หยุดให้ถ้าปิด) · `-DWEB_ENABLE=0` = ไม่มี server/web task (mailbox ยังอยู่) · แก้ `index.html` แล้วต้อง build + flash ใหม่เพราะหน้าเว็บฝังใน firmware · host test `test_web_json.c`

---

## 7. แผนที่ไฟล์เฟิร์มแวร์ทีละไฟล์

```
firmware/
├── CMakeLists.txt · sdkconfig.defaults (flash 16 MB · PSRAM octal · HTTPD WS · lwIP core 0) · partitions.csv
├── main/                     ← เฉพาะโปรเจกต์นี้: task + การเดินสาย
│   ├── CMakeLists.txt        SRCS · REQUIRES · EMBED_TXTFILES web/index.html · สวิตช์ -D (ข้างล่าง)
│   ├── main.c                app_main: ลำดับบูต (§2.2)
│   ├── board_pins.h          เลข GPIO ทุกขา (ตรงกับ CLAUDE.md §4) · GPIO4 = ขาดีบัก sampler · GPIO7 = ขาดีบัก CO · _Static_assert ห้ามขาต้องห้าม
│   ├── task_config.h         priority / core / stack พร้อมที่มาของแต่ละค่า (ยกเว้น io)
│   ├── tasks_sampler.c/.h    รอบ 1 Hz (§3) · เจ้าของบัส I²C, UART1, สถานะ MARK · sampler_i2c_bus()
│   ├── tasks_storage.c/.h    serial mirror (AGCSV/AGEV/AGML) · SD เฉพาะเมื่อ STORAGE_SD_ENABLE=1
│   ├── tasks_io.c/.h         ปุ่ม (ISR → queue → debounce) · LED/buzzer · FSM · health/shown mailbox
│   ├── tasks_co.c/.h         CO task (§4)
│   ├── tasks_oled.c/.h       จอ OLED (§6.1)
│   ├── tasks_web.c/.h        HTTP server + web task (§6.2)
│   ├── web_mailbox.c/.h      mailbox แถว/สถานะสำหรับ dashboard (§2.3)
│   ├── ml_mailbox.c/.h       mailbox ผล ML (§2.3)
│   ├── io_compose.h          รวม CO + FSM แบบ pure C (§5.2)
│   ├── web/index.html        หน้า dashboard (ฝังตอน build)
│   ├── time_sync.c/.h        TZ ICT-7 · Wi-Fi STA + SNTP pool.ntp.org · time_sync_is_valid() · time_sync_wifi_started()
│   ├── console.c/.h          คำสั่ง settime / help
│   └── secrets.h.example     ชื่อ/รหัส Wi-Fi → ก๊อปเป็น secrets.h (ไม่ขึ้น git)
├── components/               ← ไดรเวอร์/ตรรกะที่แยก "ส่วน pure C" ออกมาให้ host test ได้
│   ├── pms7003/   pms7003_parse.c (เฟรม 32 ไบต์ · checksum · ใช้ชุด atmospheric) · pms7003_uart.c (UART1 9600 + ring buffer)
│   ├── aht21/     aht21_parse.c (CRC8 0x31 init 0xFF · แปลง 20 บิต → °C/%RH) · aht21.c (I²C 0x38)
│   ├── ens160/    ens160_parse.c (ถอด status/validity · กติกาถือค่า 2.5 s · แปลงค่าชดเชย) · ens160.c (I²C 0x53 · OPMODE · MISR)
│   ├── ze07/      ze07_parse.c · co_safety.c · co_alert.c (§4)
│   ├── storage/   csv_row.c — แปลง csv_sample_t → ข้อความตามสัญญา §7 · ชื่อไฟล์ AG_/EV_ · header
│   ├── logutil/   parse ISO 8601 (settime) · ปัดวินาที · จำแนกวินาทีซ้ำ/ข้าม/ถอยหลัง · กติกาความสดของเฟรม · สถิติ min/max/jitter
│   ├── mahal/     ag_mahal.c + model/ag_mahal_pm_th.h (§5.1)
│   ├── fsm/       ag_fsm.c (§5.2)
│   ├── oled_ui/   oled_ui.c · oled_font.c (§6.1)
│   └── webui/     web_json.c — JSON + ประวัติ 10 นาที (§6.2)
├── test_host/                ← unit test ที่คอมไพล์ด้วย gcc บน PC (ไม่ต้องใช้บอร์ด) · stubs/ = header ปลอมของ ESP-IDF
└── tools/                    ← สคริปต์ Python ฝั่ง PC (§8)
```

**แบบแผนที่ใช้ทั้ง repo:** ทุกเซนเซอร์แยกเป็น `*_parse.c` (pure C: ไม่มี FreeRTOS, ไม่มี I/O) กับไฟล์ที่คุยกับฮาร์ดแวร์ · ส่วน parse คือส่วนที่ host test ทดสอบด้วยเฟรมจริงจาก datasheet · ถ้าจะเพิ่มเซนเซอร์ใหม่ ให้ทำตามแบบนี้ · จอ (`oled_ui`) และ dashboard (`webui`) ใช้แบบเดียวกัน

**บัสและ pin ตามที่ตั้งในโค้ด:** I²C `I2C_NUM_0` 100 kHz (`I2C_SCL_HZ` · SDA 8 / SCL 9 · ไม่เปิด pull-up ภายใน) ร่วมกัน AHT21 0x38 · ENS160 0x53 · OLED 0x3C · UART1 = PMS7003 (`PMS_UART`) · UART2 = ZE07 (`CO_UART`) · SPI2 = microSD (`SD_SPI_HOST` · ใช้เฉพาะ `STORAGE_SD_ENABLE=1`) · console = `CONFIG_ESP_CONSOLE_UART_NUM` · เลขขาทั้งหมดอยู่ใน `board_pins.h`

**partition (`partitions.csv`):** nvs 0x9000 (0x6000) · phy_init 0xf000 (0x1000) · factory app 0x10000 ขนาด 4 MB · ไม่มี OTA

**สวิตช์ตอน build / ค่าคอนฟิก**
- CMake `-D` (ต้อง build ในโฟลเดอร์แยก ถ้าตั้งเป็น 0 ใน `build/` CMake จะหยุดให้): `ENS160_ENABLE` · `ZE07_ENABLE` · `OLED_ENABLE` (ค่าเริ่มต้น 1 ทั้งหมด) · `WEB_ENABLE` (ค่าเริ่มต้น 1)
- macro ในซอร์ส (ไม่มีทางส่งผ่าน CMake ต้องแก้ไฟล์): `STORAGE_SD_ENABLE` = 0 · `STORAGE_SERIAL_MIRROR` = 1 · `STORAGE_SERIAL_ML` = 1 (`tasks_storage.c`)
- `AIRGUARD_HAVE_SECRETS` ตั้งโดย CMake เมื่อมี `main/secrets.h`
- ไม่มีไฟล์ Kconfig ของโปรเจกต์ · ไม่มี NVS key ของเราเอง (NVS ใช้โดย Wi-Fi ตามคอมเมนต์ `nvs_init()` ใน `main.c`)

**สัญญา CSV ในโค้ด:** `csv_sample_t` และ `csv_format_row()` ใน `components/storage/` เป็น**ที่เดียว**ที่กำหนดลำดับคอลัมน์ฝั่งบอร์ด · ฝั่ง PC คือ `RAW_COLUMNS` ใน `ml/load.py` และ `CSV_HEADER` ใน `serial_logger.py` · มี test ที่เทียบสามที่นี้กับ CLAUDE.md §7 · เปลี่ยนที่ใดที่หนึ่ง ต้องตกลงกันทั้งทีมก่อน

---

## 8. host test และเครื่องมือฝั่ง PC

| ไฟล์ | ทดสอบ/ทำอะไร | รันอย่างไร |
|---|---|---|
| `test_host/run_tests.ps1` | คอมไพล์และรัน `test_*.c` ทั้ง 13 ชุดด้วย gcc (`-Wall -Wextra -Werror -pedantic`) | `powershell firmware/test_host/run_tests.ps1` |
| `test_pms7003_parse.c` · `test_pms7003_uart.c` | เฟรม PMS (ใช้ทั้งชุด CF=1 และ atmospheric · CSV ใช้ atmospheric) · การอ่านผ่าน UART | ″ |
| `test_aht21_parse.c` · `test_csv_row.c` · `test_logutil.c` | CRC · การแปลงค่า · รูปแบบแถว · settime · วินาทีซ้ำ/ข้าม | ″ |
| `test_ens160_parse.c` | status/validity/กติกาถือค่า | ″ |
| `test_ze07_parse.c` · `test_co_safety.c` | เฟรม CO · เกณฑ์ ALARM/FAULT/hysteresis/ค่าเฉลี่ย | ″ |
| `test_mahal.c` · `test_fsm.c` · `test_io_compose.c` | golden D² · persistence · ลำดับความสำคัญของ FSM · CO ต้องชนะเสมอ | ″ |
| `test_oled_ui.c` · `test_web_json.c` | ข้อความ/frame buffer/คำสั่งของจอ · JSON ของ dashboard และประวัติ | ″ |
| `tools/serial_logger.py` | อ่าน COM port → `AG_*_usb.csv` · `EV_*_usb.csv` · `serial_*.log` (ตรวจแต่ละแถวกับสัญญา §7) | `python tools\serial_logger.py --port COM4 --out ..\data\raw\usb_train` |
| `tools/heater_compare.py` | เทียบ T/RH ระหว่างช่วง ENS160 ปิด/เปิด (ผลกระทบจากฮีตเตอร์) | ดู docstring |
| `tools/ze07_sim.py` | ส่งเฟรม CO ปลอมผ่าน USB-UART 3.3 V เพื่อทดสอบ ALARM/FAULT บนบอร์ด | ต้องถอด J2 ก่อน · ดู docstring |
| `tools/test_*.py` | unit test ของสามสคริปต์ข้างบน | `python -m unittest discover -s firmware/tools -p "test_*.py" -v` |

---

## 9. ML pipeline ฝั่ง PC (R0–R6)

แผนเต็มอยู่ใน `docs/10-ML-REAL-DATA-PROMPTS.md` · คำสั่งจริงอยู่ใน [ml/RUNBOOK-REAL.md](../ml/RUNBOOK-REAL.md) · โปรโตคอลเก็บข้อมูลอยู่ใน `data/PROTOCOL-TRAIN-2026-10.md`

```
data/raw/usb_train/AG_*_usb.csv ──┐
                                  ├─ R0  ml.qa       ตรวจช่องว่าง · ค่าค้าง · ค่านอกช่วง · สถิติรายวัน → qa.json/qa.md/png (อ่านอย่างเดียว)
data/raw/usb_train/EV_*_usb.csv ──┤
                                  ├─ R1  ml.events draft → แก้ TODO ด้วยมือ (incense/dusting/cooking/other) → ml.events check → OK
                                  │       → data/events/2026-10_train.csv   (ขึ้น git ได้ · เป็น ground truth)
                                  ├─ R3  ดูผล qa → freeze t1/t2 · แถว train ที่อยู่ในเหตุการณ์ที่ไม่ใช่ normal ถูกตัดออกจาก fit (drop_event_rows)
                                  ├─ R4  ml.baseline --raw … --events … --mapping … --row-support-s 1 --t1 --t2 --gap-s 60 --feature-set pm_th
                                  │       ├ load.load_raw / load_events          อ่านตามสัญญา §7
                                  │       ├ split.chronological_split            train < t1 · val · test  (ตามเวลา ห้ามสุ่ม)
                                  │       ├ split.normal_only + fit_normalizer   fit ด้วยแถว label=normal ของ train เท่านั้น
                                  │       ├ baseline.fit                         transform → z → Ledoit–Wolf → ตรวจ condition number → P
                                  │       ├ select_operating_point (val เท่านั้น)  เลือก threshold + K ภายใต้งบเตือนผิด ≤ 1 ครั้ง/วัน
                                  │       ├ frozen_hash                          hash โมเดลก่อนแตะ test
                                  │       └ metrics.evaluate (test ครั้งเดียว)    → model.json · norm.json · results.json · ag_mahal_pm_th.h
                                  │                                                 · test-mahalanobis/metrics.json + points.csv
                                  ├─ R5  ml.dryrun check-header <header> → PASS → ก๊อปไป firmware/components/mahal/model/
                                  └─ R6  รูปและตารางสำหรับสไลด์
```

ผล R4 ที่ freeze แล้วอยู่ใน `data/processed/ml-real/pm_th/` (header ตัวเดียวกับที่อยู่ใน firmware · §5.1)

**กฎ ML ที่ฝังอยู่ในโค้ดแล้ว (CLAUDE.md §2)**
- **แบ่งตามเวลา:** `chronological_split()` · มีช่วง gap คั่นระหว่างชุดเพื่อไม่ให้หน้าต่างเวลาคร่อมขอบ · เหตุการณ์ที่คร่อมขอบหรือแตะ gap จะ error (`check_event_boundaries`)
- **normalize ด้วยค่าจาก train เท่านั้น:** `fit_normalizer()` บันทึกลง `norm.json` → ฝังลง header → บอร์ดใช้ค่าคงที่ชุดนั้น
- **ไม่ใช้ accuracy และไม่ใช้ Point Adjustment:** `metrics.py` (นิยาม `ml2-v1` ใน `METRICS.md`) วัดแบบ**นับเหตุการณ์จากจุดเริ่ม (onset)**: `event_detection_rate` (detected/event_count) · `mttd_detected_s` · `false_alarm_episodes_per_day` = episode × 86400 / เวลาปกติที่สังเกตได้ · AP (average precision) ระดับจุด · ช่วงที่ map เป็น `unknown` ไม่นับทั้งถูกและผิด
- **threshold เลือกบน val เท่านั้น:** แล้ว hash (`frozen_sha256`) ก่อนให้คะแนน test · 🔴 รัน R4 แล้วห้ามแก้ค่าแล้วรันใหม่ (RUNBOOK §4)
- **ทำซ้ำได้:** ทุกคำสั่งรับ `--seed` · ผลลัพธ์ของ `qa.json` รันซ้ำได้ไฟล์เดิมทุกไบต์
- **ปัดค่าเป็น float32 ก่อนให้คะแนน:** Python จึงให้คะแนนด้วยโมเดลตัวเดียวกับที่บอร์ดรัน · golden vectors ใน header ยืนยันว่าเลข C ตรงกับ Python ภายใน `TOL_ABS + TOL_REL·|ref|`

**ซ้อมโดยไม่ต้องมีข้อมูลจริง:** `ml.dryrun make` สร้างไฟล์ปลอมที่หน้าตาเหมือนไฟล์จาก serial_logger (มีรีบูต มีแถวหาย) แล้วรันทั้งสายตาม RUNBOOK §6 · ผลจากการซ้อม**ห้ามรายงานเป็นผลโมเดล**

**เทียบวิธี (ML-3b · นอกสาย R0–R6):** `ml.compare` เทียบ Mahalanobis กับ 3σ, PCA, LOF, IsolationForest และ autoencoder ขนาดเล็ก (`MLPRegressor`) บน**ข้อมูลสังเคราะห์**เท่านั้น ด้วย protocol เดียวกับ `baseline.py` (แบ่งตามเวลา · normalizer จาก train · เลือก threshold บน val · test ครั้งเดียว) · ผลเป็นสมมติฐานของข้อมูลสังเคราะห์ ไม่ใช่ค่าวัด

---

## 10. แผนที่ไฟล์ `ml/` ทีละไฟล์

| ไฟล์ | ขั้น | หน้าที่ | ฟังก์ชันหลัก |
|---|---|---|---|
| `load.py` | ML-1 | อ่าน CSV §7 และไฟล์ events · header ต้องตรงทุกตัว · ค่าว่าง = NaN · timestamp ซ้ำ = error | `load_raw` · `load_events` · `RAW_COLUMNS` |
| `split.py` | ML-1 | แบ่งตามเวลา + normalizer ที่ fit จาก train ที่ปกติเท่านั้น | `chronological_split` · `normal_only` · `fit_normalizer` · `apply_normalizer` · `save_normalizer` |
| `synth.py` | ML-1 | ข้อมูลสังเคราะห์ที่ฝัง anomaly 3 แบบ (spike · drift · decorrelation) — ไม่ใช่หลักฐานความแม่นของเซนเซอร์ | `generate` · `write_csv` |
| `metrics.py` · `METRICS.md` | ML-2 | นิยามการวัดผล `ml2-v1` (§9) · เขียน `metrics.json` + `points.csv` ได้เฉพาะใต้ `data/processed/` | `evaluate` · `save_result` |
| `metrics_example.py` | ML-2 | ตัวอย่างคิดด้วยมือ ใช้อธิบายนิยามในรายงาน | `python -m ml.metrics_example` |
| `baseline.py` · `BASELINE.md` | ML-3 / R3–R4 | 3σ (max\|z\|) เทียบกับ Mahalanobis · ตัดแถวเหตุการณ์ออกจาก fit · เลือก threshold/K บน val · export header ภาษา C + golden | `fit` · `score_d2` · `score_zmax` · `persist` · `drop_event_rows` · `check_event_boundaries` · `select_operating_point` · `run` · `export` · `c_header` |
| `compare.py` | ML-3b | เทียบวิธี anomaly detection บนข้อมูลสังเคราะห์ (§9) | `compare_one` · `build_methods` · `python -m ml.compare --seeds … --feature-set … --out …` |
| `qa.py` | R0 | ตรวจคุณภาพไฟล์ raw · ไม่แก้ ไม่ลบแถว · เกณฑ์อยู่ใน `QAConfig` | `analyze` · `run` |
| `events.py` | R1 | ร่างไฟล์ events จาก EV_* และตรวจกับ raw, t1/t2, คำศัพท์ 4 คำ | `draft` · `check` |
| `dryrun.py` | R2 | ข้อมูลซ้อม · เติม TODO อัตโนมัติ · คอมไพล์ header ด้วย gcc แล้วเทียบ golden | `make` · `fill` · `check_header` |
| `fetch_dustboy.py` | — | ดึงข้อมูล DustBoy (สถานีกลางแจ้ง) ต้องมี API key (`DUSTBOY_API_KEY`) · ไม่ได้อยู่ในสาย R0–R6 | — |
| `tests/test_*.py` | — | pytest ของทุกโมดูลข้างบน | `python -m pytest ml -q` (ต้องลง `ml/requirements.txt` ก่อน) |

---

## 11. "อยากแก้ X ต้องเปิดไฟล์ไหน"

| อยากรู้/แก้ | ไฟล์ |
|---|---|
| เปลี่ยนขา GPIO | `firmware/main/board_pins.h` (และต้องตรงกับ CLAUDE.md §4 ห้ามใช้ขาต้องห้าม) |
| priority/stack ของ task | `firmware/main/task_config.h` · io อยู่ที่ต้น `tasks_io.c` |
| คาบ 1 Hz · jitter · กติกาถือค่า PM/VOC/CO 2.5 s | `tasks_sampler.c` (ต้นไฟล์) · `PMS_MAX_AGE_MS` อยู่ใน `logutil.h` |
| รูปแบบแถว CSV | `components/storage/csv_row.c` + `ml/load.py` + `serial_logger.py` (🔴 สัญญา §7) |
| เปิด/ปิด microSD · serial mirror · AGML | `firmware/main/tasks_storage.c` (`STORAGE_SD_ENABLE` · `STORAGE_SERIAL_MIRROR` · `STORAGE_SERIAL_ML`) |
| ทำไมแถวไม่ถูกเขียน | log `no real time yet` (ยังไม่มี SNTP) · สรุป 60 s ของ sampler/storage |
| เกณฑ์ CO | `components/ze07/include/co_safety.h` (ที่มาอยู่ใน `docs/03-CO_SAFETY.md`) |
| ไฟ/เสียงของ CO | `components/ze07/include/co_alert.h` |
| ไฟ/เสียงของ ML/FAULT · demo | `components/fsm/include/ag_fsm.h` |
| เปลี่ยนโมเดลบนบอร์ด | ทับ `components/mahal/model/ag_mahal_pm_th.h` ด้วย header จาก R4 → `run_tests.ps1` → `idf.py build` |
| ชิป/การหมุน/ที่อยู่ของจอ | ต้นไฟล์ `firmware/main/tasks_oled.c` (`OLED_CHIP` · `OLED_ROTATE_180` · `OLED_I2C_ADDR`) |
| ข้อความ/เลย์เอาต์บนจอ | `components/oled_ui/oled_ui.c` (`oled_ui_format` · `oled_ui_boot_lines`) |
| หน้าเว็บ dashboard | `firmware/main/web/index.html` (ฝังใน firmware ต้อง build + flash ใหม่) |
| รูปแบบ JSON ของ dashboard | `components/webui/web_json.c` (`WEB_JSON_VERSION`) |
| endpoint · จำนวน socket · task ของ dashboard | `firmware/main/tasks_web.c` · `WEB_MAX_SOCKETS` ใน `task_config.h` |
| ชุดฟีเจอร์ · transform | `ml/baseline.py` `FEATURE_SETS` · `transform_name` |
| งบเตือนผิด · ตัวเลือก K | `ml/baseline.py` `FA_BUDGET_PER_DAY` · `PERSISTENCE_GRID` |
| เกณฑ์ QA | `ml/qa.py` `QAConfig` |
| คำศัพท์เหตุการณ์ | `ml/events.py` `DEFAULT_ALLOWED` (🔴 ล็อกตามโปรโตคอล §2 จนจบ R4) |

---

## 12. เอกสารเก่าที่ไม่ตรงกับโค้ดแล้ว

เอกสารเหล่านี้ไม่ตรงกับโค้ดใน `main` @ `1ed5ac2` ถ้าเนื้อหาขัดกัน **ให้ยึดเอกสารนี้หรือโค้ด** (ตรวจ 4 พ.ย. 2569 · ยังไม่ได้แก้เอกสารต้นทาง)

| ที่ | สิ่งที่ล้าสมัย |
|---|---|
| `CLAUDE.md` §10 บล็อก 4 พ.ย. | บอกว่า "OLED และการปิด SD ยังไม่ merge เข้า `main`" — โค้ดทั้งสองอยู่ใน `main` แล้ว (`components/oled_ui/` · `STORAGE_SD_ENABLE 0`) |
| `README.md` ตารางสถานะ แถว OLED | "ยังอยู่เฉพาะ branch demo ยังไม่ merge เข้า `main`" — อยู่ใน `main` แล้ว |
| `README.md` ตาราง "ฮาร์ดแวร์ย่อ" | ยังมี SCD40 (ไม่ใช้แล้ว ตั้งแต่ 9 ต.ค. · ไม่มีโค้ด) และไม่มีจอ OLED |
| `firmware/README.md` หัวข้อโครงสร้าง | ยังอ้าง `main` @ `7e926b4` และติดป้าย [fw67] "ยังไม่ merge" ให้ `ml_mailbox` · `io_compose.h` · `mahal/` · `fsm/` · ไม่มี `tasks_oled` · `tasks_web` · `web_mailbox` · `oled_ui/` · `webui/` |
| `ml/README.md` หัวข้อเปลี่ยน header | ยังบอกว่า header เป็นข้อมูลสังเคราะห์ (`ml3-v1+pm_th+b07dd6863436`) — ตัวจริงคือ `ml3-v1+pm_th+e6f29d2be5b3` (`data=real`) |
| `docs/03-CO_SAFETY.md` §5 ตาราง priority | คอมเมนต์ใน `task_config.h` ระบุว่าเป็นผังเก่า ไม่ได้ใช้ |
| `docs/diagrams.html` §4 FSM | ยังไม่ได้ตรวจเทียบกับ `ag_fsm.h` `[ต้องตรวจ]` |

---

อุปกรณ์นี้เป็นเครื่องเฝ้าระวังสภาพแวดล้อมเพื่อการวิจัย ไม่ใช่เครื่องมือแพทย์
และไม่ใช่เครื่องเตือนภัยคาร์บอนมอนอกไซด์ ห้ามใช้แทน CO alarm ที่ได้มาตรฐาน UL 2034 หรือ EN 50291
