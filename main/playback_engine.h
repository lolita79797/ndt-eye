#ifndef PLAYBACK_ENGINE_H
#define PLAYBACK_ENGINE_H

#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t playback_engine_init(void);
esp_err_t playback_engine_play_file(const char *filename);
void playback_engine_stop(void);
void playback_engine_stop_and_free(void);
bool playback_engine_is_playing(void);
const char* playback_engine_get_current_file(void);


#ifdef __cplusplus
}
#endif

#endif // PLAYBACK_ENGINE_H
