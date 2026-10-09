# ML-2: นิยามการประเมิน `ml2-v1`

`evaluate(raw, scores, events, observed_until=..., event_mapping=...,
threshold=..., threshold_source=...)` คืน `(summary, audit)` โดยไม่แก้ input
ใช้ `load_raw` / `load_events` ของ ML-1; raw CSV §7 และ events CSV เดิมไม่เปลี่ยน
ไม่มีการ train, normalize, เลือก threshold หรือ persistence ใน metrics

## Truth มาจาก annotation ของชนิด event

- `raw.label=marked` หมายถึงช่วงกด MARK เท่านั้น ค่าเริ่มต้นเป็น **unknown**
- raw normal นอก annotations ถือเป็น normal **ภายใต้สมมติฐานว่า annotation ครบ**
  หากยังตรวจสอบช่วงนั้นไม่ได้ ผู้เตรียมข้อมูลต้องจัดเป็น unknown annotation ก่อนวัดผล
  ไม่มีระบบพิสูจน์ว่า raw normal เป็นสภาพปกติทางกายภาพ
- join เวลา `[start_iso,end_iso)` แล้ว mapping ชนิด event อย่างชัดเจนจาก caller
  เช่น `{"cooking":"normal", "decorrelation":"anomaly", "spike":"anomaly",
  "drift":"anomaly"}` สำหรับ synthetic recipe ของ ML-1
  cooking override raw marked เป็น normal; typed anomaly override raw normal เป็น anomaly
- ไม่มี default anomaly mapping; ชื่อที่ไม่อยู่ใน mapping, ชื่อว่าง และ `marked`
  เป็น unknown ไม่แปลชื่อ/อ่านข้อความ `note` เพื่อสร้าง truth ไม่อนุญาต map `marked`
  เป็นชนิดที่ทราบ แม้ note จะไม่ว่าง ผู้ใช้ต้องเติมชื่อชนิดใน label และ mapping
  ชื่อว่างรองรับใน API แต่ `load_events` เดิมของ ML-1 ปฏิเสธ label ว่าง
- raw marked ที่ไม่มี event matching ยัง unknown และไม่สร้าง positive event เอง
  unknown ถูกตัดจาก point metrics และ normal duration แต่ alarm ที่เริ่มในช่วงนี้
  เก็บเป็น `unscored_alarm_episodes` ไม่ลบทิ้ง
- interval มีความยาวบวกและไม่ overlap; event ต่อกันได้ที่ขอบ
  annotations ต้องเรียงเวลาและอยู่ภายในขอบข้อมูล split ที่ส่งมาทั้ง interval
  ไม่ clip หรือทิ้ง event นอกขอบโดยเงียบ ๆ ไม่ขยายช่วงตามรูป score

## คะแนนระดับจุด

score สูง = ผิดปกติมาก; `alarm = score >= threshold` รวมกรณีเท่ากัน
point ใช้เวลาแถวนั้นเทียบ event `[start,end)` โดยตรง ไม่ smear prediction ข้าม event
unknown และ missing score ไม่เข้าตัวหาร; counts ทุกแบบบันทึกใน JSON ให้ตรวจ exclusion ได้

**PR-AUC ในงานนี้หมายถึง unweighted, noninterpolated Average Precision (AP)**
ไม่ใช้ trapezoidal PR area และไม่ทำ precision envelope:

`AP = Σ (R_k − R_(k−1)) P_k`

