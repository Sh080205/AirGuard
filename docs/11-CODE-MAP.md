# 11 · แผนที่โค้ดและคำอธิบายโค้ด — firmware + ml

> **อ้างอิงโค้ด:** `main` @ `7e926b4` และ branch `fw/fw67-integrate` @ `87b8137` (31 ต.ค. · **ยังไม่ merge**) · เขียน 1 พ.ย. 2569
> ส่วนที่มีเฉพาะใน `fw/fw67-integrate` ติดป้าย **[fw67]** · ส่วนที่ไม่มีป้ายคือของที่อยู่ใน `main` แล้ว
> เอกสารนี้เล่าว่า**โค้ดทำอะไรและอยู่ตรงไหน** ส่วนตัวเลขที่ยังไม่ได้วัดให้ดูป้ายในคอมเมนต์ของโค้ดต้นทาง
> ถ้าเอกสารนี้ขัดกับโค้ด ให้ยึดโค้ด แล้วแก้เอกสารนี้

**สารบัญ**
1. ภาพรวมทั้งระบบในหน้าเดียว
2. เฟิร์มแวร์: task, ลำดับบูต และช่องทางสื่อสารระหว่าง task
3. หนึ่งวินาทีของ sampler (เส้นทางของหนึ่งแถว CSV)
4. เส้นทาง CO (แยกจาก ML)
5. **[fw67]** ML บนบอร์ด (FW-7) + FSM (FW-6)
6. แผนที่ไฟล์เฟิร์มแวร์ทีละไฟล์
7. host test และเครื่องมือฝั่ง PC
8. ML pipeline ฝั่ง PC (R0–R6)
9. แผนที่ไฟล์ `ml/` ทีละไฟล์
10. "อยากแก้ X ต้องเปิดไฟล์ไหน"
11. เอกสารเก่าที่ไม่ตรงกับโค้ดแล้ว

---

## 1. ภาพรวมทั้งระบบในหน้าเดียว

```
 เซนเซอร์                     ESP32-S3 (firmware/)                         PC (ml/ + firmware/tools/)
 ─────────                    ─────────────────────                        ──────────────────────────
 PMS7003 ─UART1─┐
 AHT21  ─I²C───┤→ [sampler 1 Hz] ─row queue→ [storage] ─USB serial "AGCSV,"→ serial_logger.py
 ENS160 ─I²C───┘        │  ↑                    │                             → data/raw/usb_train/AG_*_usb.csv
                        │  │ peek               └─(SD ถ้าใช้ได้ → AG_*.csv)      → EV_*_usb.csv (MARK)
                        │  │                                                          │
 ZE07-CO ─UART2─→ [co] ─mailbox─┘                                                     ▼
                     │                                                  ml.qa → ml.events → ml.baseline
                     └─mailbox→ [io] → LED เขียว/เหลือง/แดง + buzzer                    │
 ปุ่ม MARK/MODE/ACK ─ISR→ [io] ─mark queue→ [sampler]                                  ▼
                                                         ag_mahal_pm_th.h (ค่าโมเดลที่ freeze แล้ว)
                     [fw67] sampler ─ml_mailbox→ io (FSM)  ◄── copy เข้า firmware/components/mahal/model/
```

แนวคิดหลักของโปรเจกต์ (CLAUDE.md §1) อยู่ใน **3 จุด** ในโค้ด:
- **บันทึกข้อมูลดิบทุกวินาที:** `tasks_sampler.c` → `tasks_storage.c` → `serial_logger.py`
- **ตัดสินว่าผิดปกติด้วย Mahalanobis D² (หลายตัวแปรพร้อมกัน) ไม่ใช่เกณฑ์ทีละตัว:** ฝั่ง PC คือ `ml/baseline.py` · บนบอร์ดคือ `components/mahal/` **[fw67]**
- **เส้นทาง CO ตัดสินด้วยเกณฑ์ ppm ตายตัว แยกจาก ML:** `components/ze07/co_safety.c` + `tasks_co.c`

---

## 2. เฟิร์มแวร์: task, ลำดับบูต และช่องทางสื่อสารระหว่าง task

### 2.1 ตาราง task

ค่า priority และ stack อยู่ที่ [task_config.h](../firmware/main/task_config.h) ซึ่งเขียนเหตุผลไว้ทุกค่า และ [tasks_io.c](../firmware/main/tasks_io.c) (สำหรับ io)

