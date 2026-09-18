#include "web_server.h"
#include "sdcard_mgr.h"
#include "playback_engine.h"
#include "rgbv_format.h"
#include "nvs_mgr.h"
#include "ff.h"
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_timer.h>
#include <lwip/sockets.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#define UPLOAD_BOUNCE_BUF_SIZE (64 * 1024)

static const char *TAG = "WEB_SERVER";

#define EXAMPLE_ESP_WIFI_SSID      "NDT-EYE"
#define EXAMPLE_ESP_WIFI_PASS      "prodndt2310"
#define EXAMPLE_MAX_STA_CONN       4

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

static httpd_handle_t s_server = NULL;

static void drain_req_body(httpd_req_t *req, size_t remaining) {
    if (remaining == 0) return;
    ESP_LOGI(TAG, "Đang xả bỏ (drain) %zu byte còn lại của request POST để ngăn socket leak...", remaining);
    char scratch[512];
    int retries = 0;
    while (remaining > 0 && retries < 10) {
        int r = httpd_req_recv(req, scratch, MIN(remaining, sizeof(scratch)));
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                retries++;
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            break;
        }
        remaining -= r;
        retries = 0;
    }
}

static esp_err_t send_500_error_with_drain(httpd_req_t *req, size_t remaining, const char *msg) {
    ESP_LOGE(TAG, "Lỗi Upload 500: %s", msg);
    drain_req_body(req, remaining);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
    return ESP_FAIL;
}

static TaskHandle_t s_dns_task_handle = NULL;

