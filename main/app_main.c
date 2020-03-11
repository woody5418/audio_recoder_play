/* Record WAV file to SD Card

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_peripherals.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "tcpip_adapter.h"

#include "audio_common.h"
#include "audio_element.h"
#include "audio_event_iface.h"
#include "audio_mem.h"
#include "audio_pipeline.h"

#include "board.h"

#include "fatfs_stream.h"
#include "http_stream.h"
#include "i2s_stream.h"
#include "raw_stream.h"
#include "spiffs_stream.h"

#include "mp3_decoder.h"
#include "wav_encoder.h"

#include "esp_vad.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "filter_resample.h"
#include "rec_eng_helper.h"

#include "display_service.h"
#include "periph_button.h"
#include "periph_sdcard.h"
#include "periph_spiffs.h" 0
#include "periph_wifi.h"
#include "recorder_engine.h"

#include "m_includes.h"
#include "m_smartconfig.h"

static const char* TAG = "< app >";

static display_service_handle_t disp_serv = NULL;

static audio_pipeline_handle_t pipeline_rec, pipeline_http_mp3, pipeline_asr,
    pipeline_play, pipeline_sdcard;

static audio_element_handle_t i2s_stream_reader_rec, wav_encoder_rec,
    http_stream_writer_rec;
static audio_element_handle_t i2s_stream_writer_http_mp3, mp3_decoder_http_mp3,
    http_stream_reader_http_mp3;
static audio_element_handle_t i2s_stream_reader_asr, filter_asr, raw_read_asr;
static audio_element_handle_t i2s_stream_writer_play, mp3_decoder_play,
    spiffs_stream_reader_play;
static audio_element_handle_t fatfs_stream_reader_sdcard,
    i2s_stream_writer_sdcard, mp3_decoder_sdcard;

static input_stream_t input_type_flag;
static output_stream_t output_type_flag;
static choose_stream_t choose_type_flag;

esp_wn_iface_t* wakenet;
model_coeff_getter_t* model_coeff_getter;
model_iface_data_t* model_data;

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    tcpip_adapter_init();

    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);

    ESP_LOGI(TAG, "[ 1 ] Create asr model");
    get_wakenet_iface(&wakenet);
    get_wakenet_coeff(&model_coeff_getter);
    model_data = wakenet->create(model_coeff_getter, DET_MODE_90);
    int num = wakenet->get_word_num(model_data);
    for (int i = 1; i <= num; i++) {
        char* name = wakenet->get_word_name(model_data, i);
        ESP_LOGI(TAG, "keywords: %s (index = %d)", name, i);
    }
    float threshold = wakenet->get_det_threshold(model_data, 1);
    int sample_rate = wakenet->get_samp_rate(model_data);
    int audio_chunksize = wakenet->get_samp_chunksize(model_data);
    ESP_LOGI(TAG,
             "keywords_num = %d, threshold = %f, sample_rate = %d, chunksize = "
             "%d, sizeof_uint16 = %d",
             num, threshold, sample_rate, audio_chunksize, sizeof(int16_t));
    int16_t* buff = (int16_t*)malloc(audio_chunksize * sizeof(short));
    if (NULL == buff) {
        ESP_LOGE(TAG, "Memory allocation failed!");
        wakenet->destroy(model_data);
        model_data = NULL;
        return;
    }

    ESP_LOGI(TAG, "[ 2 ] Initialize the peripherals");
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);
    ESP_LOGI(TAG, "[ 2.1 ] Set led service");
    disp_serv = audio_board_led_init();

    periph_button_cfg_t btn_cfg = {.gpio_mask = GPIO_SEL_36 | GPIO_SEL_39,
                                   .long_press_time_ms = 5000};
    esp_periph_handle_t button_handle = periph_button_init(&btn_cfg);

    ESP_LOGI(TAG, "[ 2.2 ] Start button peripheral");
    esp_periph_start(set, button_handle);

    ESP_LOGI(TAG, "[ wifi ] wait Airkiss");
    Wifi_Init_Airkiss();
    ESP_LOGI(TAG, "[ wifi ] Airkiss  SUCCESS ");

    // Initialize Spiffs peripheral
    periph_spiffs_cfg_t spiffs_cfg = {.root = "/spiffs",
                                      .partition_label = NULL,
                                      .max_files = 5,
                                      .format_if_mount_failed = true};
    esp_periph_handle_t spiffs_handle = periph_spiffs_init(&spiffs_cfg);
    ESP_LOGI(TAG, "[ 2.3 ] Start Spiffs peripheral");
    esp_periph_start(set, spiffs_handle);
    // Wait until spiffs is mounted
    while (!periph_spiffs_is_mounted(spiffs_handle)) {
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }

    ESP_LOGI(TAG, "[ 2.3 ] Start SDCard peripheral");
    audio_board_sdcard_init(set);

    ESP_LOGI(TAG, "[ 3 ] Start codec chip");
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE,
                         AUDIO_HAL_CTRL_START);

    ESP_LOGI(TAG, "[ 4 ] Create pipeline for play");
    pipeline_http_mp3 = create_play_pipeline(OUTPUT_STREAM_HTTP);
    pipeline_play = create_play_pipeline(OUTPUT_STREAM_SPIFFS);
    pipeline_sdcard = create_play_pipeline(OUTPUT_STREAM_SDCARD);
    pipeline_asr = create_rec_pipeline(INPUT_STREAM_ASR);
    pipeline_rec = create_rec_pipeline(INPUT_STREAM_REC);

    ESP_LOGI(TAG, "[ 5 ] Set up  event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

    ESP_LOGI(TAG, "[ 6 ] Listening event from pipeline");
    audio_pipeline_set_listener(pipeline_rec, evt);
    audio_pipeline_set_listener(pipeline_play, evt);
    audio_pipeline_set_listener(pipeline_http_mp3, evt);
    audio_pipeline_set_listener(pipeline_sdcard, evt);

    ESP_LOGI(TAG, "[ 7 ] Listening event from peripherals");
    audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), evt);

    ESP_LOGI(TAG, "[ 8 ] Start audio_pipeline asr");
    // audio_pipeline_run(pipeline_asr);
    ESP_LOGI(
        TAG,
        "[ Start ] PLease speek Chinese the 'nihaoxiaozhi' to wake up ...");
    choose_type_flag = CHOOSE_STREAM_SDCAED;
    while (1) {
        BUTTON_WIFI_Config(evt);
        switch (choose_type_flag) {
            case CHOOSE_STREAM_SDCAED:
                SDcard_Task(evt);
                break;
            case CHOOSE_STREAM_ASR:
                ASR_Task(buff, audio_chunksize);
                break;
            case CHOOSE_STREAM_PLAY:
                SpiffsMp3_Task(evt);
                ESP_LOGI(TAG, "SpiffsMp3_Task stop");
                break;
            case CHOOSE_STREAM_REC:
                RecHttp_Task(evt);
                ESP_LOGI(TAG, "RecHttp_Task stop");
                break;
            case CHOOSE_STREAM_HTTP_PLAY:
                HTTPMp3_Task(evt);
                ESP_LOGI(TAG, "HTTPMp3_Task stop");
                break;
            default:
                break;
        }
    }
    ESP_LOGI(TAG, "[ *** ] release all resources");
    stop_all_pipelines();
    esp_periph_set_stop_all(set);
    audio_event_iface_remove_listener(esp_periph_set_get_event_iface(set), evt);
    audio_event_iface_destroy(evt);
    esp_periph_set_destroy(set);
}
/////////////////////////////////////////////////////////////////////////////////////////////////////
/*                                       Task processing function */
/////////////////////////////////////////////////////////////////////////////////////////////////////
void BUTTON_WIFI_Config(audio_event_iface_handle_t evt_t) {
    audio_event_iface_msg_t msg;
    audio_event_iface_listen(evt_t, &msg, 10 / portMAX_DELAY);
    if (msg.cmd == PERIPH_BUTTON_PRESSED) {
        if ((int)msg.data == GPIO_NUM_39) {
            ESP_LOGI(TAG, "[ a ] MOde Btn press...");
        }
    } else if (msg.cmd == PERIPH_BUTTON_LONG_PRESSED) {
        if ((int)msg.data == GPIO_NUM_39) {
            ESP_LOGI(TAG, "[ a ] long button pressed ...");
            Break_Wifi_Connect();  // The distribution network
            ESP_LOGI(TAG, "[ a ] Start Airkiss...");
        }
    }
}