| task | ไฟล์ | prio | core | stack | หน้าที่ | ห้ามทำอะไร |
|---|---|---|---|---|---|---|
| `co` | `tasks_co.c` | **11** | 1 | 3072 | อ่าน ZE07 ผ่าน UART2 · ตัดสิน CO ALARM/FAULT · publish snapshot | ห้ามรอ task อื่น · ไม่ใช้เวลาจริง/SD/Wi-Fi/โมเดล |
| `sampler` | `tasks_sampler.c` | **10** | 1 | 5120 | รอบละ 1 s: อ่าน PMS/AHT21/ENS160 + peek ค่า CO → สร้างแถว → ส่งเข้าคิว · **[fw67]** คำนวณ D² | ห้าม block (ส่งคิวด้วย timeout 0) |
| `io` | `tasks_io.c` | 5 | ไม่ pin | 4096 | debounce ปุ่ม · ขับ LED/buzzer (เป็น**เจ้าของ GPIO ขาออกแต่ผู้เดียว**) | — |
| `storage` | `tasks_storage.c` | 3 | ไม่ pin | 6144 | เขียน SD · พิมพ์ `AGCSV,`/`AGEV,` ออก serial | เป็นงานช้า จึงให้ priority ต่ำกว่า sampler |
| `console` | `console.c` | 2 | ไม่ pin | 3072 | คำสั่ง `settime` ทาง UART0 | ⚠️ บนบอร์ดจริงพอร์ต UART ใช้ไม่ได้ (CLAUDE.md §10) |
| (ระบบ) Wi-Fi/SNTP | `time_sync.c` | ของ IDF | 0 | — | ตั้งเวลาจาก SNTP · ต่อ Wi-Fi ใหม่ทุก 10 s | — |

**ทำไม co > sampler:** CO เป็นเส้นทางความปลอดภัย latency จากเฟรมถึงการตัดสินจึงต้องไม่ขึ้นกับงานของ sampler · งานต่อเฟรมของ CO สั้นมาก (parse 9 ไบต์ + เทียบเกณฑ์) จึงแทบไม่กินเวลาของ sampler
**ทำไม pin ไว้ core 1:** Wi-Fi และ lwIP มี priority สูงกว่าและอยู่ core 0 · pin ไว้ core 1 เพื่อไม่ให้ถูกแทรก (เป็นสมมติฐาน ยืนยันด้วย jitter ที่วัดจาก GPIO4)

### 2.2 ลำดับบูต — [main.c](../firmware/main/main.c) `app_main()`

```
1. log reset reason · ขนาด flash · ขนาด PSRAM
2. สร้าง mark_queue (ความยาว 8)
3. co_mailbox_init()          ← ต้องมาก่อน io เพราะ io อ่าน mailbox นี้
   [fw67] ml_mailbox_init()   ← ก่อน io ด้วยเหตุผลเดียวกัน
4. io_init(mark_queue)        → ตั้ง GPIO, ISR ปุ่ม, สร้าง io task
5. co_start()                 → UART2 + co task       🔴 CO เริ่มทำงานตั้งแต่ตรงนี้
6. nvs_init()                 → Wi-Fi ต้องใช้ NVS
7. time_sync_start()          → Wi-Fi STA + SNTP (ถ้ามี secrets.h) · ไม่ abort แม้ Wi-Fi พัง
8. console_start()
9. storage_start()            → SPI + คิว + storage task (mount การ์ดทำใน task)
10. sampler_start()           → I²C + UART1 + sampler task
```

**ทำไมเรียงแบบนี้:** CO ต้องเริ่มก่อนทุกอย่างที่อาจพังได้ (NVS, Wi-Fi, SD, เซนเซอร์อื่น) · ข้อ 7–10 ถ้า error จะ**แค่ log ไม่ abort** เพราะถ้า abort บอร์ดจะรีบูตวน และเส้นทาง CO จะดับไปด้วย (คอมเมนต์ FW-5b ใน `main.c`)

### 2.3 ช่องทางสื่อสารระหว่าง task

กฎ CLAUDE.md §2 ห้ามแชร์ตัวแปรด้วย `volatile` อย่างเดียว ทุกช่องทางจึงเป็น **FreeRTOS queue** ซึ่งก๊อปข้อมูลทั้งก้อนภายใต้ critical section ของตัว queue ผู้อ่านจึงไม่มีทางเห็นข้อมูลครึ่งเก่าครึ่งใหม่

| ช่องทาง | ชนิด | ผู้เขียน → ผู้อ่าน | ข้อมูล | ไฟล์ |
|---|---|---|---|---|
| `s_event_queue` | queue 16 ช่อง (`int`) | ISR ปุ่ม + `io_set_*()` → io | เลข GPIO (ค่าบวก) หรือคำสั่ง (ค่าลบ `COMMAND_*`) | `tasks_io.c` |
| `mark_queue` | queue 8 ช่อง | io → sampler | `io_mark_event_t` (กด MARK ที่ยืนยันแล้ว) | สร้างใน `main.c` |
| `s_row_q` | queue แบบ static 64 ช่อง | sampler → storage | `csv_sample_t` · **[fw67]** `storage_row_t` = แถว + ผล ML | `tasks_storage.c` |
| `s_ev_q` | queue แบบ static 4 ช่อง | sampler → storage | `storage_event_t` (เวลาเริ่ม/จบ MARK) | `tasks_storage.c` |
| CO mailbox | queue ยาว 1 · `xQueueOverwrite`/`xQueuePeek` | co → io, sampler | `co_status_t` | `tasks_co.c` |
| **[fw67]** `ml_mailbox` | queue ยาว 1 · overwrite/peek | sampler → io | `ag_ml_result_t` | `ml_mailbox.c` |
| **[fw67]** sensor health | mailbox (overwrite) | sampler → io | `io_sensor_health_t` (เวลาที่อ่านสำเร็จครั้งล่าสุดของแต่ละเซนเซอร์) | `tasks_io.c` |

