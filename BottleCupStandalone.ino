// AMB82-MINI standalone Bottle/Cup detector with a built-in web page.
// Open Serial Monitor at 115200 baud to find the web address.
#include <stdio.h>
#include <string.h>
#include "WiFi.h"
#include "StreamIO.h"
#include "VideoStream.h"
#include "NNObjectDetection.h"
#include <lwip/sockets.h>
#ifdef read
#undef read
#endif
#ifdef write
#undef write
#endif
extern "C" {
#include "ard_socket.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
}

static char WIFI_SSID[] = "LAPTOP";
static char WIFI_PASSWORD[] = "12345678";
static const int MIN_SCORE = 35;
static const int BOTTLE_CLASS = 39; // zero-based COCO index
static const int CUP_CLASS = 41;    // zero-based COCO index

VideoSetting videoConfig(640, 360, 5, VIDEO_JPEG, 1);
VideoSetting nnConfig(576, 320, 10, VIDEO_RGB, 0);
NNObjectDetection detector;
StreamIO nnLink(1, 1);
WiFiServer cameraServer(81);

struct WebObject {
    uint8_t kind;
    uint8_t score;
    uint16_t x1, y1, x2, y2;
};

struct DetectionState {
    uint32_t sequence;
    uint32_t updatedMs;
    int bottleScore;
    int cupScore;
    uint8_t count;
    WebObject objects[8];
};

static DetectionState latest = {};
static SemaphoreHandle_t resultMutex = NULL;
static int listenSocket = -1;
static char requestBuffer[1024];
static char jsonBuffer[1400];