เรียง score จากมากไปน้อย แล้วเพิ่มจุดทั้งหมดที่ score เท่ากันพร้อมกัน
แต่ละขั้น recall ที่เพิ่มคือน้ำหนัก precision ณ ขั้นนั้น ตามนิยาม
[scikit-learn average_precision_score](https://scikit-learn.org/stable/modules/generated/sklearn.metrics.average_precision_score.html)
โค้ดใช้ NumPy คำนวณสูตรเดียวกัน ไม่ต้องเพิ่ม dependency เพื่อรัน metrics
AP แต่ละจุดน้ำหนักเท่ากัน ไม่ weight ตาม duration; เช่น ranked truth `1,0,1,0`
ได้ `(1 + 2/3)/2 = 5/6`; หาก score ทุกจุดเท่ากัน AP เท่ากับ positive fraction
ไม่มี positive / ไม่มีจุดที่ประเมินได้ → `ap=null` (undefined พร้อม positive count)
มีแต่ positive → AP = 1

precision = `TP/(TP+FP)` และ recall = `TP/(TP+FN)` จาก alarm ของแต่ละจุดจริง
ไม่มี predicted positive → precision null; ไม่มี positive truth → recall null
เมื่อมี positive แต่ไม่เตือน → recall 0 ไม่ใช้ accuracy หรือ point adjustment
ไม่แก้ prediction ย้อนหลังตาม truth แม้ event จะถูกตรวจพบครั้งเดียว

## Alarm episode และ event metrics

episode คือ alarm ต่อเนื่องใน **observed support ที่ชนกัน** ไม่ใช่ truth ต่อเนื่อง
เริ่มที่เวลาแถว alarm แรก; จบที่ observed_until ของแถว alarm สุดท้าย เป็น `[start,end)`
แถวไม่ alarm, missing score หรือ gap ของ support จบ episode ไม่เติมค่าข้าม gap
ขอบ event/unknown ไม่ตัด episode และไม่มีการ cooldown/merge gap/persistence เพิ่ม
caller ที่มี score หลัง persistence ต้องส่งคะแนน/การตัดสินบน timeline จริงมาแล้ว

**onset-based event detection:** typed anomaly event ตรวจพบเมื่อมี episode onset
อย่างน้อยหนึ่งครั้งใน `[event.start,event.end)` หนึ่ง event นับครั้งเดียว แม้มีหลาย alarm
onset ที่ start นับ; onset ที่ end ไม่นับ event เดิม; ไม่มี tolerance ก่อน/หลัง
episode ที่เริ่มก่อน event แล้วค้างอยู่เป็น early overlapping alarm รายงานแยก
ไม่ใช้เป็น detection ของ event นี้ หากไม่มี onset ใหม่ event นั้นยัง missed
นิยามนี้ให้เครดิตการแจ้งเตือนใหม่หลังเริ่มเหตุ ไม่ให้ alarm ที่ค้างมาก่อนสร้าง delay ติดลบ/ศูนย์เอง

- `event_detection_rate = detected_events / event_count`
- `delay_s = first eligible onset − event.start` หน่วยวินาที
- `mttd_detected_s = mean(delay_s เฉพาะ event ที่ detected)`
- บังคับอ่าน/นำเสนอ MTTD **คู่กับ detection rate, detected/event_count และ missed_events**
  event ที่พลาดมี delay null ไม่ใส่ 0, ไม่ตัดออกจาก event denominator
  ไม่มี event → detection rate null; ไม่มี detected → MTTD null
- events ที่ตกใน gap หรือมีแต่ missing scores ยังอยู่ในตัวหารและเป็น missed
  แต่ละ event บันทึก observed/scored points ให้เห็นว่าไม่มีข้อมูลประเมินจริง
  ต้องแยกประเด็น coverage จากความสามารถของโมเดลเมื่อแปลผล

## False alarm episodes/วัน และ observed duration

false episode คือ episode **ที่ onset อยู่ใน known normal** รวม cooking ที่นิยาม normal
ถ้าต่อเข้า anomaly ยังคงเป็น false episode ตาม onset; ถ้าเริ่มใน anomaly แล้วค้างเข้า normal
ไม่เป็น false episode ใหม่ (แต่จุด normal นั้นยังเป็น FP ใน precision)
onset unknown ไม่สร้าง false count แม้ episode ต่อเข้า normal ภายหลัง
ทั้ง episode และ onset truth อยู่ใน audit ของ JSON ให้เห็นข้อจำกัดของนิยามนี้

`false_alarm_episodes_per_day = false_alarm_episodes × 86400 / normal_observed_s`

`observed_until` เป็น Series ที่ index ตรงกับ raw และ scores ทุกแถว
บอกช่วงที่สังเกตได้จริงของแต่ละแถว `[timestamp, observed_until)`
ช่วงต้องยาวบวก ไม่ overlap; timezone-aware ไม่มี NaT และเรียงเวลาไม่ซ้ำ
**ผู้เตรียมข้อมูลต้องระบุจากหลักฐานการบันทึก** ไม่มี default 1 s,
ไม่ใช้ `last−first`, จำนวนวันปฏิทิน หรือ next timestamp เพื่อครอบ downtime
ตัวอย่างการบันทึกต่อเนื่อง 1 Hz ที่มีหลักฐานอาจให้ end = timestamp + 1 s
สำหรับทุกแถวรวมแถวท้าย; ต้องตัด support ตาม stop time จริงหากแถวท้ายสังเกตได้สั้นกว่า

normal duration คือผลรวมความยาวส่วน known normal ของ support ที่มี finite score
คิด intersection กับ annotation แม้ขอบ event อยู่กลางแถว รวม cooking และตัด anomaly/unknown
missing score ไม่นับ exposure เพราะไม่มีการประเมิน alarm ในช่วงนั้น; ส่งทั้ง
`observed_s`, `scored_observed_s`, `normal_observed_s`, `normal_observed_days`
เพื่อไม่ซ่อนข้อมูลที่ขาด ไม่มี normal exposure → rate null ไม่หารด้วย 0
เช่นแถวเวลา 0,1,10,20 มี support แถวละ 1 s และ score ที่ t10 missing
ได้ normal exposure 3 s ไม่ใช่ 21 s; alarm ที่ t0,t1 และ t20 เป็น 2 episodes
rate = `2×86400/3 = 57600/day` เป็นการ normalize จากช่วงสั้น **ไม่ใช่ผลทดลองหนึ่งวัน**

## Threshold และการใช้งาน

ส่ง threshold ที่ freeze ไว้ก่อนประเมิน test เสมอ `threshold_source` ต้องเป็น
`fixed`, `train` หรือ `validation`; ค่า `test` ถูกปฏิเสธและไม่มี threshold optimizer
metadata บันทึกค่าและแหล่ง แต่ไม่สามารถตรวจว่าผู้เรียกนำเลขจาก test มาแอบใส่หรือไม่
ควรเก็บ config/หลักฐานเลือก threshold จาก train/val พร้อมการทดลองต่อไป
AP กวาด ranking threshold เพื่อรายงาน curve summary เท่านั้น ไม่เลือก operating threshold
sample times ต้องตรงกันทุกแถว; ไม่ join score ใกล้เคียง ไม่ drop timestamp mismatch
infinity/ข้อความ score ผิดปฏิเสธ; score NaN เป็น missing ตามนโยบายที่ระบุ

```powershell
python -m ml.metrics_example
pytest ml/ -v
ruff check ml/
```

ตัวอย่างมือ deterministic ไม่มี RNG ไม่มีการ train และไม่วัด sensor/model accuracy
output `data/processed/metrics-example/metrics.json` และ `points.csv`
`save_result` ยอมเขียนเฉพาะใต้ `data/processed/`; undefined เป็น JSON null
audit คง raw label, truth, event id, score และ alarm ทุกแถวให้ตรวจการตัดสินได้
กรณี score missing ค่า alarm=False หมายถึงไม่มีการแจ้งเตือนที่สังเกตได้ ไม่ใช่ normal prediction

อุปกรณ์นี้เป็นเครื่องเฝ้าระวังสภาพแวดล้อมเพื่อการวิจัย ไม่ใช่เครื่องมือแพทย์
และไม่ใช่เครื่องเตือนภัยคาร์บอนมอนอกไซด์ ห้ามใช้แทน CO alarm
ที่ได้มาตรฐาน UL 2034 หรือ EN 50291
