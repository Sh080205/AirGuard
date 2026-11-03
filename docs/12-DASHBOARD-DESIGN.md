# 12 · FW-8 Dashboard บนตัวเครื่อง — แผนออกแบบ (ขั้น 1 · รอรีวิว)

> **สถานะ:** ร่างออกแบบ 3 พ.ย. 2569 · **ยังไม่มีโค้ด** · ต่อยอดจาก branch `fw/fw7-real-model` (= `fw/fw67-integrate` + โมเดลจริง) ซึ่ง**ยังไม่เคย build ด้วย ESP-IDF และยังไม่ได้ทดสอบบนบอร์ด**
> ทุกตัวเลขเวลา/หน่วยความจำ/กระแสในไฟล์นี้คือ **ค่าคาดหมาย ต้องยืนยันด้วยการวัด**
> ข้อเท็จจริงของ `esp_http_server` อ้างจากเอกสาร ESP-IDF v5.5 (esp32s3) หน้า *HTTP Server* `[ขึ้นกับเวอร์ชัน ESP-IDF]`

---

## 1. เป้าหมาย — ขั้นต่ำเท่านั้น

ESP32-S3 เปิดหน้าเว็บเอง (ไม่ใช้ server ภายนอก) บนเครือข่าย hotspot เดิม แล้วส่งค่าขึ้นหน้าเว็บทุก 1 วินาทีผ่าน WebSocket

**อยู่ในขอบเขต**

| ส่วน | รายละเอียด |
|---|---|
| ค่าล่าสุด | ทุกคอลัมน์ §7 (pm1_0 · pm2_5 · pm10 · temp_c · humidity_pct · tvoc_ppb · eco2_ppm · co_ppm) + `marked` · ค่าว่างแสดง "—" ไม่ใช่ 0 |
| สถานะเครื่อง | สิ่งที่ LED แสดงอยู่: `NORMAL` / `ML WATCH` / `ML ALARM` / `SYS FAULT` / `CO` (+ demo) และสาเหตุของ SYS FAULT |
| ML | D² ล่าสุด เทียบ threshold · status (`below` / `above` / `persistent` / `missing` / `model_fault`) · streak · ชื่อโมเดล |
| CO | ค่า ppm · ค่าเฉลี่ย 1 ชม. · ALARM / FAULT / อุ่นเครื่อง (สำเนาจาก CO mailbox — **ไม่ตัดสินอะไรใหม่**) |
| กราฟ | pm2_5 · temp_c · humidity_pct · D² ย้อนหลัง **10 นาที** |
| แจ้งเตือนบนหน้า | แบนเนอร์เปลี่ยนสี + **จังหวะกะพริบเดียวกับ LED** (§5 CLAUDE.md · คนตาบอดสีใช้ได้) + เสียงจากเบราว์เซอร์ (ต้องแตะปุ่ม "เปิดเสียง" ก่อน เพราะเบราว์เซอร์บล็อกเสียงอัตโนมัติ) |
| การเชื่อมต่อขาด | ไม่ได้รับข้อความ > 3 s → แบนเนอร์ "ขาดการเชื่อมต่อ" แทนสถานะ (หน้าเว็บที่ค้างค่าเก่าต้องไม่ดูเหมือน NORMAL — หลักเดียวกับ CO FAULT) |
| ข้อความบังคับ | ข้อความ §8 แสดงถาวรบนหน้า |

**นอกขอบเขต (ทำทีหลังถ้ามีเวลา)**
- แจ้งเตือนตอนปิดหน้าเว็บ (Telegram bot / ntfy) — ต้องใช้อินเทอร์เน็ตและบริการภายนอก
- Notification API ของเบราว์เซอร์ — ต้องเป็น secure context (HTTPS หรือ localhost) ใช้กับ `http://192.168.x.x` ไม่ได้
- mDNS `airguard.local` (ใน v5.x เป็น managed component แยก · Android ไม่รองรับทุกเครื่อง) · โหมด AP+STA · ล็อกอิน · ตั้งค่าผ่านเว็บ · ดาวน์โหลด CSV · จอ OLED

---

## 2. โครงสร้าง