**"mailbox" คืออะไร:** queue ยาว 1 ช่องที่ผู้เขียนใช้ `xQueueOverwrite` (เขียนทับ ไม่มีวันรอ) และผู้อ่านใช้ `xQueuePeek` (อ่านโดยไม่เอาออก ไม่มีวันรอ) ใช้กับข้อมูลที่เป็น**สถานะ** ซึ่งสนใจแค่ค่าล่าสุด เช่นค่า CO ตอนนี้ · ส่วนข้อมูลที่เป็น**เหตุการณ์**และห้ามหาย เช่นแถว CSV และการกด MARK ใช้ queue ปกติ

**ISR ปุ่ม** (`button_isr` ใน `tasks_io.c`) เป็น `IRAM_ATTR` ทำแค่ `xQueueSendFromISR(เลขขา)` · ถ้าคิวเต็มจะทิ้ง edge นั้นไปเลย ไม่รอและไม่ log · ส่วน debounce 50 ms ทำใน io task

---

## 3. หนึ่งวินาทีของ sampler (เส้นทางของหนึ่งแถว CSV)

`sampler_task()` ใน [tasks_sampler.c](../firmware/main/tasks_sampler.c) วนรอบละ 1000 ms ด้วย `xTaskDelayUntil`

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
[fw67] publish_health()   เวลาที่อ่านแต่ละเซนเซอร์สำเร็จ → io (ใช้ตัดสิน SYS FAULT)
[fw67] ml_step()          D² + นับ persistence → ml_mailbox
ถ้า time_valid:  record_row()  ปัดเวลาเป็นวินาที · ตรวจวินาทีซ้ำ/ข้าม/ถอยหลัง · ใส่ label · ส่งเข้า s_row_q (timeout 0)
ไม่งั้น:         ไม่เขียน CSV (D1) · เตือนทุก 10 s
update_indication()  ไฟเขียวบอกสถานะ logger: ยังไม่มีเวลา / กำลังบันทึก / marked
summary()            ทุก 60 s: jitter · แถวที่หาย · ตัวนับ error · stack HWM · heap
GPIO4 = 0
xTaskDelayUntil(1000 ms)   (หรือ re-align ให้ตื่นหลังขอบวินาที 5 ms ถ้าเฟสเลื่อนเกิน 200 ms)
```

**จุดที่มักสงสัย**
- **ทำไมไม่แยก task ต่อเซนเซอร์:** งานต่อรอบเป็นแค่ poll ที่ไม่มีการรอ (ENS160 ใช้ I²C ราว 1.85 ms ต่อรอบ วัดเมื่อ 16 ต.ค.) แยก task จะเพิ่มทั้ง stack และ latency โดยไม่ได้อะไรกลับมา (คอมเมนต์ FW-5a)
- **ทำไม AHT21 ไม่รอ 80 ms:** trigger ในรอบหนึ่งแล้วอ่านในรอบถัดไป ห่างกัน 1 s ซึ่งเกิน 80 ms ไปมาก จึงไม่ต้องรอแบบ busy-wait ใน loop
- **ทำไมเวลาของแถวคือ "เวลาระบบปัดเป็นวินาที":** loop ล็อกเฟสให้ตื่นหลังขอบวินาทีเล็กน้อย ทำให้การปัดอยู่ห่างจุด .5 · jitter ระดับ ms จึงไม่ทำให้เกิดวินาทีซ้ำหรือข้าม (`ALIGN_*` ต้นไฟล์)
- **ค่าว่างไม่ใช่ 0:** `csv_sample_t` มีธง `has_*` ของแต่ละกลุ่มค่า · `csv_format_row()` เขียนช่องว่างเมื่อธงเป็น false (CLAUDE.md §7)
- **MARK เก็บสถานะใน sampler ไม่ใช่ io:** เพราะ sampler เท่านั้นที่รู้ว่าการกดครั้งนั้นตรงกับแถวไหน · EV row ใช้เวลาของแถว marked แรกและแถว marked **สุดท้าย** (`mark_last`) ส่วน `ml.events draft` บวก 1 s ให้เป็นช่วงครึ่งเปิด

**storage task** ([tasks_storage.c](../firmware/main/tasks_storage.c)):
1. รับแถวจากคิว แล้ว `format_row()` หนึ่งครั้ง
2. พิมพ์ `AGCSV,<row>` ออก serial (serial CSV mirror · `STORAGE_SERIAL_MIRROR=1`)
3. เขียนลง SD ถ้า mount อยู่ (`fsync` ทุก 10 แถว · เปลี่ยนไฟล์ทุกวัน)
4. ถ้า SD หายไป จะพยายาม mount ใหม่ทุก 5 s · ระหว่างนั้นถ้าคิวใกล้เต็ม (`MIRROR_HIGH_WATER` = 60) จะดึงแถวออกไปพิมพ์ทาง serial อย่างเดียว เพื่อไม่ให้ sampler ต้องทิ้งแถว
5. **[fw67]** พิมพ์ `AGML,<timestamp>,<d2>,<status>,<streak>` ต่อท้ายทุกแถว · ไม่อยู่ในสัญญา CSV §7 · `serial_logger.py` เก็บไว้แค่ใน `serial_*.log`

---

## 4. เส้นทาง CO (แยกจาก ML)

```
ZE07 ─UART2 (GPIO5 RX, 9600)─→ tasks_co.c read_uart() → parse_buffer() → ze07_parse()   (components/ze07/ze07_parse.c)
                                                       → co_safety_on_frame()/on_bad_frame()/tick()  (co_safety.c · pure C)
                                                       → publish(): xQueueOverwrite(co_status_t)
