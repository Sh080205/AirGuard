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
2. **ลบ `firmware/sdkconfig` เดิม** (ถ้ามี) เพื่อให้ `CONFIG_FATFS_LFN_HEAP` และ partition
   table จาก `sdkconfig.defaults` มีผล — ไม่ลบ build จะหยุดที่ CMake (partition) หรือ
   `#error` ใน `tasks_storage.c` (LFN) · ดูหัวข้อ "partition table" ด้านล่าง
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

## ENS160 → `tvoc_ppb` / `eco2_ppm` (FW-5a)

แหล่งตัวเลขทุกตัว: ScioSense ENS160 Datasheet **Version 1.3** (SC-001224-DS-9, 2023-03-29) ·
อ้างหัวข้อไว้ในคอมเมนต์ `components/ens160/include/ens160_parse.h`

- `components/ens160/ens160_parse.c` pure C (host test `test_ens160_parse.c`) ·
  `ens160.c` คุย I²C · อ่านใน **sampler** เอง (burst 6 ไบต์ + MISR ทุกรอบ, เขียน T/RH ชดเชยทุก 2 s
  ไม่มีการรอ) — เวลาจริงต่อรอบอยู่ใน log `ens160 i2c per cycle`
- เปิดใช้: อ่าน PART_ID (0x0160) → OPMODE IDLE → STANDARD (อ่านกลับยืนยัน) · ล้มเหลว → ลองใหม่ทุก 10 s
- คอลัมน์ว่างเมื่อ: warm-up (≤ 3 นาทีหลังจ่ายไฟ) · **initial start-up (ชั่วโมงแรกของอายุเซนเซอร์;
  ถ้าดับไฟก่อนครบ 24 ชม. ต่อเนื่องจะกลับมาเป็นชั่วโมงแรกใหม่ — DS §10.1, เชิงอรรถ 24)** · invalid ·
  STATER · ค่านอกช่วง (TVOC 0–65000 ppb, eCO2 400–65000 ppm) · ค่าที่เห็น NEWDAT ล่าสุดเก่ากว่า 2.5 s ·
  ENS160 ไม่ตอบ (แถวยังเขียนต่อ PM/T/RH ไม่หาย)
- log ทุก 60 s: `rows VOC new/reused/empty` · จำนวนครั้งแต่ละสถานะ · `newdat gap` (คาบข้อมูลจริง —
  datasheet ไม่ระบุ) · `comp writes` · `sensor uses T=… RH=…` (ค่าที่ ENS160 ใช้ชดเชยจริง) ·
  `sampler work per cycle` · และบรรทัด `ENS160 status warm-up -> normal at uptime … s` ทุกครั้งที่สถานะเปลี่ยน
- **ปิด ENS160 (ทดสอบผลฮีตเตอร์):** `idf.py -DENS160_ENABLE=0 build` → เซนเซอร์อยู่ DEEP SLEEP
  คอลัมน์ว่าง · ค่าค้างใน CMake cache ต้องกลับด้วย `idf.py -DENS160_ENABLE=1 build`
- เทียบ `temp_c/humidity_pct` ช่วงปิด/เปิด ENS160: `python tools\heater_compare.py --help`
  (OFF/ON/OFF + เทอร์โมมิเตอร์อ้างอิงถ้ามี · ทดสอบ: `python -m unittest discover -s tools -p "test_*.py"`)
- `-DENS160_MISR_CHECK=0` ใช้เฉพาะถ้าบอร์ดแสดง `misr=` เท่ากับจำนวนครั้งที่อ่านทุกครั้ง
  (แปลว่าอัลกอริทึม MISR ที่ตีความจาก datasheet ผิด ไม่ใช่บัสเสีย) — ต้องใส่ผ่าน
  `target_compile_definitions` ของ component ens160 (ยังไม่ได้ต่อสายผ่าน CMake)

## partition table (FW-5a · `partitions.csv`)

ค่าเริ่มต้นของ ESP-IDF (`partitions_singleapp.csv`) ให้ factory app แค่ **1 MB** ·
build 14 ต.ค. ใช้ไป 0xe3990 B เหลือ ~11% → ใส่ FW-5/FW-6/FW-7 + โมเดลไม่พอ จึงทำตารางเอง

