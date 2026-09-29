#pragma once
/*
 * A stand-in for the obsconfig.h that OBS's own CMake generates.
 *
 * WHY THIS FILE EXISTS
 * libobs/obs-config.h does `#include "obsconfig.h"`, and that header is produced from
 * obsconfig.h.in when OBS itself is configured. macOS ships no libobs development package, so the
 * headers here come from a plain checkout of the obs-studio source, which has the .in template and
 * not the generated result.
 *
 * WHY THE VALUES DO NOT MATTER
 * Everything defined here feeds libobs's own path resolution — where OBS looks for its data files
 * and bundled plugins. A plugin never uses them: OBS is already running, has already resolved its
 * paths, and hands this module its own data directory through obs_module_file() /
 * obs_module_config_path(). These defines exist only so the preprocessor can get through
 * obs-config.h.
 *
 * The one to be careful about is the version macros, and those are NOT here — LIBOBS_API_*_VER
 * live in obs-config.h itself, come from the checked-out source, and are what OBS compares against
 * at load time. So the API version this module reports is still the real one from the tag that was
 * checked out. Keep the checkout at the same version as the OBS being targeted.
 */

#define OBS_DATA_PATH "data"
#define OBS_PLUGIN_PATH "obs-plugins"
#define OBS_PLUGIN_DESTINATION "obs-plugins"

#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