io task: co_peek_status() → co_alert_eval() (co_alert.c) → ถ้า co.active จะ "ทับ" output อื่นทั้งหมด
sampler: co_peek_status() → co_status_csv_value() → คอลัมน์ co_ppm
```

| ไฟล์ | หน้าที่ |
|---|---|
| `components/ze07/include/ze07.h` · `ze07_parse.c` | เฟรม 9 ไบต์ตาม manual V1.7 Table 4 (0xFF 0x04 …) · ตรวจ checksum · ค่าเป็นจำนวนเต็มหน่วย 0.1 ppm |
| `components/ze07/include/co_safety.h` · `co_safety.c` | **ตรรกะตัดสิน CO** · ALARM ทันทีเมื่อค่าเดี่ยว ≥ 90 ppm หรือค่าเฉลี่ย 1 ชม. ≥ 30 ppm · ค่าเฉลี่ย 8 ชม. ≥ 9 ppm = ธง "elevated" เท่านั้น ไม่มีไฟ/เสียง · hysteresis −10 % ค้าง 60 s · FAULT เมื่อเงียบ 6 s · อุ่นเครื่อง 5 นาที · ค่าเฉลี่ยใช้ถัง 480 ช่อง ช่องละ 1 นาที |
| `components/ze07/include/co_alert.h` · `co_alert.c` | แปลงสถานะเป็นไฟและเสียง: ALARM = แดง 5 Hz + buzzer · FAULT = แดงกะพริบคู่ทุก 2 s + chirp ทุก 60 s · ACK ตอน ALARM ปิดเสียง 5 นาที **แต่ไม่ล้างสถานะ ALARM** (ไฟแดงยังกะพริบ · มีเงื่อนไข ALARM ใหม่จะดังทันที) · ACK ตอน FAULT ปิด chirp จนกว่า FAULT จะหาย |
| `main/tasks_co.c` | task + UART2 + mailbox + watchdog · log สรุปทุก 60 s · `ZE07_ENABLE=0` = ไม่ build เส้นทาง CO เลย |

**ทำไม FAULT ต้องดังด้วย:** ถ้าเซนเซอร์เงียบไป แล้วเครื่องยังแสดงว่า "ปกติ" ผู้ใช้จะเข้าใจผิด · ผู้อ่าน (io) ยังตรวจ `co_status_stale()` เองอีกชั้น ถ้า CO task หยุด publish ก็จะแสดง FAULT เช่นกัน
**ทำไมค่าเฉลี่ยหารด้วยจำนวนตัวอย่างที่มีจริง:** ช่วงแรกหลังบูต ค่าเฉลี่ยจะได้**สูงกว่าหรือเท่ากับ**การนับช่องที่หายเป็น 0 ppm จึงเตือนเกินได้ แต่ไม่มีทางเตือนขาด
⚠️ ALARM ทดสอบด้วย host test เท่านั้น (`test_co_safety.c`) · `ze07_sim.py` มีไว้ทดสอบบนบอร์ดแต่ยังไม่ได้ทำ (CLAUDE.md §10) · ข้อความ §8: **อุปกรณ์นี้ไม่ใช่เครื่องเตือนภัย CO**

---

## 5. [fw67] ML บนบอร์ด (FW-7) + FSM (FW-6)

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
- **ค่าว่าง:** ถ้าฟีเจอร์ที่เลือกตัวใดว่าง → `MISSING` และ persistence นับใหม่ **ไม่เติมค่าแทน** (no imputation)
- **ช่องว่างเวลา:** ถ้าสองตัวอย่างห่างกันเกิน 1.5 s จะไม่นับว่าติดกัน (`AG_MAHAL_GAP_RESET_US`)
- **self-test ตอนบูต:** รัน golden vectors ที่ฝังมาใน header ด้วยเลขคณิต float32 ของบอร์ด · ถ้าไม่ผ่านจะได้ `AG_ML_MODEL_FAULT` ตลอดจนกว่าจะรีบูต · log `mahal self-test PASS: model=...` บอกว่าบอร์ดรันโมเดลไหนอยู่
- **header โมเดล** `model/ag_mahal_pm_th.h` **สร้างอัตโนมัติจาก `ml.baseline` ห้ามแก้ด้วยมือ** · 🔴 ตัวปัจจุบันเป็นข้อมูลสังเคราะห์ (`ml3-v1+pm_th+b07dd6863436`) ต้องเอา header จาก R5 มาทับก่อน demo (วิธีเปลี่ยนเขียนไว้ใน `model/ag_mahal_model.h`)
- `ml_step()` คำนวณใน sampler เพราะใช้แค่ log1pf หนึ่งครั้งกับ quadratic form 3×3 ซึ่งเป็นเวลาคงที่ ไม่มี I/O · เวลาที่ใช้จริงดูได้จากบรรทัด `ml compute` ใน log 60 s

### 5.2 FSM — `components/fsm/ag_fsm.c` + `main/io_compose.h`

io task ตัดสินว่าจะแสดงอะไร โดย**เลือกบรรทัดแรกที่เป็นจริง**:

| ลำดับ | สถานะ | เงื่อนไข | ไฟ/เสียง |
|---|---|---|---|
| 1 | **CO** | `co_alert` active | รูปแบบของ CO (§4) — 🔴 ไม่มีอะไรบังได้ |
| 2 | ML_ALARM | PERSISTENT ติดกัน ≥ 30 แถว | แดง 5 Hz + beep 3 ครั้งทุก 10 s |
| 3 | SYS_FAULT | เซนเซอร์อ่านไม่สำเร็จนานเกินกำหนด · ML self-test พัง · ผล ML เก่ากว่า 3 s | เหลืองกะพริบคู่ทุก 2 s ไม่มีเสียง |
| 4 | ML_WATCH | PERSISTENT | เหลือง 1 Hz |
| 5 | NORMAL | อื่นๆ | เขียวติดค้าง / สถานะ logger (§2) |

- ลดระดับได้เมื่อ BELOW ติดต่อกัน 30 s และลดทีละขั้น (ALARM → WATCH → NORMAL) ไฟจึงไม่กะพริบไปมาเมื่อ D² แกว่งรอบ threshold
- **ปุ่ม:** ACK ไปที่ CO ก่อน (`io_route_ack`) ถ้า CO ไม่ได้ใช้ ACK ค่อยไปปิดเสียง ML ALARM · กด MODE ค้าง 3 s = demo 60 s ที่วนแสดง NORMAL → WATCH → ALARM → FAULT · **FSM ไม่เขียนคอลัมน์ใดลง CSV** demo จึงไม่ทำให้ข้อมูลเสีย
- `io_compose.h` เป็นฟังก์ชัน pure C ที่รวม output ของ CO กับ FSM เข้าด้วยกัน แยกไว้เพื่อให้ host test (`test_io_compose.c`) ทดสอบด้วยโค้ดชุดเดียวกับที่รันบนบอร์ด
- ⚠️ **ต่างจาก main:** ใน `main` ปุ่ม MODE วน NORMAL → WATCH → ALARM (เป็น demo จาก FW-1) และ ACK บังคับกลับ NORMAL · ใน fw67 ทั้งสองพฤติกรรมนี้ถูกเอาออก และ `io_set_mode(WATCH/ALARM)` คืน `ESP_ERR_NOT_SUPPORTED`
- ⚠️ **build และ host test ผ่าน แต่ยังไม่ได้ทดสอบบนบอร์ด** (ข้อความ commit `87b8137`)

---

## 6. แผนที่ไฟล์เฟิร์มแวร์ทีละไฟล์

```
firmware/
├── CMakeLists.txt · sdkconfig.defaults (flash 16 MB · PSRAM octal) · partitions.csv (factory 4 MB)
├── main/                     ← เฉพาะโปรเจกต์นี้: task + การเดินสาย
│   ├── main.c                app_main: ลำดับบูต (§2.2)
│   ├── board_pins.h          เลข GPIO ทุกขา (ตรงกับ CLAUDE.md §4) · GPIO4 = ขาดีบัก sampler
│   ├── task_config.h         priority / core / stack พร้อมที่มาของแต่ละค่า
│   ├── tasks_sampler.c/.h    รอบ 1 Hz (§3) · เจ้าของ I²C, UART1, สถานะ MARK
│   ├── tasks_storage.c/.h    SD + serial mirror (AGCSV/AGEV · [fw67] AGML)
│   ├── tasks_io.c/.h         ปุ่ม (ISR → queue → debounce) · LED/buzzer · [fw67] FSM + sensor health
│   ├── tasks_co.c/.h         CO task (§4)
│   ├── time_sync.c/.h        TZ ICT-7 · Wi-Fi STA + SNTP pool.ntp.org · time_sync_is_valid()
│   ├── console.c/.h          คำสั่ง settime / help
│   ├── secrets.h.example     ชื่อ/รหัส Wi-Fi → ก๊อปเป็น secrets.h (ไม่ขึ้น git)
│   ├── [fw67] ml_mailbox.c/.h   mailbox ผล ML (§2.3)
│   └── [fw67] io_compose.h      รวม CO + FSM แบบ pure C (§5.2)
├── components/               ← ไดรเวอร์/ตรรกะที่แยก "ส่วน pure C" ออกมาให้ host test ได้
│   ├── pms7003/   pms7003_parse.c (เฟรม 32 ไบต์ · checksum · ใช้ชุด atmospheric) · pms7003_uart.c (UART1 + ring buffer)
│   ├── aht21/     aht21_parse.c (CRC8 0x31 init 0xFF · แปลง 20 บิต → °C/%RH) · aht21.c (I²C 0x38)
│   ├── ens160/    ens160_parse.c (ถอด status/validity · กติกาถือค่า 2.5 s · แปลงค่าชดเชย) · ens160.c (I²C 0x53 · OPMODE · MISR)
│   ├── ze07/      ze07_parse.c · co_safety.c · co_alert.c (§4)
│   ├── storage/   csv_row.c — แปลง csv_sample_t → ข้อความตามสัญญา §7 · ชื่อไฟล์ AG_/EV_ · header
│   ├── logutil/   parse ISO 8601 (settime) · ปัดวินาที · จำแนกวินาทีซ้ำ/ข้าม/ถอยหลัง · กติกาความสดของเฟรม · สถิติ min/max/jitter
│   ├── [fw67] mahal/  ag_mahal.c + model/ag_mahal_pm_th.h (§5.1)
│   └── [fw67] fsm/    ag_fsm.c (§5.2)
├── test_host/                ← unit test ที่คอมไพล์ด้วย gcc บน PC (ไม่ต้องใช้บอร์ด)
└── tools/                    ← สคริปต์ Python ฝั่ง PC (§7)
```

**แบบแผนที่ใช้ทั้ง repo:** ทุกเซนเซอร์แยกเป็น `*_parse.c` (pure C: ไม่มี FreeRTOS, ไม่มี I/O) กับไฟล์ที่คุยกับฮาร์ดแวร์ · ส่วน parse คือส่วนที่ host test ทดสอบด้วยเฟรมจริงจาก datasheet · ถ้าจะเพิ่มเซนเซอร์ใหม่ ให้ทำตามแบบนี้

**สัญญา CSV ในโค้ด:** `csv_sample_t` และ `csv_format_row()` ใน `components/storage/` เป็น**ที่เดียว**ที่กำหนดลำดับคอลัมน์ฝั่งบอร์ด · ฝั่ง PC คือ `RAW_COLUMNS` ใน `ml/load.py` และ `CSV_HEADER` ใน `serial_logger.py` · มี test ที่เทียบสามที่นี้กับ CLAUDE.md §7 · เปลี่ยนที่ใดที่หนึ่ง ต้องตกลงกันทั้งทีมก่อน

---

## 7. host test และเครื่องมือฝั่ง PC

| ไฟล์ | ทดสอบ/ทำอะไร | รันอย่างไร |
|---|---|---|
| `test_host/run_tests.ps1` | คอมไพล์และรัน `test_*.c` ทุกชุดด้วย gcc (`-Wall -Wextra -Werror -pedantic`) | `powershell firmware/test_host/run_tests.ps1` |
| `test_pms7003_parse.c` · `test_pms7003_uart.c` | เฟรม PMS (ใช้ทั้งชุด CF=1 และ atmospheric · CSV ใช้ atmospheric) · การอ่านผ่าน UART | ″ |
| `test_aht21_parse.c` · `test_csv_row.c` · `test_logutil.c` | CRC · การแปลงค่า · รูปแบบแถว · settime · วินาทีซ้ำ/ข้าม | ″ |
| `test_ens160_parse.c` | status/validity/กติกาถือค่า | ″ |
| `test_ze07_parse.c` · `test_co_safety.c` | เฟรม CO · เกณฑ์ ALARM/FAULT/hysteresis/ค่าเฉลี่ย | ″ |
| **[fw67]** `test_mahal.c` · `test_fsm.c` · `test_io_compose.c` | golden D² · persistence · ลำดับความสำคัญของ FSM · CO ต้องชนะเสมอ | ″ |
| `tools/serial_logger.py` | อ่าน COM port → `AG_*_usb.csv` · `EV_*_usb.csv` · `serial_*.log` (ตรวจแต่ละแถวกับสัญญา §7) | `python tools\serial_logger.py --port COM4 --out ..\data\raw\usb_train` |
| `tools/heater_compare.py` | เทียบ T/RH ระหว่างช่วง ENS160 ปิด/เปิด (ผลกระทบจากฮีตเตอร์) | ดู docstring |
| `tools/ze07_sim.py` | ส่งเฟรม CO ปลอมผ่าน USB-UART 3.3 V เพื่อทดสอบ ALARM/FAULT บนบอร์ด | ต้องถอด J2 ก่อน · ดู docstring |
| `tools/test_*.py` | unit test ของสามสคริปต์ข้างบน | `python -m unittest discover -s firmware/tools -p "test_*.py" -v` |

---

## 8. ML pipeline ฝั่ง PC (R0–R6)

แผนเต็มอยู่ใน `docs/10-ML-REAL-DATA-PROMPTS.md` · คำสั่งจริงอยู่ใน [ml/RUNBOOK-REAL.md](../ml/RUNBOOK-REAL.md) · โปรโตคอลเก็บข้อมูลอยู่ใน `data/PROTOCOL-TRAIN-2026-10.md`

```
data/raw/usb_train/AG_*_usb.csv ──┐
                                  ├─ R0  ml.qa       ตรวจช่องว่าง · ค่าค้าง · ค่านอกช่วง · สถิติรายวัน → qa.json/qa.md/png (อ่านอย่างเดียว)
