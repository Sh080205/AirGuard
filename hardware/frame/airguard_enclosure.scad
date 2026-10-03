// =====================================================================
//  AirGuard-S3 — กล่องปิด-เปิดได้ (enclosure: body + lid)
//  พัฒนาต่อจาก airguard_frame.scad — ตำแหน่งภายในใช้ชุดเดิม ฐานเดิม = พื้นกล่อง
//  OpenSCAD 2021.01+ · หน่วย mm · ทุกมิติอยู่ในส่วนพารามิเตอร์ด้านบน
//
//  ป้ายที่มาของค่า:
//    [DXF]     = DXF_PCB3_2026-10-03_AutoCAD2007.dxf (export จาก EasyEDA)  ยืนยันแล้ว
//    [CLAUDE]  = CLAUDE.md §6 (ยืนยันกับ Espressif แล้ว)
//    [MANUAL]  = PMS7003 series data manual V2.5 (Plantower)
//    [TODO]    = ยังไม่มีแหล่ง → ค่าในไฟล์เป็น "ตัวยึดตำแหน่ง" ห้ามเชื่อ
//               ต้องวัด/เปิดเอกสาร แล้วเปลี่ยนธง ok_* เป็น true
//
//  ระบบพิกัด: (0,0) = มุมซ้ายล่างของขอบบอร์ด carrier (ตามมุมมองใน EasyEDA ด้านบน)
//             +x = ขวา · +y = ขึ้น (ฝั่ง J7/C13) · z = 0 ที่ใต้ฐาน
//
//  สั่ง export (ดู README.md):
//    openscad -D 'part="fit_board"'  -o fit_board.stl  airguard_frame.scad
//    openscad -D 'part="fit_coupon"' -o fit_coupon.stl airguard_frame.scad
//    openscad -D 'part="fit_pms"'    -o fit_pms.stl    airguard_frame.scad
//    openscad -D 'part="frame"'      -o frame.stl      airguard_frame.scad
//      ↑ จะ assert ไม่ผ่านจนกว่าธง ok_* จะเป็น true ทั้งหมด
//        (ใส่ -D allow_draft=true เพื่อดูร่างเท่านั้น ห้ามพิมพ์ร่าง)
// =====================================================================

/* [Build] */
part        = "enc_preview"; // "enc_body" | "enc_lid" | "enc_preview" | "fit_board" | "fit_coupon" | "fit_pms"
allow_draft = false;     // true = ยอมเรนเดอร์ frame ทั้งที่ยังมี TODO (ไว้ดูหน้าตาเท่านั้น)
$fn = 48;

// =====================================================================
// 0) ธงยืนยัน — เปลี่ยนเป็น true เมื่อใส่ค่าจริงในหัวข้อนั้นแล้วเท่านั้น
// =====================================================================
ok_devkit_orient = false; // ยืนยันด้วยตาว่าโมดูล/เสาอากาศอยู่ฝั่ง J7-C13 และ USB อยู่ฝั่ง J8-BZ1
ok_pcb_t         = false; // ความหนา PCB ที่สั่ง JLCPCB (ดูใบสั่ง)
ok_under_leads   = false; // ขา THT ที่ยื่นใต้บอร์ดยาวสุด (วัดหลังตัดขา)
ok_print_fit     = false; // fit_clear / m3_tap_d / zt_* เลือกจาก fit_coupon ที่พิมพ์จริง
ok_printer       = false; // ขนาดฐานพิมพ์ของเครื่องที่ใช้
ok_pms_ports     = false; // ด้านที่มีช่องลม + ตำแหน่งครีบกั้น (วัดจากตัวจริงหรือรูปใน manual)
ok_ze07_dims     = false; // ขนาด ZE07-CO (วัดเวอร์เนียร์ — manual ฉบับข้อความไม่มีมิติ)
ok_bme_dims      = false; // ขนาดโมดูล ENS160+AHT21 ที่ซื้อมา
ok_sd_dims       = false; // ขนาดโมดูล microSD + ด้านที่ช่องเสียบการ์ดอยู่
ok_modules_used  = false; // ยืนยันว่าใช้ OLED / SCD40 จริงหรือไม่
ok_oled_dims     = false; // ขนาดโมดูล OLED (ถ้า use_oled)
ok_scd40_dims    = false; // ขนาดโมดูล SCD40 (ถ้า use_scd40)
ok_ant_keepout   = false; // รัศมีเว้นโลหะรอบเสาอากาศ จาก ESP32-S3-WROOM-1 datasheet
ok_bme_sep       = false; // ระยะห่างขั้นต่ำ AHT21 ↔ ESP32/LDO (ยังไม่มีแหล่งอ้างอิง)

// =====================================================================
// 1) Carrier PCB — จาก DXF (ทั้งหมดแปลงเป็นพิกัดเทียบมุมซ้ายล่างของขอบบอร์ดแล้ว)
//    DXF origin offset = (-1.651, -2.921)
// =====================================================================
pcb_w      = 82.126;  // [DXF] Board-Outline
pcb_h      = 72.921;  // [DXF]
pcb_hole_d = 3.2;     // [DXF] Hole-Layer → รู M3
pcb_holes  = [        // [DXF] Hole-Layer (ไม่สมมาตรเป๊ะ — ใช้ค่าตาม DXF)
    [ 3.810,  3.683],  // H1 ซ้ายล่าง
    [78.232,  3.683],  // H2 ขวาล่าง
    [78.359, 69.596],  // H3 ขวาบน
    [ 3.937, 69.215]   // H4 ซ้ายบน
];
// กรอบ DevKit จาก silkscreen U1: 25.40 × 62.74 ตรงกับ CLAUDE.md §6
devkit_rect = [[33.911, 3.171], [59.311, 65.911]]; // [DXF]+[CLAUDE]
// pad สี่เหลี่ยม (pin 1 = 3V3) อยู่ปลายบน + R14/R15 (console) อยู่ปลายบน
// → โมดูล/เสาอากาศอยู่ปลาย y สูง, USB อยู่ปลาย y ต่ำ  [อนุมานจาก DXF → ok_devkit_orient]
ant_ref    = [(33.911 + 59.311) / 2, 65.911];  // กึ่งกลางขอบปลายเสาอากาศของ DevKit
j8_rect    = [[16.001, 0.671], [26.161, 11.371]]; // [DXF] silkscreen J8 (ไฟเข้า)