void SDcard_Task(audio_event_iface_handle_t evt_t) {
    ESP_LOGI(TAG, "[ Task ]start task SDcard_Task.");
    audio_element_set_uri(fatfs_stream_reader_sdcard, SERVER_URL_SDCARD);
    audio_pipeline_run(pipeline_sdcard);
    while (1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt_t, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
            continue;
        }
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT &&
            msg.source == (void*)mp3_decoder_sdcard &&
            msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
            audio_element_info_t music_info = {0};
            audio_element_getinfo(mp3_decoder_sdcard, &music_info);

            ESP_LOGI(TAG,
                     "[ * ] Receive music info from mp3 decoder, "
                     "sample_rates=%d, bits=%d, ch=%d",
                     music_info.sample_rates, music_info.bits,
                     music_info.channels);

            audio_element_setinfo(i2s_stream_writer_sdcard, &music_info);
            i2s_stream_set_clk(i2s_stream_writer_sdcard,
                               music_info.sample_rates, music_info.bits,
                               music_info.channels);
            continue;
        }

        /* Stop when the last pipeline element (i2s_stream_writer in this case)
         * receives stop event */
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT &&
            msg.source == (void*)i2s_stream_writer_sdcard &&
            msg.cmd == AEL_MSG_CMD_REPORT_STATUS &&
            (((int)msg.data == AEL_STATUS_STATE_STOPPED) ||
             ((int)msg.data == AEL_STATUS_STATE_FINISHED))) {
            ESP_LOGW(TAG, "[ * ] Stop event received");
            stop_pipeline_element(pipeline_sdcard, fatfs_stream_reader_sdcard,
                                  mp3_decoder_sdcard, i2s_stream_writer_sdcard);
            audio_pipeline_run(pipeline_asr);
            choose_type_flag = CHOOSE_STREAM_ASR;
            break;
        }
    }
}

