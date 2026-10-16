#pragma once
#include <errno.h>
#include <string.h>
#include "WebAssets.h"
extern "C" {
#include "ard_socket.h"
#include <lwip/sockets.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
}

static const uint16_t WEB_WIDTH = 640;
static const uint16_t WEB_HEIGHT = 360;
static const uint32_t WEB_STALE_MS = 2000;
static const uint32_t WEB_JPEG_CAPACITY = 160 * 1024;

struct BrowserObject {
    uint8_t kind; // 0: Bottle, 1: Cup
    uint8_t score;
    uint16_t x1, y1, x2, y2;
};
struct BrowserSnapshot {
    uint32_t sequence;
    uint32_t observedMs;
    uint8_t count;
    BrowserObject objects[MAX_OBJ_DET];
};
static BrowserSnapshot webResult = {};
static SemaphoreHandle_t webResultMutex = NULL;
static SemaphoreHandle_t webFrameMutex = NULL;
static int webListenSocket = -1;
static int webJpegChannel = 1;
static uint8_t webJpegCache[WEB_JPEG_CAPACITY];
static uint8_t webJpegResponse[WEB_JPEG_CAPACITY];
static uint32_t webJpegLength = 0;
static uint32_t webJpegCapturedMs = 0;
static char webRequestBuffer[2048];
static char webJsonBuffer[3072];

// The SDK's get_available() treats an empty non-blocking accept as an error and
// can spin inside the call. Poll lwIP directly so "no browser yet" is normal.
static int webAcceptClient()
{
    struct sockaddr_in clientAddress = {};
    socklen_t clientLength = sizeof(clientAddress);
    int socket = lwip_accept(webListenSocket,
                             reinterpret_cast<struct sockaddr *>(&clientAddress),
                             &clientLength);
    if (socket < 0) {
        int socketError = get_sock_errno(webListenSocket);
        if (socketError == EAGAIN || socketError == EWOULDBLOCK) return -1;
        return -1;
    }
    return socket;
}

static void webPublishDetection(BrowserSnapshot &next)
{
    if (webResultMutex == NULL) return;
    xSemaphoreTake(webResultMutex, portMAX_DELAY);
    next.sequence = webResult.sequence + 1;
    webResult = next;
    xSemaphoreGive(webResultMutex);
}

