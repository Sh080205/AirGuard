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

## บิลด์

```bash
idf.py set-target esp32s3
idf.py menuconfig      # ครั้งแรก: Flash 16MB · PSRAM Octal 8MB
idf.py build
idf.py -p <PORT> flash monitor
```

> ⚠️ ตั้ง flash/PSRAM ผิด = บูตไม่ขึ้น หรือใช้ PSRAM ไม่ได้ แล้วโมเดลจะโหลดไม่ลง
