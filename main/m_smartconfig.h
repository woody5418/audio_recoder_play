#ifndef _M_SMARTCONFIG_H_
#define _M_SMARTCONFIG_H_

#include <stdlib.h>
#include <string.h>
#include "display_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

void Led_Display(display_pattern_t display_ctl);
void Break_Wifi_Connect(void);
void Wifi_Init_Airkiss(void);

#endif
