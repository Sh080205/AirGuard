# AirGuard-S3

**เครื่องเฝ้าระวังคุณภาพอากาศในห้อง + ตรวจจับความผิดปกติบนอุปกรณ์ สำหรับผู้ป่วยภูมิแพ้และโรคทางเดินหายใจ**

ESP32-S3 · TinyML · บอร์ดออกแบบเอง
วิชา 03603323 / 03603324 · วิศวกรรมคอมพิวเตอร์ ม.เกษตรศาสตร์ ศรีราชา

---

## โปรเจกต์นี้ต่างจากเครื่องวัดอากาศทั่วไปยังไง

เครื่องที่มีขายเตือนเมื่อค่าเกินเกณฑ์ แต่**สิ่งที่กระตุ้นอาการหลายครั้งไม่ได้ทำให้ค่าไหนเกินเกณฑ์เลย**

```
ทำอาหาร       PM2.5 ↑↑   อุณหภูมิ ↑   ความชื้น ↑    →  ขึ้นด้วยกัน มีเหตุผล  =  ปกติ
ปัดฝุ่น / ราฟุ้ง   PM2.5 ↑↑   อุณหภูมิ →   ความชื้น →    →  ฝุ่นขึ้นลอยๆ         =  ผิดปกติ
```

ทั้งสองกรณี PM2.5 อ่านได้เท่ากันได้ — **เกณฑ์ตัวเลขแยกไม่ออก แต่ multivariate แยกออก**

ตัวอย่างอธิบายแนวคิดใน [`docs/05`](docs/05-MULTIVARIATE.md) §1 เป็นข้อมูล T/RH 2 มิติ ที่มีจุดผิดปกติ**จุดเดียว** (T 28 °C กับ RH 68 % พร้อมกัน): 3σ ทีละตัวจับจุดนี้ไม่ได้ แต่ Mahalanobis จับได้
ตัวอย่างนี้ไม่ใช่ผลทดลอง และใน repo ไม่มีสคริปต์ที่ใช้คำนวณ

โมเดลที่รันบนบอร์ดจริงคือ **Mahalanobis D²** บนชุดฟีเจอร์ `pm_th` (PM2.5 · T · RH) **ไม่ได้ใช้ autoencoder** ผลที่อ้างได้มีดังนี้

- **ข้อมูลสังเคราะห์** (ML-3b · `ml/compare.py` · 5 seed · ชุด `pm_th`): ในเหตุการณ์แบบ "ความสัมพันธ์ขาด" Mahalanobis จับได้ 15/15 ส่วน 3σ ทีละตัวจับได้ 0/15 · PCA, LOF และ autoencoder ขนาดเล็กก็จับได้ 15/15 เท่ากัน · การเตือนผิดของ Mahalanobis ทั้ง 16 ครั้งเริ่มตอนทำอาหาร
  ตัวเลขชุดนี้มาจากสมมติฐานของข้อมูลสังเคราะห์ **ไม่ใช่ค่าวัด** · ไฟล์ผล `data/processed/compare/ml3b/compare.md` ไม่ขึ้น git ถ้าต้องการให้สร้างใหม่ด้วย `ml/compare.py`