void ASR_Task(int16_t* buff_t, int audio_size_t) {
    raw_stream_read(raw_read_asr, (char*)buff_t, audio_size_t * sizeof(short));
    int keyword = wakenet->detect(model_data, (int16_t*)buff_t);
    if (keyword == 1) {
        ESP_LOGI(TAG, "Wake up");
        stop_pipeline_element(pipeline_asr, i2s_stream_reader_asr, raw_read_asr,
                              filter_asr);
        set_spiffs_play_mp3_url(3);
        audio_pipeline_run(pipeline_play);
        choose_type_flag = CHOOSE_STREAM_PLAY;
    }
}
void HTTPMp3_Task(audio_event_iface_handle_t evt_t) {
    ESP_LOGI(TAG, "[ Task ]start task Play_SpiffsMp3_Task.");
    audio_element_set_uri(http_stream_reader_http_mp3, SERVER_URL_PLAY_MP3);
    audio_pipeline_run(pipeline_http_mp3);
    while (1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt_t, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
            continue;
        }
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT &&
            msg.source == (void*)mp3_decoder_http_mp3 &&
            msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
            audio_element_info_t music_info = {0};
            audio_element_getinfo(mp3_decoder_http_mp3, &music_info);

            ESP_LOGI(TAG,
                     "[ * ] Receive music info from mp3 decoder, "
                     "sample_rates=%d, bits=%d, ch=%d",
                     music_info.sample_rates, music_info.bits,
                     music_info.channels);

            audio_element_setinfo(i2s_stream_writer_http_mp3, &music_info);
            i2s_stream_set_clk(i2s_stream_writer_http_mp3,
                               music_info.sample_rates, music_info.bits,
                               music_info.channels);
            continue;
        }

        /* Stop when the last pipeline element (i2s_stream_writer_http_mp3 in
         * this case) receives stop event */
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT &&
            msg.source == (void*)i2s_stream_writer_http_mp3 &&
            msg.cmd == AEL_MSG_CMD_REPORT_STATUS &&
            (((int)msg.data == AEL_STATUS_STATE_STOPPED) ||
             ((int)msg.data == AEL_STATUS_STATE_FINISHED))) {
            ESP_LOGW(TAG, "[ * ] Stop HTTPMp3_Task ...");
            stop_pipeline_element(
                pipeline_http_mp3, http_stream_reader_http_mp3,
                mp3_decoder_http_mp3, i2s_stream_writer_http_mp3);
            audio_pipeline_run(pipeline_asr);
            choose_type_flag = CHOOSE_STREAM_ASR;
            break;
        }
    }
}

