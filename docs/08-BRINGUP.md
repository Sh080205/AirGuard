# ขึ้นบอร์ดครั้งแรก (bring-up) + วิธี build / flash

> สำหรับคนทำฮาร์ดแวร์ (คน A) · ใช้คู่กับ `CLAUDE.md` (กฎหลัก) · `docs/01-HARDWARE_ACTION.md` · `docs/02-HARDWARE_REFERENCE.md`
> ตัวเลขที่วัดได้ให้จดลง `docs/bringup/YYYY-MM-DD.md` — เป็นข้อมูลทำ lab report 03603324

---

## A. เตรียมเครื่องคอม (ครั้งเดียว)

1. **ติดตั้ง ESP-IDF v5.5.5** — https://dl.espressif.com/dl/esp-idf/ → *ESP-IDF v5.5.5 Offline Installer*
   path ติดตั้ง **ห้ามมีช่องว่าง / วงเล็บ · ไม่เกิน 90 ตัวอักษร** (ใช้ค่าเริ่มต้น `C:\Espressif` ได้)
2. **clone repo** (ต้องได้รับเชิญเป็น collaborator ก่อน เพราะ repo เป็น private)
   ```bat
   git clone https://github.com/Sh080205/AirGuard.git
   ```
   ครั้งต่อไปใช้ `git pull` ในโฟลเดอร์ repo
3. **ไดรเวอร์ USB** — เสียบสายเข้าพอร์ต **UART** ของ DevKit → Device Manager → Ports ต้องเห็น *Silicon Labs CP210x (COMx)*
   ถ้าไม่เห็น ติดตั้งไดรเวอร์ CP210x จาก Silicon Labs

## B. build / flash — ต้องใช้หน้าต่าง ESP-IDF เท่านั้น

`idf.py` ใช้ได้เฉพาะในหน้าต่างที่โหลด environment ของ ESP-IDF แล้ว
→ เปิดจาก Start menu: **"ESP-IDF 5.5 CMD"** หรือ **"ESP-IDF 5.5 PowerShell"** (PowerShell/CMD ธรรมดาหา `idf.py` ไม่เจอ)

### ครั้งแรกบนเครื่องนี้
```bat
cd <โฟลเดอร์ repo>\firmware
copy main\secrets.h.example main\secrets.h
notepad main\secrets.h
```
ใส่ชื่อ/รหัส hotspot มือถือ **(ต้องเป็นคลื่น 2.4 GHz — ESP32-S3 ไม่รองรับ 5 GHz)** · `secrets.h` ห้าม commit (git ignore ไว้แล้ว)
```bat
del sdkconfig                 :: ถ้ามีไฟล์นี้อยู่ — ต้องลบเพื่อให้อ่าน sdkconfig.defaults ใหม่
idf.py set-target esp32s3
idf.py build                  :: ต้องจบด้วย "Project build complete"
```

### ใช้ประจำ
```bat
idf.py -p COMx flash monitor  :: flash แล้วเปิด log · ออกจาก monitor: Ctrl + ]
idf.py -p COMx monitor        :: ดู log อย่างเดียว
```

| ทำอะไร | CMD | PowerShell |
|---|---|---|
| ลบไฟล์ | `del ไฟล์` | `Remove-Item ไฟล์` |
| copy | `copy ก ข` | `Copy-Item ก ข` |
| หาข้อความ | `findstr คำ ไฟล์` | `Select-String คำ ไฟล์` |

### คำสั่งใน monitor
- ตั้งเวลาเองเมื่อไม่มี hotspot: `settime 2026-10-05T10:00:00+07:00`

### ไฟสถานะ (LED เขียว)
| จังหวะ | ความหมาย |
|---|---|
| กะพริบสั้นทุก 2 s | ยังไม่มีเวลาจริง → **ยังไม่บันทึก** |
| ติดค้าง | กำลังบันทึกปกติ |
| ติดยาว ดับสั้น ทุก 2 s | อยู่ในช่วงเหตุการณ์ (กด MARK แล้ว) |

ปุ่ม MODE เป็นเดโม: เหลือง 1 Hz → แดง 5 Hz + buzzer · ACK กลับปกติ — 🔴 **ห้ามกด MODE ระหว่างเก็บข้อมูล**

---

## C. ขั้นตอนขึ้นบอร์ด