pcb_t = 1.6;            // [TODO ok_pcb_t] ค่ามาตรฐาน JLCPCB แต่ต้องดูใบสั่ง

// =====================================================================
// 2) การพิมพ์ — [TODO ok_print_fit / ok_printer] ใช้ fit_coupon เลือกค่า
//    รุ่นเครื่อง / nozzle / วัสดุ: ยังไม่ทราบ
// =====================================================================
fit_clear   = 0.3;   // ช่องว่างรอบโมดูลแต่ละด้าน
m3_tap_d    = 2.7;   // รูให้สกรู M3 เกลียวปล่อยกัดพลาสติก (ไม่ใช้ insert ทองเหลือง = ไม่มีโลหะเพิ่ม)
zt_w        = 3.0;   // ช่องเคเบิลไทร์: ความกว้าง (ตามเคเบิลไทร์ที่มี)
zt_t        = 1.6;   // ช่องเคเบิลไทร์: ความหนา
bed_x       = 220;   // ฐานพิมพ์
bed_y       = 220;

// =====================================================================
// 3) ฐาน + เสายึดบอร์ด
// =====================================================================
plate_t        = 3.0;
plate_r        = 4.0;   // รัศมีมุมฐาน
board_margin   = 4.0;   // ขอบฐานเลยขอบบอร์ด (ซ้าย/ขวา/บน)
board_margin_b = 0.0;   // ด้านล่าง (USB + J8) = ชิดขอบบอร์ด ให้สายงอลงได้
board_window   = true;  // เจาะช่องใต้บอร์ด ลดเวลาพิมพ์ + ระบายความร้อน
window_inset   = 11;    // ระยะขอบช่องจากขอบบอร์ด

under_lead_max = 3.0;   // [TODO ok_under_leads] ขา THT ยาวสุดใต้บอร์ด
under_margin   = 2.0;   // เผื่อจากปลายขาถึงผิวฐาน
standoff_h     = under_lead_max + under_margin; // ความสูงเสาเหนือผิวฐาน
standoff_od    = 7.0;

// =====================================================================
// 4) โซนเซนเซอร์ (อยู่ฝั่งซ้ายของบอร์ด = ฝั่ง J1–J5) — ตำแหน่ง [x,y] = มุมซ้ายล่างของ footprint
// =====================================================================
zone_margin   = 7.0; // ขอบฐานด้านซ้ายของโซน (ต้องพอให้ช่องเคเบิลไทร์ทะลุฐานได้)
zone_margin_y = 3.0; // ขอบฐานบน/ล่างของโซน (บางไว้ ให้นิ้วถึงช่อง microSD)
post_leg    = 5.0;   // ความยาวขาของเสามุมรูปตัว L
post_t      = 2.0;

// ---- PMS7003 ---- [MANUAL] 48 × 37 × 12, เปลือกโลหะต่อ GND
pms_L = 48; pms_W = 37; pms_H = 12;
// [TODO ok_pms_ports] ช่องลมเข้า/ออกอยู่ด้านไหน + ตรงไหน
pms_ports_on_long_side = true;  // true = ช่องลมอยู่บนหน้า 48 × 12
pms_fin_at   = 24;   // ระยะตามแนวหน้าช่องลม (จากมุมล่าง) ถึงครีบกั้น = ระหว่างช่องเข้ากับช่องออก
pms_fin_len  = 0;    // กล่อง: ผนังซ้ายทำหน้าที่กั้น inlet/outlet แทนครีบ (manual: plane of inlet/outlet ชิดผนัง host)
pms_fin_t    = 1.6;
pms_inlet_c  = 12;   // (ไว้แสดงในภาพ preview) กึ่งกลางช่องเข้า
pms_outlet_c = 36;   //                       กึ่งกลางช่องออก
pms_port_d   = 8;    //                       ขนาดช่อง
pms_pos      = [-75, 26];
pms_ties     = 2;

// ---- ZE07-CO ---- [TODO ok_ze07_dims]  (32×22 มาจาก docs/3d.html ซึ่งไม่มีแหล่งอ้างอิง)
ze07_dx = 22; ze07_dy = 32; ze07_H = 15;
ze07_pos = [-27, 38];
ze07_tie_at = 0.25;   // ตำแหน่งสายรัด (สัดส่วนตามแกน y) — ให้พ้นตัวเซนเซอร์ทรงกระบอก [TODO ตามโมดูลจริง]

// ---- ENS160 + AHT21 บนเสาตั้ง ---- [TODO ok_bme_dims]
bme_W = 20; bme_L = 26; bme_T = 1.6;   // กว้าง (แกน x) · สูง (แกน z บนเสา) · หนา PCB
bme_center_z = 20;     // ความสูงกึ่งกลางโมดูล (กล่อง: ลดลงให้พ้นฝา)
bme_pos  = [-66, 90];  // มุมซ้ายของเสา (เสาหันหน้าออก +y)
mast_t   = 2.4;
bme_min_sep = 30;      // [TODO ok_bme_sep] ตัวเลขนี้มาจาก docs/3d.html ไม่มีแหล่งอ้างอิง

// ---- microSD module ---- [TODO ok_sd_dims]  ช่องการ์ดหัน -y ชิดขอบฐาน
sd_dx = 20; sd_dy = 24; sd_H = 4;
sd_pos = [-47, -4];   // กล่อง: ย้ายมาตรงกลาง ให้มุมหน้าซ้ายว่างสำหรับเสาสกรูฝา

// ---- OLED SSD1306 บนแท่นเอียง ---- [TODO ok_modules_used / ok_oled_dims]
use_oled  = true;
oled_W = 27; oled_H = 27;  // ขนาด PCB โมดูล
oled_tilt = 60;            // องศาจากแนวนอน (เลือกตามมุมมองที่ต้องการ)
oled_pos  = [-47, -4];

// ---- SCD40 ---- [TODO ok_modules_used / ok_scd40_dims]  README ระบุว่าเป็น "ตัวเลือก"
use_scd40 = false;
scd_dx = 22; scd_dy = 24; scd_H = 8;
scd_pos = [-29, 8];

