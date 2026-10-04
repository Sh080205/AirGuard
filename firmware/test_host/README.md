# PMS7003 host tests (FW-2)

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