void RecHttp_Task(audio_event_iface_handle_t evt_t) {
    ESP_LOGI(TAG, "[ Task ]start task Play_SpiffsMp3_Task.");
    i2s_stream_set_clk(i2s_stream_reader_rec, 16000, 16, 1);
    audio_element_set_uri(http_stream_writer_rec, SERVER_URL_REC_HTTP);
    audio_pipeline_run(pipeline_rec);
    Led_Display(DISPLAY_PATTERN_TURN_ON);
    vTaskDelay(3000 / portTICK_RATE_MS);
    Led_Display(DISPLAY_PATTERN_TURN_OFF);
    stop_pipeline_element(pipeline_rec, i2s_stream_reader_rec, wav_encoder_rec,
                          http_stream_writer_rec);
    http_stream_restart(http_stream_writer_rec);
}

void SpiffsMp3_Task(audio_event_iface_handle_t evt_t) {
    ESP_LOGI(TAG, "[ Task ]start task Play_SpiffsMp3_Task.");
    while (1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt_t, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
            continue;
        }
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT &&
            msg.source == (void*)mp3_decoder_play &&
            msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
            audio_element_info_t music_info_s = {0};
            audio_element_getinfo(mp3_decoder_play, &music_info_s);
            ESP_LOGI(TAG,
                     "[ * ] Receive music info from mp3 decoder, "
                     "sample_rates=%d, bits=%d, ch=%d",
                     music_info_s.sample_rates, music_info_s.bits,
                     music_info_s.channels);

            audio_element_setinfo(i2s_stream_writer_play, &music_info_s);
            i2s_stream_set_clk(i2s_stream_writer_play,
                               music_info_s.sample_rates, music_info_s.bits,
                               music_info_s.channels);
            continue;
        }
        // Stop when the last pipeline element receives stop event
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT &&
            msg.source == (void*)i2s_stream_writer_play &&
            msg.cmd == AEL_MSG_CMD_REPORT_STATUS &&
            (((int)msg.data == AEL_STATUS_STATE_STOPPED) ||
             ((int)msg.data == AEL_STATUS_STATE_FINISHED))) {
            ESP_LOGW(TAG, "[ * ] Stop SpiffsMp3_Task......");
            stop_pipeline_element(pipeline_play, spiffs_stream_reader_play,
                                  mp3_decoder_play, i2s_stream_writer_play);
            choose_type_flag = CHOOSE_STREAM_REC;
            break;
        }
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////

