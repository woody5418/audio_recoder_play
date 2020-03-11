/* Record WAV file to SD Card

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include <time.h>
#include "esp_http_client.h"
#include "esp_peripherals.h"

#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "audio_mem.h"

#include "board.h"

#include "raw_stream.h"
#include "i2s_stream.h"
#include "http_stream.h"
#include "spiffs_stream.h"

#include "wav_encoder.h"
#include "mp3_decoder.h"

#include "filter_resample.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "rec_eng_helper.h"
#include "esp_vad.h"

#include "periph_spiffs.h"
#include "periph_wifi.h"
#include "periph_button.h"
#include "m_smartconfig.h"
#include "recorder_engine.h"

#include "display_service.h"

static const char *TAG = "main_app";
static const char *EVENT_TAG = "main_event";

static audio_pipeline_handle_t pipeline_rec, pipeline_http_mp3, pipeline_asr, pipeline_play;

static audio_element_handle_t i2s_stream_reader_rec,wav_encoder_rec,http_stream_writer_rec;
static audio_element_handle_t i2s_stream_writer_http_mp3,mp3_decoder_http_mp3,http_stream_reader_http_mp3;
static audio_element_handle_t i2s_stream_reader_asr,filter_asr,raw_read_asr;
static audio_element_handle_t i2s_stream_writer_play,mp3_decoder_play,spiffs_stream_reader_play;

static display_service_handle_t disp_serv = NULL;
static uint8_t Control_play_wake = 0;
static char PLay_MP3_FLAG = 0;
static int r_temp = 0;

#define RECORD_TIME_SECONDS (10)

void led_wifi_display(uint8_t ctl)
{
    switch (ctl)
    {
    case 0:
        display_service_set_pattern(disp_serv, DISPLAY_PATTERN_WIFI_CONNECTED, 0);
        break;
    case 1:
        display_service_set_pattern(disp_serv, DISPLAY_PATTERN_WIFI_DISCONNECTED, 0);
        break;
    case 2:
        display_service_set_pattern(disp_serv, DISPLAY_PATTERN_WIFI_SETTING, 0);
        break;
    case 3:
        display_service_set_pattern(disp_serv, DISPLAY_PATTERN_TURN_OFF, 0);
        break;
    default:
        break;
    }
}

esp_err_t audio_element_event_handler(audio_element_handle_t self, audio_event_iface_msg_t *event, void *ctx)
{
    ESP_LOGI(TAG, "[ aeh ] Audio event %d from %s element", event->cmd, audio_element_get_tag(self));
    if (event->cmd == AEL_MSG_CMD_REPORT_STATUS) {
        switch ((int) event->data) {
            /*case AEL_STATUS_STATE_RUNNING:
                ESP_LOGI(TAG, "[ aeh ] AEL_STATUS_STATE_RUNNING");
                break;
            case AEL_STATUS_STATE_STOPPED:
                ESP_LOGI(TAG, "[ aeh ] AEL_STATUS_STATE_STOPPED");
                break;
            case AEL_STATUS_STATE_FINISHED:
                ESP_LOGI(TAG, "[ aeh ] AEL_STATUS_STATE_FINISHED");
                break;*/
            case AEL_STATUS_ERROR_OPEN:
                ESP_LOGI(TAG, "[ aeh ] AEL_STATUS_ERROR_OPEN");
                audio_pipeline_stop(pipeline_rec);
                audio_pipeline_wait_for_stop(pipeline_rec);
                http_stream_restart(http_stream_writer_rec);
                Control_play_wake = 1;
                r_temp=3;
                audio_element_set_uri(spiffs_stream_reader_play, "/spiffs/wlydkqcxlj.mp3");
                audio_pipeline_run(pipeline_play); //error
                break;
            default:
                ESP_LOGI(TAG, "[ aeh ] Some other event = %d", (int) event->data);
        }
    }
    return ESP_OK;
}

