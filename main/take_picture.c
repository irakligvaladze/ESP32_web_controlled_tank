/**
 * This example takes a picture every 5s and print its size on serial monitor.
 */

// =============================== SETUP ======================================

// 1. Board setup (Uncomment):
// #define BOARD_WROVER_KIT
// #define BOARD_ESP32CAM_AITHINKER
// #define BOARD_ESP32S3_WROOM
// #define BOARD_ESP32S3_XIAO
// #define BOARD_ESP32S3_GOOUUU
// #define BOARD_ESP32S3_XIAO

/**
 * 2. Kconfig setup
 *
 * If you have a Kconfig file, copy the content from
 *  https://github.com/espressif/esp32-camera/blob/master/Kconfig into it.
 * In case you haven't, copy and paste this Kconfig file inside the src directory.
 * This Kconfig file has definitions that allows more control over the camera and
 * how it will be initialized.
 */

/**
 * 3. Enable PSRAM on sdkconfig:
 *
 * CONFIG_ESP32_SPIRAM_SUPPORT=y
 *
 * More info on
 * https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/kconfig.html#config-esp32-spiram-support
 */

// ================================ CODE ======================================

#include "sdkconfig.h"

#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include <string.h>

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#define PIN_O1 32
#define PIN_O2 33
#define PIN_O3 12
#define PIN_O4 14

// support IDF 5.x
#ifndef portTICK_RATE_MS
#define portTICK_RATE_MS portTICK_PERIOD_MS
#endif

#include "esp_camera.h"

#if defined(CONFIG_CAMERA_AF_SUPPORT) && CONFIG_CAMERA_AF_SUPPORT
#include "esp_camera_af.h"
#endif

volatile camera_fb_t *fb;

bool new_frame_ready = 0;
bool new_frame_used = 0;
uint8_t *frame_copy = NULL;
size_t frame_size = 0;

static int f = 0, b = 0, l = 0, r = 0;

const int motion_loop_ms = 10;
#define BOARD_WROVER_KIT 1

#define WIFI_SSID "i_dont_know"
#define WIFI_PASS "okay1234"

#include "camera_pinout.h"

static const char *TAG = "example:take_picture";

#if ESP_CAMERA_SUPPORTED
static camera_config_t camera_config = {
    .pin_pwdn = CAM_PIN_PWDN,
    .pin_reset = CAM_PIN_RESET,
    .pin_xclk = CAM_PIN_XCLK,
    .pin_sccb_sda = CAM_PIN_SIOD,
    .pin_sccb_scl = CAM_PIN_SIOC,

    .pin_d7 = CAM_PIN_D7,
    .pin_d6 = CAM_PIN_D6,
    .pin_d5 = CAM_PIN_D5,
    .pin_d4 = CAM_PIN_D4,
    .pin_d3 = CAM_PIN_D3,
    .pin_d2 = CAM_PIN_D2,
    .pin_d1 = CAM_PIN_D1,
    .pin_d0 = CAM_PIN_D0,
    .pin_vsync = CAM_PIN_VSYNC,
    .pin_href = CAM_PIN_HREF,
    .pin_pclk = CAM_PIN_PCLK,

    //XCLK 20MHz or 10MHz for OV2640 double FPS (Experimental)
    .xclk_freq_hz = 20000000,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,

    .pixel_format = PIXFORMAT_JPEG, //YUV422,GRAYSCALE,RGB565,JPEG
    .frame_size = FRAMESIZE_QVGA,    //QQVGA-UXGA, For ESP32, do not use sizes above QVGA when not JPEG. The performance of the ESP32-S series has improved a lot, but JPEG mode always gives better frame rates.

    .jpeg_quality = 10, //0-63, for OV series camera sensors, lower number means higher quality
    .fb_count = 1,       //When jpeg mode is used, if fb_count more than one, the driver will work in continuous mode.
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
};

static esp_err_t init_camera(void)
{
    //initialize the camera
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Camera Init Failed");
        return err;
    }

    return ESP_OK;
}

#if defined(CONFIG_CAMERA_AF_SUPPORT) && CONFIG_CAMERA_AF_SUPPORT
static void maybe_init_autofocus(void)
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        ESP_LOGW(TAG, "AF: no sensor handle");
        return;
    }

    if (!esp_camera_af_is_supported(s)) {
        ESP_LOGI(TAG, "AF: not supported by this sensor");
        return;
    }

    esp_camera_af_config_t af_cfg = {
        .mode = ESP_CAMERA_AF_MODE_AUTO,
        .timeout_ms = CONFIG_CAMERA_AF_DEFAULT_TIMEOUT_MS,
    };

    esp_err_t ret = esp_camera_af_init(s, &af_cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AF init failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "AF initialized (AUTO mode)");
}
#endif
#endif

