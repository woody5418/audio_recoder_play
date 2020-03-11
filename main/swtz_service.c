#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"


#include "board.h"
#include "sdkconfig.h"
#include "audio_mem.h"
#include "swtz_service.h"
#include "recorder_engine.h"
#include "esp_audio.h"
#include "esp_log.h"

/*
#define SWTZ_TASK_PRIORITY        5
#define SWTZ_TASK_STACK_SIZE      6*1024


typedef enum {
    SWTZ_CMD_UNKNOWN,
    SWTZ_CMD_LOGIN,
    SWTZ_CMD_CONNECTED,
    SWTZ_CMD_START,
    SWTZ_CMD_STOP,
    SWTZ_CMD_QUIT,
    SWTZ_CMD_DESTROY,
} SWTZ_task_cmd_t;

typedef struct {
    xQueueHandle            swtz_que;
    service_state_t         swtz_state;
} swtz_service_t;

typedef struct {
    swtz_task_cmd_t     type;
    uint32_t            *pdata;
    int                 index;
    int                 len;
} swtz_task_msg_t;

static audio_service_handle_t swtz_serv_handle = NULL;

static void swtz_que_send(void *que, SWTZ_task_cmd_t type, void *data, int index, int len, int dir)
{
    swtz_task_msg_t evt = {0};
    evt.type = type;
    evt.pdata = data;
    evt.index = index;
    evt.len = len;
    if (dir) {
        xQueueSendToFront(que, &evt, 0) ;
    } else {
        xQueueSend(que, &evt, 0);
    }
}


esp_err_t swtz_start(audio_service_handle_t handle)
{
    swtz_service_t *serv = audio_service_get_data(handle);
    duer_que_send(serv->swtz_que, SWTZ_CMD_START, NULL, 0, 0, 0);
    return ESP_OK;
}

esp_err_t swtz_stop(audio_service_handle_t handle)
{
    swtz_service_t *serv = audio_service_get_data(handle);
    duer_que_send(serv->swtz_que, SWTZ_CMD_STOP, NULL, 0, 0, 0);
    return ESP_OK;
}

esp_err_t dueros_connect(audio_service_handle_t handle)
{
    swtz_service_t *serv = audio_service_get_data(handle);
    duer_que_send(serv->swtz_que, SWTZ_CMD_LOGIN, NULL, 0, 0, 0);
    return ESP_OK;
}

esp_err_t dueros_disconnect(audio_service_handle_t handle)
{
    swtz_service_t *serv = audio_service_get_data(handle);
    duer_que_send(serv->swtz_que, SWTZ_CMD_QUIT, NULL, 0, 0, 0);
    return ESP_OK;
}

esp_err_t dueros_destroy(audio_service_handle_t handle)
{
    swtz_service_t *serv = audio_service_get_data(handle);
    duer_que_send(serv->swtz_que, SWTZ_CMD_DESTROY, NULL, 0, 0, 0);
    return ESP_OK;
}

service_state_t dueros_service_state_get()
{
    swtz_service_t *serv = audio_service_get_data(swtz_serv_handle);
    return serv->swtz_que;
}

service_state_t dueros_service_state_get()
{
    dueros_service_t *serv = audio_service_get_data(duer_serv_handle);
    return serv->duer_state;
}

static void dueros_task(void *pvParameters)
{
    vTaskDelay(1000 / portTICK_PERIOD_MS); 


}
audio_service_handle_t swtz_service_create(void)
{
    swtz_service_t *serv =  audio_calloc(1, sizeof(swtz_service_t));
    serv->duer_que = xQueueCreate(3, sizeof(swtz_task_msg_t));
    serv->duer_state = SERVICE_STATE_UNKNOWN;
    audio_service_config_t swtz_cfg = {
        .task_stack = SWTZ_TASK_STACK_SIZE,
        .task_prio  = SWTZ_TASK_PRIORITY,
        .task_core  = 1,
        .task_func  = swtz_task,
        .service_start = swtz_start,
        .service_stop = swtz_stop,
        .service_connect = swtz_connect,
        .service_disconnect = swtz_disconnect,
        .service_destroy = swtz_destroy,
        .service_name = "swtz_serv",
        .user_data = serv,
    };
    audio_service_handle_t swtz = audio_service_create(&swtz_cfg);
    swtz_serv_handle = swtz;
    return swtz;
}

*/


