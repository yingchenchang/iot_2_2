// AMB82-MINI: camera -> built-in YOLOv7 / custom YOLOv7 -> OSD + serial.
#include <stdio.h>
#include <string.h>
#include "WiFi.h"
#include "StreamIO.h"
#include "VideoStream.h"
#include "RTSP.h"
#include "NNObjectDetection.h"
#include "VideoStreamOverlay.h"
#include "config.h"
#include "WebBridge.h"

#define VIDEO_CHANNEL 0
#define WEB_JPEG_CHANNEL 1
#define NN_CHANNEL 3

VideoSetting videoConfig(VIDEO_HD, 30, VIDEO_H264, 0);
VideoSetting webJpegConfig(640, 360, 4, VIDEO_JPEG, 1);
// YOLOv7-tiny runs at about 5 FPS on AMB82-MINI. Feeding it faster only
// increases contention with the JPEG web preview without adding detections.
VideoSetting nnConfig(576, 320, 5, VIDEO_RGB, 0);
NNObjectDetection detector;
RTSP rtsp;
StreamIO videoLink(1, 1);
StreamIO nnLink(1, 1);

// Zero-based COCO class indices used by the built-in YOLO model.
#if USE_CUSTOM_BOTTLE_CUP_MODEL
static const int BOTTLE_CLASS = 0;
static const int CUP_CLASS = 1;
#else
static const int BOTTLE_CLASS = 39;
static const int CUP_CLASS = 41;
#endif

void onDetection(std::vector<ObjectDetectionResult> results);

int pixel(float value, int extent)
{
    int position = (int)(value * extent);
    if (position < 0) return 0;
    if (position >= extent) return extent - 1;
    return position;
}

void fatal(const char *message)
{
    Serial.println(message);
    while (true) delay(1000);
}

void setup()
{
    Serial.begin(115200);
    Serial.println("Connecting WiFi...");
    unsigned long started = millis();
    while (WiFi.status() != WL_CONNECTED) {
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        delay(1000);
        if (millis() - started > 30000UL) {
            fatal("WiFi failed: check config.h and restart.");
        }
    }

    if (!webInit()) fatal("HTTP server initialization failed.");
    videoConfig.setBitrate(2 * 1024 * 1024);
    webJpegConfig.setJpegQuality(5);
    Camera.configVideoChannel(VIDEO_CHANNEL, videoConfig);
    Camera.configVideoChannel(WEB_JPEG_CHANNEL, webJpegConfig);
    Camera.configVideoChannel(NN_CHANNEL, nnConfig);
    Camera.videoInit();
    rtsp.configVideo(videoConfig);
    rtsp.begin();

    // Initialize overlay before inference can invoke the callback.
    OSD.configVideo(VIDEO_CHANNEL, videoConfig);
    OSD.begin();
    OSD.createBitmap(VIDEO_CHANNEL);
    OSD.drawText(VIDEO_CHANNEL, 10, 10, "Starting Bottle/Cup detection", OSD_COLOR_WHITE);
    OSD.update(VIDEO_CHANNEL);

    detector.configVideo(nnConfig);
    detector.configThreshold(MIN_SCORE_PERCENT / 100.0f, 0.3f);
#if USE_CUSTOM_BOTTLE_CUP_MODEL
    detector.modelSelect(OBJECT_DETECTION, CUSTOMIZED_YOLOV7TINY, NA_MODEL, NA_MODEL);
#else
    detector.modelSelect(OBJECT_DETECTION, DEFAULT_YOLOV7TINY, NA_MODEL, NA_MODEL);
#endif
    detector.setResultCallback(onDetection);
    detector.begin();

    videoLink.registerInput(Camera.getStream(VIDEO_CHANNEL));
    videoLink.registerOutput(rtsp);
    if (videoLink.begin() != 0) fatal("Video StreamIO start failed.");
    Camera.channelBegin(VIDEO_CHANNEL);
    Camera.channelBegin(WEB_JPEG_CHANNEL);
    if (!webStartCapture(WEB_JPEG_CHANNEL)) fatal("Web JPEG capture task failed.");

    nnLink.registerInput(Camera.getStream(NN_CHANNEL));
    nnLink.setStackSize();
    nnLink.setTaskPriority();
    nnLink.registerOutput(detector);
    if (nnLink.begin() != 0) fatal("NN StreamIO start failed.");

    Serial.print("Open low-latency stream: rtsp://");
    Serial.print(WiFi.localIP());
    Serial.print(":");
    Serial.println(rtsp.getPort());
    Serial.println("Bottle/Cup detection starting. Logs use milliseconds since boot.");
    Serial.print("Browser dashboard: http://");
    Serial.println(WiFi.localIP());
    Camera.channelBegin(NN_CHANNEL);
}