```
sampler (prio 10 · 1 Hz) ──overwrite──► row mailbox   [ใหม่]  ─┐
co      (prio 11)        ──overwrite──► CO mailbox    [มีแล้ว] ─┤ peek ทุก 1 s
sampler                  ──overwrite──► ml_mailbox    [มีแล้ว fw67]┼──► web task (prio 2) ── สร้าง JSON ──► ring 10 นาที
io      (prio 5 · FSM)   ──overwrite──► show mailbox  [ใหม่]  ─┘            │                  ▲ mutex
                                                                         httpd_queue_work      │
                                                                               ▼               │
                                                     httpd task (prio 2) ── WS frame ทุก client · GET / · GET /history.json
```

### 2.1 ทำไมเลือกแบบนี้

- **ไม่มีงาน Wi-Fi/socket ใน sampler, co หรือ io** (CLAUDE.md §2) — สามตัวนั้นแค่ `xQueueOverwrite` ลง mailbox ความยาว 1 ซึ่ง**ไม่มีวันรอ** แบบเดียวกับ CO mailbox และ `ml_mailbox` ที่มีอยู่แล้ว
- **mailbox ไม่ใช่ queue ปกติ** เพราะหน้าเว็บสนใจแค่ "สถานะล่าสุด" · ถ้า web task ช้า ค่าเก่าถูกเขียนทับ ไม่มีคิวล้นไปกระทบผู้เขียน
- **ไม่ใช้ `volatile`/ตัวแปร global ร่วมกัน** — queue ก๊อปทั้ง struct ภายใต้ critical section ของตัวเอง ผู้อ่านไม่เห็นข้อมูลครึ่งรอบ (เหตุผลเดียวกับ `ml_mailbox.h`)
- **web task แยกจาก httpd task:** httpd task ทำงานเมื่อมี request · การ push ทุก 1 s ต้องมีตัวตั้งจังหวะ · ส่งผ่าน `httpd_queue_work` เพราะเอกสารเขียนว่า `httpd_ws_send_frame_async` "should rarely be called directly, with an exception of asynchronous send using httpd_queue_work" → การส่งจึงเกิดใน httpd task เสมอ
- **CO path ไม่เปลี่ยนแม้แต่บรรทัดเดียว** · หน้าเว็บอ่านสำเนา snapshot เท่านั้น · web/httpd พัง → LED/buzzer ของ CO ยังทำงาน

### 2.2 Priority (CLAUDE.md §2: ทุก task ต้องมีเหตุผล)

| task | prio | เหตุผล |
|---|---|---|
| co | 11 | เดิม |
| sampler | 10 | เดิม |
| io | 5 | เดิม |
| storage | 3 | เดิม · **การบันทึกข้อมูลสำคัญกว่าหน้าเว็บ** |
| **web** | **2** | ต่ำกว่า storage: dashboard ช้าหรือหยุดได้ แต่แถว CSV ห้ามหาย · เท่ากับ console (งานของคน ไม่เร่งด่วน) |
| **httpd** | **2** | ตั้งผ่าน `httpd_config_t.task_priority` เหตุผลเดียวกัน |
| Wi-Fi / lwIP / sys_evt | 18–23 | ของ IDF (task_config.h) — **ความเสี่ยงจริงอยู่ตรงนี้**: traffic มากขึ้น = lwIP ทำงานมากขึ้น แต่ pin อยู่ core 0 ส่วน sampler/co อยู่ core 1 → ต้องวัด (§6) |

web และ httpd **ไม่ pin core** (เหมือน io/storage) · stack เริ่มที่ 4096 B ทั้งคู่แล้ววัด HWM (ร่าง: JSON buffer 768 + snprintf/float ~2 KB + log ~2 KB) — ค่า default ของ `HTTPD_DEFAULT_CONFIG()` ไม่มีในหน้าเอกสาร `[ต้องเปิด esp_http_server.h ของ v5.5 ยืนยัน]` จึงตั้งค่าเองทุกตัวที่ใช้

### 2.3 Mailbox ใหม่ 2 ตัว