static void dns_server_task(void *pvParameters) {
    char rx_buffer[128];
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(53); // Standard DNS Port
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Không thể tạo DNS socket");
        vTaskDelete(NULL);
        return;
    }

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Không thể bind DNS socket port 53");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS Captive Portal Server đã chạy trên Cổng 53 (Redirect 100% tên miền về 192.168.4.1)");

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&client_addr, &addr_len);
        if (len > 12) {
            // Chuẩn bị phản hồi DNS A Record trỏ tất cả domain về 192.168.4.1
            rx_buffer[2] |= 0x80; // Response flag
            rx_buffer[3] |= 0x80;
            rx_buffer[6] = 0x00; // Answer count = 1
            rx_buffer[7] = 0x01;

            int reply_len = len;
            rx_buffer[reply_len++] = 0xc0; // Pointer to query domain name
            rx_buffer[reply_len++] = 0x0c;
            rx_buffer[reply_len++] = 0x00; // Type A
            rx_buffer[reply_len++] = 0x01;
            rx_buffer[reply_len++] = 0x00; // Class IN
            rx_buffer[reply_len++] = 0x01;
            rx_buffer[reply_len++] = 0x00; // TTL (60s)
            rx_buffer[reply_len++] = 0x00;
            rx_buffer[reply_len++] = 0x00;
            rx_buffer[reply_len++] = 0x3c;
            rx_buffer[reply_len++] = 0x00; // Data length (4 bytes IP)
            rx_buffer[reply_len++] = 0x04;
            rx_buffer[reply_len++] = 192;  // IP: 192.168.4.1
            rx_buffer[reply_len++] = 168;
            rx_buffer[reply_len++] = 4;
            rx_buffer[reply_len++] = 1;

            sendto(sock, rx_buffer, reply_len, 0, (struct sockaddr *)&client_addr, addr_len);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t wifi_init_softap(void) {
    ESP_LOGI(TAG, "Đang khởi chạy Wi-Fi SoftAP SSID: %s", EXAMPLE_ESP_WIFI_SSID);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .ssid_len = strlen(EXAMPLE_ESP_WIFI_SSID),
            .channel = 1,
            .password = EXAMPLE_ESP_WIFI_PASS,
            .max_connection = EXAMPLE_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    if (strlen(EXAMPLE_ESP_WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT40));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    // Khởi chạy DNS Captive Portal Server
    if (!s_dns_task_handle) {
        xTaskCreate(dns_server_task, "dns_server_task", 3072, NULL, 5, &s_dns_task_handle);
    }

    ESP_LOGI(TAG, "Wi-Fi SoftAP đã khởi chạy (Captive Portal DNS Active). Kết nối tới SSID '%s' -> Auto Pop-up 192.168.4.1",
             EXAMPLE_ESP_WIFI_SSID);
    return ESP_OK;
}

// -------------------------------------------------------------
// HTML Web Dashboard UI
// -------------------------------------------------------------
static const char INDEX_HTML[] = 
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>NDT-EYE Video Manager</title>"
"<style>"
"body{font-family:system-ui,-apple-system,sans-serif;background:#0f172a;color:#f8fafc;margin:0;padding:20px;display:flex;flex-direction:column;align-items:center;}"
".container{max-width:800px;width:100%;background:#1e293b;border-radius:16px;padding:24px;box-shadow:0 10px 25px rgba(0,0,0,0.5);}"
"h1{margin-top:0;color:#38bdf8;font-size:24px;text-align:center;}"
".card{background:#334155;border-radius:12px;padding:16px;margin-bottom:20px;}"
".upload-box{border:2px dashed #0ea5e9;border-radius:12px;padding:20px;text-align:center;cursor:pointer;background:#0f172a55;}"
"input[type=file]{display:none;}"
"button{background:#0284c7;color:#fff;border:none;padding:10px 18px;border-radius:8px;font-weight:bold;cursor:pointer;transition:0.2s;}"
"button:hover{background:#0369a1;}"
".btn-danger{background:#ef4444;}.btn-danger:hover{background:#dc2626;}"
".file-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:16px;margin-top:16px;}"
".file-card{background:#1e293b;border:2px solid #475569;border-radius:12px;overflow:hidden;display:flex;flex-direction:column;align-items:center;padding:12px;position:relative;}"
".file-card.active{border-color:#10b981;box-shadow:0 0 15px rgba(16,185,129,0.4);}"
".file-card img{width:160px;height:160px;object-fit:cover;border-radius:8px;background:#000;}"
".file-name{margin:8px 0;font-weight:600;font-size:14px;word-break:break-all;text-align:center;}"
".actions{display:flex;gap:8px;width:100%;justify-content:center;margin-top:auto;}"
".badge{background:#10b981;color:#000;font-size:10px;font-weight:bold;padding:2px 6px;border-radius:4px;position:absolute;top:8px;right:8px;}"
".info-box{background:#0284c722;border:1px solid #0284c7;border-radius:8px;padding:12px;margin-bottom:16px;font-size:13px;line-height:1.5;color:#e0f2fe;}"
".progress-container{display:none;margin-top:16px;background:#0f172a;border-radius:10px;padding:14px;border:1px solid #0ea5e9;}"
".progress-bar-bg{background:#334155;height:18px;border-radius:9px;overflow:hidden;margin-bottom:8px;}"
".progress-bar-fill{background:linear-gradient(90deg,#0284c7,#10b981);height:100%;width:0%;transition:width 0.15s ease-out;}"
".progress-text{display:flex;justify-content:space-between;font-size:13px;font-weight:600;color:#38bdf8;}"
"</style></head><body>"
"<div class='container'>"
"<h1>NDT-EYE LCD Video Controller</h1>"
"<div class='info-box'>"
"📌 <b>Chế độ Wi-Fi / Quản lý Video:</b> Tải tệp mới lên thẻ SD hoặc chọn video cần phát. Sau khi chọn video, bạn có thể nhấn nút <b>Khởi Động Lại</b> bên dưới để thiết bị phát ngay."
" <button onclick='restartDevice()' style='margin-left:10px;background:#ef4444;color:#fff;border:none;padding:6px 14px;border-radius:6px;cursor:pointer;font-weight:bold;'>🔄 Khởi Động Lại Thiết Bị</button>"
"</div>"
"<div class='card'>"
"<h3>Tải Tệp Video .rgbv Mới</h3>"
"<form id='upForm'>"
"<div class='upload-box' onclick=\"document.getElementById('fileInput').click()\">"
"<span>Bấm vào đây để chọn tệp .rgbv tải lên thẻ nhớ SD</span>"
"<input type='file' id='fileInput' name='file' accept='.rgbv' onchange=\"uploadSelectedFile()\">"
"</div>"
"</form>"
"<div id='progressBox' class='progress-container'>"
"<div class='progress-bar-bg'><div id='progFill' class='progress-bar-fill'></div></div>"
"<div class='progress-text'><span id='progLabel'>Đang tải lên: 0%</span><span id='progStats'>0 MB / 0 MB</span></div>"
"</div>"
"</div>"
"<div class='card'>"
"<h3>Danh Sách Video Trên Thẻ Nhớ SD (/sdcard)</h3>"
"<div id='fileList' class='file-grid'>Đang tải danh sách...</div>"
"</div>"
"</div>"
"<script>"
"async function restartDevice(){"
"if(!confirm('Bạn có chắc chắn muốn khởi động lại mạch NDT-EYE?')) return;"
"try{"
"await fetch('/api/restart',{method:'POST'});"
"alert('🔄 Thiết bị đang khởi động lại...');"
"}catch(e){"
"alert('🔄 Đã phát lệnh khởi động lại.');"
"}"
"}"
"async function loadFiles(){"
"let res=await fetch('/api/list');"
"let data=await res.json();"
"let html='';"
"data.files.forEach(f=>{"
"let active=f.name===data.current?'active':'';"
"let badge=f.name===data.current?\"<span class='badge'>ĐÃ CHỌN</span>\":'';"
"html+=`<div class='file-card ${active}'>${badge}`+"
"`<img src='/api/thumb?file=${encodeURIComponent(f.name)}' alt='thumb'/>`+"
"`<div class='file-name'>${f.name}</div>`+"
"`<div class='actions'>`+"
"`<button onclick=\"playFile('${f.name}')\">Chọn Phát</button>`+"
"`<button class='btn-danger' onclick=\"deleteFile('${f.name}')\">Xóa</button>`+"
"`</div></div>`;"
"});"
"document.getElementById('fileList').innerHTML=html||'Không có tệp .rgbv nào trên thẻ nhớ SD';"
"}"
"function uploadSelectedFile(){"
"let fileInput=document.getElementById('fileInput');"
"if(!fileInput.files||fileInput.files.length===0) return;"
"let file=fileInput.files[0];"
"let formData=new FormData();"
"formData.append('file',file);"
"let pBox=document.getElementById('progressBox');"
"let pFill=document.getElementById('progFill');"
"let pLabel=document.getElementById('progLabel');"
"let pStats=document.getElementById('progStats');"
"pBox.style.display='block';"
"pFill.style.width='0%';"
"pLabel.innerText='Đang chuẩn bị gửi...';"
"pStats.innerText='0 MB / '+(file.size/(1024*1024)).toFixed(1)+' MB';"
"let xhr=new XMLHttpRequest();"
"xhr.open('POST','/api/upload',true);"
"let startTime=Date.now();"
"xhr.upload.onprogress=function(e){"
"if(e.lengthComputable){"
"let pct=(e.loaded/e.total*100).toFixed(1);"
"let loadedMB=(e.loaded/(1024*1024)).toFixed(1);"
"let totalMB=(e.total/(1024*1024)).toFixed(1);"
"let elapsedSec=(Date.now()-startTime)/1000;"
"let speedMBs=elapsedSec>0?(e.loaded/(1024*1024)/elapsedSec).toFixed(2):'0.00';"
"pFill.style.width=pct+'%';"
"pLabel.innerText='Đang tải lên SD: '+pct+'% ('+speedMBs+' MB/s)';"
"pStats.innerText=loadedMB+' MB / '+totalMB+' MB';"
"}"
"};"
"xhr.onload=function(){"
"if(xhr.status>=200&&xhr.status<400){"
"pFill.style.width='100%';"
"pLabel.innerText='✅ Tải tệp thành công! Đã tự động chọn tệp mới.';"
"alert('✅ Đã tải tệp '+file.name+' thành công lên thẻ nhớ SD!\\n\\n👉 Vui lòng nhấn nút RESET thiết bị để bắt đầu phát video.');"
"loadFiles();"
"}else{"
"pLabel.innerText='❌ Lỗi khi tải tệp ('+xhr.status+')';"
"alert('❌ Lỗi tải tệp: '+xhr.statusText);"
"}"
"};"
"xhr.onerror=function(){"
"pLabel.innerText='❌ Lỗi kết nối Wi-Fi!';"
"alert('❌ Thất bại: Gián đoạn kết nối Wi-Fi.');"
"};"
"xhr.send(formData);"
"}"
"async function playFile(name){"
"let fd=new FormData();fd.append('file',name);"
"let res=await fetch('/api/play',{method:'POST',body:fd});"
"let data=await res.json();"
"alert('✅ Đã chọn video: '+name+'\\n\\n👉 Vui lòng nhấn nút RESET (hoặc rút nguồn cắm lại) để thiết bị bắt đầu phát video này.');"
"loadFiles();"
"}"
"async function deleteFile(name){"
"if(!confirm('Bạn có chắc muốn xóa tệp '+name+'?')) return;"
"let fd=new FormData();fd.append('file',name);"
"await fetch('/api/delete',{method:'POST',body:fd});"
"loadFiles();"
"}"
"loadFiles();"
"</script></body></html>";

static esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

// Handler bắt tất cả request kiểm tra Wi-Fi của hệ điều hành (Android / iOS / Windows / macOS)
static esp_err_t captive_portal_handler(httpd_req_t *req) {
    if (strcmp(req->uri, "/") == 0) {
        return root_get_handler(req);
    }
    // Chuyển hướng 302 về IP gốc 192.168.4.1
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

// -------------------------------------------------------------
// GET /api/list
// -------------------------------------------------------------
static esp_err_t list_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");

    DIR *dir = opendir(SDCARD_MOUNT_POINT);
    if (!dir) {
        return httpd_resp_send_500(req);
    }

    char buf[1024];
    int len = snprintf(buf, sizeof(buf), "{\"current\":\"%s\",\"files\":[", playback_engine_get_current_file());

    struct dirent *entry;
    bool first = true;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG && strstr(entry->d_name, ".rgbv")) {
            if (!first) len += snprintf(buf + len, sizeof(buf) - len, ",");
            first = false;
            len += snprintf(buf + len, sizeof(buf) - len, "{\"name\":\"%s\"}", entry->d_name);
        }
    }
    closedir(dir);

    len += snprintf(buf + len, sizeof(buf) - len, "]}");
    return httpd_resp_send(req, buf, len);
}

