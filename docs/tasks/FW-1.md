# FW-1 — โครง ESP-IDF + LED · buzzer · ปุ่ม

**branch:** `fw/skeleton` · **ต้องรอ:** — · **เป้าหมายส่ง:** ก่อน จ. 5 ต.ค. 2569

## เป้าหมาย

`firmware/` กลายเป็น ESP-IDF project ที่ **build ผ่าน** และพอ flash ลงบอร์ดจริงครั้งแรก
ต้องพิสูจน์ได้ทันทีว่า (1) ตั้ง flash/PSRAM ถูก (2) LED · buzzer · ปุ่มทุกตัวต่อถูกขา

## อ่านก่อน

- `CLAUDE.md` §2 (กฎเฟิร์มแวร์) · §3 · **§4 (ตารางขา — ใช้ได้เฉพาะขาในนี้)** · §5 (จังหวะ LED)
- `firmware/README.md` (โครงสร้างโฟลเดอร์ · กฎเขียนโค้ด)

## ไฟล์ที่สร้าง/แก้ — เท่านี้

```
firmware/CMakeLists.txt
firmware/main/CMakeLists.txt
firmware/main/main.c
firmware/main/board_pins.h      ← เลข GPIO ทั้งหมดอยู่ที่นี่ที่เดียว
firmware/main/tasks_io.c / .h   ← LED · buzzer · ปุ่ม
```
ลบ `firmware/main/.gitkeep` · **ห้ามแก้ `firmware/sdkconfig.defaults`** (ถ้าคิดว่าต้องแก้ ให้เสนอในรายงาน)

## ข้อกำหนด

1. **`board_pins.h`** — `#define` ทุกขาจาก `CLAUDE.md` §4 พร้อมคอมเมนต์ชื่อเน็ต
   ใส่ `_Static_assert` ให้ build ล้มถ้าขาไหนตรงกับรายการต้องห้าม (35 36 37 · 3 46 45 0 · 38 48 · 19 20)
2. **บูต** — log ขนาด flash (`esp_flash_get_size`) และ PSRAM (`esp_psram_get_size`)
   *(ไว้ยืนยันบนบอร์ดว่าเป็น 16 MB / 8 MB จริง)*
3. **LED** GPIO2 / 39 / 40 เป็น output เริ่มต้นดับ
   `io_set_mode(NORMAL | WATCH | ALARM)` ตาม `CLAUDE.md` §5:
   NORMAL = LED_G ติดค้าง · WATCH = LED_Y กะพริบ 1 ครั้ง/วินาที · ALARM = LED_R กะพริบ 5 ครั้ง/วินาที + buzzer
   ใช้ `esp_timer` หรือ task ก็ได้ — **คอมเมนต์ว่าทำไมเลือกแบบนั้น**
4. **Buzzer** GPIO41 output เริ่มต้น `0` (ดับ)
5. **ปุ่ม** GPIO42 / 47 / 1 — ตั้งค่า **ตามโค้ดใน `CLAUDE.md` §4 ทุกฟิลด์** (pull-up ภายใน · `GPIO_INTR_NEGEDGE`)
   - ISR มี `IRAM_ATTR` · ทำแค่ `xQueueSendFromISR` เลขขา + `portYIELD_FROM_ISR` · ไม่มีอย่างอื่น
   - task ปุ่มทำ debounce ด้วยซอฟต์แวร์ — ไม่รับ event ขาเดิมซ้ำภายใน `DEBOUNCE_MS` (เริ่มที่ 50 ms *ค่าคาดหมาย ต้องยืนยันด้วยการวัด*)
     และเช็คซ้ำด้วย `gpio_get_level` ว่ายังเป็น 0 จริง
     *(บอร์ดมี RC กรองไว้แล้ว τ ≈ 4.5 ms ตาม `docs/02` — ซอฟต์แวร์เป็นชั้นที่สอง)*
6. **พฤติกรรมเดโม** (ไว้ทดสอบบนบอร์ด)
   - `BTN_MODE` วน NORMAL → WATCH → ALARM → NORMAL
   - `BTN_ACK` ปิด buzzer และกลับ NORMAL
   - `BTN_MARK` log `MARK #n` (นับสะสม)
7. **stack** — ทุก task มีคอมเมนต์ที่มาของขนาด stack · log `uxTaskGetStackHighWaterMark()` ของทุก task ทุก 10 วินาที
8. **`esp_err_t`** — ตอน init ใช้ `ESP_ERROR_CHECK` ได้ · ตอนทำงานต้องจัดการ error ไม่ปล่อยทิ้ง
9. `main/CMakeLists.txt` ระบุ `REQUIRES` ให้ถูกกับ v5.5 `[ขึ้นกับเวอร์ชัน ESP-IDF]`

## ห้าม

- ใช้ขานอก §4 หรือเขียนเลข GPIO นอก `board_pins.h`
- ใน ISR: `printf` / `ESP_LOG*` / delay / malloc / float
- ตัวแปรที่ ISR/timer กับ task ใช้ร่วมกันแบบ `volatile` อย่างเดียว — ใช้ queue / critical section / atomic **และคอมเมนต์ว่าทำไม**
- Arduino API · ทำงานของ FW-2 ขึ้นไป

## เกณฑ์ผ่าน — คนรีวิวรันเองได้

```bash
cd firmware
idf.py set-target esp32s3
idf.py build          # ESP-IDF v5.5.x · ต้องไม่มี warning จากไฟล์ใน main/
idf.py size           # แปะสรุปขนาดในรายงาน
```
และ `grep -rnE "GPIO_NUM_[0-9]+|\(1ULL ?<< ?[0-9]+\)" firmware/main --include=*.c` ต้องไม่เจออะไรนอก `board_pins.h`

## ทดสอบบนบอร์ด — ทำที่มหาลัย (agent ไม่ต้องทำ)

| # | ทำ | ต้องได้ | ถ้าไม่ได้ |
|---|---|---|---|
| 1 | flash แล้วดู log บูต | flash 16 MB · PSRAM 8 MB | sdkconfig ผิด |
| 2 | กด MODE วน 3 รอบ | G ค้าง → Y กะพริบช้า → R กะพริบเร็ว + buzzer | ขา/LED กลับขั้ว |
| 3 | กดแต่ละปุ่ม 20 ครั้ง | log นับได้ 20 พอดี | debounce ไม่พอ |
| 4 | เอามือเข้าใกล้ปุ่มโดยไม่กด | ไม่มี event | pull-up ไม่ทำงาน |
| 5 | logic analyzer จับ LED_Y / LED_R | คาบ 1000 ms / 200 ms | `[ต้องวัดจริง]` |
| 6 | จดค่า high-water mark ทุก task | — | ใช้ปรับ stack ใน FW-4 |