esp_err_t _http_stream_event_handle(http_stream_event_msg_t *msg)
{
    esp_http_client_handle_t http = (esp_http_client_handle_t)msg->http_client;
    char len_buf[16];
    static int total_write = 0;

    if (msg->event_id == HTTP_STREAM_PRE_REQUEST)
    {
        // set header
        ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_PRE_REQUEST, lenght=%d", msg->buffer_len);
        esp_http_client_set_header(http, "x-audio-sample-rates", "16000");
        esp_http_client_set_header(http, "x-audio-bits", "16");
        esp_http_client_set_header(http, "x-audio-channel", "1");
        total_write = 0;
        return ESP_OK;
    }

    if (msg->event_id == HTTP_STREAM_ON_REQUEST)
    {
        // write data
        int wlen = sprintf(len_buf, "%x\r\n", msg->buffer_len);
        if (esp_http_client_write(http, len_buf, wlen) <= 0)
        {
            return ESP_FAIL;
        }
        if (esp_http_client_write(http, msg->buffer, msg->buffer_len) <= 0)
        {
            return ESP_FAIL;
        }
        if (esp_http_client_write(http, "\r\n", 2) <= 0)
        {
            return ESP_FAIL;
        }
        total_write += msg->buffer_len;
        printf("\033[A\33[2K\rTotal bytes written: %d\n", total_write);
        return msg->buffer_len;
    }

    if (msg->event_id == HTTP_STREAM_POST_REQUEST)
    {
        ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_POST_REQUEST, write end chunked marker");
        if (esp_http_client_write(http, "0\r\n\r\n", 5) <= 0)
        {
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    if (msg->event_id == HTTP_STREAM_FINISH_REQUEST)
    {
        ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_FINISH_REQUEST");
        char *buf = calloc(1, 2048);
        assert(buf);
        int read_len = esp_http_client_read(http, buf, 2048);
        if (read_len <= 0)
        {
            free(buf);
            return ESP_FAIL;
        }
        buf[read_len] = 0;
        ESP_LOGI(TAG, "Got HTTP length = %d", strlen((char *)buf));
        ESP_LOGI(TAG, "Got HTTP Response = %s", (char *)buf);
        PLay_MP3_FLAG = 1;
        free(buf);
        return ESP_OK;
    }
    return ESP_OK;
}

void check_Stop_wakeup(void *parm)
{
    ESP_LOGI(TAG, "[ Task ]start  task check_Stop_wakeup.");
    display_service_set_pattern(disp_serv, DISPLAY_PATTERN_TURN_ON, 0);
    vTaskDelay(3000 / portTICK_RATE_MS);
    ESP_LOGI(TAG, "[ Task ] Stop...");
    display_service_set_pattern(disp_serv, DISPLAY_PATTERN_TURN_OFF, 0);
    audio_pipeline_stop(pipeline_rec);
    audio_pipeline_wait_for_stop(pipeline_rec);
    http_stream_restart(http_stream_writer_rec);
    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES)
    {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);
    esp_log_level_set(EVENT_TAG, ESP_LOG_INFO);

    ESP_LOGI(TAG, "Initialize SR handle");
    esp_wn_iface_t *wakenet;
    model_coeff_getter_t *model_coeff_getter;
    model_iface_data_t *model_data;

    get_wakenet_iface(&wakenet);
    get_wakenet_coeff(&model_coeff_getter);
    model_data = wakenet->create(model_coeff_getter, DET_MODE_90);
    int num = wakenet->get_word_num(model_data);
    for (int i = 1; i <= num; i++)
    {
        char *name = wakenet->get_word_name(model_data, i);
        ESP_LOGI(EVENT_TAG, "keywords: %s (index = %d)", name, i);
    }
    float threshold = wakenet->get_det_threshold(model_data, 1);
    int sample_rate = wakenet->get_samp_rate(model_data);
    int audio_chunksize = wakenet->get_samp_chunksize(model_data);
    ESP_LOGI(TAG, "keywords_num = %d, threshold = %f, sample_rate = %d, chunksize = %d, sizeof_uint16 = %d", num, threshold, sample_rate, audio_chunksize, sizeof(int16_t));
    int16_t *buff = (int16_t *)malloc(audio_chunksize * sizeof(short));
    if (NULL == buff)
    {
        ESP_LOGE(EVENT_TAG, "Memory allocation failed!");
        wakenet->destroy(model_data);
        model_data = NULL;
        return;
    }

    // Initialize peripherals management
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);

    disp_serv = audio_board_led_init();

    Wifi_Init_Airkiss();
    ESP_LOGI(TAG, "[ 0 ] Airkiss  SUCCESS ");

    ESP_LOGI(TAG, "[ 1 ] Set Button");
    // Initialize button peripheral
    periph_button_cfg_t btn_cfg = {
        .gpio_mask = GPIO_SEL_36 | GPIO_SEL_39,
        .long_press_time_ms = 5000};
    esp_periph_handle_t button_handle = periph_button_init(&btn_cfg);

    // Start button peripheral
    esp_periph_start(set, button_handle);

    ESP_LOGI(TAG, "[ 1.1 ] Set Spiffs");
    // Initialize Spiffs peripheral
    periph_spiffs_cfg_t spiffs_cfg = {
        .root = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true};
    esp_periph_handle_t spiffs_handle = periph_spiffs_init(&spiffs_cfg);

    // Start spiffs
    esp_periph_start(set, spiffs_handle);
    // Wait until spiffs is mounted
    while (!periph_spiffs_is_mounted(spiffs_handle))
    {
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }

    ESP_LOGI(TAG, "[ 2.0 ] Start codec chip");
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START);

    ESP_LOGI(TAG, "[3.0] Create audio pipeline for recording");
    audio_pipeline_cfg_t pipeline_cfg_rec = DEFAULT_AUDIO_PIPELINE_CONFIG();
    audio_pipeline_cfg_t pipeline_cfg_http_mp3 = DEFAULT_AUDIO_PIPELINE_CONFIG();
    audio_pipeline_cfg_t pipeline_cfg_asr = DEFAULT_AUDIO_PIPELINE_CONFIG();
    audio_pipeline_cfg_t pipeline_cfg_play = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline_rec = audio_pipeline_init(&pipeline_cfg_rec);
    pipeline_http_mp3 = audio_pipeline_init(&pipeline_cfg_http_mp3);
    pipeline_asr = audio_pipeline_init(&pipeline_cfg_asr);
    pipeline_play = audio_pipeline_init(&pipeline_cfg_play);
    mem_assert(pipeline_rec);
    mem_assert(pipeline_http_mp3);
    mem_assert(pipeline_asr);
    mem_assert(pipeline_play);

    ESP_LOGI(TAG, "[3.1] Create http stream to post data to server");
    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.type = AUDIO_STREAM_WRITER;
    http_cfg.event_handle = _http_stream_event_handle;
    http_stream_writer_rec = http_stream_init(&http_cfg);

    http_stream_cfg_t http_cfg_p = HTTP_STREAM_CFG_DEFAULT();
    http_stream_reader_http_mp3 = http_stream_init(&http_cfg_p);

    ESP_LOGI(TAG, "[3.2] Create i2s stream to read audio data from codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_READER;
    i2s_stream_reader_rec = i2s_stream_init(&i2s_cfg);

    i2s_stream_cfg_t i2s_cfg_p = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg_p.type = AUDIO_STREAM_WRITER;
    i2s_stream_writer_http_mp3 = i2s_stream_init(&i2s_cfg_p);

    i2s_stream_cfg_t i2s_cfg_a = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg_a.i2s_config.sample_rate = 48000;
    i2s_cfg_a.type = AUDIO_STREAM_READER;
    i2s_stream_reader_asr = i2s_stream_init(&i2s_cfg_a);

    ESP_LOGI(TAG, "[3.3] Create i2s stream to write data to codec chip");
    i2s_stream_cfg_t i2s_cfg_s = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg_s.type = AUDIO_STREAM_WRITER;
    i2s_stream_writer_play = i2s_stream_init(&i2s_cfg_s);

    ESP_LOGI(TAG, "[ 2.2 ] Create filter_asr to resample audio data");
    rsp_filter_cfg_t rsp_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    rsp_cfg.src_rate = 48000;
    rsp_cfg.src_ch = 2;
    rsp_cfg.dest_rate = 16000;
    rsp_cfg.dest_ch = 1;
    rsp_cfg.type = AUDIO_CODEC_TYPE_ENCODER;
    filter_asr = rsp_filter_init(&rsp_cfg);

    ESP_LOGI(TAG, "[ 2.3 ] Create raw to receive data");
    raw_stream_cfg_t raw_cfg = {
        .out_rb_size = 8 * 1024,
        .type = AUDIO_STREAM_READER,
    };
    raw_read_asr = raw_stream_init(&raw_cfg);

    ESP_LOGI(TAG, "[3.1] Create spiffs stream to read data from sdcard");
    spiffs_stream_cfg_t flash_cfg = SPIFFS_STREAM_CFG_DEFAULT();
    flash_cfg.type = AUDIO_STREAM_READER;
    spiffs_stream_reader_play = spiffs_stream_init(&flash_cfg);

    ESP_LOGI(TAG, "[3.2] Create mp3 decoder to decode mp3 file");
    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    mp3_decoder_cfg_t mp3_cfg_s = DEFAULT_MP3_DECODER_CONFIG();
    mp3_decoder_http_mp3 = mp3_decoder_init(&mp3_cfg);
    mp3_decoder_play = mp3_decoder_init(&mp3_cfg_s);

    ESP_LOGI(TAG, "[3.3] Create wav encoder to encode wav format");
    wav_encoder_cfg_t wav_cfg = DEFAULT_WAV_ENCODER_CONFIG();
    wav_encoder_rec = wav_encoder_init(&wav_cfg);

    ESP_LOGI(TAG, "[3.4] Register all elements to audio pipeline recoder");
    audio_pipeline_register(pipeline_rec, i2s_stream_reader_rec, "i2s");
    audio_pipeline_register(pipeline_rec, wav_encoder_rec, "wav");
    audio_pipeline_register(pipeline_rec, http_stream_writer_rec, "http");

    ESP_LOGI(TAG, "[ 3.5 ] Register all elements to audio pipeline play");
    audio_pipeline_register(pipeline_http_mp3, http_stream_reader_http_mp3, "http");
    audio_pipeline_register(pipeline_http_mp3, mp3_decoder_http_mp3, "mp3");
    audio_pipeline_register(pipeline_http_mp3, i2s_stream_writer_http_mp3, "i2s");

    ESP_LOGI(TAG, "[ 3.6 ] Register all elements to audio pipeline play");
    audio_pipeline_register(pipeline_asr, i2s_stream_reader_asr, "i2s");
    audio_pipeline_register(pipeline_asr, raw_read_asr, "raw");
    audio_pipeline_register(pipeline_asr, filter_asr, "filter");

    ESP_LOGI(TAG, "[3.7] Register all elements to audio pipeline");
    audio_pipeline_register(pipeline_play, spiffs_stream_reader_play, "spiffs");
    audio_pipeline_register(pipeline_play, mp3_decoder_play, "mp3");
    audio_pipeline_register(pipeline_play, i2s_stream_writer_play, "i2s");

    ESP_LOGI(TAG, "[ 3.7 ] Link it together [codec_chip]-->i2s_stream->wav_encoder_rec->http_stream-->[http_server]");
    audio_pipeline_link(pipeline_rec, (const char *[]){"i2s", "wav", "http"}, 3);

    ESP_LOGI(TAG, "[ 3.8 ] Link it together http_stream-->mp3_decoder_http_mp3-->i2s_stream-->[codec_chip]");
    audio_pipeline_link(pipeline_http_mp3, (const char *[]){"http", "mp3", "i2s"}, 3);

    ESP_LOGI(TAG, "[ 3.9 ] Link elements together [codec_chip]-->i2s_stream-->filter_asr-->raw-->[SR]");
    audio_pipeline_link(pipeline_asr, (const char *[]){"i2s", "filter", "raw"}, 3);

    ESP_LOGI(TAG, "[3.5] Link it together [flash]-->spiffs-->mp3_decoder_http_mp3-->i2s_stream-->[codec_chip]");
    audio_pipeline_link(pipeline_play, (const char *[]){"spiffs", "mp3", "i2s"}, 3);

    ESP_LOGI(TAG, "[ 4.0 ] Set up  event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

    ESP_LOGI(TAG, "[4.1] Listening event from pipeline");
    audio_pipeline_set_listener(pipeline_rec, evt);
    audio_pipeline_set_listener(pipeline_http_mp3, evt);
    audio_pipeline_set_listener(pipeline_play, evt);

    ESP_LOGI(TAG, "[4.2] Listening event from peripherals");
    audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), evt);

    ESP_LOGI(TAG, "[ 5 ] Start audio_pipeline");
    i2s_stream_set_clk(i2s_stream_reader_rec, 16000, 16, 1);
    audio_pipeline_run(pipeline_asr);

    audio_element_set_event_callback(http_stream_writer_rec,audio_element_event_handler,NULL);

    ESP_LOGI(TAG, "[ 6.0 ] Press But(REC) for recoder ");
    while (1)
    {
        audio_event_iface_msg_t msg;
        audio_event_iface_listen(evt, &msg, 0);
        raw_stream_read(raw_read_asr, (char *)buff, audio_chunksize * sizeof(short));
        int keyword = wakenet->detect(model_data, (int16_t *)buff);
        switch (keyword)
        {
        case 1:
        {
            ESP_LOGI(TAG, "Wake up");
            audio_pipeline_stop(pipeline_asr);
            audio_pipeline_wait_for_stop(pipeline_asr);
            srand(time(0));
            r_temp = rand() % 3;
            Control_play_wake = 1;
            switch (r_temp)
            {
                case 0:
                    ESP_LOGI(TAG, "[ mp3 ]The path Settings --->/spiffs/enwozai.mp3");
                    audio_element_set_uri(spiffs_stream_reader_play, "/spiffs/enwozai.mp3");
                    break;
                case 1:
                    ESP_LOGI(TAG, "[ mp3 ]The path Settings --->/spiffs/youshenmefenfu.mp3");
                    audio_element_set_uri(spiffs_stream_reader_play, "/spiffs/youshenmefenfu.mp3");
                    break;
                case 2:
                    ESP_LOGI(TAG, "[ mp3 ]The path Settings --->/spiffs/zainenishuo.mp3");
                    audio_element_set_uri(spiffs_stream_reader_play, "/spiffs/zainenishuo.mp3");
                    break;
                default:
                    ESP_LOGI(TAG, "rand is %d", r_temp);
                    break;
            }
            audio_pipeline_run(pipeline_play); //error
            vTaskDelay(100/portTICK_RATE_MS);
        }
        break;
        }
        if (Control_play_wake == 1)
        {
            //ESP_LOGI(TAG, "[ pw ] Get semaphore %d, %d", msg.source_type, msg.cmd);
            if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *)mp3_decoder_play && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO)
            {
                audio_element_info_t music_info_s = {0};
                audio_element_getinfo(mp3_decoder_play, &music_info_s);

                ESP_LOGI(TAG, "[ * ] Receive music info from mp3 decoder, sample_rates=%d, bits=%d, ch=%d",
                         music_info_s.sample_rates, music_info_s.bits, music_info_s.channels);

                audio_element_setinfo(i2s_stream_writer_play, &music_info_s);
                i2s_stream_set_clk(i2s_stream_writer_play, music_info_s.sample_rates, music_info_s.bits, music_info_s.channels);
                continue;
            }

            // Stop when the last pipeline element (i2s_stream_writer_http_mp3 in this case) receives stop event 
            if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *)i2s_stream_writer_play && msg.cmd == AEL_MSG_CMD_REPORT_STATUS && (((int)msg.data == AEL_STATUS_STATE_STOPPED) || ((int)msg.data == AEL_STATUS_STATE_FINISHED)))
            {
                if(r_temp == 3){
                    Control_play_wake = 3;
                }else{
                    Control_play_wake = 2;
                }
                
                ESP_LOGW(TAG, "[ * ] Stop event received......");
                audio_pipeline_stop(pipeline_play);
                audio_pipeline_wait_for_stop(pipeline_play);
                audio_element_reset_state(spiffs_stream_reader_play);
                audio_element_reset_state(mp3_decoder_play);
                audio_element_reset_state(i2s_stream_writer_play);
                audio_pipeline_reset_ringbuffer(pipeline_play);
                audio_pipeline_reset_items_state(pipeline_play);
                audio_pipeline_change_state(pipeline_play, AEL_STATE_INIT);
            }
            continue;
        }
        if (Control_play_wake == 2)
        {
            Control_play_wake = 0;
            ESP_LOGI(TAG, "[ cw ] stop task check_Stop_wakeup");
            xTaskCreate(check_Stop_wakeup, "check_Stop_wakeup", 4096, NULL, 4, NULL);
            audio_element_set_uri(http_stream_writer_rec, CONFIG_SERVER_URI); //"http://192.168.0.176:8000/ai/speech/mac"
            audio_pipeline_run(pipeline_rec);
        }
        if(Control_play_wake == 3){
            Control_play_wake = 0;
            ESP_LOGI(TAG, "[ cw ] start run pipeline_asr");
            audio_pipeline_run(pipeline_asr);
        }
        if (PLay_MP3_FLAG == 1)
        {
            audio_element_set_uri(http_stream_reader_http_mp3, "http://192.168.0.159/ai/tts/output.mp3"); //"http://192.168.0.176:8000/ai/speech/mac"
            audio_pipeline_run(pipeline_http_mp3);

            ESP_LOGI(TAG, "[ mf ] Get semaphore %d, %d", msg.source_type, msg.cmd);
            if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *)mp3_decoder_http_mp3 && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO)
            {
                audio_element_info_t music_info = {0};
                audio_element_getinfo(mp3_decoder_http_mp3, &music_info);

                ESP_LOGI(TAG, "[ * ] Receive music info from mp3 decoder, sample_rates=%d, bits=%d, ch=%d",
                         music_info.sample_rates, music_info.bits, music_info.channels);

                audio_element_setinfo(i2s_stream_writer_http_mp3, &music_info);
                i2s_stream_set_clk(i2s_stream_writer_http_mp3, music_info.sample_rates, music_info.bits, music_info.channels);
                continue;
            }
            if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *)i2s_stream_writer_http_mp3 && msg.cmd == AEL_MSG_CMD_REPORT_STATUS && (((int)msg.data == AEL_STATUS_STATE_STOPPED) || ((int)msg.data == AEL_STATUS_STATE_FINISHED)))
            {
                PLay_MP3_FLAG = 0;
                ESP_LOGW(TAG, "[ * ] Stop event received");
                audio_pipeline_stop(pipeline_http_mp3);
                audio_pipeline_wait_for_stop(pipeline_http_mp3);
                audio_element_reset_state(mp3_decoder_http_mp3);
                audio_element_reset_state(i2s_stream_writer_http_mp3);
                audio_element_reset_state(http_stream_reader_http_mp3);
                audio_pipeline_reset_ringbuffer(pipeline_http_mp3);
                audio_pipeline_reset_items_state(pipeline_http_mp3);
                audio_pipeline_run(pipeline_asr);
                //break;
            }
            continue;
        }

        if (msg.cmd == PERIPH_BUTTON_PRESSED)
        {
            if ((int)msg.data == GPIO_NUM_39)
            {
                ESP_LOGI(TAG, "[ a ] MOde Btn press...");
            }
        }
        else if (msg.cmd == PERIPH_BUTTON_LONG_PRESSED)
        {
            if ((int)msg.data == GPIO_NUM_39)
            {
                ESP_LOGI(TAG, "[ a ] long button pressed ...");
                Break_Wifi_Connect(); //The distribution network
                ESP_LOGI(TAG, "[ a ] Start Airkiss...");
            }
        }
    }
    ESP_LOGI(TAG, "[ 7 ] Stop audio_pipeline");
    audio_pipeline_terminate(pipeline_rec);
    audio_pipeline_terminate(pipeline_http_mp3);
    audio_pipeline_terminate(pipeline_asr);
    audio_pipeline_terminate(pipeline_play);

    audio_pipeline_unregister(pipeline_rec, i2s_stream_reader_rec);
    audio_pipeline_unregister(pipeline_rec, wav_encoder_rec);
    audio_pipeline_unregister(pipeline_rec, http_stream_writer_rec);

    audio_pipeline_unregister(pipeline_http_mp3, http_stream_reader_http_mp3);
    audio_pipeline_unregister(pipeline_http_mp3, mp3_decoder_http_mp3);
    audio_pipeline_unregister(pipeline_http_mp3, i2s_stream_writer_http_mp3);

    audio_pipeline_unregister(pipeline_asr, raw_read_asr);
    audio_pipeline_unregister(pipeline_asr, i2s_stream_reader_asr);
    audio_pipeline_unregister(pipeline_asr, filter_asr);

    audio_pipeline_unregister(pipeline_play, spiffs_stream_reader_play);
    audio_pipeline_unregister(pipeline_play, i2s_stream_writer_play);
    audio_pipeline_unregister(pipeline_play, mp3_decoder_play);

    // Terminal the pipeline before removing the listener
    audio_pipeline_remove_listener(pipeline_rec);
    audio_pipeline_remove_listener(pipeline_http_mp3);
    audio_pipeline_remove_listener(pipeline_asr);
    audio_pipeline_remove_listener(pipeline_play);

    // Stop all periph before removing the listener
    esp_periph_set_stop_all(set);
    audio_event_iface_remove_listener(esp_periph_set_get_event_iface(set), evt);

    // Make sure audio_pipeline_recemove_listener & audio_event_iface_remove_listener are called before destroying event_iface
    audio_event_iface_destroy(evt);

    // Release all resources
    audio_pipeline_deinit(pipeline_rec);
    audio_pipeline_deinit(pipeline_http_mp3);
    audio_pipeline_deinit(pipeline_asr);
    audio_pipeline_deinit(pipeline_play);

    audio_element_deinit(http_stream_reader_http_mp3);
    audio_element_deinit(i2s_stream_reader_rec);
    audio_element_deinit(i2s_stream_writer_http_mp3);
    audio_element_deinit(i2s_stream_reader_asr);
    audio_element_deinit(spiffs_stream_reader_play);
    audio_element_deinit(i2s_stream_writer_play);
    audio_element_deinit(mp3_decoder_play);
    audio_element_deinit(wav_encoder_rec);
    audio_element_deinit(mp3_decoder_http_mp3);
    audio_element_deinit(raw_read_asr);
    audio_element_deinit(filter_asr);
    esp_periph_set_destroy(set);

    ESP_LOGI(EVENT_TAG, "[ 7 ] Destroy model");
    wakenet->destroy(model_data);
    model_data = NULL;
    free(buff);
    buff = NULL;
}