// ---- เสาอากาศ ---- [TODO ok_ant_keepout] ESP32-S3-WROOM-1 datasheet หัวข้อ antenna/placement
ant_keepout_r = 15;

// =====================================================================
// 5) ตรวจ TODO
// =====================================================================
todo = [
    if (!ok_devkit_orient) "devkit orientation (antenna end)",
    if (!ok_pcb_t)         "pcb_t",
    if (!ok_under_leads)   "under_lead_max",
    if (!ok_print_fit)     "fit_clear / m3_tap_d / zt_w / zt_t  (print fit_coupon)",
    if (!ok_printer)       "bed_x / bed_y",
    if (!ok_pms_ports)     "pms_ports_on_long_side / pms_fin_at",
    if (!ok_ze07_dims)     "ze07_dx / ze07_dy / ze07_H",
    if (!ok_bme_dims)      "bme_W / bme_L / bme_T",
    if (!ok_sd_dims)       "sd_dx / sd_dy (+ card slot side)",
    if (!ok_modules_used)  "use_oled / use_scd40",
    if (use_oled  && !ok_oled_dims)  "oled_W / oled_H",
    if (use_scd40 && !ok_scd40_dims) "scd_dx / scd_dy",
    if (!ok_ant_keepout)   "ant_keepout_r",
    if (!ok_bme_sep)       "bme_min_sep"
];

// =====================================================================
// 6) เรขาคณิตที่คำนวณได้
// =====================================================================
pms_dx = pms_ports_on_long_side ? pms_W : pms_L;   // หน้าช่องลมหัน -x เสมอ
pms_dy = pms_ports_on_long_side ? pms_L : pms_W;

mast_w      = bme_W + 2 * fit_clear + 10;
mast_h      = bme_center_z + bme_L / 2 + 4;
mast_gusset = 12;
shelf_d     = bme_T + 2 * fit_clear + 1.5;
oled_pw     = oled_W + 2 * fit_clear + 6;
oled_ph     = oled_H + 2 * fit_clear + 6;

function R(p, d)    = [p, p + d];                   // rect จากมุม + ขนาด
function grow(r, m) = [r[0] - [m, m], r[1] + [m, m]];
function pt_rect_dist(p, r) = let(
    dx = max(r[0][0] - p[0], 0, p[0] - r[1][0]),
    dy = max(r[0][1] - p[1], 0, p[1] - r[1][1])) sqrt(dx * dx + dy * dy);
function rect_rect_dist(a, b) = let(
    dx = max(b[0][0] - a[1][0], a[0][0] - b[1][0], 0),
    dy = max(b[0][1] - a[1][1], a[0][1] - b[1][1], 0)) sqrt(dx * dx + dy * dy);

pms_rect  = R(pms_pos, [pms_dx, pms_dy]);
pms_ext   = [pms_rect[0] - [pms_fin_len + fit_clear, 0], pms_rect[1]];  // รวมครีบ
ze07_rect = R(ze07_pos, [ze07_dx, ze07_dy]);
sd_rect   = R(sd_pos, [sd_dx, sd_dy]);
scd_rect  = R(scd_pos, [scd_dx, scd_dy]);
mast_rect = [bme_pos - [0, mast_gusset], bme_pos + [mast_w, mast_t + shelf_d]];
bme_c2d   = bme_pos + [mast_w / 2, mast_t];        // กึ่งกลางโมดูลในแนวราบ
oled_front = (2.4 + 3) * sin(oled_tilt);          // ความหนาแผง+ขอบ ที่ยื่นไปทาง -y หลังเอียง
oled_rect = R(oled_pos, [oled_pw, oled_front + oled_ph * cos(oled_tilt)]);

zone_rects = concat(
    [pms_ext, ze07_rect, sd_rect, mast_rect],
    use_oled  ? [oled_rect] : [],
    use_scd40 ? [scd_rect]  : []);
module_rects = concat(
    [pms_rect, ze07_rect, sd_rect, mast_rect],
    use_oled  ? [oled_rect] : [],
    use_scd40 ? [scd_rect]  : []);

zone_bb = [
    [min([for (r = zone_rects) r[0][0]]), min([for (r = zone_rects) r[0][1]])],
    [max([for (r = zone_rects) r[1][0]]), max([for (r = zone_rects) r[1][1]])]];
board_bb = [[-board_margin, -board_margin_b], [pcb_w + board_margin, pcb_h + board_margin]];
// ฐาน = สองสี่เหลี่ยมต่อกัน: ฝั่งบอร์ด (ขอบล่างชิดบอร์ด ให้สาย USB/J8 งอลงได้) + ฝั่งเซนเซอร์
zone_plate_bb = [[zone_bb[0][0] - zone_margin, zone_bb[0][1] - zone_margin_y],
                 [board_bb[0][0] + 2 * plate_r, zone_bb[1][1] + zone_margin_y]];
plate_bb = [[zone_plate_bb[0][0], min(zone_plate_bb[0][1], board_bb[0][1])],
            [board_bb[1][0],      max(zone_plate_bb[1][1], board_bb[1][1])]];
plate_size = plate_bb[1] - plate_bb[0];

// =====================================================================
// 7) รายงานตัวเลขตรวจสอบ (ดูใน console)
// =====================================================================
echo(str("PLATE size = ", plate_size[0], " x ", plate_size[1], " mm"));
for (i = [0 : 3]) echo(str("H", i + 1, " screw → antenna ref = ",
    round(norm(pcb_holes[i] - ant_ref) * 10) / 10, " mm",
    norm(pcb_holes[i] - ant_ref) < ant_keepout_r ? "  !! inside keepout → ใช้สกรูไนลอน" : ""));
for (r = module_rects) echo(str("module rect ", r, " → antenna = ",
    round(pt_rect_dist(ant_ref, r) * 10) / 10, " mm"));
bme_sep = pt_rect_dist(bme_c2d, devkit_rect);
echo(str("AHT21 centre → DevKit outline (2D) = ", round(bme_sep * 10) / 10, " mm"));
echo(str("PMS7003 body → antenna ref = ", round(pt_rect_dist(ant_ref, pms_rect) * 10) / 10,
         " mm  (เปลือกโลหะ)"));