static const char WEB_PAGE[] = R"WEBPAGE(<!doctype html>
<html lang="zh-Hant"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Ameba Bottle/Cup</title><style>
*{box-sizing:border-box}body{margin:0;background:#eef5f1;color:#17352c;font-family:system-ui,"Noto Sans TC",sans-serif}
main{width:min(760px,calc(100% - 28px));margin:35px auto}header{display:flex;justify-content:space-between;align-items:center;gap:12px}
h1{font-size:clamp(26px,6vw,44px);margin:6px 0 20px}.tag{padding:8px 13px;border-radius:99px;background:#dce7e2;font-weight:800}
.tag.live{background:#c9f3df;color:#075c3d}.tag.error{background:#ffd8d2;color:#8a281d}.panel{padding:22px;border-radius:20px;background:#fff;box-shadow:0 12px 30px #17352c18}.stage{position:relative;aspect-ratio:16/9;margin-bottom:18px;overflow:hidden;border-radius:14px;background:#10251f}.stage img,.stage canvas{position:absolute;width:100%;height:100%;object-fit:contain}.stage canvas{pointer-events:none}
.results{display:grid;grid-template-columns:1fr 1fr;gap:15px}.card{padding:24px;border:2px solid #e3ece8;border-radius:16px}.card.active{border-color:#22a473;background:#e3f8ef}
.card small{letter-spacing:.15em;color:#71827c;font-weight:800}.card h2{margin:5px 0}.score{font-size:28px;font-weight:900;color:#81918b}.active .score{color:#087852}
#message{margin:18px 2px 0;color:#5d716a}@media(max-width:560px){header{align-items:flex-start;flex-direction:column}.results{grid-template-columns:1fr}}
</style></head><body><main><header><div><small>AMB82-MINI · YOLOv7-tiny</small><h1>水杯／寶特瓶辨識</h1></div><span id="status" class="tag">連線中</span></header>
<section class="panel"><div class="stage"><img id="camera" alt="Ameba camera"><canvas id="overlay" width="640" height="360"></canvas></div><div class="results"><article id="cup" class="card"><small>CUP</small><h2>水杯</h2><div id="cupScore" class="score">未偵測</div></article>
<article id="bottle" class="card"><small>BOTTLE</small><h2>寶特瓶</h2><div id="bottleScore" class="score">未偵測</div></article></div><p id="message">正在等待 AI 模型…</p></section></main>
<script>
const e=id=>document.getElementById(id);let busy=false,canvas=e('overlay'),ctx=canvas.getContext('2d');e('camera').src='http://'+location.hostname+':81/';
function result(name,score){e(name).classList.toggle('active',score>0);e(name+'Score').textContent=score>0?score+'%':'未偵測'}
function draw(d){ctx.clearRect(0,0,640,360);if(!d.ready||d.age_ms>2000)return;for(const o of d.objects){const color=o.class==='cup'?'#ffb54a':'#42d49b',label=(o.class==='cup'?'Cup 水杯':'Bottle 寶特瓶')+' '+o.score+'%';ctx.strokeStyle=color;ctx.fillStyle=color;ctx.lineWidth=3;ctx.font='bold 16px sans-serif';ctx.strokeRect(o.x1,o.y1,o.x2-o.x1,o.y2-o.y1);const w=ctx.measureText(label).width+12,y=Math.max(0,o.y1-25);ctx.fillRect(o.x1,y,w,25);ctx.fillStyle='#10251f';ctx.fillText(label,o.x1+6,y+18)}}
async function update(){if(busy)return;busy=true;const c=new AbortController(),t=setTimeout(()=>c.abort(),1800);try{const r=await fetch('/api/status?t='+Date.now(),{cache:'no-store',signal:c.signal});if(!r.ok)throw Error('HTTP '+r.status);const d=await r.json();result('cup',d.cup);result('bottle',d.bottle);draw(d);e('status').className='tag live';e('status').textContent='已連線';e('message').textContent=d.ready?'辨識持續更新｜'+(d.age_ms/1000).toFixed(1)+' 秒前｜'+d.objects.length+' 個物件':'裝置已連線，AI 模型正在啟動…'}catch(x){e('status').className='tag error';e('status').textContent='連線失敗';e('message').textContent=x.name==='AbortError'?'裝置回應逾時':x.message}finally{clearTimeout(t);busy=false}}
update();setInterval(update,500);
</script></body></html>)WEBPAGE";

static void fatal(const char *message);
static bool writeAll(int socket, const uint8_t *data, size_t length);
static void respond(int socket, const char *status, const char *type, const char *body, size_t length);
static int acceptClient();
static bool startWebServer();
static void sendCameraHeader(WiFiClient &client);
static void cameraTask(void *);
static void handleWebClient();
void onDetection(std::vector<ObjectDetectionResult> results);
void setup();
void loop();
static void fatal(const char *message)
{
    Serial.println(message);
    while (true) delay(1000);
}

static bool writeAll(int socket, const uint8_t *data, size_t length)
{
    size_t sent = 0;
    uint32_t started = millis();
    while (sent < length && millis() - started < 1200UL) {
        size_t chunk = length - sent;
        if (chunk > 2048) chunk = 2048;
        int amount = send_data(socket, data + sent, chunk, 0);
        if (amount > 0) sent += amount;
        else {
            int error = get_sock_errno(socket);
            if (error != 0 && error != EAGAIN && error != EWOULDBLOCK) return false;
            delay(1);
        }
    }
    return sent == length;
}

static void respond(int socket, const char *status, const char *type,
                    const char *body, size_t length)
{
    char header[300];
    int headerLength = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
        status, type, (unsigned long)length);
    if (headerLength > 0 && headerLength < (int)sizeof(header)) {
        if (writeAll(socket, (const uint8_t *)header, headerLength) && length) {
            writeAll(socket, (const uint8_t *)body, length);
        }
    }
}

static int acceptClient()
{
    struct sockaddr_in address = {};
    socklen_t length = sizeof(address);
    return lwip_accept(listenSocket, (struct sockaddr *)&address, &length);
}

static bool startWebServer()
{
    listenSocket = start_server(80, TCP_MODE);
    return listenSocket >= 0 && set_nonblocking(listenSocket) == 0 &&
           sock_listen(listenSocket, 3) == 0;
}

static void sendCameraHeader(WiFiClient &client)
{
    client.print("HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\n");
    client.print("Cache-Control: no-store\r\nConnection: close\r\n\r\n");
}

static void cameraTask(void *)
{
    cameraServer.begin();
    for (;;) {
        WiFiClient client = cameraServer.available();
        if (!client) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        uint32_t waitStarted = millis();
        while (client.connected() && !client.available() && millis() - waitStarted < 1000UL) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        while (client.available()) client.read();
        sendCameraHeader(client);
        while (client.connected()) {
            uint32_t address = 0, length = 0;
            Camera.getImage(0, &address, &length);
            if (!address || !length) break;
            client.print("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ");
            client.print(length);
            client.print("\r\n\r\n");
            client.write((const uint8_t *)address, length);
            client.print("\r\n");
            vTaskDelay(pdMS_TO_TICKS(180));
        }
        client.stop();
    }
}

static void handleWebClient()
{
    int socket = acceptClient();
    if (socket < 0) return;
    if (set_nonblocking(socket) != 0) { close_socket(socket); return; }

    size_t used = 0;
    uint32_t started = millis();
    while (millis() - started < 250UL && used < sizeof(requestBuffer) - 1) {
        int amount = recv_data(socket, (uint8_t *)requestBuffer + used,
                               sizeof(requestBuffer) - 1 - used, 0);
        if (amount > 0) {
            used += amount;
            requestBuffer[used] = 0;
            if (strstr(requestBuffer, "\r\n\r\n")) break;
        } else delay(1);
    }

    char method[8] = {}, path[96] = {};
    if (sscanf(requestBuffer, "%7s %95s", method, path) != 2) {
        respond(socket, "400 Bad Request", "text/plain", "Bad request", 11);
    } else {
        char *query = strchr(path, '?');
        if (query) *query = 0;
        if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
            respond(socket, "200 OK", "text/html; charset=utf-8", WEB_PAGE, sizeof(WEB_PAGE) - 1);
        } else if (!strcmp(path, "/api/status")) {
            DetectionState copy;
            xSemaphoreTake(resultMutex, portMAX_DELAY);
            copy = latest;
            xSemaphoreGive(resultMutex);
            bool ready = copy.sequence != 0;
            uint32_t age = ready ? millis() - copy.updatedMs : 0;
            int size = snprintf(jsonBuffer, sizeof(jsonBuffer),
                "{\"ready\":%s,\"age_ms\":%lu,\"cup\":%d,\"bottle\":%d,\"objects\":[",
                ready ? "true" : "false", (unsigned long)age,
                age <= 2000 ? copy.cupScore : 0,
                age <= 2000 ? copy.bottleScore : 0);
            if (ready && age <= 2000) {
                for (uint8_t i = 0; i < copy.count && size < (int)sizeof(jsonBuffer); ++i) {
                    WebObject &o = copy.objects[i];
                    size += snprintf(jsonBuffer + size, sizeof(jsonBuffer) - size,
                        "%s{\"class\":\"%s\",\"score\":%u,\"x1\":%u,\"y1\":%u,\"x2\":%u,\"y2\":%u}",
                        i ? "," : "", o.kind ? "cup" : "bottle", o.score,
                        o.x1, o.y1, o.x2, o.y2);
                }
            }
            if (size < (int)sizeof(jsonBuffer) - 3) {
                jsonBuffer[size++] = ']'; jsonBuffer[size++] = '}'; jsonBuffer[size] = 0;
            }
            respond(socket, "200 OK", "application/json", jsonBuffer, size);
        } else {
            respond(socket, "404 Not Found", "text/plain", "Not found", 9);
        }
    }
    close_socket(socket);
}

void onDetection(std::vector<ObjectDetectionResult> results)
{
    DetectionState next = {};
    next.updatedMs = millis();
    for (ObjectDetectionResult item : results) {
        if (item.score() < MIN_SCORE) continue;
        const char *name = NULL;
        if (item.type() == BOTTLE_CLASS) {
            name = "Bottle";
            if (item.score() > next.bottleScore) next.bottleScore = item.score();
        }
        if (item.type() == CUP_CLASS) {
            name = "Cup";
            if (item.score() > next.cupScore) next.cupScore = item.score();
        }
        if (name != NULL && next.count < 8) {
            WebObject &o = next.objects[next.count++];
            o.kind = item.type() == CUP_CLASS ? 1 : 0;
            o.score = item.score() > 100 ? 100 : item.score();
            o.x1 = item.xMin() * 640; o.y1 = item.yMin() * 360;
            o.x2 = item.xMax() * 640; o.y2 = item.yMax() * 360;
        }
    }
    xSemaphoreTake(resultMutex, portMAX_DELAY);
    next.sequence = latest.sequence + 1;
    latest = next;
    xSemaphoreGive(resultMutex);

    if (next.cupScore) { Serial.print("Cup "); Serial.print(next.cupScore); Serial.println("%"); }
    if (next.bottleScore) { Serial.print("Bottle "); Serial.print(next.bottleScore); Serial.println("%"); }
}

void setup()
{
    Serial.begin(115200);
    Serial.println("Connecting to WiFi...");
    uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED) {
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        delay(1000);
        if (millis() - started > 30000UL) fatal("WiFi connection failed.");
    }

    resultMutex = xSemaphoreCreateMutex();
    if (resultMutex == NULL || !startWebServer()) fatal("Web server failed.");

    videoConfig.setJpegQuality(5);
    Camera.configVideoChannel(0, videoConfig);
    Camera.configVideoChannel(3, nnConfig);
    Camera.videoInit();
    detector.configVideo(nnConfig);
    detector.configThreshold(MIN_SCORE / 100.0f, 0.3f);
    detector.modelSelect(OBJECT_DETECTION, DEFAULT_YOLOV7TINY, NA_MODEL, NA_MODEL);
    detector.setResultCallback(onDetection);
    detector.begin();

    Camera.channelBegin(0);

    nnLink.registerInput(Camera.getStream(3));
    nnLink.setStackSize();
    nnLink.setTaskPriority();
    nnLink.registerOutput(detector);
    if (nnLink.begin() != 0) fatal("NN stream failed.");
    Camera.channelBegin(3);
    if (xTaskCreate(cameraTask, "web camera", 2048, NULL, 1, NULL) != pdPASS) {
        fatal("Camera web task failed.");
    }

    Serial.print("Web page: http://");
    Serial.println(WiFi.localIP());
    Serial.print("Web camera stream: http://");
    Serial.print(WiFi.localIP());
    Serial.println(":81");
}

void loop()
{
    handleWebClient();
    delay(1);
}