void loop()
{
    webHandleClient();
    delay(1); // Inference and JPEG capture run in separate tasks.
}

void onDetection(std::vector<ObjectDetectionResult> results)
{
    static bool previouslyPresent[2] = {false, false};
    static unsigned long lastReport[2] = {0, 0};
    static unsigned long previousCallback = 0;
    bool present[2] = {false, false};
    int bestScore[2] = {0, 0};
    const char *names[2] = {"Bottle", "Cup"};
    unsigned long now = millis();
    BrowserSnapshot browserNext = {};
    browserNext.observedMs = now;
    unsigned long callbackGap = previousCallback ? now - previousCallback : 0;
    previousCallback = now;

    OSD.createBitmap(VIDEO_CHANNEL); // Clear previous boxes on every result.
    for (ObjectDetectionResult item : results) {
        int slot;
        if (item.type() == BOTTLE_CLASS) slot = 0;
        else if (item.type() == CUP_CLASS) slot = 1;
        else continue;
        if (item.score() < MIN_SCORE_PERCENT) continue;

        int xmin = pixel(item.xMin(), videoConfig.width());
        int xmax = pixel(item.xMax(), videoConfig.width());
        int ymin = pixel(item.yMin(), videoConfig.height());
        int ymax = pixel(item.yMax(), videoConfig.height());
        if (xmax <= xmin || ymax <= ymin) continue;
        present[slot] = true;
        if (item.score() > bestScore[slot]) bestScore[slot] = item.score();
        if (browserNext.count < MAX_OBJ_DET) {
            BrowserObject &webObject = browserNext.objects[browserNext.count];
            webObject.kind = slot;
            webObject.score = item.score() > 100 ? 100 : item.score();
            webObject.x1 = pixel(item.xMin(), WEB_WIDTH);
            webObject.y1 = pixel(item.yMin(), WEB_HEIGHT);
            webObject.x2 = pixel(item.xMax(), WEB_WIDTH);
            webObject.y2 = pixel(item.yMax(), WEB_HEIGHT);
            if (webObject.x2 > webObject.x1 && webObject.y2 > webObject.y1) ++browserNext.count;
        }
        uint32_t color = slot == 0 ? OSD_COLOR_GREEN : OSD_COLOR_CYAN;
        OSD.drawRect(VIDEO_CHANNEL, xmin, ymin, xmax, ymax, 3, color);
        char label[32];
        snprintf(label, sizeof(label), "%s %d%%", names[slot], item.score());
        int textY = ymin - OSD.getTextHeight(VIDEO_CHANNEL);
        if (textY < 0) textY = 0;
        int textX = xmin;
        int maxTextX = (int)videoConfig.width() - (int)strlen(label) * OSD.getTextWidth(VIDEO_CHANNEL);
        if (maxTextX < 0) maxTextX = 0;
        if (textX > maxTextX) textX = maxTextX;
        OSD.drawText(VIDEO_CHANNEL, textX, textY, label, color);
    }
    webPublishDetection(browserNext);
    const char *status = present[0] ? (present[1] ? "Bottle + Cup" : "Bottle")
                                    : (present[1] ? "Cup" : "No Bottle/Cup");
    OSD.drawText(VIDEO_CHANNEL, 10, 10, status, OSD_COLOR_WHITE);
    OSD.update(VIDEO_CHANNEL);

    // Report new detections immediately; throttle repeated logs only.
    for (int slot = 0; slot < 2; ++slot) {
        if (present[slot] && (!previouslyPresent[slot] ||
            now - lastReport[slot] >= SERIAL_REPORT_INTERVAL_MS)) {
            Serial.print("[t_ms="); Serial.print(now);
            Serial.print("] "); Serial.print(names[slot]);
            Serial.print(" score="); Serial.print(bestScore[slot]);
            Serial.print("% callback_gap_ms="); Serial.println(callbackGap);
            lastReport[slot] = now;
        } else if (!present[slot] && previouslyPresent[slot]) {
            Serial.print("[t_ms="); Serial.print(now);
            Serial.print("] "); Serial.print(names[slot]); Serial.println(" absent");
        }
        previouslyPresent[slot] = present[slot];
    }
    if (callbackGap > 2000UL) {
        Serial.println("WARNING: inference callback gap exceeds 2 seconds.");
    }
}