esp_err_t _http_stream_event_handle(http_stream_event_msg_t* msg) {
    esp_http_client_handle_t http = (esp_http_client_handle_t)msg->http_client;
    char len_buf[16];
    static int total_write = 0;

    if (msg->event_id == HTTP_STREAM_PRE_REQUEST) {
        // set header
        ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_PRE_REQUEST, lenght=%d",
                 msg->buffer_len);
        esp_http_client_set_header(http, "x-audio-sample-rates", "16000");
        esp_http_client_set_header(http, "x-audio-bits", "16");
        esp_http_client_set_header(http, "x-audio-channel", "1");
        total_write = 0;
        return ESP_OK;
    }

    if (msg->event_id == HTTP_STREAM_ON_REQUEST) {
        // write data
        int wlen = sprintf(len_buf, "%x\r\n", msg->buffer_len);
        if (esp_http_client_write(http, len_buf, wlen) <= 0) {
            return ESP_FAIL;
        }
        if (esp_http_client_write(http, msg->buffer, msg->buffer_len) <= 0) {
            return ESP_FAIL;
        }
        if (esp_http_client_write(http, "\r\n", 2) <= 0) {
            return ESP_FAIL;
        }
        total_write += msg->buffer_len;
        printf("\033[A\33[2K\rTotal bytes written: %d\n", total_write);
        return msg->buffer_len;
    }

    if (msg->event_id == HTTP_STREAM_POST_REQUEST) {
        ESP_LOGI(TAG,
                 "[ + ] HTTP client HTTP_STREAM_POST_REQUEST, write end "
                 "chunked marker");
        if (esp_http_client_write(http, "0\r\n\r\n", 5) <= 0) {
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    if (msg->event_id == HTTP_STREAM_FINISH_REQUEST) {
        ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_FINISH_REQUEST");
        char* buf = calloc(1, 2048);
        assert(buf);
        int read_len = esp_http_client_read(http, buf, 2048);
        if (read_len <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        buf[read_len] = 0;
        ESP_LOGI(TAG, "Got HTTP length = %d", strlen((char*)buf));
        ESP_LOGI(TAG, "Got HTTP Response = %s", (char*)buf);
        free(buf);
        choose_type_flag = CHOOSE_STREAM_HTTP_PLAY;
        return ESP_OK;
    }
    return ESP_OK;
}

// sspmu_num  3
esp_err_t set_spiffs_play_mp3_url(char sspmu_num) {
    char _temp = 0;
    srand(time(0));
    _temp = rand() % sspmu_num;
    switch (_temp) {
        case 0:
            ESP_LOGI(TAG, "[ mp3 ]The path Settings --->/spiffs/enwozai.mp3");
            audio_element_set_uri(spiffs_stream_reader_play,
                                  "/spiffs/enwozai.mp3");
            break;
        case 1:
            ESP_LOGI(TAG,
                     "[ mp3 ]The path Settings --->/spiffs/youshenmefenfu.mp3");
            audio_element_set_uri(spiffs_stream_reader_play,
                                  "/spiffs/youshenmefenfu.mp3");
            break;
        case 2:
            ESP_LOGI(TAG,
                     "[ mp3 ]The path Settings --->/spiffs/zainenishuo.mp3");
            audio_element_set_uri(spiffs_stream_reader_play,
                                  "/spiffs/zainenishuo.mp3");
            break;
        case 3:
            ESP_LOGI(TAG, "[ mp3 ]The path Settings--->/spiffs/wlydkqcxlj.mp3");
            audio_element_set_uri(spiffs_stream_reader_play,
                                  "/spiffs/wlydkqcxlj.mp3");
            break;
        default:
            ESP_LOGI(TAG, "rand is %d", _temp);
            break;
    }
    return ESP_OK;
}

esp_err_t stop_pipeline_element(audio_pipeline_handle_t pe_handle,
                                audio_element_handle_t eh1,
                                audio_element_handle_t eh2,
                                audio_element_handle_t eh3) {
    audio_pipeline_stop(pe_handle);
    audio_pipeline_wait_for_stop(pe_handle);
    audio_pipeline_terminate(pe_handle);
    audio_element_reset_state(eh1);
    audio_element_reset_state(eh2);
    audio_element_reset_state(eh3);
    audio_pipeline_reset_ringbuffer(pe_handle);
    audio_pipeline_reset_items_state(pe_handle);
    audio_pipeline_change_state(pe_handle, AEL_STATE_INIT);
    return ESP_OK;
}

audio_pipeline_handle_t create_play_pipeline(output_stream_t output_type) {
    audio_pipeline_handle_t pipeline;
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline);

    output_type_flag = output_type;
    switch (output_type) {
        case OUTPUT_STREAM_HTTP:
            ESP_LOGI(TAG, "[ * ] Play from HTTP");
            http_stream_cfg_t http_cfg_p = HTTP_STREAM_CFG_DEFAULT();
            http_stream_reader_http_mp3 = http_stream_init(&http_cfg_p);

            i2s_stream_cfg_t i2s_cfg_p = I2S_STREAM_CFG_DEFAULT();
            i2s_cfg_p.type = AUDIO_STREAM_WRITER;
            i2s_stream_writer_http_mp3 = i2s_stream_init(&i2s_cfg_p);

            mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
            mp3_decoder_http_mp3 = mp3_decoder_init(&mp3_cfg);

            audio_pipeline_register(pipeline, http_stream_reader_http_mp3,
                                    "http");
            audio_pipeline_register(pipeline, mp3_decoder_http_mp3, "mp3");
            audio_pipeline_register(pipeline, i2s_stream_writer_http_mp3,
                                    "i2s");
            ESP_LOGI(TAG,
                     "[ out ] Link it together "
                     "http_stream-->mp3_decoder_http_mp3-->i2s_stream-->[codec_"
                     "chip]");
            audio_pipeline_link(pipeline,
                                (const char* []){"http", "mp3", "i2s"}, 3);
            break;
        case OUTPUT_STREAM_SPIFFS: {
            ESP_LOGI(TAG, "[ * ] Play from spiffs");
            spiffs_stream_cfg_t flash_cfg = SPIFFS_STREAM_CFG_DEFAULT();
            flash_cfg.type = AUDIO_STREAM_READER;
            spiffs_stream_reader_play = spiffs_stream_init(&flash_cfg);

            mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
            mp3_decoder_play = mp3_decoder_init(&mp3_cfg);

            i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
            i2s_cfg.type = AUDIO_STREAM_WRITER;
            i2s_stream_writer_play = i2s_stream_init(&i2s_cfg);

            audio_pipeline_register(pipeline, spiffs_stream_reader_play,
                                    "spiffs");
            audio_pipeline_register(pipeline, mp3_decoder_play, "mp3");
            audio_pipeline_register(pipeline, i2s_stream_writer_play, "i2s");

            ESP_LOGI(TAG,
                     "[ out ] Link it together "
                     "[flash]-->spiffs-->mp3_decoder-->i2s_stream-->[codec_"
                     "chip]");
            audio_pipeline_link(pipeline,
                                (const char* []){"spiffs", "mp3", "i2s"}, 3);
            break;
        }
        case OUTPUT_STREAM_SDCARD: {
            ESP_LOGI(TAG, "[ * ] Play from sdcard");
            fatfs_stream_cfg_t fatfs_cfg = FATFS_STREAM_CFG_DEFAULT();
            fatfs_cfg.type = AUDIO_STREAM_READER;
            fatfs_stream_reader_sdcard = fatfs_stream_init(&fatfs_cfg);

            i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
            i2s_cfg.type = AUDIO_STREAM_WRITER;
            i2s_stream_writer_sdcard = i2s_stream_init(&i2s_cfg);

            mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
            mp3_decoder_sdcard = mp3_decoder_init(&mp3_cfg);

            audio_pipeline_register(pipeline, fatfs_stream_reader_sdcard,
                                    "file");
            audio_pipeline_register(pipeline, mp3_decoder_sdcard, "mp3");
            audio_pipeline_register(pipeline, i2s_stream_writer_sdcard, "i2s");

            ESP_LOGI(TAG,
                     "[3.5] Link it together "
                     "[sdcard]-->fatfs_stream-->mp3_decoder-->i2s_stream-->["
                     "codec_chip]");
            audio_pipeline_link(pipeline,
                                (const char* []){"file", "mp3", "i2s"}, 3);
            break;
        }

        default:
            ESP_LOGE(TAG, "The %d type is not supported!", output_type);
            break;
    }
    return pipeline;
}

audio_pipeline_handle_t create_rec_pipeline(input_stream_t input_type) {
    audio_pipeline_handle_t pipeline;
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline);
    input_type_flag = input_type;
    switch (input_type) {
        case INPUT_STREAM_ASR: {
            ESP_LOGI(TAG, "[ input ] Create INPUT_STREAM_ASR");
            i2s_stream_cfg_t i2s_asr_cfg = I2S_STREAM_CFG_DEFAULT();
            i2s_asr_cfg.i2s_config.sample_rate = 48000;
            i2s_asr_cfg.type = AUDIO_STREAM_READER;
            i2s_stream_reader_asr = i2s_stream_init(&i2s_asr_cfg);

            rsp_filter_cfg_t rsp_asr_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
            rsp_asr_cfg.src_rate = 48000;
            rsp_asr_cfg.src_ch = 2;
            rsp_asr_cfg.dest_rate = 16000;
            rsp_asr_cfg.dest_ch = 1;
            rsp_asr_cfg.type = AUDIO_CODEC_TYPE_ENCODER;
            filter_asr = rsp_filter_init(&rsp_asr_cfg);

            raw_stream_cfg_t raw_asr_cfg = {
                .out_rb_size = 8 * 1024,
                .type = AUDIO_STREAM_READER,
            };
            raw_read_asr = raw_stream_init(&raw_asr_cfg);

            audio_pipeline_register(pipeline, i2s_stream_reader_asr, "i2s");
            audio_pipeline_register(pipeline, filter_asr, "filter");
            audio_pipeline_register(pipeline, raw_read_asr, "raw_read");

            ESP_LOGI(TAG,
                     "[ input ] Link elements together "
                     "[codec_chip]-->i2s_stream-->filter_asr-->raw-->[SR]");
            audio_pipeline_link(
                pipeline, (const char* []){"i2s", "filter", "raw_read"}, 3);
            break;
        }
        case INPUT_STREAM_REC:
            ESP_LOGI(TAG, "[ input ] Create INPUT_STREAM_REC");
            http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
            http_cfg.type = AUDIO_STREAM_WRITER;
            http_cfg.event_handle = _http_stream_event_handle;
            http_stream_writer_rec = http_stream_init(&http_cfg);

            wav_encoder_cfg_t wav_cfg = DEFAULT_WAV_ENCODER_CONFIG();
            wav_encoder_rec = wav_encoder_init(&wav_cfg);

            i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
            i2s_cfg.type = AUDIO_STREAM_READER;
            i2s_stream_reader_rec = i2s_stream_init(&i2s_cfg);

            audio_pipeline_register(pipeline, i2s_stream_reader_rec, "i2s");
            audio_pipeline_register(pipeline, wav_encoder_rec, "wav");
            audio_pipeline_register(pipeline, http_stream_writer_rec, "http");

            ESP_LOGI(TAG,
                     "[ input ] Link it together "
                     "[codec_chip]-->i2s_stream->wav_encoder_rec->http_stream--"
                     ">[http_server]");
            audio_pipeline_link(pipeline,
                                (const char* []){"i2s", "wav", "http"}, 3);
            break;
    }
    return pipeline;
}

