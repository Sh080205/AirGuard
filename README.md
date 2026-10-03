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

เราจำลองไว้แล้ว: **3σ แบบดูทีละตัวจับ anomaly ประเภทนี้ได้ 0% · Mahalanobis จับได้ 100%**
นี่คือเหตุผลทั้งหมดที่ต้องมี autoencoder แทน `if (pm25 > 50)`

---

## สถานะ

| ส่วน | สถานะ |
|---|---|
| Schematic | ✅ เสร็จ · ตรวจกับ netlist ครบ 28 จุด 37 เน็ต |
| PCB + PCBA | ✅ มาถึงแล้ว · DevKit เสียบซ็อกเก็ตได้พอดี · 🔧 THT ค้าง: J1 · C1 · BZ1 · J7 |
| ตรวจกับ datasheet | ✅ ตารางขา · LDO · เซนเซอร์ · ระยะขา DevKit |
| เฟิร์มแวร์ | ⬜ ยังไม่เริ่ม |
| ML pipeline | ⬜ ยังไม่เริ่ม *(มีแค่ `ml/fetch_dustboy.py`)* |
| เก็บข้อมูลจริง | ⬜ รอเฟิร์มแวร์ logger |

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
| [`docs/00-TEAM_PLAN.md`](docs/00-TEAM_PLAN.md) | เป้าหมาย · แบ่งงาน A/B/C · แผนรายสัปดาห์ · prompt สำเร็จรูป |
| [`docs/01-HARDWARE_ACTION.md`](docs/01-HARDWARE_ACTION.md) | งานที่ต้องทำ · รายการซื้อของ · เช็คลิสต์บัดกรี |
| [`docs/02-HARDWARE_REFERENCE.md`](docs/02-HARDWARE_REFERENCE.md) | 📖 ตารางขา · pinout · ค่าอุปกรณ์ · ตัวเลขสำหรับ lab report |
| [`docs/03-CO_SAFETY.md`](docs/03-CO_SAFETY.md) | 🔴 เกณฑ์ CO · ทำไมไม่ใช่ CO alarm |
| [`docs/04-DATASETS.md`](docs/04-DATASETS.md) | DustBoy · Air4Thai · OpenAQ |
| [`docs/05-MULTIVARIATE.md`](docs/05-MULTIVARIATE.md) | Mahalanobis · χ² · collinearity · PCA vs autoencoder |
| [`docs/06-PRIOR_ART.md`](docs/06-PRIOR_ART.md) | งานที่คนอื่นทำแล้ว + ช่องว่างที่เราเติม |
| [`docs/07-GIT_WORKFLOW.md`](docs/07-GIT_WORKFLOW.md) | ⚙️ วิธีขึ้น GitHub · กฎ branch · สิ่งที่ห้าม commit |
| [`docs/diagrams.html`](docs/diagrams.html) | ไดอะแกรม 5 แผ่น — การต่อโมดูล · ไฟเลี้ยง · data flow · FSM · บรรจุภัณฑ์ |
| [`docs/3d.html`](docs/3d.html) | โมเดล 3 มิติหมุนดูได้ + โหลดไฟล์ Pick&Place ได้ |

---

## ฮาร์ดแวร์ย่อ

| | |
|---|---|
| MCU | ESP32-S3-DevKitC-1 (N16R8) บนซ็อกเก็ต 2×22 |
| ฝุ่น | PMS7003 — PM1.0 / PM2.5 / PM10 · UART |
| ก๊าซ + อุณหภูมิ/ความชื้น | โมดูลคอมโบ ENS160 (TVOC, eCO2) + AHT21 (T/RH) · I²C |
| CO | ZE07-CO · UART · 🔴 แยก safety path จาก ML |
| CO₂ | SCD40 NDIR *(ตัวเลือก)* · I²C |
| เก็บข้อมูล | microSD · SPI |
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