if (len(todo) > 0) echo(str("TODO (", len(todo), "): ", todo));

// =====================================================================
// 8) ชิ้นส่วนพื้นฐาน
// =====================================================================
module rrect(bb, r) {   // สี่เหลี่ยมมุมมน 2D จาก bounding box
    translate(bb[0] + [r, r]) offset(r = r) square(bb[1] - bb[0] - [2 * r, 2 * r]);
}

module standoff(h) {
    difference() {
        cylinder(d = standoff_od, h = h);
        translate([0, 0, -1]) cylinder(d = m3_tap_d, h = h + 2);
    }
}

// เสามุม L รอบ footprint (ไม่ปิดผนังเต็ม → ลมผ่านได้ สายเข้าได้)
module corner_posts(rect, h) {
    c = fit_clear;
    lo = rect[0] - [c, c]; hi = rect[1] + [c, c];
    for (sx = [0, 1], sy = [0, 1]) {
        p = [sx ? hi[0] : lo[0], sy ? hi[1] : lo[1]];   // มุมของช่องว่าง
        // ขาแนว x (อยู่นอกขอบบน/ล่าง)
        translate([sx ? p[0] - post_leg : p[0] - post_t, sy ? p[1] : p[1] - post_t, 0])
            cube([post_leg + post_t, post_t, h]);
        // ขาแนว y (อยู่นอกขอบซ้าย/ขวา)
        translate([sx ? p[0] : p[0] - post_t, sy ? p[1] - post_leg : p[1] - post_t, 0])
            cube([post_t, post_leg + post_t, h]);
    }
}

// ช่องเคเบิลไทร์ทะลุฐาน สองข้างของโมดูล (สายรัดคร่อมโมดูลตามแกน x)
module tie_slots_x(rect, n, fracs = undef) {
    c = fit_clear;
    len_y = rect[1][1] - rect[0][1];
    fs = (fracs == undef) ? [for (k = [1 : n]) k / (n + 1)] : fracs;
    for (f = fs) {
        y = rect[0][1] + len_y * f;
        for (x = [rect[0][0] - c - post_t - 1.2 - zt_t, rect[1][0] + c + post_t + 1.2])
            translate([x, y - zt_w / 2 - 0.2, -1]) cube([zt_t + 0.3, zt_w + 0.4, plate_t + 2]);
    }
}

// แบบเดียวกัน แต่สายรัดคร่อมตามแกน y (ช่องอยู่ด้านบน/ล่างของโมดูล)
module tie_slots_y(rect, n) {
    c = fit_clear;
    len_x = rect[1][0] - rect[0][0];
    for (k = [1 : n]) {
        x = rect[0][0] + len_x * k / (n + 1);
        for (y = [rect[0][1] - c - post_t - 1.2 - zt_t, rect[1][1] + c + post_t + 1.2])
            translate([x - zt_w / 2 - 0.2, y, -1]) cube([zt_w + 0.4, zt_t + 0.3, plate_t + 2]);
    }
}

// =====================================================================
// 9) ตัวยึดแต่ละโมดูล
// =====================================================================
module pms_cradle(h = pms_H * 0.6) {
    corner_posts(pms_rect, h);
    // ครีบกั้นระหว่าง inlet/outlet  (manual: "some shield should be placed between
    // inset and outset in order to prevent the air flow from inner loop")
    if (pms_fin_len > 0)
    translate([pms_rect[0][0] - fit_clear - pms_fin_len,
               pms_rect[0][1] + pms_fin_at - pms_fin_t / 2, 0])
        cube([pms_fin_len, pms_fin_t, pms_H + 2]);
}

module bme_mast() {
    x0 = bme_pos[0]; y0 = bme_pos[1];
    zc = bme_center_z;
    difference() {
        union() {
            translate([x0, y0, 0]) cube([mast_w, mast_t, mast_h]);
            // ชั้นรองขอบล่างโมดูล + ขอบกันตก
            translate([x0, y0 + mast_t, zc - bme_L / 2 - fit_clear - 2]) {
                cube([mast_w, shelf_d, 2]);
                translate([0, shelf_d - 1.2, 0]) cube([mast_w, 1.2, 4]);
            }
            // ค้ำด้านหลัง (ฝั่ง -y) สองข้าง
            for (x = [x0, x0 + mast_w - 2])
                translate([x, y0, 0]) hull() {
                    cube([2, 0.01, mast_h * 0.8]);
                    translate([0, -mast_gusset, 0]) cube([2, mast_gusset, 0.01]);
                }
        }
        // ช่องเคเบิลไทร์สองเส้น ทะลุเสา ซ้าย-ขวาของโมดูล
        for (z = [zc - bme_L / 4, zc + bme_L / 4])
            for (x = [x0 + mast_w / 2 - bme_W / 2 - fit_clear - 1.2 - zt_t,
                      x0 + mast_w / 2 + bme_W / 2 + fit_clear + 1.2])
                translate([x, y0 - 1, z - zt_w / 2 - 0.2]) cube([zt_t + 0.3, mast_t + 2, zt_w + 0.4]);
        // ช่องระบายด้านหลังโมดูล (ไม่ให้ความร้อนสะสมระหว่างโมดูลกับเสา)
        translate([x0 + mast_w / 2 - bme_W / 4, y0 - 1, zc - bme_L / 4 + zt_w])
            cube([bme_W / 2, mast_t + 2, bme_L / 2 - 2 * zt_w]);
    }
}

module oled_stand() {
    t = 2.4;
    translate([oled_pos[0], oled_pos[1] + oled_front, plate_t - 0.6]) {   // จมลงฐาน 0.6 กันขอบแตะเป็นเส้น (non-manifold)
        difference() {
            rotate([oled_tilt, 0, 0]) union() {
                cube([oled_pw, oled_ph, t]);
                cube([oled_pw, 2, t + 3]);   // ขอบล่างกันโมดูลไหล
            }
            // ช่องเคเบิลไทร์บนแผงเอียง
            rotate([oled_tilt, 0, 0])
                for (x = [1, oled_pw - 1 - zt_t - 0.3])
                    translate([x, oled_ph / 2 - zt_w / 2 - 0.2, -1]) cube([zt_t + 0.3, zt_w + 0.4, t + 5]);
        }
        for (x = [0, oled_pw - 2]) hull() {
            translate([x, 0, 0]) cube([2, oled_ph * cos(oled_tilt), 0.01]);
            translate([x, 0, 0]) rotate([oled_tilt, 0, 0]) cube([2, oled_ph, 0.01]);
        }
    }
}