🔴 **กฎความปลอดภัย** (จาก `CLAUDE.md`)
- **ห้ามใส่ C13 ถาวร** — LDO บน DevKit = SGM2212-3.3 (`MN4`) รับ COUT 1–10 µF
- **จ่ายไฟทีละทาง** — เสียบ USB เพื่อ flash ห้ามจ่าย J8 พร้อมกัน
- **J3/J4/J5 (I²C) และ J6 (microSD) = 3.3 V เท่านั้น** · ห้ามจ่ายโมดูล ENS160+AHT21 ด้วย 5 V
- **J2 (ZE07-CO) = 5 V** · ขาโมดูลไม่ได้เรียง 1-2-3-4-5 → ดูตารางใน `docs/02` §3
- **ปิดไฟทุกครั้งก่อนเสียบ/ถอดอะไร** · เสียบเซนเซอร์ทีละตัว

| # | ขั้น | ต้องได้ |
|---|---|---|
| 1 | **ถอด DevKit** · วัดความต้านทาน TP6↔TP1 · TP5↔TP1 | ≫ 1 kΩ |
| | TP2↔TP5 · TP3↔TP5 | ≈ 4.7 kΩ |
| | gate Q1 ↔ TP1 | ≈ 100 kΩ |
| | แถบ D1 | อยู่ด้าน +3V3 |
| 2 | บัดกรี J1 · C1 (ขา + ไป +5V) · BZ1 (หาขั้วด้วยถ่าน 3 V · ขา 2 = บวก) · J7 (ไม่บังคับ) | — |
| 3 | จ่าย 5 V เข้า J8 **ยังไม่เสียบ DevKit** | ไม่มีอะไรร้อน · TP6 ≈ 5 V |
| 4 | ปิดไฟ · เสียบ DevKit · beep ขา 5V↔TP6 · 3V3↔TP5 · G↔TP1 | ต่อถึงทั้ง 3 คู่ |
| 5 | `idf.py -p COMx flash monitor` | esptool: *Detected flash size: 16MB* · log: PSRAM 8 MB |
| 6 | MODE วน 3 รอบ · ACK | เขียว → เหลือง 1 Hz → แดง 5 Hz + buzzer → กลับเขียว |
| | กดแต่ละปุ่ม 20 ครั้ง · กดค้าง 1 s แล้วปล่อย 20 ครั้ง | นับได้ 20 พอดีทั้งสองแบบ |
| | มือเข้าใกล้ปุ่มโดยไม่กด · กด RESET 10 ครั้ง | ไม่มี event · buzzer ไม่ดังแวบ |
| | logic analyzer จับ LED เหลือง / แดง | คาบ 1000 / 200 ms → **จดค่าจริง** |
| 7 | เสียบ ENS160+AHT21 → J3 (เช็คลำดับขา VCC-GND-SDA-SCL) · microSD → J6 · PMS7003 → J1 · **ZE07-CO ยังไม่ต่อ** | — |
| 8 | บูตโดยเปิด hotspot | ได้เวลาจริงภายใน 1 นาที · เขียวติดค้าง |
| | บูตโดยปิด hotspot → `settime ...` | เริ่มบันทึก |
| | log 5 เฟรมแรกของ AHT21 | CRC ผ่านทุกเฟรม · T/RH สมเหตุสมผล |
| | รัน 10 นาที · นับแถวใน `AG_*.csv` | 600 ± 1 แถว · header 10 คอลัมน์ตาม `CLAUDE.md` §7 |
| | ตัวนับ PM ใน log (new / reused / empty) | empty ≈ 0 · reused ≈ 55–60% → **จดค่าจริง** |
| | ถอดสาย PMS ระหว่างรัน | PM ว่างภายใน ≤ 3 แถว · เสียบกลับแล้วมาต่อ |
| | กด MARK 2 ครั้งห่าง 30 s | ~30 แถว `marked` + 1 แถวใน `EV_*.csv` |
| | เทียบ T/RH กับเทอร์โมมิเตอร์ | **จดส่วนต่าง** (ฮีตเตอร์ ENS160 อยู่บนบอร์ดเดียวกับ AHT21) |
| | ถอด SD ระหว่างรันแล้วเสียบกลับ | ไม่ค้าง · เขียนต่อได้ |
| | logic analyzer ที่ GPIO4 10 นาที | คาบ 1000 ms → **จด jitter** |
| 9 | log สรุปหลัง 1 ชม. · แรงดันตกที่ TP5 ตอนเขียน SD (ถ้ามีสโคป) | dropped = 0 → **จด SD max · queue สูงสุด · heap min** |
| 10 | วางเครื่อง **ในห้อง** จุดเดิม สูง 1.0–1.5 m ห่างหน้าต่าง/ประตู/แอร์ · จดเวลาเริ่ม | 5 นาทีต่อมาไฟล์ต้องโตขึ้น · เก็บช่วงปกติ ≥ 3 วัน |

**เจอปัญหา:** หยุดที่ขั้นนั้น · ถ่ายรูป + ก๊อป log ส่งให้ทีมซอฟต์แวร์ · **อย่าแก้โค้ดเฟิร์มแวร์เอง**