static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    esp_wifi_connect();

    ESP_LOGI(TAG, "WiFi connecting...");
}

static esp_err_t index_handler(httpd_req_t *req)
{
    const char *html =
        "<!DOCTYPE html>"
        "<html>"
        "<head>"
        "<meta name='viewport' content='width=device-width, initial-scale=1.0', meta charset='UTF-8'>"
        "<style>"

        "body {"
        "  font-family: Arial;"
        "  background: #111;"
        "  color: white;"
        "  text-align: center;"
        "}"

        ".container {"
        "  display: flex;"
        "  flex-direction: column;"
        "  align-items: center;"
        "  margin-top: 20px;"
        "}"

        "img {"
        "  border-radius: 10px;"
        "  margin-bottom: 20px;"
        "  border: 2px solid #444;"
        "}"

        ".pad {"
        "  display: grid;"
        "  grid-template-columns: 80px 80px 80px;"
        "  grid-template-rows: 80px 80px 80px;"
        "  gap: 10px;"
        "  justify-content: center;"
        "  align-items: center;"
        "}"

        ".btn {"
        "  width: 80px;"
        "  height: 80px;"
        "  font-size: 24px;"
        "  border: none;"
        "  border-radius: 12px;"
        "  background: #222;"
        "  color: white;"
        "  box-shadow: 0 4px 10px rgba(0,0,0,0.4);"
        "  transition: 0.1s;"
        "}"

        ".btn:active {"
        "  background: #222;"
        "  transform: scale(0.95);"
        "}"

        ".center {"
        "  background: #444;"
        "}"

        ".extra {"
        "  margin-top: 20px;"
        "  padding: 10px 20px;"
        "  font-size: 18px;"
        "  border-radius: 10px;"
        "  background: #333;"
        "  border: none;"
        "  color: white;"
        "}"

        "</style>"
        "</head>"

        "<body>"
        "<div class='container'>"

        "<h2>ESP32 RC Tank</h2>"

        "<img id=\"cam\" width=\"400\" loading=\"eager\"/>"

        "<div class='pad'>"
        "  <div></div>"
        "  <button id='f' class='btn'>⬆</button>"
        "  <div></div>"

        "  <button id='l' class='btn'>⬅</button>"
        "  <div class='center'></div>"
        "  <button id='r' class='btn'>➡</button>"

        "  <div></div>"
        "  <button id='bw' class='btn'>⬇</button>"
        "  <div></div>"
        "</div>"

        "</div>"

        "<script>"

        "let state = {f:0,b:0,l:0,r:0};"
        "let sending = false;"

        "function bind(id, key) {"
        "  const el = document.getElementById(id);"
        "  if (!el) return;"

        "  function press(e) {"
        "    e.preventDefault();"
        "    state[key] = 1;"
        "    el.classList.add('active');"
        "  }"

        "  function release(e) {"
        "    e.preventDefault();"
        "    state[key] = 0;"
        "    el.classList.remove('active');"
        "  }"

        "  el.addEventListener('touchstart', press);"
        "  el.addEventListener('touchend', release);"
        "  el.addEventListener('touchcancel', release);"

        "  el.addEventListener('mousedown', press);"
        "  el.addEventListener('mouseup', release);"
        "}"

        "bind('f','f');"
        "bind('bw','b');"
        "bind('l','l');"
        "bind('r','r');"

        "document.addEventListener('keydown', function(e) {"
        "  if (['ArrowUp','ArrowDown','ArrowLeft','ArrowRight'].includes(e.key)) e.preventDefault();"

        "  if (e.key === 'ArrowUp') { state.f = 1; document.getElementById('f')?.classList.add('active'); }"
        "  if (e.key === 'ArrowDown') { state.b = 1; document.getElementById('bw')?.classList.add('active'); }"
        "  if (e.key === 'ArrowLeft') { state.l = 1; document.getElementById('l')?.classList.add('active'); }"
        "  if (e.key === 'ArrowRight') { state.r = 1; document.getElementById('r')?.classList.add('active'); }"
        "});"

        "document.addEventListener('keyup', function(e) {"
        "  if (e.key === 'ArrowUp') { state.f = 0; document.getElementById('f')?.classList.remove('active'); }"
        "  if (e.key === 'ArrowDown') { state.b = 0; document.getElementById('bw')?.classList.remove('active'); }"
        "  if (e.key === 'ArrowLeft') { state.l = 0; document.getElementById('l')?.classList.remove('active'); }"
        "  if (e.key === 'ArrowRight') { state.r = 0; document.getElementById('r')?.classList.remove('active'); }"
        "});"

        "function controlLoop() {"
        "  if (!sending) {"
        "    sending = true;"
        "    fetch('/control?f=' + state.f + '&b=' + state.b + '&l=' + state.l + '&r=' + state.r)"
        "      .finally(() => { sending = false; });"
        "  }"
        "  setTimeout(controlLoop, 50);"
        "}"

        "controlLoop();"

        "</script>"

        "<script>"

        "let running = true;"

        "function loadFrame() {"
        "  if (!running) return;"

        "  const img = document.getElementById('cam');"

        "  const timeout = setTimeout(() => {"
        "    running = false;"
        "    setTimeout(() => { running = true; loadFrame(); }, 1000);"
        "  }, 500);"

        "  img.onload = () => {"
        "    clearTimeout(timeout);"
        "    setTimeout(loadFrame, 30);"
        "  };"

        "  img.onerror = () => {"
        "    clearTimeout(timeout);"
        "    running = false;"
        "    setTimeout(() => { running = true; loadFrame(); }, 1000);"
        "  };"

        "  img.src = '/frame?t=' + Date.now();"
        "}"

        "loadFrame();"

        "</script>"
        
        "</body>"
        "</html>";

    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}