// -------------------------------------------------------------
// GET /api/thumb?file=filename.rgbv
// Reads embedded JPEG block from header byte 32 without decoding on ESP32!
// -------------------------------------------------------------
static esp_err_t thumb_get_handler(httpd_req_t *req) {
    char url_buf[256];
    char filename[128] = {0};

    if (httpd_req_get_url_query_str(req, url_buf, sizeof(url_buf)) == ESP_OK) {
        httpd_query_key_value(url_buf, "file", filename, sizeof(filename));
    }

    if (strlen(filename) == 0) {
        return httpd_resp_send_404(req);
    }

    char filepath[300];
    snprintf(filepath, sizeof(filepath), "%s/%s", SDCARD_MOUNT_POINT, filename);

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        return httpd_resp_send_404(req);
    }

    rgbv_header_t header;
    if (fread(&header, 1, sizeof(header), f) != sizeof(header) || memcmp(header.magic, "RGBV", 4) != 0) {
        fclose(f);
        return httpd_resp_send_500(req);
    }

    if (header.thumb_size == 0) {
        fclose(f);
        return httpd_resp_send_404(req);
    }

    // Seek to exact byte 32 where JPEG thumbnail data is stored
    fseek(f, 32, SEEK_SET);

    httpd_resp_set_type(req, "image/jpeg");

    char buffer[1024];
    size_t remaining = header.thumb_size;
    while (remaining > 0) {
        size_t to_read = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        size_t read_bytes = fread(buffer, 1, to_read, f);
        if (read_bytes == 0) break;
        httpd_resp_send_chunk(req, buffer, read_bytes);
        remaining -= read_bytes;
    }

    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