static void audio_free_pipline(audio_pipeline_handle_t afp) {
    audio_pipeline_stop(afp);
    audio_pipeline_wait_for_stop(afp);
    audio_pipeline_terminate(afp);
}

void stop_all_pipelines(void) {
    audio_free_pipline(pipeline_rec);
    audio_free_pipline(pipeline_http_mp3);
    audio_free_pipline(pipeline_asr);
    audio_free_pipline(pipeline_sdcard);
    switch (input_type_flag) {
        case INPUT_STREAM_ASR:
            audio_pipeline_unregister(pipeline_asr, i2s_stream_reader_asr);
            audio_pipeline_unregister(pipeline_asr, raw_read_asr);
            audio_pipeline_unregister(pipeline_asr, filter_asr);
            audio_pipeline_remove_listener(pipeline_asr);
            audio_pipeline_deinit(pipeline_asr);
            audio_element_deinit(i2s_stream_reader_asr);
            audio_element_deinit(raw_read_asr);
            audio_element_deinit(filter_asr);
            wakenet->destroy(model_data);
            model_data = NULL;
            break;
        case INPUT_STREAM_REC:
            break;
    }
    switch (output_type_flag) {
        case OUTPUT_STREAM_SPIFFS:
            audio_pipeline_unregister(pipeline_play, spiffs_stream_reader_play);
            audio_pipeline_unregister(pipeline_play, mp3_decoder_play);
            audio_pipeline_unregister(pipeline_play, i2s_stream_writer_play);
            audio_pipeline_remove_listener(pipeline_play);
            audio_pipeline_deinit(pipeline_play);
            audio_element_deinit(spiffs_stream_reader_play);
            audio_element_deinit(mp3_decoder_play);
            audio_element_deinit(i2s_stream_writer_play);
            break;
        case OUTPUT_STREAM_HTTP:
            audio_pipeline_unregister(pipeline_http_mp3,
                                      http_stream_reader_http_mp3);
            audio_pipeline_unregister(pipeline_http_mp3, mp3_decoder_http_mp3);
            audio_pipeline_unregister(pipeline_http_mp3,
                                      i2s_stream_writer_http_mp3);
            audio_pipeline_remove_listener(pipeline_http_mp3);
            audio_pipeline_deinit(pipeline_http_mp3);
            audio_element_deinit(http_stream_reader_http_mp3);
            audio_element_deinit(mp3_decoder_http_mp3);
            audio_element_deinit(i2s_stream_writer_http_mp3);
            break;
        case OUTPUT_STREAM_SDCARD:
            audio_pipeline_unregister(pipeline_sdcard,
                                      fatfs_stream_reader_sdcard);
            audio_pipeline_unregister(pipeline_sdcard, mp3_decoder_sdcard);
            audio_pipeline_unregister(pipeline_sdcard,
                                      i2s_stream_writer_sdcard);
            audio_pipeline_remove_listener(pipeline_sdcard);
            audio_pipeline_deinit(pipeline_sdcard);
            audio_element_deinit(spiffs_stream_reader_play);
            audio_element_deinit(mp3_decoder_sdcard);
            audio_element_deinit(i2s_stream_writer_sdcard);
            break;
        default:
            break;
    }
}

void Led_Display(display_pattern_t display_ctl) {
    display_service_set_pattern(disp_serv, display_ctl, 0);
}
