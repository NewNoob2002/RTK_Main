// host 替身：esp_http_server（严格按 IDF v6.1 WS 握手行为：握手调 ws_post_handshake_cb、不调 handler）。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *httpd_handle_t;
typedef void (*httpd_free_ctx_fn_t)(void *ctx);
typedef void (*httpd_close_func_t)(httpd_handle_t hd, int sockfd);
typedef void (*httpd_work_fn_t)(void *arg);

typedef enum {
    HTTP_GET = 1,
    HTTP_POST = 3,
} httpd_method_t;

typedef struct httpd_req {
    httpd_handle_t handle;
    int method;
    const char *uri;
    size_t content_len;
    void *aux;
    void *user_ctx;
    int sockfd;
    const char *host;
    const char *content_length;
    const char *content_type;
} httpd_req_t;

typedef enum {
    HTTPD_500_INTERNAL_SERVER_ERROR = 0,
    HTTPD_400_BAD_REQUEST,
    HTTPD_404_NOT_FOUND,
    HTTPD_ERR_CODE_MAX,
} httpd_err_code_t;

typedef esp_err_t (*httpd_err_handler_func_t)(httpd_req_t *req, httpd_err_code_t error);

#define HTTPD_SOCK_ERR_TIMEOUT  (-3)
#define ESP_ERR_HTTPD_RESP_SEND 0xb006

typedef struct {
    int core_id;
    uint16_t max_open_sockets;
    uint16_t max_uri_handlers;
    bool lru_purge_enable;
    httpd_close_func_t close_fn;
    void *global_user_ctx;
    httpd_free_ctx_fn_t global_user_ctx_free_fn;
} httpd_config_t;

#define HTTPD_DEFAULT_CONFIG() {0, 7, 8, false, NULL, NULL, NULL}

typedef struct httpd_uri {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *r);
    void *user_ctx;
    bool is_websocket;
    bool handle_ws_control_frames;
    const char *supported_subprotocol;
    esp_err_t (*ws_post_handshake_cb)(httpd_req_t *req);
} httpd_uri_t;

typedef enum {
    HTTPD_WS_TYPE_CONTINUE = 0x0,
    HTTPD_WS_TYPE_TEXT = 0x1,
    HTTPD_WS_TYPE_BINARY = 0x2,
    HTTPD_WS_TYPE_CLOSE = 0x8,
    HTTPD_WS_TYPE_PING = 0x9,
    HTTPD_WS_TYPE_PONG = 0xA,
} httpd_ws_type_t;

typedef enum {
    HTTPD_WS_CLIENT_INVALID = 0x0,
    HTTPD_WS_CLIENT_HTTP = 0x1,
    HTTPD_WS_CLIENT_WEBSOCKET = 0x2,
} httpd_ws_client_info_t;

typedef struct httpd_ws_frame {
    bool final;
    bool fragmented;
    httpd_ws_type_t type;
    uint8_t *payload;
    size_t len;
} httpd_ws_frame_t;

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config);
esp_err_t httpd_stop(httpd_handle_t handle);
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler);
esp_err_t httpd_register_err_handler(httpd_handle_t handle, httpd_err_code_t error,
                                     httpd_err_handler_func_t handler_fn);
void *httpd_get_global_user_ctx(httpd_handle_t handle);
int httpd_req_to_sockfd(httpd_req_t *r);
int httpd_req_recv(httpd_req_t *r, char *buf, size_t len);
size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *field);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *val,
                                      size_t val_size);
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status);
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value);
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t buf_len);
esp_err_t httpd_resp_send_err(httpd_req_t *req, httpd_err_code_t error, const char *msg);
esp_err_t httpd_ws_recv_frame(httpd_req_t *req, httpd_ws_frame_t *pkt, size_t max_len);
esp_err_t httpd_ws_send_frame(httpd_req_t *req, httpd_ws_frame_t *pkt);
esp_err_t httpd_ws_send_frame_async(httpd_handle_t hd, int fd, httpd_ws_frame_t *frame);
httpd_ws_client_info_t httpd_ws_get_fd_info(httpd_handle_t hd, int sockfd);
esp_err_t httpd_sess_trigger_close(httpd_handle_t handle, int sockfd);
esp_err_t httpd_queue_work(httpd_handle_t handle, httpd_work_fn_t work, void *arg);

#ifdef __cplusplus
}
#endif