// =====================================================================
// 10) ประกอบ
// =====================================================================
module plate2d() {
    difference() {
        union() { rrect(board_bb, plate_r); rrect(zone_plate_bb, plate_r);
                  // เติมมุมด้านในตรงรอยต่อ
                  translate([board_bb[0][0], max(zone_plate_bb[0][1], board_bb[0][1])])
                      square([2 * plate_r, min(zone_plate_bb[1][1], board_bb[1][1]) - max(zone_plate_bb[0][1], board_bb[0][1])]); }
        if (board_window)
            translate([window_inset, window_inset])
                square([pcb_w - 2 * window_inset, pcb_h - 2 * window_inset]);
    }
}

// แยกเป็นชิ้นย่อย (ใช้ export แยกสีใน 3D viewer) — frame() = union ของทั้งหมด
module frame_plate() {
    difference() {
        linear_extrude(plate_t) plate2d();
        tie_slots_x(pms_rect, pms_ties);
        tie_slots_x(ze07_rect, 1, [ze07_tie_at]);   // แนว x ด้านล่างโมดูล ไม่พาดทับหน้ารับก๊าซ
        tie_slots_x(sd_rect, 1);
        if (use_scd40) tie_slots_x(scd_rect, 1);
        for (p = pcb_holes) translate([p[0], p[1], -1]) cylinder(d = m3_tap_d, h = plate_t + 2);
    }
    translate([0, 0, plate_t]) for (p = pcb_holes) translate(p) standoff(standoff_h);
}
module frame_pms()  translate([0, 0, plate_t]) pms_cradle();
module frame_ze07() translate([0, 0, plate_t]) corner_posts(ze07_rect, min(6, ze07_H * 0.5));
module frame_sd()   translate([0, 0, plate_t]) corner_posts(sd_rect,   min(4, sd_H + 1));
module frame_scd()  translate([0, 0, plate_t]) corner_posts(scd_rect,  min(5, scd_H * 0.6));

module frame() {
    frame_plate();
    frame_pms();
    frame_ze07();
    frame_sd();
    if (use_scd40) frame_scd();
    bme_mast();     // วางบนผิว z = 0 แล้วยืดขึ้นทะลุฐาน (union กับฐาน)
    if (use_oled) oled_stand();
}

// ---- ภาพผีของชิ้นจริง (เฉพาะ preview — z ของ DevKit/โมดูลไม่ใช่มิติจริง) ----
module ghosts() {
    zb = plate_t + standoff_h;
    color([0.1, 0.5, 0.2, 0.55]) translate([0, 0, zb]) difference() {
        cube([pcb_w, pcb_h, pcb_t]);
        for (p = pcb_holes) translate([p[0], p[1], -1]) cylinder(d = pcb_hole_d, h = 5);
    }
    color([0.1, 0.1, 0.1, 0.6]) translate([devkit_rect[0][0], devkit_rect[0][1], zb + pcb_t + 8])
        cube([devkit_rect[1][0] - devkit_rect[0][0], devkit_rect[1][1] - devkit_rect[0][1], 1.6]);
    color([1, 0.2, 0.2, 0.25]) translate([ant_ref[0], ant_ref[1], zb]) cylinder(r = ant_keepout_r, h = 0.4);
    color([0.6, 0.6, 0.65, 0.6]) translate([pms_rect[0][0], pms_rect[0][1], plate_t]) cube([pms_dx, pms_dy, pms_H]);
    for (c = [pms_inlet_c, pms_outlet_c])
        color([0.2, 0.4, 1, 0.8]) translate([pms_rect[0][0] - 0.5, pms_rect[0][1] + c, plate_t + pms_H / 2])
            rotate([0, 90, 0]) cylinder(d = pms_port_d, h = 1);
    color([0.2, 0.3, 0.8, 0.5]) translate([ze07_rect[0][0], ze07_rect[0][1], plate_t]) cube([ze07_dx, ze07_dy, ze07_H]);
    color([0.5, 0.2, 0.6, 0.6]) translate([bme_c2d[0] - bme_W / 2, bme_c2d[1] + fit_clear, bme_center_z - bme_L / 2])
        cube([bme_W, bme_T, bme_L]);
    color([0.2, 0.3, 0.8, 0.5]) translate([sd_rect[0][0], sd_rect[0][1], plate_t]) cube([sd_dx, sd_dy, sd_H]);
}

// =====================================================================
// 11) ชิ้นทดสอบความพอดี — พิมพ์ก่อนชิ้นจริง
// =====================================================================
// (A) วงแหวนตามขอบบอร์ด + เสาสูงจริง 4 ต้น → เช็ครูตรง DXF, สกรูกินเกลียว, ขาใต้บอร์ดไม่ชนฐาน
module fit_board() {
    t = 1.2;
    difference() {
        linear_extrude(t) difference() {
            offset(delta = fit_clear + 2) square([pcb_w, pcb_h]);
            offset(delta = -6) square([pcb_w, pcb_h]);
        }
        for (p = pcb_holes) translate([p[0], p[1], -1]) cylinder(d = m3_tap_d, h = t + 2);
    }
    // ครีบมุมกันบอร์ดขยับ สูงเลยผิวเสา 1 mm  → ทาบแล้วเห็นทันทีว่าขอบตรงไหม
    for (sx = [0, 1], sy = [0, 1])
        translate([sx ? pcb_w + fit_clear : -fit_clear - 1.2, sy ? pcb_h - 8 : 0, 0])
            cube([1.2, 8, t + standoff_h + 1]);
    // เสา (ฐานบางกว่าชิ้นจริง จึงชดเชยความสูงให้ผิวบนเสาอยู่ระดับเดียวกับชิ้นจริงเทียบผิวฐาน)
    for (p = pcb_holes) translate([p[0], p[1], t]) standoff(standoff_h);
    // ป้ายบอกความสูงเสา
    translate([pcb_w / 2 - 14, 1.5, t]) linear_extrude(0.6)
        text(str("SO ", standoff_h, "  M3 ", m3_tap_d), size = 3, font = "DejaVu Sans:style=Bold");
}

