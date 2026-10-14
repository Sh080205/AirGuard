# firmware — คน B

ESP-IDF **v5.5.x** · target `esp32s3` · ESP32-S3-DevKitC-1 (N16R8)

> 📖 GPIO ทุกตัวอยู่ใน [`../CLAUDE.md`](../CLAUDE.md) §4 — **ห้ามเดา ห้ามลอกจากตัวอย่างในเน็ต**

---

## ลำดับงาน — ทำตามลำดับ อย่าข้าม

| # | งาน | ต้องมีบอร์ดไหม |
|---|---|---|
| 1 | โครง ESP-IDF project + CI ที่ build ผ่าน | ❌ |
| 2 | `components/pms7003/` — parser frame 32 ไบต์ | ❌ |
| 3 | `test/` — unit test ของ parser ด้วย frame ปลอม | ❌ |
| 4 | `main/` — FSM + FreeRTOS tasks | ❌ |
| 5 | ไดรเวอร์ I²C + SPI + เขียน CSV | ❌ *(mock ได้)* |
| 6 | ลงบอร์ดจริง ไล่บั๊กร่วมกับคน A | ✅ |

> 🔑 **ข้อ 1–5 ทำได้หมดโดยไม่ต้องมีบอร์ด** — อย่ารอของ

---

## 🔑 กฎออกแบบข้อ 2 ที่สำคัญที่สุด

**แยกส่วน parse ออกจากส่วน UART**

```c
// ✅ ทดสอบได้โดยไม่ต้องมีบอร์ด
esp_err_t pms7003_parse(const uint8_t *buf, size_t len,
                        pms7003_data_t *out, size_t *consumed);

// ส่วนที่คุยกับฮาร์ดแวร์ แยกไฟล์ต่างหาก
esp_err_t pms7003_init(void);
esp_err_t pms7003_read(pms7003_data_t *out, TickType_t timeout);
```

`consumed` บอกจำนวนไบต์ที่ทิ้งได้ โดยเก็บเฟรมที่ยังไม่ครบไว้เรียกครั้งถัดไป
ดูสัญญา API ใน `components/pms7003/include/pms7003.h` และคำสั่ง gcc
สำหรับทดสอบบน PC ใน [`test_host/README.md`](test_host/README.md)

ถ้าเขียนรวมกัน จะทดสอบไม่ได้จนกว่าบอร์ดจะมา — **เสียเวลาไป 3 สัปดาห์ฟรีๆ**

### เคสที่ unit test ต้องครอบคลุม

- frame ถูกต้อง → ค่าออกมาตรง
- checksum ผิด → ต้องคืน error **ไม่ใช่คืนค่าขยะ**
- frame ขาดกลางคัน
- มีขยะนำหน้าก่อน header `0x42 0x4D`
- frame ติดกันสองอันในบัฟเฟอร์เดียว

---

## โครงสร้างที่วางไว้

```
firmware/
├── CMakeLists.txt
├── sdkconfig.defaults          ← Flash 16MB · PSRAM Octal
├── main/
│   ├── CMakeLists.txt
│   ├── main.c                  จุดเริ่ม + สร้าง task
│   ├── fsm.c / fsm.h           BOOT→WARMUP→NORMAL→WATCH→ALARM
│   ├── tasks_sensor.c          อ่านเซนเซอร์ทุก 1 วิ → queue
│   ├── tasks_logic.c           feature + โมเดล + ตัดสิน
│   ├── tasks_io.c              LED · buzzer · OLED · SD
│   └── model/                  .tflite int8 จากคน C
├── components/
│   ├── pms7003/
│   ├── ze07co/
│   └── storage/                เขียน CSV ตามสัญญาใน CLAUDE.md §7
└── test/                       unit test (Unity)
```

---

## 🔴 กฎเขียนโค้ด

**เช็ค `esp_err_t` ทุกจุด** — โค้ดที่ไม่เช็ค return คือโค้ดผิด

**ใน ISR** ห้าม `printf`/`ESP_LOG` · ห้าม delay · ห้าม malloc · ห้าม float
ISR ต้องสั้น ส่งงานต่อให้ task ผ่าน queue · พิจารณา `IRAM_ATTR`

**ตัวแปรที่ ISR กับ task ใช้ร่วมกัน** `volatile` อย่างเดียวไม่พอ
ต้องใช้ critical section / atomic / queue **และคอมเมนต์ว่าทำไมเลือกอันนั้น**

**ปุ่มต้องเปิด internal pull-up** ไม่มี pull-up ภายนอกบนบอร์ด — ดู CLAUDE.md §4

**stack size ห้ามตั้งมั่ว** คำนวณคร่าวๆ แล้ววัดด้วย `uxTaskGetStackHighWaterMark()`

---

## 🔴 เส้นทาง CO ต้องแยกจาก ML

```c
// อยู่ใน tasks_logic.c แต่เรียกก่อนโมเดลเสมอ
if (co_ppm >= CO_ALARM_PPM) {
    fsm_force_alarm(ALARM_CAUSE_CO);    // ไม่ผ่านโมเดล ไม่รอ persistence
}
```

ต้องทำงานได้แม้โมเดลยังโหลดไม่เสร็จ หรือ SD card พัง
เกณฑ์ ppm อ้างจาก [`../docs/03-CO_SAFETY.md`](../docs/03-CO_SAFETY.md) **ไม่ใช่เลขที่คิดเอง**

---

## LED แยกสถานะด้วยจังหวะ ไม่ใช่สี