// -------------------------------------------------------------
// POST /api/play (form-data field: file)
// In 1-way Wi-Fi mode: Saves target filename to NVS for next boot
// -------------------------------------------------------------
static esp_err_t play_post_handler(httpd_req_t *req) {
    char buf[256] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return httpd_resp_send_500(req);

    char *filename = strstr(buf, "name=\"file\"\r\n\r\n");
    if (filename) {
        filename += strlen("name=\"file\"\r\n\r\n");
        char *end = strstr(filename, "\r\n");
        if (end) *end = '\0';
    } else {
        filename = buf;
    }

    ESP_LOGI(TAG, "Đã lưu video mới vào NVS: %s", filename);
    nvs_mgr_set_current_file(filename);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"saved\",\"message\":\"Đã chọn video. Vui lòng nhấn nút RESET thiết bị để phát.\"}", HTTPD_RESP_USE_STRLEN);
}

// -------------------------------------------------------------
// POST /api/delete (form-data field: file)
// Unlinks file from SD card VFS
// -------------------------------------------------------------
static esp_err_t delete_post_handler(httpd_req_t *req) {
    char buf[256] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return httpd_resp_send_500(req);

    char *filename = strstr(buf, "name=\"file\"\r\n\r\n");
    if (filename) {
        filename += strlen("name=\"file\"\r\n\r\n");
        char *end = strstr(filename, "\r\n");
        if (end) *end = '\0';
    } else {
        filename = buf;
    }

    char current_file[128] = {0};
    if (nvs_mgr_get_current_file(current_file, sizeof(current_file)) == ESP_OK) {
        if (strcmp(current_file, filename) == 0) {
            nvs_mgr_set_current_file("");
        }
    }

    char filepath[300];
    snprintf(filepath, sizeof(filepath), "%s/%s", SDCARD_MOUNT_POINT, filename);
    unlink(filepath);

    ESP_LOGI(TAG, "Đã xóa tệp: %s", filepath);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"deleted\"}", HTTPD_RESP_USE_STRLEN);
}