// (B) แผ่นวัดค่ารูและช่อง → เลือก m3_tap_d, fit_clear ของรูทะลุ, zt_w/zt_t
module fit_coupon() {
    tap  = [2.4, 2.5, 2.6, 2.7, 2.8, 2.9, 3.0];
    thru = [3.2, 3.3, 3.4, 3.5, 3.6];
    ztw  = [2.5, 3.0, 3.5, 4.0, 4.5];
    w = 80; h = 40; t = 4;
    difference() {
        cube([w, h, t]);
        for (i = [0 : len(tap) - 1])  translate([6 + i * 11, 31, -1]) cylinder(d = tap[i], h = t + 2);
        for (i = [0 : len(thru) - 1]) translate([6 + i * 11, 19, -1]) cylinder(d = thru[i], h = t + 2);
        for (i = [0 : len(ztw) - 1])  translate([4 + i * 11, 4, -1]) cube([zt_t + 0.3, ztw[i], t + 2]);
        // ป้ายแกะลึก 0.6
        for (i = [0 : len(tap) - 1])  translate([3 + i * 11, 35.5, t - 0.6]) linear_extrude(1)
            text(str(tap[i] * 10), size = 2.6, font = "DejaVu Sans:style=Bold");
        for (i = [0 : len(thru) - 1]) translate([3 + i * 11, 23.5, t - 0.6]) linear_extrude(1)
            text(str(thru[i] * 10), size = 2.6, font = "DejaVu Sans:style=Bold");
        for (i = [0 : len(ztw) - 1])  translate([7 + i * 11, 5, t - 0.6]) linear_extrude(1)
            text(str(ztw[i] * 10), size = 2.6, font = "DejaVu Sans:style=Bold");
    }
}

// (C) แท่นวาง PMS7003 + ครีบ (ใช้มิติจาก manual) — เช็คขนาดพอดี และครีบอยู่ระหว่างช่องลมจริงไหม
module fit_pms() {
    t = 1.2;
    translate(-pms_rect[0] + [pms_fin_len + 4, 4, 0]) {
        linear_extrude(t) difference() {
            offset(delta = fit_clear + post_t + 1) translate(pms_rect[0]) square([pms_dx, pms_dy]);
            offset(delta = -6) translate(pms_rect[0]) square([pms_dx, pms_dy]);
        }
        translate([pms_rect[0][0] - fit_clear - pms_fin_len, pms_rect[0][1] + pms_fin_at - 3, 0])
            cube([pms_fin_len, 6, t]);   // แผ่นรองครีบ
        translate([0, 0, t]) pms_cradle(pms_H);
    }
}

// =====================================================================
// 12) กล่อง — ค่าที่ต้องวัดดูใน README / หน้า "ค่าที่ต้องวัด" ของ viewer (รหัส A1…)
// =====================================================================
ok_heights  = false;  // A3 A4 A5 A9 A11 — ความสูงซ็อกเก็ต / DevKit / USB / ชิ้นสูงสุด / ปุ่ม
ok_openings = false;  // A6 A7 A8 A10 — ตำแหน่งและขนาดช่อง USB, J8 + ขนาดหัวปลั๊ก
ok_gasket   = false;  // ความหนาฟองน้ำระหว่าง PMS กับผนัง
ok_oled_win = false;  // F2 — พื้นที่แสดงผลของ OLED และตำแหน่ง
ok_lightpipe= false;  // วิธีพา LED ขึ้นฝา (ท่อแสง / ผนังบาง) — ต้องพิมพ์ทดสอบ
ok_service  = false;  // H — ร้านพิมพ์: กระบวนการ วัสดุ ค่าเผื่อ

wall_t    = 2.4;
floor_t   = plate_t;    // พื้นกล่อง = ระดับฐานเดิม → z ภายในทุกตัวใช้ต่อได้
lid_t     = 2.4;
lid_lip   = 3.0;        // ขอบฝาที่สอดลงในกล่อง
lip_t     = 1.6;
lid_clear = 0.3;
box_r     = 6;
gasket_t  = 2.0;        // [TODO ok_gasket]

in_x0 = pms_pos[0] - fit_clear - gasket_t;   // ผนังซ้ายแนบหน้าช่องลม PMS (ผ่านฟองน้ำ)
in_x1 = pcb_w + 10;                          // เผื่อเสาสกรูฝามุมขวา
in_y0 = sd_pos[1] - fit_clear;               // ผนังหน้าแนบช่องการ์ด microSD
in_y1 = 98;
in_h  = 38;             // [TODO ok_heights] พื้น → ใต้ฝา

boss_d = 8; boss_in = 5;
bosses = [[in_x0 + boss_in, in_y0 + boss_in], [in_x1 - boss_in, in_y0 + boss_in],
          [in_x1 - boss_in, in_y1 - boss_in], [in_x0 + boss_in, in_y1 - boss_in]];

div_x = -2.5; div_t = 1.6;              // ผนังกั้นห้องเซนเซอร์ | ห้องบอร์ด
div_win = [[10, 70], [14, 30]];         // ช่องร้อยสาย: ช่วง y, ช่วง z (อุดฟองน้ำหลังร้อยสาย)