| mailbox | ผู้เขียน | เนื้อหา | ทำไมผู้เขียนคนนี้ |
|---|---|---|---|
| `row_mailbox` | sampler · ทุกรอบ หลัง `ml_step` | `csv_sample_t` + `time_valid` + `uptime_us` | sampler เป็นเจ้าของแถว · เขียนทุกรอบแม้ยังไม่มีเวลาจริง (หน้าเว็บแสดงค่าได้ แต่ timestamp = null และขึ้นป้าย "ยังไม่มีเวลาจริง — ยังไม่บันทึก CSV") |
| `show_mailbox` | io · หลัง `ag_fsm_step` ทุกครั้ง | `shown` · `demo` · `ml_level` · `sys_fault` · `fault_reasons` · `muted` | io เป็นเจ้าของ FSM คนเดียว (ag_fsm.h) · หน้าเว็บต้องแสดง**สิ่งเดียวกับ LED** จึงต้องเอาผลจาก FSM ไม่ใช่คำนวณใหม่ |

### 2.4 ประวัติ 10 นาที

- ring buffer 600 ช่อง × ~20 B (pm2_5 · temp · rh · d2 · shown · เวลา) ≈ 12 KB ใน RAM ภายใน
- **ผู้เขียน:** web task (1 ช่อง/วินาที) · **ผู้อ่าน:** httpd task (`GET /history.json` ตอนเปิดหน้าครั้งแรก)
- ป้องกันด้วย **FreeRTOS mutex** ไม่ใช่ critical section: ต้องก๊อป/แปลงเป็น JSON หลายร้อยแถว · critical section ปิด interrupt และห้ามเรียก printf ระหว่างนั้น · ทั้งสองฝั่งเป็น task ธรรมดา (ไม่มี ISR) mutex จึงพอและมี priority inheritance
- เปิดหน้าครั้งแรก: โหลด `/history.json` แล้วต่อ WebSocket · หลังจากนั้นกราฟเติมจาก WS อย่างเดียว

---

## 3. รูปแบบข้อมูล (WebSocket · 1 ข้อความ/วินาที · JSON)

**ไม่ใช่สัญญา CSV §7** และไม่ไปแก้ CSV · ค่าว่าง = `null` (หลักเดียวกับ "ว่าง ไม่ใช่ 0")

```json
{"v":1,"seq":1234,"up_s":5021,
 "t":"2026-11-03T15:04:05+07:00",
 "pm1_0":8,"pm2_5":12,"pm10":15,"temp_c":33.8,"humidity_pct":57.0,
 "tvoc_ppb":145,"eco2_ppm":520,"co_ppm":0.5,"marked":false,
 "shown":"NORMAL","demo":false,"sys_fault":[],"muted":false,
 "ml":{"status":"below","d2":3.21,"thr":17.92,"streak":0,"model":"ml3-v1+pm_th+e6f29d2be5b3"},
 "co":{"alarm":false,"fault":false,"warmup":false,"mean_1h":0.5,"stale":false}}
```

- `temp_c` / `humidity_pct` / `co_ppm` ปัดด้วย `csv_round_tenths` **ตัวเดียวกับ CSV** → ค่าบนหน้าเว็บตรงกับไฟล์ที่บันทึก
- `t` = `null` เมื่อยังไม่มีเวลาจริง · `seq` เพิ่มทีละ 1 → หน้าเว็บรู้ว่าข้อความหาย
- `v` = เวอร์ชันรูปแบบ เผื่อแก้ภายหลัง
- ขนาด ≈ 450 B · 1 client ≈ 0.5 KB/s (ค่าคาดหมาย)

---

## 4. ไฟล์ที่จะเพิ่ม/แก้ (ขั้น 2–3)