// -------------------------------------------------------------
// POST /api/restart
// Restart ESP32-S3 immediately
// -------------------------------------------------------------
static esp_err_t restart_post_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"Đang khởi động lại thiết bị...\"}", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// -------------------------------------------------------------
// POST /api/upload
// High-Speed 64KB Block Accumulation & Direct FatFs SDMMC Write
// -------------------------------------------------------------
// -------------------------------------------------------------
// POST /api/upload
// High-Speed PSRAM Async Ring Buffer + SDMMC Writer Task
// -------------------------------------------------------------
#define NUM_WRITE_BUFS 32          // 32 x 128KB = 4MB PSRAM Ring Buffer
#define WRITE_BUF_SIZE (128 * 1024) // 128KB (Khớp đúng ranh giới Flash Erase Block 128KB của thẻ SD)

typedef struct {
    uint8_t *buf;
    size_t size;
} async_write_msg_t;

typedef struct {
    FIL *file;
    QueueHandle_t ready_queue;
    QueueHandle_t free_queue;
    SemaphoreHandle_t done_sem;
    volatile bool error;
    size_t total_written;
} sd_writer_ctx_t;

static void async_sd_writer_task(void *pvParameters) {
    sd_writer_ctx_t *ctx = (sd_writer_ctx_t *)pvParameters;
    ESP_LOGI(TAG, "Task ghi SD bất đồng bộ (PSRAM -> Internal SRAM DMA) đã khởi chạy trên Core %d", xPortGetCoreID());

    // Cấp phát bộ đệm DMA 32KB trong Internal SRAM để SDMMC Host thực thi 100% Hardware DMA
    size_t dma_chunk_size = 32 * 1024;
    uint8_t *sram_dma_buf = (uint8_t *)heap_caps_aligned_alloc(32, dma_chunk_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!sram_dma_buf) {
        ESP_LOGE(TAG, "Thất bại khi cấp phát bộ đệm Internal SRAM DMA cho Task ghi SD!");
        ctx->error = true;
    }

    async_write_msg_t msg;
    while (xQueueReceive(ctx->ready_queue, &msg, portMAX_DELAY) == pdTRUE) {
        if (msg.size == 0) {
            // EOF Sentinel message
            break;
        }

        if (!ctx->error && sram_dma_buf) {
            size_t bytes_left = msg.size;
            size_t offset = 0;
            while (bytes_left > 0 && !ctx->error) {
                size_t to_write = MIN(bytes_left, dma_chunk_size);
                memcpy(sram_dma_buf, msg.buf + offset, to_write);

                UINT bw = 0;
                FRESULT wres = f_write(ctx->file, sram_dma_buf, to_write, &bw);
                if (wres != FR_OK || bw != to_write) {
                    ESP_LOGE(TAG, "Lỗi ghi SDMMC DMA (err %d, yêu cầu %zu, đã ghi %u)", wres, to_write, bw);
                    ctx->error = true;
                } else {
                    ctx->total_written += bw;
                    bytes_left -= bw;
                    offset += bw;
                }
            }
        }

        // Trả bộ đệm PSRAM về hàng đợi tự do cho Wi-Fi Task nạp tiếp
        xQueueSend(ctx->free_queue, &msg.buf, 0);
    }

    if (sram_dma_buf) {
        heap_caps_free(sram_dma_buf);
    }

    if (ctx->done_sem) {
        xSemaphoreGive(ctx->done_sem);
    }
    vTaskDelete(NULL);
}

