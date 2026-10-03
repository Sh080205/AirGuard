# AirGuard-S3 — Prior Art & Literature Review

**เอกสารประกอบแผน** `AirGuard-S3_project_plan.md`
**สืบค้น:** 26 ก.ค. 2569

---

## สรุป 30 วินาที

- **ไม่มีใครทำสิ่งที่คุณจะทำ "ทั้งชุด"** — งานที่มีอยู่แยกเป็น 2 ค่ายที่ไม่คุยกัน
  - ค่าย **วิจัย**: LSTM-Autoencoder บน air quality data → แม่นมาก แต่ **รันบน server ไม่เคยลง MCU** ไม่มีตัวเลข latency/energy/memory
  - ค่าย **TinyML/maker**: autoencoder ลง ESP32 ได้จริง → แต่เป็น **vibration/audio ไม่ใช่ air quality หลายช่อง**
- 🔑 **ช่องว่างที่คุณเติมได้:** เอา 2 ค่ายมาชนกันบนข้อมูลชุดเดียว + วัดผลเชิงระบบ 6 มิติ + ปล่อย labeled dataset
- ⚠️ **ข่าวดีที่สำคัญที่สุด:** งานวิจัยสาย air quality anomaly detection ส่วนใหญ่ **รายงานเมตริกผิด** (ใช้ accuracy หรือ point-adjusted F1) มีหลักฐานวิชาการชัดว่าผิด (Kim et al. AAAI'22) → คุณทำให้ถูกได้ง่ายๆ และนั่นคือ contribution ที่แท้จริง

---

## Part A — โปรเจกต์ที่ "ทำเสร็จแล้ว" (ของจริง ก็อปโครงได้)

### A1. ⭐ ShawnHymel — TinyML Anomaly Detection (ESP32 + Autoencoder) — **template ที่ดีที่สุด**

| | |
|---|---|
| Repo | `github.com/ShawnHymel/tinyml-example-anomaly-detection` |
| บทความคู่กัน | DigiKey series "Edge AI Anomaly Detection" (4 ตอน, ตอน 4 = deploy ลง ESP32) |
| โดเมน | vibration (accelerometer MSA301 ติดพัดลมเพดาน) — **ไม่ใช่ air quality** |
| ทำอะไร | เก็บข้อมูล 3 แกน ~200 ตัวอย่าง/1 วิ ผ่าน ESP32 → train autoencoder (TF/Keras) → TFLite → รัน inference บน ESP32 → บัซเซอร์ดังเมื่อเจอ anomaly |

**ทำไมสำคัญกับคุณมาก:** โครงสร้าง pipeline **เหมือนโปรเจกต์คุณ 100%** ต่างแค่ชนิดเซนเซอร์
```
ESP32 เก็บข้อมูล → POST/CSV → train AE ใน Python → TFLite convert
→ ฝังเป็น C array → รันบน ESP32 → threshold บน reconstruction MSE → alert
```
🧪 **จุดที่ต้องอ่านให้ละเอียด:** ซีรีส์นี้เทียบ autoencoder กับ **Mahalanobis distance** ด้วย — ตรงกับที่แผนเราบอกว่าต้องมี non-deep baseline (§4.3) ไปดูว่าเขาสรุปว่าอันไหนชนะและเพราะอะไร

**ต่างจากเราตรงไหน:** เขาใช้ 1 เซนเซอร์ 3 แกน homogeneous / เราใช้ **หลายเซนเซอร์ heterogeneous** (PM หน่วย µg/m³, TVOC เป็น index, T เป็น °C) → ปัญหา normalization ยากกว่าและน่าสนใจกว่า พูดถึงข้อนี้ในรายงานได้

---

### A2. Kutluhan Aktar — AI-Assisted Air Quality Monitor (FireBeetle ESP32) — **ต้นแบบเรื่องการเก็บข้อมูล**

| | |
|---|---|
| ที่อยู่ | Hackster.io + Edge Impulse Expert Projects (มีหน้าใน docs.edgeimpulse.com และ repo `edgeimpulse/expert-projects`) |
| เซนเซอร์ | NO₂, O₃ + weather data |
| ทำอะไร | เก็บข้อมูลลง SD card เป็น **CSV แยกไฟล์ตามคลาสมลพิษ** → train NN → deploy ESP32 → PHP web app แสดงผล + กล้อง |

⚠️ **หมายเหตุสำคัญ:** งานนี้เป็น **supervised classification** (จัดคลาสมลพิษ) **ไม่ใช่ anomaly detection** อย่าเอาไปอ้างว่าเป็นงานเดียวกัน
✅ **สิ่งที่ยืมมาใช้ได้ตรงๆ:** วิธี logging ลง SD เป็นไฟล์แยกตาม label ตอนทำ **Phase B induced experiments** (§5.2 ในแผน) — คุณจุดธูปแล้วกดปุ่มให้บอร์ดเขียนลงไฟล์ `E1_incense_run1.csv` เลย ง่ายและกันพลาดเรื่อง timestamp

---

### A3. UPB CS OpenCourseWare — Air Quality Monitoring Using Edge Impulse — **ตัวอย่างกับดักที่ต้องเลี่ยง**

| | |
|---|---|
| ที่อยู่ | `ocw.cs.pub.ro/courses/iothings/proiecte/2021/airqualityedgeimpulse` |
| ระดับ | โปรเจกต์นักศึกษา (คล้ายคุณ) |
| อ้างว่า | "identifies anomalies in air quality data through embedded ML" บน ESP32 |
| จริงๆ ทำ | เครือข่าย dense 5 ชั้น → **output 2 คลาส (normal / anomaly)** |

⚠️ **นี่คือ supervised binary classification ที่แปะป้ายว่า anomaly detection** — ปัญหาคือ:
1. ต้องมี labeled anomaly เยอะพอจะ train ซึ่งในโลกจริงไม่มี
2. จับได้แค่ anomaly ประเภทที่เคยเห็นตอน train — anomaly แบบใหม่ (unseen) จับไม่ได้เลย ซึ่งขัดกับจุดประสงค์ของ anomaly detection

⭐ **ใช้ประโยชน์:** อ้างงานนี้ในรายงานส่วน related work แล้วอธิบายว่าทำไมคุณเลือก **unsupervised reconstruction-based** แทน → แสดงว่าคุณเข้าใจความต่างเชิงแนวคิด นี่คือย่อหน้าที่ทำให้รายงานคุณดูโตกว่าเพื่อน

---

### A4. Marcelo Rovai (mjrobot) — Exploring ML with XIAO ESP32S3

Hackster.io — workflow TinyML บนชิป **ESP32-S3 โดยเฉพาะ** (ตรงกับบอร์ดคุณ) ใช้เป็นคู่มืออ้างอิงตอนติดปัญหา build/quantize บน S3 โดยเฉพาะ

---

### A5. สรุปเทียบ: ที่มีอยู่ vs ที่คุณจะทำ

| | A1 ShawnHymel | A2 Aktar | A3 UPB | **AirGuard-S3 (คุณ)** |
|---|---|---|---|---|
| Anomaly detection แท้ (unsupervised) | ✅ | ❌ classify | ❌ classify | ✅ |
| Air quality หลายช่อง | ❌ vibration | ✅ | ✅ | ✅ (8+ ช่อง) |
| รันบน MCU จริง | ✅ | ✅ | ✅ | ✅ |
| **เทียบ edge vs server หัวต่อหัว** | ❌ | ❌ | ❌ | ✅ 🏆 |
| **วัด latency/energy/memory** | บางส่วน | ❌ | ❌ | ✅ 🏆 |
| **labeled induced-anomaly dataset เปิด** | ❌ | ❌ | ❌ | ✅ 🏆 |
| **เมตริกถูกต้อง (PR-AUC/event-level)** | — | ❌ | ❌ | ✅ 🏆 |
| RH confound ablation | ❌ | ❌ | ❌ | ✅ 🏆 |

→ 5 ช่อง 🏆 นั่นคือเหตุผลที่โปรเจกต์นี้เขียนเป็น write-up ได้ ไม่ใช่แค่การบ้าน

---

## Part B — งานวิจัย (เรียงตามความสำคัญกับโปรเจกต์)

### B1. 🔑 Wei, Jang-Jaccard, Xu, Sabrina, Camtepe, Boulic (2022) — LSTM-Autoencoder based Anomaly Detection for Indoor Air Quality Time Series Data

- **arXiv:2204.06701** (ต่อมาตีพิมพ์ใน IEEE Access) · 14 หน้า, 16 รูป, 5 ตาราง · CC-BY (อ่านฟรี)
- **ข้อมูล:** Dunedin CO₂ time-series จากการติดตั้งจริงในโรงเรียนนิวซีแลนด์
- **วิธี:** LSTM หลายเซลล์เรียน long-term dependency + AE หา threshold ที่เหมาะที่สุดจาก reconstruction loss
- **ผล:** accuracy 99.50% เคลมว่าชนะโมเดลอื่น
- **เหตุผลที่ paper นี้สำคัญ:** เป็น **motivation ที่ตรงที่สุด** — เขาระบุชัดว่า "สถิติดั้งเดิมและ shallow ML จับ anomaly ที่ต้องดู correlation ข้ามหลายจุดเวลาไม่ได้" นั่นคือประโยคที่คุณอ้างได้ตรงๆ ในบทนำ

⚠️ **จุดอ่อนที่คุณต้องชี้ (สำคัญมาก — นี่คือ contribution ของคุณ):**
1. **รายงาน accuracy บนงาน anomaly detection ที่ข้อมูลไม่สมดุล** — ถ้า anomaly เป็น 0.5% ของข้อมูล โมเดลที่ตอบ "ปกติ" ตลอดได้ 99.5% เท่ากันพอดี ตัวเลข 99.50% จึงบอกอะไรแทบไม่ได้เลยถ้าไม่เห็น confusion matrix
2. **ใช้ CO₂ ช่องเดียวเป็นหลัก** ไม่ใช่ multivariate จริงจัง
3. **ไม่มีการ deploy ลง edge** ไม่มีตัวเลข latency / model size / พลังงาน

→ 📐 **ใน 3 ข้อนี้ ข้อ 1 คือของขวัญ**: คุณเขียนได้ว่า *"เราทำซ้ำแนวทาง B1 แต่รายงานด้วย PR-AUC และ event-level detection rate ตามข้อเสนอของ Kim et al. (B2) และเพิ่มการวัดผลบน MCU จริง"* — นั่นคือ abstract ของ write-up คุณเลย

---

### B2. ⭐⭐ Kim, Choi, Choi, Lee, Yoon (2022) — Towards a Rigorous Evaluation of Time-Series Anomaly Detection

- **AAAI 2022** · arXiv:2109.05257 · โค้ด: `github.com/tuslkkk/tadpak`
- **ค้นพบ:** โปรโตคอลประเมินผลที่ชื่อ **Point Adjustment (PA)** ซึ่งงาน TAD เกือบทั้งวงการใช้ **ทำให้ประเมินค่าสูงเกินจริงอย่างรุนแรง** — พิสูจน์ทั้งทฤษฎีและการทดลองว่า **anomaly score ที่สุ่มขึ้นมาเฉยๆ ก็กลายเป็น SOTA ได้ง่ายๆ** หลังใส่ PA
- ยังชี้ว่า **โมเดลที่ไม่ได้ train เลย ก็ได้ผลเทียบเท่าโมเดลที่มีอยู่** เมื่อห้ามใช้ PA
- เสนอ baseline และโปรโตคอลประเมินผลใหม่

🔑 **ทำไม paper นี้เปลี่ยนโปรเจกต์คุณ:** มันคือ**หลักฐานทางวิชาการ** ที่รองรับสิ่งที่แผนเราเขียนไว้ใน §4.4 ว่าห้ามใช้ accuracy และต้องใช้ event-level metric
⭐ **ประโยคที่ใส่ในรายงานได้เลย:** *"เราหลีกเลี่ยงโปรโตคอล point adjustment ตามข้อวิจารณ์ของ Kim et al. (AAAI 2022) และรายงาน PR-AUC พร้อม event-level detection rate และ false-alarm rate ต่อวันแทน"*
→ 🏆 นิสิตปริญญาตรีที่อ้าง paper นี้ได้ถูกจุด = อาจารย์และกรรมการรับสมัคร ป.โท จะเห็นทันทีว่าคุณอ่านงานจริง ไม่ได้ลอก tutorial

**อ่านเพิ่ม:** มีงานปี 2569 ที่ stress-test เมตริกรุ่นหลัง-PA ต่ออีก (arXiv:2607.11969) และ **PATE** (KDD 2024) ที่เสนอเมตริกแบบ proximity-aware — ถ้าจะทำให้ลึกขึ้น ไปทางนี้

---

### B3. Banbury et al. — MLPerf Tiny Benchmark

- **arXiv:2106.07597** (MLCommons)
- **งาน anomaly detection ในเบนช์มาร์กนี้:** fully-connected autoencoder, **ขนาดไฟล์ 270 KB**, เป้า **AUC ≥ 0.85**, ข้อมูล ToyADMOS / DCASE 2020 Task 2 (เสียงเครื่องจักร)
- โครงสร้าง: input 128 → encoder/decoder ละ 2 ชั้น fully-connected 72 units + batch norm + ReLU (quantized)
- เป้าหมายฮาร์ดแวร์: MCU/DSP ที่ 10–250 MHz

📐 **ใช้ทำอะไร:** เป็น **จุดอ้างอิงอุตสาหกรรม** สำหรับ 2 เรื่อง
1. **ขนาดโมเดล** — ของเขา 270 KB, ของคุณ (§4.2) **32.4 KB int8** → คุณเล็กกว่า 8× ในขณะที่ทำงานหลายช่องสัญญาณ **นี่คือตัวเลขที่ควรเขียนในรายงาน**
2. **เมตริก** — MLPerf ใช้ **AUC ไม่ใช่ accuracy** ยืนยันจุดยืนของ B2 อีกชั้น
- 🧪 MLPerf Tiny มีเวอร์ชัน v1.3 ที่เพิ่ม streaming benchmark (2025) — ถ้าอยากอ่านเรื่องการวัด latency/energy ให้ถูกวิธี ไปดูตรงนี้

---

### B4. Saha, Sandha, Srivastava — Machine Learning for Microcontroller-Class Hardware: A Review

- **arXiv:2205.14550**
- รีวิวครอบคลุม software stack (TensorFlow Lite Micro, LiteRT for MCU, CMSIS-NN, **MCUNet/TinyEngine**) และเทคนิค quantization / pruning / knowledge distillation / on-device transfer learning / federated learning
- 📌 ใช้เป็น **แหล่งอ้างอิงหลักในบท background** ของ write-up สาย Efficient ML

**เสริม:** มี systematic literature mapping เรื่อง anomaly detection บน MCU (คัด 18 จาก 162 papers, ปี 2021–2023) ซึ่งสรุปว่า **สถาปัตยกรรมที่ใช้บน MCU มี 2 แบบหลัก: fully-connected autoencoder (FC-AE) และ depthwise CNN**
→ ✅ **ยืนยันว่าการเลือก dense AE + Conv1D AE ของเรา (§4.2) ตรงกับสิ่งที่วงการใช้จริง** ไม่ได้เลือกมั่ว — อ้างข้อนี้เพื่อ justify design decision ได้

---

### B5. 🔌 Hygroscopic growth calibration for low-cost PM2.5 sensors — **paper ที่แก้ปัญหาความชื้นไทยของคุณ**

- **Atmospheric Measurement Techniques (AMT), vol. 17, p. 1051, 2024** (Copernicus, open access) · มี preprint ใน EGUsphere 2023
- **วิธี:** สร้าง calibration ที่เปลี่ยนตามเวลาสำหรับ **Plantower PMS5003** โดยใช้ correction factor จาก **κ-Köhler theory** หาพารามิเตอร์เชิงประจักษ์เทียบกับข้อมูลอ้างอิง EPA AQS
- **ข้อมูล:** 2 ปีจากเครือข่าย BEACO2N ใน San Francisco และ Los Angeles
- **ตัวเลขที่ต้องจำ:** ⚠️ **ที่ RH เกิน ~75% เซนเซอร์ Plantower อ่านค่า PM สูงเกินจริงอย่างมีนัยสำคัญ**

🔑 **นี่คือ paper ที่เปลี่ยน "บั๊ก" เป็น "งานวิจัย"** — กรุงเทพ/ศรีราชา RH เกิน 75% แทบทุกคืนและทุกวันฝน ถ้าคุณไม่จัดการเรื่องนี้ โมเดลจะเตือนทุกเช้าตรู่ (RH สูงสุด) แล้วคุณจะสรุปผิดว่าโมเดลห่วย

**วิธีใช้ในโปรเจกต์:**
- ระดับง่าย: ใส่ RH + absolute humidity เป็น feature ให้ AE เรียน context เอง แล้วทำ **ablation** เทียบมี/ไม่มี → มีตัวเลขให้รายงาน
- ระดับลึก (portfolio โดดเด่น): implement correction factor แบบ κ-Köhler ก่อนป้อนโมเดล แล้วเทียบ 3 เงื่อนไข (ไม่แก้ / RH เป็น feature / κ-Köhler correction)
- 📌 หมายเหตุ: paper ใช้ **PMS5003** ส่วนคุณใช้ **PMS7003** — ตระกูลเดียวกันและหลักการกระเจิงแสงเหมือนกัน แต่ **ค่าพารามิเตอร์ใช้แทนกันตรงๆ ไม่ได้** ต้อง fit ใหม่หรือระบุเป็น limitation

**งานเสริมสายเดียวกัน:**
- npj Climate and Atmospheric Science (2025) — แนวทางปฏิบัติในการทำ RH correction ให้เครือข่าย low-cost sensor
- Air Quality, Atmosphere & Health (2025) — calibrate PMSA003(A) ที่ 3 ระดับ RH ด้วยอนุภาค **ชอบน้ำ (NaCl) vs ไม่ชอบน้ำ (น้ำมันคอมเพรสเซอร์)** → อธิบายว่าทำไมควันธูป (E1) กับฝุ่นกวาดบ้าน (E6) ตอบสนองต่อความชื้นไม่เหมือนกัน
- Tandfonline (2019) — corrections + long-term performance ของ low-cost PM sensor

---

### B6. งานอ้างอิงพื้นฐาน (ควรอ้างแต่ ⚠️ ยังไม่ได้ verify รายละเอียดในการค้นครั้งนี้ — เช็คก่อนใส่ bibliography)

| งาน | ใช้ทำอะไร |
|---|---|
| **Chandola, Banerjee, Kumar — "Anomaly Detection: A Survey"** (ACM Computing Surveys, 2009) | taxonomy **point / contextual / collective** ที่แผนเราใช้ใน §1.2 — เป็น citation มาตรฐานของสาขา |
| **De Vito et al. — UCI Air Quality Data Set** | dataset ที่แผนแนะนำให้ใช้ช่วง D0–D20 + งานต้นฉบับพูดเรื่อง **sensor drift ของ MOX** ตรงกับความเสี่ยงข้อ "drift" ในแผน |
| **Liu & Layland (1973)** | RMS utilization bound สำหรับ timing analysis (§3.2) — โยงวิชา OS 03603332 |
| **Sensirion / ScioSense application notes** | ⚠️ ต้องอ่านเองเพื่อยืนยันว่า **TVOC index และ eCO2 ของ ENS160 คำนวณมาอย่างไร** ก่อนเคลมอะไรในรายงาน |

---

## Part C — ช่องว่างที่โปรเจกต์คุณเติม (ใส่ใน related work ได้เลย)

| # | ช่องว่างในงานที่มีอยู่ | สิ่งที่ AirGuard-S3 ทำ |
|---|---|---|
| 1 | งานสาย air quality anomaly detection (B1) **ไม่เคย deploy ลง MCU** — ไม่มี latency/energy/memory | วัดครบบน ESP32-S3 จริง ด้วย `esp_timer_get_time`, `heap_caps_get_free_size`, INA219 |
| 2 | งานสาย TinyML anomaly detection (A1, B3) ใช้ **vibration/audio** ไม่ใช่ air quality หลายช่อง heterogeneous | 8+ ช่อง ต่างหน่วย ต่างอัตราการเปลี่ยนแปลง — ปัญหา normalization ที่ยากกว่าจริง |
| 3 | **ไม่มีการเทียบ edge vs server หัวต่อหัวบนข้อมูลชุดเดียว** | ตารางเปรียบเทียบ 6 มิติ (§9 ในแผน) รวม **availability ตอน Wi-Fi ล่ม** ซึ่งไม่มีใครวัด |
| 4 | **ไม่มี labeled anomaly dataset สาธารณะ** ที่มี ground truth จากเหตุการณ์ที่จงใจสร้าง | 9 การทดลอง × 3 รอบ + baseline 14 วัน ปล่อยเป็น open dataset |
| 5 | เมตริกที่ใช้กันอยู่ **ผิด** (accuracy ใน B1, PA-F1 ตามที่ B2 วิจารณ์) | PR-AUC + event-level detection rate + false alarms/วัน + MTTD |
| 6 | งาน RH correction (B5) กับงาน anomaly detection **เป็นสองวงการที่ไม่คุยกัน** | ทำ ablation ว่า RH confound กระทบ false-alarm rate ของ anomaly detector เท่าไร ← **อันนี้น่าสนใจที่สุดในเชิงวิจัย** |

⭐ **ถ้าจะเลือกทำให้ดีแค่อย่างเดียว เลือกข้อ 6** — มันเป็นคำถามวิจัยที่ตอบได้ด้วยอุปกรณ์ 3,000 บาทและเทอมเดียว แต่ยังไม่มีใครตอบชัดๆ และมันเชื่อม 2 สาขาเข้าด้วยกัน นี่คือของที่เขียนเป็น workshop paper ได้จริง

---

## Part D — ลำดับการอ่าน (อย่าอ่านทุกอันพร้อมกัน)

| สัปดาห์ | อ่าน | เพื่อ |
|---|---|---|
| **W00** (ตอนนี้) | A1 repo + DigiKey ตอน 4 | ก็อป pipeline ทั้งเส้น เริ่มโค้ดได้ทันที |
| **W00–W01** | B1 (abstract + method) | เขียนบทนำ/motivation ของรายงาน |
| **W01** | **B2 (ทั้งฉบับ — สำคัญที่สุด)** | ตั้งกรอบการวัดผลให้ถูกตั้งแต่ต้น **ก่อน**เก็บข้อมูล ไม่ใช่มาแก้ทีหลัง |
| **W02** | B3 (ส่วน anomaly detection task) | ตั้งเป้าขนาดโมเดลและ AUC ที่จะเทียบ |
| **W04–W05** | **B5 (ทั้งฉบับ)** | ก่อนวิเคราะห์ baseline 14 วัน — ต้องรู้ว่า RH กำลังหลอกคุณอยู่ |
| **W07** | A2, A3 | เขียน related work + justify ว่าทำไมไม่ใช้ supervised |
| **W08+** | B4 + systematic mapping | เขียนบท background สาย Efficient ML |

⚠️ **B2 ต้องอ่านให้จบก่อน W04 (ก่อนเริ่มเก็บ baseline)** เพราะถ้ารู้ทีหลังว่าเมตริกที่ตั้งไว้ผิด อาจต้องเก็บข้อมูลใหม่

---

## สิ่งที่ต้องจำ

- **A1 (ShawnHymel/DigiKey) = โครงที่ก็อปได้ทันที** ต่างแค่เซนเซอร์ เริ่มจากอันนี้ ไม่ต้องคิดใหม่
- **A3 คือกับดัก:** dense classifier 2 คลาสที่แปะป้ายว่า anomaly detection — จับ anomaly แบบใหม่ไม่ได้ อย่าทำตาม แต่อ้างเพื่อเปรียบเทียบ
- **B1 คือ motivation** และคือโมเดลที่ต้องเอาชนะ (เชิงเมตริกที่ถูกต้อง ไม่ใช่เชิง accuracy)
- **B2 (AAAI'22) คือ paper ที่ทำให้รายงานคุณดูโตขึ้นทั้งฉบับ** — point adjustment ทำให้ random score กลายเป็น SOTA ได้ **อ่านให้จบก่อน W04**
- **B3 ให้ตัวเลขเทียบขนาด:** MLPerf 270 KB vs ของคุณ 32.4 KB int8 → เขียนได้ว่าเล็กกว่า 8×
- **B5 คือ paper ที่กันคุณจากการสรุปผิด:** RH > 75% ทำ Plantower อ่านเกิน และไทยชื้นเกิน 75% แทบทุกคืน (พารามิเตอร์เป็นของ PMS5003 ต้อง fit ใหม่สำหรับ PMS7003)
- **ช่องว่างที่คุ้มที่สุด = ข้อ 6:** RH confound กระทบ false-alarm rate ของ anomaly detector เท่าไร — ยังไม่มีใครตอบ และคุณตอบได้ในเทอมเดียว
- **มีอยู่ 2 วงการที่ไม่คุยกัน** (วิจัย air quality ML ↔ TinyML deployment) การเป็นคนที่พูดได้ทั้งสองภาษาคือจุดขายของคุณตอนสมัครสหกิจและ ป.โท

---

### คำถามทวนความเข้าใจ (ยังไม่เฉลย)

1. B1 รายงาน accuracy 99.50% — สมมติชุดข้อมูลเขามี anomaly 0.5% พอดี ให้คุณสร้าง "โมเดล" ที่ได้ 99.5% โดยไม่ใช้ ML เลยแม้แต่บรรทัดเดียว ทำอย่างไร? และถ้าคุณเป็น reviewer จะขอให้เขาแสดงตัวเลขอะไรเพิ่ม 3 อย่างเป็นอย่างน้อย?

2. B2 บอกว่าโมเดลที่ **ไม่ได้ train** ก็ได้ผลเทียบเท่าโมเดลที่มีอยู่เมื่อห้ามใช้ PA — ถ้าเรื่องนี้จริงในโดเมนของคุณด้วย มันหมายความว่า autoencoder ไร้ประโยชน์ หรือหมายความว่าเบนช์มาร์กที่ใช้กันมันง่ายเกินไป? คุณจะออกแบบการทดลองอย่างไรเพื่อ**แยกสองคำอธิบายนี้ออกจากกัน** บนข้อมูลของคุณเอง?

3. ถ้า B5 ถูก แปลว่าตอนเช้าตรู่ RH สูง PM อ่านเกิน — แต่ตอนเช้าตรู่ก็เป็นเวลาที่คนหุงข้าว/จุดธูปจริงด้วย ⏱️ คุณจะออกแบบการทดลองอย่างไรเพื่อ**แยก RH artifact ออกจากเหตุการณ์จริง** โดยใช้อุปกรณ์ที่มีในแผน (ใบ้: คิดถึง PM2.5/PM10 ratio และ TVOC ว่ามันตอบสนองต่อน้ำหรือไม่)