| ไฟล์ | ขั้น | หน้าที่ |
|---|---|---|
| `components/webui/web_json.c/.h` | 2 | **pure C** สร้าง JSON จาก struct (escape · null · ปัดทศนิยมแบบ CSV) · ring buffer · ไม่มี FreeRTOS → host test ได้ |
| `test_host/test_web_json.c` + `run_tests.ps1` | 2 | ค่าว่าง → null · ปัดตรงกับ `csv_format_row` · buffer เล็กไป → คืน 0 ไม่ตัดกลาง · ring วนรอบถูก |
| `main/web_mailbox.c/.h` | 3 | row/show mailbox (แบบ `ml_mailbox.c`) |
| `main/tasks_web.c/.h` | 3 | web task + `httpd_start` + handler `/`, `/history.json`, `/ws` (`is_websocket = true`) |
| `main/web/index.html` | 3 | หน้าเดียว HTML+JS+CSS **ไม่โหลดอะไรจากอินเทอร์เน็ต** (กราฟวาดด้วย `<canvas>` เอง ≈ 100 บรรทัด) · ฝังด้วย `EMBED_TXTFILES` · เป้า < 20 KB |
| `main/tasks_sampler.c` | 3 | + `row_mailbox_publish()` 1 บรรทัดต่อรอบ |
| `main/tasks_io.c` | 3 | + `show_mailbox_publish()` หลัง `ag_fsm_step` |
| `main/main.c` | 3 | สร้าง mailbox ก่อน `io_init()` · `web_start()` **หลัง** sampler (ลำดับเดิมของ CO ไม่เปลี่ยน) · error = log อย่างเดียว ไม่ abort |
| `main/task_config.h` | 3 | priority/stack ของ web + httpd พร้อมเหตุผล |
| `main/CMakeLists.txt` | 3 | `esp_http_server` ใน REQUIRES · `EMBED_TXTFILES` · 🔴 หยุดตอน configure ถ้า `CONFIG_HTTPD_WS_SUPPORT` ไม่ได้เปิด (แบบเดียวกับเช็ค partition ของ FW-5a) |
| `sdkconfig.defaults` | 3 | `CONFIG_HTTPD_WS_SUPPORT=y` — ⚠️ defaults ใช้กับ sdkconfig **ใหม่** เท่านั้น เครื่องที่มี `firmware/sdkconfig` อยู่แล้วต้องลบแล้ว build ใหม่ (หรือเปิดใน menuconfig) |
| `docs/11-CODE-MAP.md` · `firmware/README.md` | 3 | เพิ่ม task/mailbox ใหม่ |

**การเข้าหน้าเว็บ:** IP ถูก log อยู่แล้ว (`Wi-Fi got IP …` ใน `time_sync.c`) · เพิ่ม `web: http://<ip>/` ในสรุป 60 s ของ web task เพื่อหาย้อนหลังได้ · บนมือถือที่เปิด hotspot ดู IP ของอุปกรณ์ที่ต่ออยู่ได้ด้วย

---

## 5. ความปลอดภัยและข้อห้ามที่ยังคงอยู่

- 🔴 เส้นทาง CO ตัดสินด้วย LED/buzzer ในเครื่องเหมือนเดิม · หน้าเว็บ**ไม่ใช่**ช่องทางเตือน CO · แสดงข้อความ §8 ถาวร
- หน้าเว็บ**อ่านอย่างเดียว** ไม่มีปุ่มสั่งเครื่อง (ไม่มี ACK/MARK/MODE ผ่านเว็บ) → ไม่มีทางที่คนใน hotspot จะปิดเสียง CO หรือแก้ label ข้อมูลได้
- ไม่มีล็อกอิน: ใครอยู่ใน hotspot เดียวกันเปิดดูได้ — รับได้สำหรับ demo · ต้องเขียนเป็นข้อจำกัด
- `max_open_sockets` จำกัดจำนวน client (เอกสาร: "3 sockets are reserved for internal working") · ตั้ง 4 + `lru_purge_enable = true` ให้ client ใหม่ไล่ตัวเก่าที่ค้าง `[ต้องตรวจกับ CONFIG_LWIP_MAX_SOCKETS ตอน build]`

---

## 6. ต้องวัดบนบอร์ดก่อนบอกว่าเสร็จ (ขั้น 5)

| วัด | วิธี | ผ่านเมื่อ |
|---|---|---|
| คาบ 1 Hz ของ sampler | สรุป 60 s (`jitter` · `overrun`) 10 นาทีแบบไม่มี client เทียบกับ 2 client เปิดค้าง | `overrun=0` และ jitter ไม่เลวกว่าเดิมอย่างมีนัย (เดิม ≤ 76 µs/นาที · 16–17 ต.ค.) |
| แถวหาย | `dropped` · `skipped` ของ sampler/storage | 0 เหมือนเดิม |
| stack | HWM ของ web/httpd/sampler/io | เหลือ ≥ 25 % |
| heap | `heap min` ในสรุป 60 s | ไม่ลดลงต่อเนื่อง (ไม่รั่ว) หลังเปิด–ปิดหน้า 20 ครั้ง |
| ไฟ | reset reason หลังรันยาว · ราง 5 V ถ้ามีมัลติมิเตอร์ | ไม่มี `BROWNOUT` (P1/P2 ยังค้าง · เคยวัด 4.59 V) |
| ความหน่วง | เวลาจากแถวเกิด (`up_s`) ถึงหน้าเว็บ | ≤ 2 s (ค่าคาดหมาย) |
| ขาดการเชื่อมต่อ | ปิด hotspot ระหว่างเปิดหน้า | หน้าขึ้น "ขาดการเชื่อมต่อ" ภายใน 3 s · CO/LED ไม่สะดุด |