- **ข้อมูลจริง** (R4 · [`data/processed/ml-real/pm_th/`](data/processed/ml-real/pm_th/) · PR #10):
  - val จับได้ 1/2 เหตุการณ์ ได้แก่ธูปที่วางผิดวิธี (D² สูงสุด 51.72) ส่วนปัดฝุ่นจับไม่ได้ (D² สูงสุด 10.87 ต่ำกว่าเกณฑ์ 17.92) · 3σ ที่ปรับเกณฑ์บน val ก็จับได้ 1/2 เท่ากัน
  - ช่วง test เตือนผิด **0 ครั้งใน 6.7 ชม.** ขณะที่ 3σ ปรับเกณฑ์บน val เตือนผิด 2 ครั้ง และ 3σ เกณฑ์ตายตัวเตือนผิด 12 ครั้ง
  - ช่วง test ไม่มีเหตุการณ์ที่กด MARK เลย จึงวัด detection rate บน test ไม่ได้

---

## สถานะ

> อัปเดต 4 พ.ย. 2569 จาก git (`demo/2026-11-04` @ `e6bd974` = tag `demo-2026-11-05`) และ [`docs/bringup/2026-11-03.md`](docs/bringup/2026-11-03.md) · รายละเอียดอยู่ใน [`CLAUDE.md`](CLAUDE.md) §10 — ถ้าขัดกัน ให้ยึด §10

| ส่วน | สถานะ | ที่มา |
|---|---|---|
| Schematic | ✅ ตรวจกับ netlist แล้ว: ต่อจริง 28 จุด · 37 เน็ต | `docs/02-HARDWARE_REFERENCE.md` |
| PCB + PCBA | ✅ ขึ้นบอร์ดแล้ว · 🔴 C13 ไม่ใส่ | `docs/bringup/2026-10-14.md` · CLAUDE.md §5 |
| ข้อมูลเทรน | ✅ เก็บตั้งแต่ `2026-10-25T20:24:17` ถึง `2026-10-30T17:42:48` (+07:00) · นอกกล่อง · ผ่าน serial mirror · แบ่งตามเวลา train 87.60 / val 18.00 / test 11.71 ชม. · ขึ้น git แล้ว | [`data/README.md`](data/README.md) · PR #7 `c6660c6` |
| ML R3–R5 | ✅ R3 ตัดแถวเหตุการณ์ที่ไม่ได้กด MARK ออกจากการ fit (`65a8403`) · R4 บันทึกผลแบบ frozen (PR #10 `c872893`) · R5 นำ header โมเดลจริงลงเฟิร์มแวร์ (`2c0474a`) | git |
| โมเดลบนบอร์ด | ✅ Mahalanobis `pm_th` โมเดลจริง (`ml3-v1+pm_th+e6f29d2be5b3`) รันบนบอร์ด · D² บนบอร์ดเทียบกับเกณฑ์ 17.92 · ML รายงาน `fault=0` · ⬜ ยังไม่เห็นบรรทัด self-test ตอนบูต | bringup 3 พ.ย. §6–7 · `2c0474a` |
| FSM (FW-6) | ✅ merge เข้า `main` แล้ว (ต่อเข้า io task) · อยู่ในเฟิร์มแวร์ demo · บันทึก 3 พ.ย. ไม่มีตารางทดสอบ FSM แยกต่างหาก | `87b8137` · `b353943` |
| Dashboard (FW-8) | ✅ รันบนบอร์ดได้ · ค่า PM/T/RH/D² ตรงกับ `AGCSV`/`AGML` · WebSocket 1 client `err=0` · ⬜ ยังไม่ได้ทดสอบมือถือ 2 เครื่อง | PR #11 `e4cf96f` · bringup 3 พ.ย. §3 |
| OLED | ✅ ทำงานแล้ว (`present=1` `err=0`) · ⚠️ chunk สูงสุด 36–70 ms ไม่ผ่านเกณฑ์ T5 (< 10 ms) แต่ไม่พบผลกระทบที่วัดได้ (ใน 610 s ได้ 608 แถวเท่ากับตอนไม่มีจอ · error เซนเซอร์ 0) · ยังอยู่เฉพาะ branch demo ยังไม่ merge เข้า `main` | bringup 3 พ.ย. §2 §5 · `5283c0c` |
| microSD | ❌ เลิกใช้ (ทีมตัดสิน 3 พ.ย.) เพราะ `0x107` timeout ทุกครั้ง · ปิดด้วย `STORAGE_SD_ENABLE 0` · ใช้ serial mirror แทน ⇒ **บันทึกได้เฉพาะตอนต่อโน้ตบุ๊ก** | bringup 3 พ.ย. §1 · `fe80122` |
| เส้นทาง CO | ✅ แยกจาก ML (FW-5b) · ⚠️ ALARM ทดสอบด้วย host test เท่านั้น · `co_ppm` ยังไม่ได้สอบเทียบ | `64c1dac` · CLAUDE.md §10 |
| เฟิร์มแวร์ demo | ✅ `main` @ `e4cf96f` + OLED + ปิด SD · build ผ่าน warning 0 · host test 13 ชุดผ่าน | bringup 3 พ.ย. §4 |
| กล่อง | 🟡 รุ่น 2 export STL แล้ว · ⬜ ยังไม่ได้ทดสอบเครื่องตอนอยู่ในกล่อง (B6) | [`hardware/enclosure_v2/`](hardware/enclosure_v2/) · bringup 3 พ.ย. §7 |

**ข้อจำกัดที่ต้องบอกในการนำเสนอ**

- **test ไม่มีเหตุการณ์ positive** เพราะเหตุการณ์ที่กด MARK ตกอยู่ใน train และ val ทั้งหมด ⇒ วัดได้แค่การเตือนผิด — `test-mahalanobis/metrics.json` (`event_count 0`)
- **val จับได้ 1/2:** เหตุการณ์ที่จับได้คือธูปที่วางผิดวิธี (ต่ำกว่าเครื่องเกือบติดบอร์ด · pm2_5 569 µg/m³) ส่วน**ปัดฝุ่นจับไม่ได้** — `model.json` · [`data/README.md`](data/README.md) ⚠️ 3
- **ช่วง test เตือนผิด 0 ครั้ง แต่วัดได้แค่ 6.7 ชม.** (หัก U5 ที่น่าจะเป็นการจุดธูปออกแล้ว) — `metrics.json` (`normal_observed_s 24106`) · [`data/README.md`](data/README.md)
- **โมเดลเทรนจากข้อมูลนอกกล่องที่ RH เฉลี่ย 56.96 ± 2.96 %** ⇒ ในห้องที่แห้ง (RH 34.4–40.6 %) D² ขึ้นไป 40–72 และ ML ALARM ค้างทั้งที่อากาศปกติ · ห้าม normalize ใหม่บนบอร์ด ⇒ demo สดจึงเน้นการวัด จอ dashboard และเส้นทาง CO ส่วนผล ML แสดงจาก R4 บนสไลด์ — bringup 3 พ.ย. §6
- **CO ALARM ทดสอบด้วย host test เท่านั้น** ยังไม่ได้ทดสอบกับ CO จริงหรือเฟรมจำลองบนบอร์ด · **`co_ppm` ยังไม่ได้สอบเทียบ** — CLAUDE.md §10 (`f943605`)

---

## โครงสร้าง

```
CLAUDE.md          บริบทถาวร — GPIO · กฎเหล็ก · สัญญารูปแบบข้อมูล
docs/              เอกสารทั้งหมด
firmware/          คน B — ESP-IDF v5.5.x
ml/                คน C — Python
data/              ข้อมูลที่เก็บได้ (raw ไม่ commit)
```

### เอกสารสำคัญ

| ไฟล์ | เนื้อหา |
|---|---|
| [`docs/11-CODE-MAP.md`](docs/11-CODE-MAP.md) | 🗺️ **แผนที่โค้ด + คำอธิบายโค้ด** — task · queue · หนึ่งรอบของ sampler · เส้นทาง CO · ML บนบอร์ด · ไฟล์ firmware/ml ทีละไฟล์ |
| [`docs/02-HARDWARE_REFERENCE.md`](docs/02-HARDWARE_REFERENCE.md) | 📖 ตารางขา · pinout · ค่าอุปกรณ์ · ตัวเลขสำหรับ lab report |
| [`docs/08-BRINGUP.md`](docs/08-BRINGUP.md) | ขึ้นบอร์ด · build / flash · ตารางทดสอบ |
| [`docs/03-CO_SAFETY.md`](docs/03-CO_SAFETY.md) | 🔴 เกณฑ์ CO · ทำไมไม่ใช่ CO alarm |
| [`docs/04-DATASETS.md`](docs/04-DATASETS.md) | DustBoy · Air4Thai · OpenAQ |
| [`docs/05-MULTIVARIATE.md`](docs/05-MULTIVARIATE.md) | Mahalanobis · χ² · collinearity · PCA vs autoencoder |
| [`docs/06-PRIOR_ART.md`](docs/06-PRIOR_ART.md) | งานที่คนอื่นทำแล้ว + ช่องว่างที่เราเติม |
| [`docs/07-GIT_WORKFLOW.md`](docs/07-GIT_WORKFLOW.md) | ⚙️ วิธีขึ้น GitHub · กฎ branch · สิ่งที่ห้าม commit |
| [`docs/diagrams.html`](docs/diagrams.html) | ไดอะแกรม 5 แผ่น — การต่อโมดูล · ไฟเลี้ยง · data flow · FSM · บรรจุภัณฑ์ |
| [`hardware/enclosure_v2/`](hardware/enclosure_v2/) | กล่องรุ่น 2 — OpenSCAD + viewer |
| [`docs/reference/BUILD_SPEC.md`](docs/reference/BUILD_SPEC.md) | สเปกบอร์ดรุ่นเก่า — ใช้ได้เฉพาะที่มาของสูตร · ค่าที่ขัดกับ CLAUDE.md ให้ยึด CLAUDE.md |

---

## ฮาร์ดแวร์ย่อ

| | |
|---|---|
| MCU | ESP32-S3-DevKitC-1 (N16R8) บนซ็อกเก็ต 2×22 |
| ฝุ่น | PMS7003 — PM1.0 / PM2.5 / PM10 · UART |
| ก๊าซ + อุณหภูมิ/ความชื้น | โมดูลคอมโบ ENS160 (TVOC, eCO2) + AHT21 (T/RH) · I²C |
| CO | ZE07-CO · UART · 🔴 แยก safety path จาก ML |
| CO₂ | SCD40 NDIR *(ตัวเลือก)* · I²C |
| เก็บข้อมูล | serial CSV mirror ผ่าน USB · *(microSD · SPI เลิกใช้ 3 พ.ย.)* |
| แจ้งเตือน | LED ×3 · buzzer ผ่าน MOSFET |
| รับคำสั่ง | ปุ่ม MARK / MODE / ACK |

### 🔴 3 อย่างที่ผิดแล้วเจ็บ

1. **ห้ามใช้ GPIO 35/36/37** (PSRAM) · **0/3/45/46** (strapping) · **38/48** (RGB LED)
2. **ห้ามใส่ C13** — 470 µF เกินพิกัด COUT ของ LDO (1–10 µF) ถึง 47 เท่า
3. **ซื้อ DevKit ต้องถามความกว้างบอร์ด** — **25.4 mm = ใช่** · 28 mm = เสียบไม่ได้

---

## ตั้งค่าเครื่องพัฒนา

**เฟิร์มแวร์**
```bash
# ต้องมี ESP-IDF v5.5.x
cd firmware
idf.py set-target esp32s3
idf.py menuconfig          # Flash 16MB · PSRAM Octal 8MB
idf.py build
idf.py -p <PORT> flash monitor
```

**ML**
```bash
cd ml
python -m venv .venv && source .venv/bin/activate   # Windows: .venv\Scripts\activate
pip install -r requirements.txt
pytest                      # ต้องผ่านก่อนเริ่มเขียนของใหม่
```

---

## ⚠️ ข้อจำกัดการใช้งาน

> **อุปกรณ์นี้เป็นเครื่องเฝ้าระวังสภาพแวดล้อมเพื่อการวิจัย ไม่ใช่เครื่องมือแพทย์
> และไม่ใช่เครื่องเตือนภัยคาร์บอนมอนอกไซด์
> ห้ามใช้แทน CO alarm ที่ได้มาตรฐาน UL 2034 หรือ EN 50291**

ผู้ผลิต ZE07-CO ระบุใน datasheet เองว่า
*"Please do not use the modules in systems which related to human being's safety."*
