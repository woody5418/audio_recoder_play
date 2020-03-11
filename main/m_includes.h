#ifndef _M_INCLUDES_H_
#define _M_INCLUDES_H_

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#define SERVER_URL_PLAY_MP3 "http://192.168.0.174/ai/tts/output.mp3"
#define SERVER_URL_REC_HTTP "http://192.168.0.174/ai/speech/test2"
#define SERVER_URL_SDCARD "/sdcard/test.mp3"

typedef enum { INPUT_STREAM_REC, INPUT_STREAM_ASR } input_stream_t;

typedef enum {
    OUTPUT_STREAM_HTTP,
    OUTPUT_STREAM_SPIFFS,
    OUTPUT_STREAM_SDCARD
} output_stream_t;

typedef enum {
    CHOOSE_STREAM_IDLE,
    CHOOSE_STREAM_SDCAED,
    CHOOSE_STREAM_ASR,
    CHOOSE_STREAM_REC,
    CHOOSE_STREAM_PLAY,
    CHOOSE_STREAM_HTTP_PLAY
} choose_stream_t;

void stop_all_pipelines(void);

void SDcard_Task(audio_event_iface_handle_t evt_t);
void ASR_Task(int16_t* buff_t, int audio_size_t);
void HTTPMp3_Task(audio_event_iface_handle_t evt_t);
void RecHttp_Task(audio_event_iface_handle_t evt_t);
void SpiffsMp3_Task(audio_event_iface_handle_t evt_t);
void BUTTON_WIFI_Config(audio_event_iface_handle_t evt_t);

esp_err_t _http_stream_event_handle(http_stream_event_msg_t* msg);
esp_err_t set_spiffs_play_mp3_url(char sspmu_num);
esp_err_t stop_pipeline_element(audio_pipeline_handle_t pe_handle,
                                audio_element_handle_t eh1,
                                audio_element_handle_t eh2,
                                audio_element_handle_t eh3);
audio_pipeline_handle_t create_play_pipeline(output_stream_t output_type);
audio_pipeline_handle_t create_rec_pipeline(input_stream_t input_type);

#endif
