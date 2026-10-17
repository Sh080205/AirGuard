#ifndef AIRGUARD_BOARD_PINS_H
#define AIRGUARD_BOARD_PINS_H

/* แหล่งอ้างอิง GPIO: CLAUDE.md §4 เท่านั้น; RX/TX ในชื่อคือชื่อเน็ตบนบอร์ด */
#define BOARD_PIN_CO_TX       5  /* CO_TX: UART2 RX ← ZE07-CO */
#define BOARD_PIN_CO_RX       6  /* CO_RX: UART2 TX → ZE07-CO */
#define BOARD_PIN_PMS_RST    16  /* PMS_RST */
#define BOARD_PIN_PMS_RX     17  /* PMS_RX: UART1 TX → PMS7003 */
#define BOARD_PIN_PMS_TX     18  /* PMS_TX: UART1 RX ← PMS7003 */
#define BOARD_PIN_PMS_SET    21  /* PMS_SET */
#define BOARD_PIN_SDA         8  /* SDA */
#define BOARD_PIN_SCL         9  /* SCL */
#define BOARD_PIN_SD_CS      10  /* SD_CS */
#define BOARD_PIN_SD_MOSI    11  /* SD_MOSI */
#define BOARD_PIN_SD_CLK     12  /* SD_CLK */
#define BOARD_PIN_SD_MISO    13  /* SD_MISO */
#define BOARD_PIN_LED_G       2  /* LED_G: NORMAL */
#define BOARD_PIN_LED_Y      39  /* LED_Y: WATCH */
#define BOARD_PIN_LED_R      40  /* LED_R: ALARM */
#define BOARD_PIN_BUZZ       41  /* BUZZ: MOSFET Q1 */
#define BOARD_PIN_BTN_MARK   42  /* BTN_MARK: internal pull-up */
#define BOARD_PIN_BTN_MODE   47  /* BTN_MODE: internal pull-up */
#define BOARD_PIN_BTN_ACK     1  /* BTN_ACK: internal pull-up */
#define BOARD_PIN_U0TXD      43  /* U0TXD: console */
#define BOARD_PIN_U0RXD      44  /* U0RXD: console */

/* ขาว่างตาม §4: นิยามไว้เท่านั้น FW-1 ไม่ตั้งค่าหรือใช้งาน */
#define BOARD_PIN_SPARE_4     4  /* ไม่มีเน็ตที่ใช้งาน */

/* FW-4: ขาดีบักสำหรับ logic analyzer ใช้ GPIO4 (ขาว่างตาม §4)
 * HIGH ที่ต้นรอบ sampler · LOW เมื่องานในรอบเสร็จ
 * → ระยะ rising→rising = คาบจริง · ความกว้างพัลส์ = เวลาทำงานต่อรอบ */
#define BOARD_PIN_DBG_SAMPLER BOARD_PIN_SPARE_4
#define BOARD_PIN_SPARE_7     7  /* ไม่มีเน็ตที่ใช้งาน */
/* FW-5b: ขาดีบัก latency ของเส้นทาง CO ใช้ GPIO7 (ขาว่างตาม §4)
 * HIGH สั้นๆ หลัง co task ตัดสินเฟรมและส่งสถานะแล้ว จนแจ้ง io task เสร็จ
 * → เทียบกับไบต์สุดท้ายบน GPIO5 และขอบ LED_R/BUZZ ด้วย logic analyzer */
#define BOARD_PIN_DBG_CO      BOARD_PIN_SPARE_7
#define BOARD_PIN_SPARE_15   15  /* ไม่มีเน็ตที่ใช้งาน */
#define BOARD_PIN_SPARE_14   14  /* ไม่มีเน็ตที่ใช้งาน */

#define BOARD_PIN_IS_FORBIDDEN(pin) ( \
    (pin) == 35 || (pin) == 36 || (pin) == 37 || \
    (pin) == 3 || (pin) == 46 || (pin) == 45 || (pin) == 0 || \
    (pin) == 38 || (pin) == 48 || (pin) == 19 || (pin) == 20)

#define BOARD_ASSERT_PIN(pin) \
    _Static_assert(!BOARD_PIN_IS_FORBIDDEN(pin), #pin " is forbidden by CLAUDE.md section 4")

BOARD_ASSERT_PIN(BOARD_PIN_CO_TX);
BOARD_ASSERT_PIN(BOARD_PIN_CO_RX);
BOARD_ASSERT_PIN(BOARD_PIN_PMS_RST);
BOARD_ASSERT_PIN(BOARD_PIN_PMS_RX);
BOARD_ASSERT_PIN(BOARD_PIN_PMS_TX);
BOARD_ASSERT_PIN(BOARD_PIN_PMS_SET);
BOARD_ASSERT_PIN(BOARD_PIN_SDA);
BOARD_ASSERT_PIN(BOARD_PIN_SCL);
BOARD_ASSERT_PIN(BOARD_PIN_SD_CS);
BOARD_ASSERT_PIN(BOARD_PIN_SD_MOSI);
BOARD_ASSERT_PIN(BOARD_PIN_SD_CLK);
BOARD_ASSERT_PIN(BOARD_PIN_SD_MISO);
BOARD_ASSERT_PIN(BOARD_PIN_LED_G);
BOARD_ASSERT_PIN(BOARD_PIN_LED_Y);
BOARD_ASSERT_PIN(BOARD_PIN_LED_R);
BOARD_ASSERT_PIN(BOARD_PIN_BUZZ);
BOARD_ASSERT_PIN(BOARD_PIN_BTN_MARK);
BOARD_ASSERT_PIN(BOARD_PIN_BTN_MODE);
BOARD_ASSERT_PIN(BOARD_PIN_BTN_ACK);
BOARD_ASSERT_PIN(BOARD_PIN_U0TXD);
BOARD_ASSERT_PIN(BOARD_PIN_U0RXD);
BOARD_ASSERT_PIN(BOARD_PIN_SPARE_4);
BOARD_ASSERT_PIN(BOARD_PIN_SPARE_7);
BOARD_ASSERT_PIN(BOARD_PIN_SPARE_15);
BOARD_ASSERT_PIN(BOARD_PIN_SPARE_14);
BOARD_ASSERT_PIN(BOARD_PIN_DBG_SAMPLER);
BOARD_ASSERT_PIN(BOARD_PIN_DBG_CO);

#endif