static esp_err_t upload_post_handler(httpd_req_t *req) {
    int64_t upload_start_time = esp_timer_get_time();
    ESP_LOGI(TAG, "=== BẮT ĐẦU UPLOAD BẤT ĐỒNG BỘ PSRAM (Content-Length: %d bytes) ===", req->content_len);

    // Tối ưu Socket SO_RCVBUF lên 128KB & TCP_NODELAY
    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd >= 0) {
        int rcvbuf = 128 * 1024;
        setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        int nodelay = 1;
        setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    }

    // Cấp phát 1MB PSRAM Ring Buffer (16 khối x 64KB)
    uint8_t *psram_pool[NUM_WRITE_BUFS] = {NULL};
    QueueHandle_t free_queue = xQueueCreate(NUM_WRITE_BUFS, sizeof(uint8_t *));
    QueueHandle_t ready_queue = xQueueCreate(NUM_WRITE_BUFS, sizeof(async_write_msg_t));
    SemaphoreHandle_t done_sem = xSemaphoreCreateBinary();

    for (int i = 0; i < NUM_WRITE_BUFS; i++) {
        psram_pool[i] = (uint8_t *)heap_caps_aligned_alloc(64, WRITE_BUF_SIZE, MALLOC_CAP_SPIRAM);
        if (!psram_pool[i]) {
            ESP_LOGE(TAG, "Không thể cấp phát bộ đệm PSRAM %d", i);
            // Dọn dẹp RAM đã cấp phát
            for (int j = 0; j < i; j++) heap_caps_free(psram_pool[j]);
            if (free_queue) vQueueDelete(free_queue);
            if (ready_queue) vQueueDelete(ready_queue);
            if (done_sem) vSemaphoreDelete(done_sem);
            return send_500_error_with_drain(req, req->content_len, "Không đủ PSRAM cho bộ đệm Upload 4MB");
        }
        xQueueSend(free_queue, &psram_pool[i], 0);
    }
    ESP_LOGI(TAG, "Đã khởi tạo PSRAM Async Ring Buffer 4MB (32x128KB) thành công.");

    // Đọc chunk đầu tiên từ HTTP socket để bóc tách filename
    uint8_t *first_buf = NULL;
    xQueueReceive(free_queue, &first_buf, portMAX_DELAY);

    int recv_bytes = httpd_req_recv(req, (char *)first_buf, WRITE_BUF_SIZE);
    if (recv_bytes <= 0) {
        ESP_LOGE(TAG, "Lỗi đọc chunk đầu tiên từ HTTP socket (code %d)", recv_bytes);
        for (int i = 0; i < NUM_WRITE_BUFS; i++) heap_caps_free(psram_pool[i]);
        vQueueDelete(free_queue);
        vQueueDelete(ready_queue);
        vSemaphoreDelete(done_sem);
        return send_500_error_with_drain(req, req->content_len, "Lỗi kết nối Wi-Fi socket");
    }

    char filename[128] = {0};
    first_buf[MIN(recv_bytes, WRITE_BUF_SIZE - 1)] = '\0';
    char *fn_start = strstr((char *)first_buf, "filename=\"");
    if (fn_start) {
        fn_start += strlen("filename=\"");
        char *fn_end = strchr(fn_start, '"');
        if (fn_end && (fn_end - fn_start) < sizeof(filename)) {
            strncpy(filename, fn_start, fn_end - fn_start);
        }
    }
    if (strlen(filename) == 0) {
        snprintf(filename, sizeof(filename), "upload_%lu.rgbv", (unsigned long)esp_timer_get_time());
    }

    char *body_start = strstr((char *)first_buf, "\r\n\r\n");
    int header_offset = body_start ? (body_start + 4 - (char *)first_buf) : 0;
    int file_data_in_first_chunk = recv_bytes - header_offset;

    char fatfs_path[300];
    snprintf(fatfs_path, sizeof(fatfs_path), "0:/%s", (filename[0] == '/') ? filename + 1 : filename);
    ESP_LOGI(TAG, "Mở tệp ghi SD Direct: %s", fatfs_path);

    FIL file;
    FRESULT res = f_open(&file, fatfs_path, FA_WRITE | FA_CREATE_ALWAYS);
    if (res != FR_OK) {
        ESP_LOGE(TAG, "Không thể mở tệp FatFs: %s (Err %d)", fatfs_path, res);
        for (int i = 0; i < NUM_WRITE_BUFS; i++) heap_caps_free(psram_pool[i]);
        vQueueDelete(free_queue);
        vQueueDelete(ready_queue);
        vSemaphoreDelete(done_sem);
        return send_500_error_with_drain(req, req->content_len - recv_bytes, "Không thể tạo tệp FatFs trên SD");
    }

    // Tiền cấp phát chuỗi cluster liên tục trên SD bằng f_expand
    if (req->content_len > 0) {
        FRESULT exp_res = f_expand(&file, (FSIZE_t)req->content_len, 1);
        if (exp_res == FR_OK) {
            ESP_LOGI(TAG, "Đã tiền cấp phát f_expand chuỗi sector liên tục (%d bytes)", req->content_len);
        } else {
            ESP_LOGW(TAG, "Bỏ qua f_expand (cảnh báo mã lỗi %d)", exp_res);
        }
    }

    // Khởi tạo context & Task ghi SD bất đồng bộ
    sd_writer_ctx_t writer_ctx = {
        .file = &file,
        .ready_queue = ready_queue,
        .free_queue = free_queue,
        .done_sem = done_sem,
        .error = false,
        .total_written = 0
    };

    TaskHandle_t writer_task_handle = NULL;
    xTaskCreatePinnedToCore(async_sd_writer_task, "async_sd_writer", 4096, &writer_ctx, 5, &writer_task_handle, 1);

    size_t remaining = req->content_len - recv_bytes;
    uint8_t *curr_buf = first_buf;
    size_t accumulated = 0;

    if (file_data_in_first_chunk > 0) {
        memmove(curr_buf, first_buf + header_offset, file_data_in_first_chunk);
        accumulated = file_data_in_first_chunk;
    }

    int64_t last_progress_time = esp_timer_get_time();
    size_t last_progress_bytes = 0;
    bool read_error = false;

    while (remaining > 0 || accumulated > 0) {
        while (accumulated < WRITE_BUF_SIZE && remaining > 0) {
            size_t space_left = WRITE_BUF_SIZE - accumulated;
            int r = httpd_req_recv(req, (char *)(curr_buf + accumulated), MIN(remaining, space_left));
            if (r <= 0) {
                if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                    vTaskDelay(pdMS_TO_TICKS(5));
                    continue;
                }
                ESP_LOGE(TAG, "Lỗi kết nối Wi-Fi socket (code %d) khi còn %zu bytes", r, remaining);
                read_error = true;
                break;
            }
            accumulated += r;
            remaining -= r;
        }

        if (read_error || writer_ctx.error) break;

        if (accumulated > 0) {
            async_write_msg_t msg = {.buf = curr_buf, .size = accumulated};
            xQueueSend(ready_queue, &msg, portMAX_DELAY);
            accumulated = 0;

            if (remaining > 0) {
                // Lấy bộ đệm PSRAM tự do tiếp theo cho Wi-Fi Task nạp dữ liệu
                if (xQueueReceive(free_queue, &curr_buf, pdMS_TO_TICKS(2000)) != pdTRUE) {
                    ESP_LOGE(TAG, "Hết bộ đệm PSRAM! Task ghi SD quá chậm.");
                    read_error = true;
                    break;
                }
            }
        }

        if (writer_ctx.total_written - last_progress_bytes >= (2 * 1024 * 1024)) {
            int64_t now = esp_timer_get_time();
            double time_sec = (now - last_progress_time) / 1000000.0;
            double bytes_diff = writer_ctx.total_written - last_progress_bytes;
            double speed_mb_s = (bytes_diff / (1024.0 * 1024.0)) / (time_sec > 0 ? time_sec : 0.001);
            float pct = ((float)writer_ctx.total_written / req->content_len) * 100.0f;

            ESP_LOGI(TAG, "Progress Upload (PSRAM Async): %zu / %zu bytes (%.1f%%) - Tốc độ Wi-Fi: %.2f MB/s",
                     writer_ctx.total_written, (size_t)req->content_len, pct, speed_mb_s);

            last_progress_bytes = writer_ctx.total_written;
            last_progress_time = now;
        }
    }

    // Gửi EOF sentinel signal dừng Task ghi SD
    async_write_msg_t eof_msg = {.buf = NULL, .size = 0};
    xQueueSend(ready_queue, &eof_msg, portMAX_DELAY);

    // Chờ Task ghi SD hoàn tất toàn bộ khối đệm cuối cùng
    xSemaphoreTake(done_sem, portMAX_DELAY);
    f_close(&file);

    // Giải phóng toàn bộ 1MB PSRAM Ring Buffer & tài nguyên IPC
    for (int i = 0; i < NUM_WRITE_BUFS; i++) {
        if (psram_pool[i]) heap_caps_free(psram_pool[i]);
    }
    vQueueDelete(free_queue);
    vQueueDelete(ready_queue);
    vSemaphoreDelete(done_sem);

    // Tính toán dung lượng dữ liệu tệp thực tế cần ghi (Tổng Content-Length trừ đi Header HTTP multipart)
    size_t expected_file_bytes = (req->content_len > header_offset) ? (req->content_len - header_offset) : req->content_len;

    // Nếu Task ghi SD thành công và đã ghi gần đủ/đủ toàn bộ tệp thô (cho phép lệch bớt boundary trailer ~256KB),
    // khẳng định tệp đã tải lên thành công 100%.
    bool upload_success = (!writer_ctx.error && writer_ctx.total_written > 0 && (writer_ctx.total_written + (256 * 1024)) >= expected_file_bytes);

    if (!upload_success) {
        ESP_LOGE(TAG, "Upload thất bại: read_error=%d, writer_error=%d, total_written=%zu, expected=%zu",
                 read_error, writer_ctx.error, writer_ctx.total_written, expected_file_bytes);
        unlink(fatfs_path);
        return send_500_error_with_drain(req, remaining, "Lỗi gián đoạn khi nạp tệp");
    }

    int64_t upload_total_time = esp_timer_get_time() - upload_start_time;
    double total_sec = upload_total_time / 1000000.0;
    double avg_mb_s = (writer_ctx.total_written / (1024.0 * 1024.0)) / (total_sec > 0 ? total_sec : 0.001);

    ESP_LOGI(TAG, "=== UPLOAD BẤT ĐỒNG BỘ THÀNH CÔNG: %s (%zu bytes trong %.1fs - Tốc độ trung bình: %.2f MB/s) ===",
             filename, writer_ctx.total_written, total_sec, avg_mb_s);

    nvs_mgr_set_current_file(filename);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"success\",\"message\":\"Upload hoàn tất\"}", HTTPD_RESP_USE_STRLEN);
}