data/raw/usb_train/EV_*_usb.csv ──┤
                                  ├─ R1  ml.events draft → แก้ TODO ด้วยมือ (incense/dusting/cooking/other) → ml.events check → OK
                                  │       → data/events/2026-10_train.csv   (ขึ้น git ได้ · เป็น ground truth)
                                  ├─ R3  ดูผล qa → freeze t1 (ตัดสินเรื่อง U1–U4)
                                  ├─ R4  ml.baseline --raw … --events … --mapping … --t1 --t2 --gap-s 60 --feature-set pm_th
                                  │       ├ load.load_raw / load_events          อ่านตามสัญญา §7
                                  │       ├ split.chronological_split            train < t1 · val · test  (ตามเวลา ห้ามสุ่ม)
                                  │       ├ split.normal_only + fit_normalizer   fit ด้วยแถว label=normal ของ train เท่านั้น
                                  │       ├ baseline.fit                         transform → z → Ledoit–Wolf → ตรวจ condition number → P
                                  │       ├ select_operating_point (val เท่านั้น)  เลือก threshold + K ภายใต้งบเตือนผิด ≤ 1 ครั้ง/วัน
                                  │       ├ frozen_hash                          hash โมเดลก่อนแตะ test
                                  │       └ metrics.evaluate (test ครั้งเดียว)    → model.json · norm.json · results.json · ag_mahal_pm_th.h
                                  ├─ R5  ml.dryrun check-header <header> → PASS → ก๊อปไป firmware/components/mahal/model/
                                  └─ R6  รูปและตารางสำหรับสไลด์
