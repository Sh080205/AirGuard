# Host tests (FW-2 PMS7003 · FW-3 CSV/AHT21 · FW-4 logutil · FW-5a ENS160 · FW-5b ZE07-CO)

`run_tests.ps1` รันทุกชุดต่อกัน: `test_pms7003_parse` · `test_pms7003_uart` ·
`test_csv_row` · `test_aht21_parse` · `test_logutil` · `test_ens160_parse` · `test_ze07_parse` · `test_co_safety` — ชุดไหนพังสคริปต์หยุดและคืน exit code นั้น

## FW-5b

- `test_ze07_parse`: parser เฟรม ZE07-CO ตาม manual V1.7 (Winsen 2020-10-19)
  · checksum ตรงกับตัวอย่างที่พิมพ์ใน manual Table 4–8 (5/5 · พิมพ์มือใน test)
  และตรงกับตัวอ้างอิงที่เขียนแยก · เฟรม Table 4 = 3.7 ppm · ค่าขอบ 0..0xFFFF
  (รวม 0xFF ในไบต์ข้อมูลและค่า ≥ full scale ที่ต้องรับ ไม่ตัด) · บิตเสียทีละบิต
  · checksum ถูกแต่ gas/unit/decimal/full range ไม่ตรง Table 4 → ปฏิเสธ
  · เฟรมขาดทุกความยาว · ไบต์ขยะ/false start 0xFF/resync หลังเฟรมเสีย
  · ป้อนทีละ chunk 1..18 · เฟรมจริง 200 เฟรมในขยะสุ่ม (seed คงที่) · NULL
  · ค่า 0.1 ppm ทั้ง 65,536 ค่าผ่าน `csv_format_row` จริงได้ข้อความ `co_ppm` ตรงทุกค่า
- `test_co_safety` (ขั้น 3 · ลำดับค่าจำลองเท่านั้น ไม่สร้าง CO จริง — `docs/03` §4):
  `co_safety` — FAULT หลังเงียบ > 6 s (ขอบพอดี 6 s ยังไม่ FAULT) · UART พัง = FAULT ถาวร
  · ออกจาก FAULT ต้องได้เฟรมดี 3 เฟรมติด · ค่าทันที 89.9/90.0 · อุ่นเครื่อง 5 นาที
  (ยังตัดสินค่าทันที แต่ไม่เอาเข้าค่าเฉลี่ย) · ค่าเฉลี่ย 1 ชม. 29.9/30.0 · hysteresis
  (ค่าแกว่ง 85–89.9 ไม่หลุด · < 81 ครบ 60 s จึงออก · 81.0 หรือช่วงเซนเซอร์หายทำให้นับใหม่)
  · ค่าเต็มสเกล = ALARM · 40 ppm 20 นาทีเทียบตัวอ้างอิงที่คำนวณค่าเฉลี่ยแบบ brute force
  ทุกวินาที · 8 ชม. 9.0/8.9 · ลำดับสุ่ม 16 ชม. (seed 2026) เทียบตัวอ้างอิงทุกก้าว
  · `co_ppm` เฉพาะนอกอุ่นเครื่อง/FAULT และอายุ ≤ 2.5 s · snapshot ค้าง > 6 s
  `co_alert` — ALARM 5 Hz + เสียง · ACK ปิดเสียง 5 นาทีไม่ล้าง ALARM · เงื่อนไขใหม่เปิดเสียงทันที
  · FAULT กะพริบคู่ + เสียงสั้นทุก 60 s · ALARM ทับ FAULT · `next_change_us` ไม่ข้ามขอบ
  · mutation check 11 ตัว (เปลี่ยน `>=`/`>` ฯลฯ) test จับได้ทุกตัว
- `main/tasks_co.c` (UART2 + task) ไม่มี host test — build ผ่านเท่านั้น ต้องทดสอบบนบอร์ด

## FW-6

- `test_fsm` (`components/fsm`, pure C): ลำดับ CO > ML ALARM > SYS FAULT >
  ML WATCH > NORMAL · PERSISTENT = WATCH, streak ≥ 30 = ALARM · ออกทีละขั้นเมื่อ
  BELOW ต่อเนื่อง 30 s (ABOVE/MISSING เริ่มนับใหม่) · MISSING 60 s → NORMAL ·
  ผล ML ค้าง > 3 s / ก่อนผลแรก 10 s · model fault · FAULT ของ AHT21/ENS160/PMS
  ตาม grace + limit ทีละตัว, mask, ออกเมื่อปกติ 10 s · จังหวะ LED/buzzer ทุกสถานะ
  (5 Hz + beep 3 ครั้ง/10 s, 1 Hz, กะพริบคู่ 2 s, indication ของ logger) ·
  ACK ปิดเสียง ML ALARM จนจบ, ไม่มีผลใต้ CO · เดโม 4 ขั้น × 5 s, จบเอง 60 s,
  ACK/กดค้างซ้ำ/CO ทำให้จบ · `next_change_us` ไม่เคยช้ากว่าขอบจริง (กวาดทีละ 1 ms)
  · เวลาถอยหลัง, streak saturate, NULL