esp_err_t web_server_start(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.core_id = 0;
    config.max_uri_handlers = 15;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    config.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&s_server, &config) == ESP_OK) {
        httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};
        httpd_uri_t list_uri = {.uri = "/api/list", .method = HTTP_GET, .handler = list_get_handler};
        httpd_uri_t thumb_uri = {.uri = "/api/thumb", .method = HTTP_GET, .handler = thumb_get_handler};
        httpd_uri_t play_uri = {.uri = "/api/play", .method = HTTP_POST, .handler = play_post_handler};
        httpd_uri_t delete_uri = {.uri = "/api/delete", .method = HTTP_POST, .handler = delete_post_handler};
        httpd_uri_t upload_uri = {.uri = "/api/upload", .method = HTTP_POST, .handler = upload_post_handler};
        httpd_uri_t restart_uri = {.uri = "/api/restart", .method = HTTP_POST, .handler = restart_post_handler};

        // Captive Portal Handlers cho iOS, Android, Windows
        httpd_uri_t gen204_uri = {.uri = "/generate_204", .method = HTTP_GET, .handler = captive_portal_handler};
        httpd_uri_t hotspot_uri = {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_portal_handler};
        httpd_uri_t conn_test_uri = {.uri = "/connecttest.txt", .method = HTTP_GET, .handler = captive_portal_handler};
        httpd_uri_t ncsi_uri = {.uri = "/ncsi.txt", .method = HTTP_GET, .handler = captive_portal_handler};

        httpd_register_uri_handler(s_server, &root_uri);
        httpd_register_uri_handler(s_server, &list_uri);
        httpd_register_uri_handler(s_server, &thumb_uri);
        httpd_register_uri_handler(s_server, &play_uri);
        httpd_register_uri_handler(s_server, &delete_uri);
        httpd_register_uri_handler(s_server, &upload_uri);
        httpd_register_uri_handler(s_server, &restart_uri);
        httpd_register_uri_handler(s_server, &gen204_uri);
        httpd_register_uri_handler(s_server, &hotspot_uri);
        httpd_register_uri_handler(s_server, &conn_test_uri);
        httpd_register_uri_handler(s_server, &ncsi_uri);

        ESP_LOGI(TAG, "Web Server & Captive Portal đã khởi chạy trên Core 0 (Stack 8KB, Timeout 10s)");
        return ESP_OK;
    }
    return ESP_FAIL;
}