LED1 ที่ประกอบมาเป็นเขียวอมเหลือง สีใกล้ LED2 เกินไป

```
NORMAL  →  ติดค้าง
WATCH   →  กะพริบ 1 ครั้ง/วินาที
ALARM   →  กะพริบ 5 ครั้ง/วินาที + buzzer
```

---

## FW-4 logger — ก่อน build ครั้งแรก

1. **Wi-Fi (hotspot มือถือ)**: `copy main\secrets.h.example main\secrets.h` แล้วแก้ SSID/รหัส
   — `secrets.h` ถูก ignore (`secrets.*`) **ห้าม commit** · ไม่มีไฟล์นี้ก็ build ได้
   แต่ไม่มี Wi-Fi/SNTP ต้องตั้งเวลาเอง (log ตอน build: `main/secrets.h not found`)
2. **ลบ `firmware/sdkconfig` เดิม** (ถ้ามี) เพื่อให้ `CONFIG_FATFS_LFN_HEAP` จาก
   `sdkconfig.defaults` มีผล — ไม่ลบ build จะหยุดที่ `#error` ใน `tasks_storage.c`
3. ไม่มีเวลาจริง = ไม่เขียน CSV · ตั้งเวลาเองผ่านพอร์ต `UART` ของ DevKit:
   `settime 2026-10-06T09:00:00+07:00` (Enter)

LED เขียวระหว่าง NORMAL: ติดค้าง = กำลังบันทึก · กะพริบสั้น 1 ครั้ง/2 วิ = รอเวลา ·
ดับสั้น 1 ครั้ง/2 วิ = กำลังบันทึกเหตุการณ์ (`marked`) — เหตุผลใน `main/tasks_io.h`
ลำดับ priority / stack / ขนาด queue / N ของ fsync อยู่ที่ `main/task_config.h`,
`main/tasks_storage.h`, `main/tasks_storage.c`

## บันทึก CSV ผ่านสาย USB ลงคอม (serial CSV mirror · เพิ่ม 14 ต.ค.)

เพิ่มเพราะช่องการ์ด microSD ใช้ไม่ได้ (`docs/bringup/2026-10-14.md` P5) — ใช้แทนหรือใช้คู่กับ SD ก็ได้

- storage task พิมพ์**ทุกแถวที่ออกจากคิว**เป็น `AGCSV,<แถว CSV>` และทุกเหตุการณ์ MARK เป็น `AGEV,<แถว EV>`
  รูปแบบแถวมาจาก `csv_format_row` ตัวเดียวกับที่เขียนลง SD ⇒ ตรงสัญญา CLAUDE.md §7 ทุกตัวอักษร
- **ไม่เคย mount SD ตั้งแต่บูต** → ดึงแถวออกจากคิวไปพิมพ์ทันที (คอมได้ข้อมูลช้ากว่าเวลาจริงไม่เกิน ~0.5 s)
- **เคย mount แล้วการ์ดหลุด** → คิวยังเป็นบัฟเฟอร์ให้ SD ตาม FW-4 §7 · ดึงไปพิมพ์เฉพาะตอนคิวเกือบเต็ม (≥ 60/64)
  ⇒ sampler ไม่ต้องทิ้งแถว · แถวที่ไปทาง serial อย่างเดียวนับใน `serial only`
- พิมพ์ใน storage task (งานช้า) ไม่ใช่ใน sampler — ถ้า console ค้าง คิวรับไว้ ไม่กระทบคาบ 1 Hz
- log ทุก 60 s: `60s: serial mirror rows=… (serial only …) errors=… | print avg/max us`
- ปิดได้ด้วย `-DSTORAGE_SERIAL_MIRROR=0` (หรือแก้ define ใน `main/tasks_storage.c`)

**ฝั่งคอม** — ปิด `idf.py monitor` ก่อน (COM ใช้ได้ทีละโปรแกรม) แล้วรันจากหน้าต่าง ESP-IDF CMD:

```bat
cd /d <repo>\firmware
python tools\serial_logger.py --port COM7
```

- เขียน `data/raw/usb/AG_YYYYMMDD_HHMMSS_usb.csv` (header ตาม §7 · ไฟล์ใหม่ทุกวัน) ·
  `EV_…_usb.csv` · `serial_….log` (ทุกบรรทัดดิบ ไว้ไล่ปัญหา)
- ตรวจทุกแถว (10 คอลัมน์ · `+07:00` · label · ชนิดตัวเลข) แถวเสียไม่เขียน · นับ duplicate/gap แจ้งทุก 60 s
- เปิดพอร์ตโดยไม่แตะ DTR/RTS (ไม่ทำให้บอร์ดรีเซ็ต) · บอร์ดรีเซ็ต/สายหลุด → ต่อใหม่เอง
- 🔴 **อย่าใช้ไฟล์ `_usb` คู่กับ AG_*.csv จาก SD ช่วงเวลาเดียวกัน** — `ml.load.load_raw` จะ error เพราะ timestamp ซ้ำ
- ทดสอบ: `python -m unittest discover -s tools -p "test_*.py"`

## บิลด์

```bash
idf.py set-target esp32s3
idf.py menuconfig      # ครั้งแรก: Flash 16MB · PSRAM Octal 8MB
idf.py build
idf.py -p <PORT> flash monitor
```

> ⚠️ ตั้ง flash/PSRAM ผิด = บูตไม่ขึ้น หรือใช้ PSRAM ไม่ได้ แล้วโมเดลจะโหลดไม่ลง
