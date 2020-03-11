#ifndef _SWTZ_SERVICE_H_
#define _SWTZ_SERVICE_H_
#include "audio_service.h"

/*
 * @brief Create the swtz service
 *
 * @return
 *     - NULL, Fail
 *     - Others, Success
 */
audio_service_handle_t swtz_service_create(void);

/*
 * @brief Get swtz service state
 *
 * @return The state of service
 *
 */
service_state_t swtz_service_state_get();




#endif