socket_h = 8.5;         // [TODO A3] ผิวบน PCB → ผิวบนซ็อกเก็ต
sw_h     = 2.8;         // [TODO A11] ผิวบน PCB → ปลายปุ่ม
usb_x    = [40.1, 53.1];// [TODO A6] กึ่งกลาง micro-USB สองช่อง
usb_dz   = 1.5;         // [TODO A5] ผิวบน DevKit → กึ่งกลางรู USB
usb_cut  = [12, 8];     // [TODO A8] ช่องเผื่อหัวปลั๊ก (กว้าง × สูง)
j8_x     = 21.08;       // [DXF] กึ่งกลาง J8
j8_dz    = 4;           // [TODO A10] ผิวบน PCB → ระดับรูสาย J8
j8_d     = 7;           // [TODO A10] รูสายไฟ
sd_slot  = [3, 14, 6];  // [TODO E2] ช่องการ์ด: ระยะจากขอบซ้ายโมดูล, กว้าง, สูง
oled_wx0 = -50;         // ขอบซ้าย OLED บนผนังหน้า
oled_T   = 1.6;
oled_win = [23, 12];    // [TODO F2] พื้นที่แสดงผล
oled_win_dz = 1.5;      // [TODO F2] ศูนย์กลางจอ เลื่อนขึ้นจากกึ่งกลาง PCB
pms_open = [10, 8];     // [TODO B2] ช่องลม PMS บนผนัง (y × z) ต้องไม่เล็กกว่าช่องของเซนเซอร์

zTop    = plate_t + standoff_h + pcb_t;
zDK     = zTop + socket_h;
usb_zc  = zDK + 1.6 + usb_dz;
lid_z   = floor_t + in_h;
oled_z0 = lid_z - oled_H - 4;

led_xy = [[70.99, 61.47], [71.32, 45.67], [71.12, 50.67], [70.99, 55.5]];  // [DXF] LED1–LED4
sw_xy  = [[74.93, 34.41], [75.95, 25.41], [66.29, 25.02]];                 // [DXF] SW1–SW3
sw_dir = [[1, 0], [0, -1], [-1, 0]];    // ทิศโคนลิ้นกดของแต่ละปุ่ม (เลี่ยงชนกัน)
bz_xy  = [67.82, 11.68];                                                     // [DXF] BZ1

enc_todo = [
    if (!ok_heights)   "socket_h / sw_h / usb_dz / in_h (A3 A4 A5 A9 A11)",
    if (!ok_openings)  "usb_x / usb_cut / j8_dz / j8_d (A6 A7 A8 A10)",
    if (!ok_gasket)    "gasket_t",
    if (!ok_oled_win)  "oled_win / oled_win_dz (F2)",
    if (!ok_lightpipe) "LED light pipe",
    if (!ok_service)   "print service tolerance (H)"
];
outer_bb = [[in_x0 - wall_t, in_y0 - wall_t], [in_x1 + wall_t, in_y1 + wall_t]];
inner_bb = [[in_x0, in_y0], [in_x1, in_y1]];
echo(str("BOX outer = ", outer_bb[1][0] - outer_bb[0][0], " x ", outer_bb[1][1] - outer_bb[0][1],
         " x ", lid_z + lid_t, " mm"));
for (b = bosses) echo(str("lid screw ", b, " → antenna = ", round(norm(b - ant_ref) * 10) / 10, " mm"));
if (len(enc_todo) > 0) echo(str("ENCLOSURE TODO: ", enc_todo));

// ---------- ชิ้นย่อย ----------
module slots_h(x0, x1, z0, z1, pitch = 4, h = 2) {   // แถวช่องแนวนอน (2D: x × z)
    for (z = [z0 : pitch : z1 - h]) translate([x0, z]) square([x1 - x0, h]);
}
module wall_cut_front(x0, x1, z0, z1) translate([x0, in_y0 - wall_t - 1, z0]) cube([x1 - x0, wall_t + 2, z1 - z0]);

module oled_wall_mount() {
    t = oled_T + 0.4; y0 = in_y0;
    xl = oled_wx0 - fit_clear; xr = oled_wx0 + oled_W + fit_clear;
    for (s = [0, 1]) {
        x = s ? xr : xl;
        translate([0, 0, oled_z0]) difference() {
            translate([s ? x - 1.5 : x - 2.5, y0, 0]) cube([4, t + 1.2, oled_H]);
            translate([s ? x - 1.6 : x - 0.4, y0 - 1, -1]) cube([2, t + 1, oled_H + 2]);
        }
    }
    translate([xl - 2.5, y0, oled_z0 - 2]) cube([xr - xl + 5, t + 1.2, 2]);
}

module boss(b) {
    hull() {
        translate([b[0], b[1], 0]) cylinder(d = boss_d, h = lid_z);
        cx = b[0] < 0 ? in_x0 : in_x1 - 0.1;  cy = b[1] < 40 ? in_y0 : in_y1 - 0.1;
        translate([cx, cy, 0]) cube([0.1, 0.1, lid_z]);
    }
}

