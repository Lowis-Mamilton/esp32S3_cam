#include "mxcam_display.h"

#include <string.h>

#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "mxcam_lcd";

/* Pins per docs/SPEC.md section 4. MISO (GPIO46, strapping pin) is NOT used. */
#define PIN_SCLK 3
#define PIN_MOSI 45
#define PIN_CS   14
#define PIN_DC   47
#define PIN_RST  21

#define LCD_HOST      SPI2_HOST
#define STRIP_LINES   20
#define STRIP_BYTES   (MXCAM_LCD_W * STRIP_LINES * 2)
#define NUM_STRIPS    (MXCAM_LCD_H / STRIP_LINES)

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_bounce[2];
static SemaphoreHandle_t s_free; /* counts free bounce buffers (starts at 2) */

static bool on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    (void)io; (void)edata; (void)ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_free, &woken);
    return woken == pdTRUE;
}

esp_err_t mxcam_display_init(void)
{
    s_free = xSemaphoreCreateCounting(2, 2);
    if (!s_free) return ESP_ERR_NO_MEM;
    for (int i = 0; i < 2; i++) {
        s_bounce[i] = heap_caps_malloc(STRIP_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_bounce[i]) {
            ESP_LOGE(TAG, "no memory for bounce buffers");
            return ESP_ERR_NO_MEM;
        }
    }

    spi_bus_config_t bus = ILI9341_PANEL_BUS_SPI_CONFIG(PIN_SCLK, PIN_MOSI, STRIP_BYTES);
    esp_err_t err = spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = ILI9341_PANEL_IO_SPI_CONFIG(PIN_CS, PIN_DC, on_trans_done, NULL);
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io);
    if (err != ESP_OK) return err;

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        /* The old TFT_eSPI setup relied on the controller default (BGR bit in MADCTL). */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_ili9341(io, &panel_cfg, &s_panel);
    if (err != ESP_OK) return err;

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    /*
     * Landscape orientation. To flip: change the mirror() flags below
     * (mirror(false,true), mirror(true,true) or mirror(false,false)); swap_xy
     * stays true for landscape. Colours are not handled here: byte order is
     * preserved by sending the camera data unswapped.
     */
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true)); /* backlight is hard-wired on */
    ESP_LOGI(TAG, "ILI9341 ready (landscape %dx%d, strips of %d lines)",
             MXCAM_LCD_W, MXCAM_LCD_H, STRIP_LINES);
    return ESP_OK;
}

esp_err_t mxcam_display_draw(const uint16_t *pixels)
{
    if (!s_panel || !pixels) return ESP_ERR_INVALID_STATE;
    for (int strip = 0; strip < NUM_STRIPS; strip++) {
        /* Wait for a free bounce buffer. Transfers complete in queue order, so
         * buffer (strip & 1) is only refilled after its previous use finished. */
        if (xSemaphoreTake(s_free, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
        uint16_t *buf = s_bounce[strip & 1];
        memcpy(buf, pixels + (size_t)strip * MXCAM_LCD_W * STRIP_LINES, STRIP_BYTES);
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, strip * STRIP_LINES,
                                                  MXCAM_LCD_W, (strip + 1) * STRIP_LINES, buf);
        if (err != ESP_OK) {
            xSemaphoreGive(s_free);
            return err;
        }
    }
    return ESP_OK;
}