// static esp_err_t capture_handler(httpd_req_t *req)
// {
//     camera_fb_t *fb = esp_camera_fb_get();
//     if (!fb) {
//         ESP_LOGE(TAG, "Camera capture failed");
//         httpd_resp_send_500(req);
//         return ESP_FAIL;
//     }

//     ESP_LOGI(TAG, "JPEG first bytes: %02X %02X %02X %02X",
//              fb->buf[0], fb->buf[1], fb->buf[2], fb->buf[3]);

//     httpd_resp_set_type(req, "image/jpeg");
//     httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");

//     esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);

//     esp_camera_fb_return(fb);

//     return res;
// }

static esp_err_t control_handler(httpd_req_t *req)
{
    char query[64];


    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
    {
        char param[8];

        if (httpd_query_key_value(query, "f", param, sizeof(param)) == ESP_OK)
            f = atoi(param);

        if (httpd_query_key_value(query, "b", param, sizeof(param)) == ESP_OK)
            b = atoi(param);

        if (httpd_query_key_value(query, "l", param, sizeof(param)) == ESP_OK)
            l = atoi(param);

        if (httpd_query_key_value(query, "r", param, sizeof(param)) == ESP_OK)
            r = atoi(param);
    }


    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}


static esp_err_t frame_handler(httpd_req_t *req)
{
    // if (!fb) {
    //     ESP_LOGE(TAG, "Camera capture failed");
    //     httpd_resp_send_500(req);
    //     return ESP_FAIL;
    // }
    while(!new_frame_ready); // set time limit and then send error response


    httpd_resp_set_type(req, "image/jpeg");

    esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);

    new_frame_used = 1;

    return res;
}

void camera_task(void *pvParameters)
{   
    while(1){
        while(!new_frame_ready){
            fb = esp_camera_fb_get();
            if (fb){
                new_frame_ready = 1;
            }
        }
        if(new_frame_used){
            if(fb){
                esp_camera_fb_return(fb);
            }

            new_frame_ready = 0;
        }
        vTaskDelay(1/portTICK_RATE_MS);
    }
    
}

static httpd_handle_t start_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    

    if (httpd_start(&server, &config) == ESP_OK)
    {
        httpd_uri_t index_uri   = { .uri="/",         .method=HTTP_GET, .handler=index_handler,   .user_ctx=NULL };
        //httpd_uri_t capture_uri = { .uri="/capture",  .method=HTTP_GET, .handler=capture_handler, .user_ctx=NULL };
        httpd_uri_t frame_uri = { .uri = "/frame", .method = HTTP_GET, .handler = frame_handler, .user_ctx = NULL };
        httpd_uri_t control_uri = { .uri = "/control", .method = HTTP_GET, .handler = control_handler, .user_ctx = NULL };

        httpd_register_uri_handler(server, &control_uri);
        httpd_register_uri_handler(server, &frame_uri);
        httpd_register_uri_handler(server, &index_uri);
        //httpd_register_uri_handler(server, &capture_uri);

    }

    return server;
}



void gpio_init(void)
{
    gpio_config_t io_conf = {};

    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask =
        (1ULL << PIN_O1) |
        (1ULL << PIN_O2) |
        (1ULL << PIN_O3) |
        (1ULL << PIN_O4);

    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;

    gpio_config(&io_conf);
}

void stop(void){
    gpio_set_level(PIN_O1, 0);
    gpio_set_level(PIN_O2, 0);
    gpio_set_level(PIN_O3, 0);
    gpio_set_level(PIN_O4, 0);
    vTaskDelay(pdMS_TO_TICKS(motion_loop_ms));
}