static void webCaptureTask(void *)
{
    for (;;) {
        uint32_t address = 0, length = 0;
        // The SDK's getImage waits for the snapshot. Keep it off the HTTP/NN tasks.
        Camera.getImage(webJpegChannel, &address, &length);
        if (address != 0 && length > 0 && length <= WEB_JPEG_CAPACITY) {
            xSemaphoreTake(webFrameMutex, portMAX_DELAY);
            memcpy(webJpegCache, reinterpret_cast<const uint8_t *>(address), length);
            webJpegLength = length;
            webJpegCapturedMs = millis();
            xSemaphoreGive(webFrameMutex);
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static bool webInit()
{
    webResultMutex = xSemaphoreCreateMutex();
    webFrameMutex = xSemaphoreCreateMutex();
    if (webResultMutex == NULL || webFrameMutex == NULL) return false;
    webListenSocket = start_server(80, TCP_MODE);
    return webListenSocket >= 0 && set_nonblocking(webListenSocket) == 0 &&
           sock_listen(webListenSocket, 4) == 0;
}

static bool webStartCapture(int channel)
{
    webJpegChannel = channel;
    return xTaskCreate(webCaptureTask, "web JPEG", 2048, NULL, 1, NULL) == pdPASS;
}

static bool webWriteAll(int socket, const uint8_t *bytes, size_t size, uint32_t started)
{
    size_t sent = 0;
    while (sent < size && millis() - started < 750UL) {
        size_t remaining = size - sent;
        if (remaining > 4096) remaining = 4096;
        // Use the SDK's raw socket API: older WiFiClient.write loses partial-send counts.
        int amount = send_data(socket, bytes + sent, remaining, 0);
        if (amount > 0) sent += static_cast<size_t>(amount);
        else if (amount == 0) return false;
        else {
            int error = get_sock_errno(socket);
            if (error != 0 && error != EAGAIN && error != EWOULDBLOCK) return false;
            delay(1);
        }
    }
    return sent == size;
}

static void webRespond(int socket, const char *status, const char *type,
                       const uint8_t *body, size_t length, bool headOnly = false,
                       uint32_t frameAge = 0)
{
    char header[512];
    int size = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n"
        "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, HEAD, OPTIONS\r\n"
        "Access-Control-Allow-Private-Network: true\r\n"
        "Access-Control-Expose-Headers: X-Frame-Age-Ms\r\nX-Frame-Age-Ms: %lu\r\n\r\n",
        status, type, static_cast<unsigned long>(length), static_cast<unsigned long>(frameAge));
    uint32_t started = millis();
    if (size <= 0 || size >= static_cast<int>(sizeof(header))) return;
    if (!webWriteAll(socket, reinterpret_cast<const uint8_t *>(header), size, started)) return;
    if (!headOnly && length) webWriteAll(socket, body, length, started);
}

static void webText(int socket, const char *status, const char *text)
{
    webRespond(socket, status, "text/plain; charset=utf-8",
               reinterpret_cast<const uint8_t *>(text), strlen(text));
}

static void webSendStatus(int socket, bool headOnly)
{
    BrowserSnapshot snapshot;
    xSemaphoreTake(webResultMutex, portMAX_DELAY);
    snapshot = webResult;
    xSemaphoreGive(webResultMutex);
    bool ready = snapshot.sequence != 0;
    uint32_t age = ready ? millis() - snapshot.observedMs : 0;
    bool stale = ready && age > WEB_STALE_MS;
    int used = snprintf(webJsonBuffer, sizeof(webJsonBuffer),
        "{\"ready\":%s,\"stale\":%s,\"seq\":%lu,\"age_ms\":%lu,"
        "\"width\":%u,\"height\":%u,\"objects\":[",
        ready ? "true" : "false", stale ? "true" : "false",
        static_cast<unsigned long>(snapshot.sequence), static_cast<unsigned long>(age),
        WEB_WIDTH, WEB_HEIGHT);
    if (used < 0 || used >= static_cast<int>(sizeof(webJsonBuffer))) {
        webText(socket, "500 Internal Server Error", "Status buffer overflow"); return;
    }
    if (ready && !stale) {
        for (uint8_t i = 0; i < snapshot.count; ++i) {
            BrowserObject &item = snapshot.objects[i];
            int written = snprintf(webJsonBuffer + used, sizeof(webJsonBuffer) - used,
                "%s{\"class\":\"%s\",\"score\":%u,\"x1\":%u,\"y1\":%u,\"x2\":%u,\"y2\":%u}",
                i ? "," : "", item.kind == 0 ? "bottle" : "cup", item.score,
                item.x1, item.y1, item.x2, item.y2);
            if (written < 0 || written >= static_cast<int>(sizeof(webJsonBuffer) - used)) {
                webText(socket, "500 Internal Server Error", "Status buffer overflow"); return;
            }
            used += written;
        }
    }
    if (used + 3 > static_cast<int>(sizeof(webJsonBuffer))) {
        webText(socket, "500 Internal Server Error", "Status buffer overflow"); return;
    }
    memcpy(webJsonBuffer + used, "]}", 3);
    webRespond(socket, "200 OK", "application/json; charset=utf-8",
               reinterpret_cast<const uint8_t *>(webJsonBuffer), used + 2, headOnly);
}

static void webHandleClient()
{
    int socket = webAcceptClient();
    if (socket < 0) return;
    if (set_nonblocking(socket) != 0) { close_socket(socket); return; }
    uint32_t started = millis();
    size_t used = 0;
    bool complete = false;
    while (millis() - started < 300UL && used < sizeof(webRequestBuffer) - 1) {
        int amount = recv_data(socket, reinterpret_cast<uint8_t *>(webRequestBuffer) + used,
                               sizeof(webRequestBuffer) - 1 - used, 0);
        if (amount > 0) {
            used += static_cast<size_t>(amount);
            webRequestBuffer[used] = '\0';
            if (strstr(webRequestBuffer, "\r\n\r\n")) { complete = true; break; }
        } else if (amount == 0) break;
        else delay(1);
    }
    if (!complete) { close_socket(socket); return; }
    char method[8] = {}, path[128] = {};
    if (sscanf(webRequestBuffer, "%7s %127s", method, path) != 2) {
        webText(socket, "400 Bad Request", "Invalid request");
    } else if (!strcmp(method, "OPTIONS")) {
        webRespond(socket, "204 No Content", "text/plain", NULL, 0);
    } else if (strcmp(method, "GET") && strcmp(method, "HEAD")) {
        webText(socket, "405 Method Not Allowed", "Use GET");
    } else {
        bool headOnly = !strcmp(method, "HEAD");
        char *query = strchr(path, '?');
        if (query) *query = '\0';
        if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
            webRespond(socket, "200 OK", "text/html; charset=utf-8", reinterpret_cast<const uint8_t *>(WEB_INDEX), sizeof(WEB_INDEX) - 1, headOnly);
        } else if (!strcmp(path, "/styles.css")) {
            webRespond(socket, "200 OK", "text/css; charset=utf-8", reinterpret_cast<const uint8_t *>(WEB_CSS), sizeof(WEB_CSS) - 1, headOnly);
        } else if (!strcmp(path, "/app.js")) {
            webRespond(socket, "200 OK", "application/javascript; charset=utf-8", reinterpret_cast<const uint8_t *>(WEB_JS), sizeof(WEB_JS) - 1, headOnly);
        } else if (!strcmp(path, "/api/status")) {
            webSendStatus(socket, headOnly);
        } else if (!strcmp(path, "/snapshot")) {
            xSemaphoreTake(webFrameMutex, portMAX_DELAY);
            uint32_t length = webJpegLength;
            uint32_t age = millis() - webJpegCapturedMs;
            if (length && age <= WEB_STALE_MS && !headOnly) memcpy(webJpegResponse, webJpegCache, length);
            xSemaphoreGive(webFrameMutex);
            if (!length || age > WEB_STALE_MS) webText(socket, "503 Service Unavailable", "Camera frame not ready");
            else webRespond(socket, "200 OK", "image/jpeg", webJpegResponse, length, headOnly, age);
        } else if (!strcmp(path, "/favicon.ico")) {
            webRespond(socket, "204 No Content", "image/x-icon", NULL, 0);
        } else webText(socket, "404 Not Found", "Not found");
    }
    close_socket(socket);
}