module enc_body() {
    difference() {
        union() {
            linear_extrude(floor_t) rrect(outer_bb, box_r);
            linear_extrude(lid_z) difference() { rrect(outer_bb, box_r); rrect(inner_bb, box_r - wall_t); }
            for (b = bosses) boss(b);
            translate([div_x - div_t / 2, in_y0, 0]) cube([div_t, in_y1 - in_y0, lid_z - 0.5]);
            translate([0, 0, plate_t]) for (p = pcb_holes) translate(p) standoff(standoff_h);
            frame_pms(); frame_ze07(); frame_sd();
            bme_mast();
            if (use_oled) oled_wall_mount();
        }
        // เกลียวสกรูฝา
        for (b = bosses) translate([b[0], b[1], lid_z - 12]) cylinder(d = m3_tap_d, h = 13);
        // ช่องในพื้น
        tie_slots_x(pms_rect, pms_ties);
        tie_slots_x(ze07_rect, 1, [ze07_tie_at]);
        tie_slots_x(sd_rect, 1);
        for (p = pcb_holes) translate([p[0], p[1], -1]) cylinder(d = m3_tap_d, h = plate_t + 2);
        // ร่องยางรองใต้กล่อง
        for (b = bosses) translate([b[0] + (b[0] < 0 ? 8 : -8), b[1], -0.01]) cylinder(d = 10.4, h = 1);
        // ข้อความบังคับ (CLAUDE.md §8) — ฉบับย่อ แกะใต้กล่อง
        translate([(in_x0 + in_x1) / 2, (in_y0 + in_y1) / 2, -0.01]) mirror([1, 0, 0]) linear_extrude(0.6)
            text("RESEARCH MONITOR - NOT A CO ALARM - NOT A MEDICAL DEVICE", size = 3.2,
                 halign = "center", valign = "center", font = "DejaVu Sans:style=Bold");
        // ---- ผนังหน้า ----
        for (x = usb_x) wall_cut_front(x - usb_cut[0] / 2, x + usb_cut[0] / 2, usb_zc - usb_cut[1] / 2, usb_zc + usb_cut[1] / 2);
        translate([j8_x, in_y0 - wall_t - 1, zTop + j8_dz]) rotate([-90, 0, 0]) cylinder(d = j8_d, h = wall_t + 2);
        wall_cut_front(sd_pos[0] + sd_slot[0], sd_pos[0] + sd_slot[0] + sd_slot[1], floor_t + 0.5, floor_t + sd_slot[2]);
        if (use_oled) {
            cxw = oled_wx0 + oled_W / 2; czw = oled_z0 + oled_H / 2 + oled_win_dz;
            wall_cut_front(cxw - oled_win[0] / 2, cxw + oled_win[0] / 2, czw - oled_win[1] / 2, czw + oled_win[1] / 2);
        }
        // ลมเข้าห้องเซนเซอร์ (ต่ำ ด้านหน้า)
        translate([0, in_y0 + 1, 0]) rotate([90, 0, 0]) linear_extrude(wall_t + 2) {
            slots_h(in_x0 + 12, sd_pos[0] - 5, floor_t + 2, floor_t + 12);
            slots_h(sd_pos[0] + sd_dx + 5, div_x - 4, floor_t + 2, floor_t + 10);
        }
        // ---- ผนังซ้าย: ช่องลม PMS (ผนังระหว่างสองช่อง = แผ่นกั้นตาม manual) ----
        for (c = [pms_inlet_c, pms_outlet_c])
            translate([in_x0 - wall_t - 1, pms_pos[1] + c - pms_open[0] / 2, floor_t + pms_H / 2 - pms_open[1] / 2])
                cube([wall_t + 2, pms_open[0], pms_open[1]]);
        // ---- ผนังหลัง: ลมผ่าน ENS160+AHT21 ----
        translate([0, in_y1 - 1, 0]) rotate([90, 0, 0]) mirror([0, 0, 1]) linear_extrude(wall_t + 2)
            slots_h(bme_c2d[0] - 12, bme_c2d[0] + 12, bme_center_z - 12, bme_center_z + 12);
        // ---- ผนังขวา: ลมเข้าห้องบอร์ด (ต่ำ) ----
        translate([in_x1 - 1, 0, 0]) rotate([90, 0, 90]) linear_extrude(wall_t + 2)
            slots_h(15, 60, floor_t + 2, floor_t + 8);
        // ---- ผนังกั้น: ช่องร้อยสาย + ช่องให้เคเบิลไทร์ ZE07 ลอด ----
        translate([div_x - 2, div_win[0][0], div_win[1][0]]) cube([4, div_win[0][1] - div_win[0][0], div_win[1][1] - div_win[1][0]]);
        translate([div_x - 2, ze07_rect[0][1] + (ze07_rect[1][1] - ze07_rect[0][1]) * ze07_tie_at - zt_w / 2 - 1, floor_t - 0.01])
            cube([4, zt_w + 2, 6]);
    }
}

module flex_button(c, d) {      // ลิ้นกดบนฝา (เส้นตัดรูปตัว U) — ความแข็งต้องพิมพ์ทดสอบ
    a = atan2(d[1], d[0]); w = 6;
    translate([c[0], c[1]]) rotate(a) difference() {
        translate([-4, -(w / 2 + 1)]) square([17, w + 2]);
        translate([-3, -w / 2]) square([17, w]);
    }
}

module enc_lid() {
    plunger = lid_z - (zTop + sw_h) - 0.5;
    difference() {
        union() {
            translate([0, 0, lid_z]) linear_extrude(lid_t) rrect(outer_bb, box_r);
            translate([0, 0, lid_z - lid_lip]) linear_extrude(lid_lip + 0.01) difference() {
                offset(delta = -lid_clear) rrect(inner_bb, box_r - wall_t);
                offset(delta = -lid_clear - lip_t) rrect(inner_bb, box_r - wall_t);
            }
            for (c = sw_xy) translate([c[0], c[1], lid_z - plunger]) cylinder(d = 4, h = plunger + 0.01);
        }
        for (b = bosses) {
            translate([b[0], b[1], lid_z - lid_lip - 1]) cylinder(d = boss_d + 1.5, h = lid_lip + 1);  // หลบเสา
            translate([b[0], b[1], lid_z - 1]) cylinder(d = 3.4, h = lid_t + 2);
            translate([b[0], b[1], lid_z + lid_t - 1.8]) cylinder(d1 = 3.4, d2 = 6.8, h = 1.81);         // หัวเรียบ
        }
        translate([0, 0, lid_z - 1]) linear_extrude(lid_t + 2) {
            for (i = [0 : 2]) flex_button(sw_xy[i], sw_dir[i]);
            for (l = led_xy) translate(l) circle(d = 3);                                   // [TODO ok_lightpipe]
            translate(bz_xy) { circle(d = 1.8); for (k = [0 : 5]) rotate(k * 60) translate([3.5, 0]) circle(d = 1.8); }
            for (x = [37 : 4 : 51]) translate([x, 14]) square([2, 42]);                    // ลมร้อนออก เหนือ DevKit
            for (x = [-24 : 4 : -9]) translate([x, ze07_rect[0][1] + 4]) square([2, 24]);  // เหนือ ZE07
        }
    }
}

module enc_ghost_lid() %enc_lid();

// =====================================================================
// 13) ตัวเลือก part
// =====================================================================
if (part == "enc_body" || part == "enc_lid") {
    assert(allow_draft || len(todo) + len(enc_todo) == 0,
        str("ยังมี TODO: ", concat(todo, enc_todo), " — ห้ามสั่งพิมพ์"));
    if (part == "enc_body") enc_body(); else enc_lid();
} else if (part == "fit_board") {
    fit_board();
} else if (part == "fit_coupon") {
    fit_coupon();
} else if (part == "fit_pms") {
    fit_pms();
} else {               // enc_preview
    enc_body();
    translate([0, 0, 25]) enc_lid();
}