void app_main(void)
{   
    f = 0, b = 0, l = 0, r = 0;  // defaults
    gpio_init();

    ESP_LOGI(TAG, "Starting WiFi...");
    wifi_init_sta();

    ESP_LOGI(TAG, "Starting camera...");
    init_camera();

    sensor_t *s = esp_camera_sensor_get();
    s->set_hmirror(s, 1);

    xTaskCreate(
        camera_task,       // Task function
        "camera_task",     // Task name
        8192,          // Stack size in bytes
        NULL,          // Parameters
        5,             // Priority
        NULL           // Task handle
    );

    vTaskDelay(5000 / portTICK_RATE_MS);

    ESP_LOGI(TAG, "Starting server...");
    httpd_handle_t server = start_server();

    if (server == NULL) {
        ESP_LOGE(TAG, "HTTP server failed to start!");
    } else {
        ESP_LOGI(TAG, "HTTP server started OK");
    }

    ESP_LOGI(TAG, "Ready");

    
    while (true) {
        ESP_LOGI(TAG, "%d %d %d %d", f,b,r,l);
        if(((f+b+l+r)==1)||((f+b+l+r)==2)){
            if(f && b){
                stop();
            } else if(l && r) {
                stop();
            } else if(f && l){
                //TURN LEFT WHILE MOVING
                gpio_set_level(PIN_O1, 1);
                gpio_set_level(PIN_O2, 0);
                gpio_set_level(PIN_O3, 1);
                gpio_set_level(PIN_O4, 0);
                vTaskDelay(pdMS_TO_TICKS(motion_loop_ms/2));
                gpio_set_level(PIN_O3, 0);
                gpio_set_level(PIN_O4, 0);
                vTaskDelay(pdMS_TO_TICKS(motion_loop_ms/2));
            } else if(f && r){
                //TURN RIGHT WHILE MOVING
                gpio_set_level(PIN_O1, 1);
                gpio_set_level(PIN_O2, 0);
                gpio_set_level(PIN_O3, 1);
                gpio_set_level(PIN_O4, 0);
                vTaskDelay(pdMS_TO_TICKS(motion_loop_ms/2));
                gpio_set_level(PIN_O1, 0);
                gpio_set_level(PIN_O2, 0);
                vTaskDelay(pdMS_TO_TICKS(motion_loop_ms/2));
            } else if(b && l){
                //TURN LEFT WHILE MOVING
                gpio_set_level(PIN_O1, 0);
                gpio_set_level(PIN_O2, 1);
                gpio_set_level(PIN_O3, 0);
                gpio_set_level(PIN_O4, 1);
                vTaskDelay(pdMS_TO_TICKS(motion_loop_ms/2));
                gpio_set_level(PIN_O3, 0);
                gpio_set_level(PIN_O4, 0);
                vTaskDelay(pdMS_TO_TICKS(motion_loop_ms/2));
            } else if(b && r){
                //TURN RIGHT WHILE MOVING
                gpio_set_level(PIN_O1, 0);
                gpio_set_level(PIN_O2, 1);
                gpio_set_level(PIN_O3, 0);
                gpio_set_level(PIN_O4, 1);
                vTaskDelay(pdMS_TO_TICKS(motion_loop_ms/2));
                gpio_set_level(PIN_O1, 0);
                gpio_set_level(PIN_O2, 0);
                vTaskDelay(pdMS_TO_TICKS(motion_loop_ms/2));
            } else if(f) {
                //MOVE FORWARD
                gpio_set_level(PIN_O1, 1);
                gpio_set_level(PIN_O2, 0);
                gpio_set_level(PIN_O3, 1);
                gpio_set_level(PIN_O4, 0);
                vTaskDelay(pdMS_TO_TICKS(motion_loop_ms));
            } else if(b){
                //MOVE BACKWARD
                gpio_set_level(PIN_O1, 0);
                gpio_set_level(PIN_O2, 1);
                gpio_set_level(PIN_O3, 0);
                gpio_set_level(PIN_O4, 1);
                vTaskDelay(pdMS_TO_TICKS(motion_loop_ms));
            } else if(l){
                //TURN LEFT
                gpio_set_level(PIN_O1, 1);
                gpio_set_level(PIN_O2, 0);
                gpio_set_level(PIN_O3, 0);
                gpio_set_level(PIN_O4, 1);
                vTaskDelay(pdMS_TO_TICKS(motion_loop_ms));
            } else if(r){
                //TURN RIGHT
                gpio_set_level(PIN_O1, 0);
                gpio_set_level(PIN_O2, 1);
                gpio_set_level(PIN_O3, 1);
                gpio_set_level(PIN_O4, 0);
                vTaskDelay(pdMS_TO_TICKS(motion_loop_ms));
            }
        } else {
            stop(); 
        }
        

        //ESP_LOGI("CTRL", "F=%d B=%d L=%d R=%d", f, b, l, r);

                
    }
}