```

**กฎ ML ที่ฝังอยู่ในโค้ดแล้ว (CLAUDE.md §2)**
- **แบ่งตามเวลา:** `chronological_split()` · มีช่วง gap คั่นระหว่างชุดเพื่อไม่ให้หน้าต่างเวลาคร่อมขอบ · เหตุการณ์ที่คร่อมขอบหรือแตะ gap จะ error (`check_event_boundaries`)
- **normalize ด้วยค่าจาก train เท่านั้น:** `fit_normalizer()` บันทึกลง `norm.json` → ฝังลง header → บอร์ดใช้ค่าคงที่ชุดนั้น
- **ไม่ใช้ accuracy และไม่ใช้ Point Adjustment:** `metrics.py` (นิยาม `ml2-v1` ใน `METRICS.md`) วัดแบบ**นับเหตุการณ์จากจุดเริ่ม (onset)**: `event_detection_rate` (detected/event_count) · `mttd_detected_s` · `false_alarm_episodes_per_day` = episode × 86400 / เวลาปกติที่สังเกตได้ · AP (average precision) ระดับจุด · ช่วงที่ map เป็น `unknown` ไม่นับทั้งถูกและผิด
- **threshold เลือกบน val เท่านั้น:** แล้ว hash (`frozen_sha256`) ก่อนให้คะแนน test · 🔴 รัน R4 แล้วห้ามแก้ค่าแล้วรันใหม่ (RUNBOOK §4)
- **ทำซ้ำได้:** ทุกคำสั่งรับ `--seed` · ผลลัพธ์ของ `qa.json` รันซ้ำได้ไฟล์เดิมทุกไบต์
- **ปัดค่าเป็น float32 ก่อนให้คะแนน:** Python จึงให้คะแนนด้วยโมเดลตัวเดียวกับที่บอร์ดรัน · golden vectors ใน header ยืนยันว่าเลข C ตรงกับ Python ภายใน `TOL_ABS + TOL_REL·|ref|`

**ซ้อมโดยไม่ต้องมีข้อมูลจริง:** `ml.dryrun make` สร้างไฟล์ปลอมที่หน้าตาเหมือนไฟล์จาก serial_logger (มีรีบูต มีแถวหาย) แล้วรันทั้งสายตาม RUNBOOK §6 · ผลจากการซ้อม**ห้ามรายงานเป็นผลโมเดล**

---

## 9. แผนที่ไฟล์ `ml/` ทีละไฟล์

| ไฟล์ | ขั้น | หน้าที่ | ฟังก์ชันหลัก |
|---|---|---|---|
| `load.py` | ML-1 | อ่าน CSV §7 และไฟล์ events · header ต้องตรงทุกตัว · ค่าว่าง = NaN · timestamp ซ้ำ = error | `load_raw` · `load_events` · `RAW_COLUMNS` |
| `split.py` | ML-1 | แบ่งตามเวลา + normalizer ที่ fit จาก train ที่ปกติเท่านั้น | `chronological_split` · `normal_only` · `fit_normalizer` · `apply_normalizer` |
| `synth.py` | ML-1 | ข้อมูลสังเคราะห์ที่ฝัง anomaly 3 แบบ (spike · drift · decorrelation) — ไม่ใช่หลักฐานความแม่นของเซนเซอร์ | `generate` · `write_csv` |
| `metrics.py` · `METRICS.md` | ML-2 | นิยามการวัดผล `ml2-v1` (§8) | `evaluate` · `save_result` |
| `metrics_example.py` | ML-2 | ตัวอย่างคิดด้วยมือ ใช้อธิบายนิยามในรายงาน | `python -m ml.metrics_example` |
| `baseline.py` · `BASELINE.md` | ML-3 | 3σ (max\|z\|) เทียบกับ Mahalanobis · เลือก threshold/K บน val · export header ภาษา C + golden | `fit` · `score_d2` · `score_zmax` · `persist` · `select_operating_point` · `run` · `export` · `c_header` |
| `qa.py` | R0 | ตรวจคุณภาพไฟล์ raw · ไม่แก้ ไม่ลบแถว · เกณฑ์อยู่ใน `QAConfig` | `analyze` · `run` |
| `events.py` | R1 | ร่างไฟล์ events จาก EV_* และตรวจกับ raw, t1/t2, คำศัพท์ 4 คำ | `draft` · `check` |
| `dryrun.py` | R2 | ข้อมูลซ้อม · เติม TODO อัตโนมัติ · คอมไพล์ header ด้วย gcc แล้วเทียบ golden | `make` · `fill` · `check_header` |
| `fetch_dustboy.py` | — | ดึงข้อมูล DustBoy (สถานีกลางแจ้ง) ต้องมี API key · ไม่ได้อยู่ในสาย R0–R6 | — |
| `tests/test_*.py` | — | pytest ของทุกโมดูลข้างบน | `python -m pytest ml -q` (ต้องลง `ml/requirements.txt` ก่อน) |

---

## 10. "อยากแก้ X ต้องเปิดไฟล์ไหน"

| อยากรู้/แก้ | ไฟล์ |
|---|---|
| เปลี่ยนขา GPIO | `firmware/main/board_pins.h` (และต้องตรงกับ CLAUDE.md §4 ห้ามใช้ขาต้องห้าม) |
| priority/stack ของ task | `firmware/main/task_config.h` · io อยู่ที่ต้น `tasks_io.c` |
| คาบ 1 Hz · jitter · กติกาถือค่า PM/VOC/CO 2.5 s | `tasks_sampler.c` (ต้นไฟล์) · `PMS_MAX_AGE_MS` อยู่ใน `logutil.h` |
| รูปแบบแถว CSV | `components/storage/csv_row.c` + `ml/load.py` + `serial_logger.py` (🔴 สัญญา §7) |
| ทำไมแถวไม่ถูกเขียน | log `no real time yet` (ยังไม่มี SNTP) · สรุป 60 s ของ sampler/storage |
| เกณฑ์ CO | `components/ze07/include/co_safety.h` (ที่มาอยู่ใน `docs/03-CO_SAFETY.md`) |
| ไฟ/เสียงของ CO | `components/ze07/include/co_alert.h` |
| **[fw67]** ไฟ/เสียงของ ML/FAULT · demo | `components/fsm/include/ag_fsm.h` |
| **[fw67]** เปลี่ยนโมเดลบนบอร์ด | ทับ `components/mahal/model/ag_mahal_pm_th.h` ด้วย header จาก R4 → `run_tests.ps1` → `idf.py build` |
| ชุดฟีเจอร์ · transform | `ml/baseline.py` `FEATURE_SETS` · `LOG1P_COLUMNS` |
| งบเตือนผิด · ตัวเลือก K | `ml/baseline.py` `FA_BUDGET_PER_DAY` · `PERSISTENCE_GRID` |
| เกณฑ์ QA | `ml/qa.py` `QAConfig` |
| คำศัพท์เหตุการณ์ | `ml/events.py` `DEFAULT_ALLOWED` (🔴 ล็อกตามโปรโตคอล §2 จนจบ R4) |

---

## 11. เอกสารเก่าที่ไม่ตรงกับโค้ดแล้ว

เอกสารเหล่านี้เขียนก่อนโค้ดจริง ถ้าเนื้อหาขัดกัน **ให้ยึดเอกสารนี้หรือโค้ด**
(1 พ.ย.: แก้ `firmware/README.md` หัวข้อโครงสร้าง · `README.md` ตารางสถานะ · `ml/README.md` ลำดับงาน/ส่งมอบ ให้ตรงโค้ดและชี้มาที่เอกสารนี้แล้ว)

| ที่ | สิ่งที่ล้าสมัย |
|---|---|
| `README.md` ย่อหน้า "นี่คือเหตุผลทั้งหมดที่ต้องมี autoencoder" | ที่บอร์ดใช้จริงคือ Mahalanobis · autoencoder ยังไม่มีโค้ด |
| `README.md` ตาราง "ฮาร์ดแวร์ย่อ" | ยังมี SCD40 (ไม่ใช้แล้ว ตั้งแต่ 9 ต.ค.) และไม่มีจอ OLED |
| `docs/03-CO_SAFETY.md` §5 ตาราง priority | คอมเมนต์ใน `task_config.h` ระบุว่าเป็นผังเก่า ไม่ได้ใช้ |
| `docs/diagrams.html` §4 FSM | ยังไม่ได้ตรวจเทียบกับ `ag_fsm.h` ของ fw67 `[ต้องตรวจ]` |

---

อุปกรณ์นี้เป็นเครื่องเฝ้าระวังสภาพแวดล้อมเพื่อการวิจัย ไม่ใช่เครื่องมือแพทย์
และไม่ใช่เครื่องเตือนภัยคาร์บอนมอนอกไซด์ ห้ามใช้แทน CO alarm ที่ได้มาตรฐาน UL 2034 หรือ EN 50291
