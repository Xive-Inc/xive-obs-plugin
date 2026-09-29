#pragma once

#include <obs-module.h>

/*
 * Logging goes to the normal OBS log, which users paste into support threads.
 *
 * NEVER log an access token, a refresh token, a PKCE verifier or a stream key through these.
 * A stream key is a publish credential: anyone holding it can take over the channel, and OBS logs
 * get shared freely. Log the fact that a credential arrived, never the credential.
 */
#define xive_log(level, format, ...) blog(level, "[xive] " format, ##__VA_ARGS__)

#define xive_info(format, ...) xive_log(LOG_INFO, format, ##__VA_ARGS__)
#define xive_warn(format, ...) xive_log(LOG_WARNING, format, ##__VA_ARGS__)
#define xive_error(format, ...) xive_log(LOG_ERROR, format, ##__VA_ARGS__)
#define xive_debug(format, ...) xive_log(LOG_DEBUG, format, ##__VA_ARGS__)