- ยังไม่ถูกเรียกจาก `main` — ต่อเข้า io task ใน FW-6 ขั้น 3 หลัง FW-5b/FW-7 merge

## FW-4

- `test_logutil`: `logutil_days_from_civil` ทุกวัน 1970-01-01..2105-12-31 ต่อเนื่อง
  (ครอบปี 2000 ที่เป็น leap และ 2100 ที่ไม่ใช่) · `logutil_parse_iso8601`
  (คำสั่ง `settime`) เทียบกับตัวนับวันแบบวนลูปที่เขียนแยกใน test, anchor
  2000-03-01Z = 951868800, เวลาเดียวกันสาม offset, leap day, รูปแบบผิด/ค่าเกินช่วง
  ต้องปฏิเสธและไม่แตะ output · ปัดวินาที · จำแนกช่องว่าง timestamp
  (ok/ซ้ำ/ข้าม/ถอยหลัง) · เปลี่ยนวันตอนเที่ยงคืน · min/max/avg/jitter
  · `logutil_frame_fresh` (hold เฟรม PMS7003 ≤ `PMS_MAX_AGE_MS` = 2500 ms จาก `logutil.h`
  ตัวเดียวกับ firmware): อายุ 0 และเท่าขอบพอดีใช้ได้, เกินขอบ 1 µs ใช้ไม่ได้,
  จำลองรอบ 1 Hz รับเฟรมที่ t=0 ถามที่ +1000/+2000/+3000 ms (±5 ms) = ใช้/ใช้/ว่าง,
  ยังไม่มีเฟรม, เฟรม "จากอนาคต"/timestamp ติดลบ
  (int64 µs ของ esp_timer ไม่ wrap ในทางปฏิบัติ จึงทดสอบ guard แทน)
- `test_csv_row` เพิ่ม `csv_event_header` = `start_iso,end_iso,label,note`
  ตาม `data/README.md` (พิมพ์มือใน test) + ทุกขนาด buffer ที่ไม่พอ
- โค้ดที่แตะ SD/I²C/Wi-Fi/FreeRTOS (`main/tasks_*.c`, `time_sync.c`,
  `console.c`, `components/aht21/aht21.c`) ไม่มี host test — build ผ่านเท่านั้น
  ต้องทดสอบบนบอร์ดตาม `docs/tasks/FW-4.md`

## FW-3

- `test_csv_row`: header เทียบกับสตริงที่พิมพ์ใน test ตาม `CLAUDE.md` §7,
  แถวตัวอย่าง §7, กลุ่มค่าที่หาย → ช่องว่าง, `normal`/`marked`,
  การปัดทศนิยม 1 ตำแหน่ง (half away from zero บนค่า float ที่เก็บจริง),
  NaN/inf → ช่องว่าง, แถวยาวสุด 99 ตัวอักษร, ทุกขนาด buffer ที่ไม่พอ
  → คืน 0 และไม่เขียนเกิน, `struct tm` ผิดช่วง, ชื่อไฟล์ `AG_`/`EV_`, แถว events
- `test_aht21_parse`: CRC8 เทียบกับตัวอ้างอิงแบบหารพหุนามที่เขียนแยก
  (ไม่ hardcode CRC), สูตร §8.1–8.2 ที่ S = 0, 2¹⁸, 2¹⁹, 3·2¹⁸, เต็มสเกล,
  การแบ่ง nibble ของไบต์ที่ 3 (หน้า 13), CRC เสียทุกบิต, busy bit7,
  NULL, `aht21_needs_init`
- ไม่ต้องใช้ stub อื่นนอกจาก `stubs/esp_err.h`

⚠️ สคริปต์สร้าง `firmware/build/test_host/` — ถ้ารันก่อน `idf.py set-target`
ครั้งแรก, `set-target` จะปฏิเสธ fullclean เพราะ `build/` ไม่ใช่ CMake build dir
(target ยังได้ `esp32s3` จาก `sdkconfig.defaults` อยู่ดี)