---

## 7. ลำดับขั้น (หยุดรอรีวิวทุกขั้น · CLAUDE.md §9)

1. **แผนนี้** ✅ อนุมัติ 3 พ.ย. (Q1–Q5 ตามเสนอ)
2. `web_json` pure C + host test ✅ (`8d0045c`)
3. mailbox + web task + httpd + หน้า HTML 🟡 **เขียนแล้ว 3 พ.ย. ยังไม่เคย build** (เครื่องที่เขียนไม่มี ESP-IDF) · หน้า HTML ลองในเบราว์เซอร์ด้วยข้อมูลจำลองแล้ว (ทุกสถานะ · จอมือถือ 375 px ไม่เลื่อนแนวนอน) · host test ทุกชุดผ่าน
   - เพิ่ม `time_sync_wifi_started()` เพราะ `httpd_start` ต้องมี TCP/IP stack · ไม่มี `secrets.h` = ไม่เปิดเว็บ (log แล้วทำงานต่อ)
   - `/history.json` ก๊อป ring ใต้ mutex ไปไว้ใน PSRAM ก่อน แล้วค่อยส่ง → โทรศัพท์ที่ช้าไม่ถือ lock ที่ web task ต้องใช้ทุกวินาที
   - ตัวนับที่ httpd task กับ web task ใช้ร่วมกันเป็น C11 atomic (ค่า 32 บิตเดี่ยว ไม่มีฟิลด์อื่นต้องเปลี่ยนพร้อมกัน)
   - `-DWEB_ENABLE=0` = ไม่มีเว็บ (Q5) · CMake หยุดให้ถ้า `CONFIG_HTTPD_WS_SUPPORT` ยังปิด
4. `idf.py build` + แก้ warning
5. flash + วัดตาม §6 · บันทึกลง `docs/bringup/`

**แนะนำให้ทดสอบ `fw/fw7-real-model` บนบอร์ดก่อนขั้น 3–5** (FW-6/FW-7 ยังไม่เคยขึ้นบอร์ด) ไม่อย่างนั้นถ้ามีปัญหาจะแยกไม่ออกว่ามาจาก FSM/ML หรือจากเว็บ

---

## 8. คำถามให้เจ้าของตัดสิน

| # | คำถาม | ข้อเสนอ |
|---|---|---|
| Q1 | priority web/httpd = 2 (ต่ำกว่า storage) | ตามเสนอ |
| Q2 | ประวัติบนหน้าเว็บ 10 นาที | ตามเสนอ (60 นาที ≈ 72 KB ต้องย้ายไป PSRAM) |
| Q3 | เสียงเตือนบนหน้าเว็บ (ต้องแตะเปิดก่อน) | ใส่ · ใช้จังหวะเดียวกับ buzzer ของ ML ALARM / CO |
| Q4 | ทดสอบ fw67 + โมเดลจริงบนบอร์ดก่อน แล้วค่อยขั้น 3 | ตามเสนอ |
| Q5 | ยอมรับความเสี่ยงไฟตกจาก Wi-Fi ที่ทำงานมากขึ้น (P1/P2 ค้าง) | วัดตาม §6 ถ้ามี `BROWNOUT` ให้ปิดเว็บด้วย build flag `WEB_ENABLE=0` |

อุปกรณ์นี้เป็นเครื่องเฝ้าระวังสภาพแวดล้อมเพื่อการวิจัย ไม่ใช่เครื่องมือแพทย์
และไม่ใช่เครื่องเตือนภัยคาร์บอนมอนอกไซด์ ห้ามใช้แทน CO alarm ที่ได้มาตรฐาน UL 2034 หรือ EN 50291