| Name | Type | SubType | Offset | Size | หมายเหตุ |
|---|---|---|---|---|---|
| `nvs` | data | nvs | 0x9000 | 24 KB | offset/ขนาดเท่าเดิม ⇒ ข้อมูล NVS (Wi-Fi) บนบอร์ดอยู่ที่เดิม |
| `phy_init` | data | phy | 0xf000 | 4 KB | เท่าเดิม (`CONFIG_ESP_PHY_INIT_DATA_IN_PARTITION` ปิดอยู่ จึงยังไม่ถูกใช้) |
| `factory` | app | factory | 0x10000 | **4 MB** | offset ของ app ต้องลงตัว 64 KB (0x10000) · เท่าเดิม |
| *(ว่าง)* | | | 0x410000 | ~11.9 MB | ยังไม่จอง |

**ทำไม 4 MB**
- ตอนนี้ (มี Wi-Fi) 0xe39b0 B = 932,272 B ≈ 0.89 MiB → 4 MB คือ ~4.5 เท่า เหลือ ~3.1 MB
- ส่วนที่จะเพิ่ม: ไดรเวอร์ ENS160/ZE07-CO + เส้นทาง CO (FW-5) · FSM (FW-6) · Mahalanobis
  (FW-7 — เมทริกซ์เล็ก) · esp-tflite-micro + โมเดล int8 ที่ฝังเป็น `const` ใน flash
  ขนาดของ tflite-micro ขึ้นกับจำนวน op ที่ลงทะเบียนใน resolver — **ทั้งหมดเป็นค่าคาดหมาย
  ต้องยืนยันด้วยการวัด** (ดูบรรทัด `binary size … free` ทุกครั้งที่ build)
- ถ้าส่วนที่เพิ่มรวมกันใช้ถึง 1–2 MB ก็ยังเหลือที่ · ถ้าเกินจริง ESP-IDF หยุด build เอง
  (`check_sizes.py`) ไม่มีทางได้ image ที่ใหญ่เกิน partition
- ไม่ขยายเต็ม 16 MB เพราะไม่มีประโยชน์ (`idf.py flash` เขียนเท่าขนาด binary ไม่ใช่ขนาด
  partition) และเว้นที่ว่างไว้เผื่อ data partition ภายหลัง โดยไม่ต้องย้าย nvs/factory
- ไม่มี OTA — อัปเดตผ่านสาย USB เท่านั้น

**🔴 sdkconfig เดิมไม่รับค่าใหม่** — `sdkconfig.defaults` ใช้ตอนสร้าง `sdkconfig` ใหม่เท่านั้น
ถ้ามี `firmware/sdkconfig` จากก่อน FW-5a ค่า `CONFIG_PARTITION_TABLE_SINGLE_APP=y` จะยังอยู่
และ `main/CMakeLists.txt` จะหยุดตอน configure ด้วยข้อความ
`sdkconfig does not use firmware/partitions.csv` — แก้โดย:

```bat
cd /d <repo>\firmware
del sdkconfig
idf.py build
```

(ไม่ต้อง `idf.py set-target` ซ้ำ ถ้า `sdkconfig.defaults` มี `CONFIG_IDF_TARGET="esp32s3"` อยู่แล้ว)

**🔴 เปลี่ยน partition แล้วต้อง flash เต็มหนึ่งครั้ง** — `idf.py -p <PORT> flash`
(เขียน bootloader + partition table + app) **ห้ามใช้ `app-flash`** เพราะจะเขียนแค่ app
แล้วบอร์ดยังใช้ตารางเดิมที่ factory 1 MB → พอ app เกิน 1 MB จะบูตไม่ขึ้น ·
ไม่ต้อง `erase-flash` เพราะ nvs อยู่ที่เดิม

## บิลด์

```bash
idf.py set-target esp32s3
idf.py menuconfig      # ครั้งแรก: Flash 16MB · PSRAM Octal 8MB
idf.py build
idf.py -p <PORT> flash monitor
```

> ⚠️ ตั้ง flash/PSRAM ผิด = บูตไม่ขึ้น หรือใช้ PSRAM ไม่ได้ แล้วโมเดลจะโหลดไม่ลง