## FW-2 PMS7003

จาก `firmware/test_host/` รันคำสั่งเดียวใน PowerShell โดยมี `gcc` ใน PATH:

```powershell
powershell -ExecutionPolicy Bypass -File ./run_tests.ps1
```

สคริปต์ compile ด้วย `-std=c11 -Wall -Wextra -Werror -pedantic -O2` แล้วรัน
parser และ UART mock tests; หยุดและคืน exit code เมื่อ compile/test ล้มเหลว
binary อยู่ใน `firmware/build/test_host/` ซึ่งถูก ignore อยู่แล้ว
ไม่ใช้ Python, ESP-IDF หรือบอร์ดสำหรับ host tests

- Parser: ค่า PM ทั้งหกฟิลด์และ endianness, checksum เสียทีละบิต,
  เฟรมขาดทุกความยาว, ขยะ/false header/split header, เฟรมติดกัน,
  length ผิด, recovery, ทุกตำแหน่งแบ่งสองก้อน, ป้อนทีละไบต์, NULL
  และ `out` ไม่ถูกแก้เมื่อ error
- UART mock: configuration/ทิศ TX-RX, ส่งต่อ error ทุกขั้นของ init,
  init ซ้ำ/ลองใหม่, polling, เก็บ partial frame ข้าม timeout,
  recovery หลัง CRC/length เสีย, deadline เมื่อข้อมูลมาช้า/ขยะต่อเนื่อง,
  tick wrap และ UART read error
- `stubs/esp_err.h` ใช้ค่าจาก ESP-IDF v5.5.5 ส่วน FreeRTOS/driver stubs
  เป็นแบบจำลองสำหรับทดสอบเท่านั้น ไม่ใช่หลักฐานว่า peripheral ทำงานบนบอร์ด
- `test_frames.h` สร้างเฟรมและคำนวณ checksum เองทุกครั้ง
  ไม่ใช้ checksum ที่ hardcode หรือ helper ของ parser
- parser translation unit ใช้ `PMS7003_PARSE_ONLY` เพื่อไม่รวม FreeRTOS/driver
  แม้ compile สำหรับ ESP-IDF; signature `esp_err_t` ไม่เปลี่ยน

## แหล่งที่มาของโปรโตคอล

[Plantower PMS7003 series data manual V2.5, 2016-06-01](https://www.es.co.th/Schemetic/PDF/PMS7003.PDF)
(สำเนาคู่มือผู้ผลิตบนเว็บผู้จัดจำหน่าย; เลขหน้าคือหน้า PDF นับจาก 1)

| โค้ด | หัวข้อใน manual |
|---|---|
| header, length, byte order, PM fields, checksum, UART format | Appendix I, หน้า 13-14 |
| SET/RESET ระดับทำงานปกติ | Pin Definition, หน้า 4 |
| active mode หลังเปิดไฟ | Output result, หน้า 4 |
| รออย่างน้อย 30 วินาทีหลังตื่นจาก sleep | Circuit Attentions ข้อ 4, หน้า 10 |

`pms7003_init()` ตั้งค่าแบบไม่รอ warm-up; `pms7003_read()` คืน raw frame
ที่ตรวจครบแล้ว ผู้เรียกต้องจัดการ warm-up ก่อนใช้เป็นค่าที่นิ่ง
การต่อเข้าลูปหลักยังเป็นงาน FW-4 ห้ามถือว่า checksum ถูกแปลว่า warm-up แล้ว
ใช้ task เดียวเป็นเจ้าของ init/read และ UART1; ถ้าเรียกหลาย task ต้อง serialize
`portMAX_DELAY` คืน `ESP_ERR_INVALID_ARG` เพื่อรักษาสัญญาว่ารอแบบจำกัดเวลา

บัฟเฟอร์ภายในเลือกให้จุสองเฟรม ส่วน RX ring จุสิบหกเฟรม เป็นขนาดที่เลือกใน
ซอฟต์แวร์ ไม่ใช่ค่ารับรองจาก sensor: ค่าคาดหมาย ต้องยืนยันด้วยการวัด
ยังไม่วัด latency, scheduling gap, overflow, stack หรือกระแสบนบอร์ด

การตรวจ ESP-IDF แยกจาก host tests:

```powershell
cd ..
idf.py build
Get-Item build/esp-idf/pms7003/libpms7003.a
```

ต้องใช้ ESP-IDF v5.5.x และ target `esp32s3`; library ยืนยันว่า component
ถูก compile แม้ยังไม่มีการเรียกจาก `main.c` ตามขอบเขต FW-2
