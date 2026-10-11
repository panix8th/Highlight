#pragma once

/* Highlight links against the official OBS Studio runtime instead of building
 * libobs. This replaces the header that the OBS build normally generates. */

#define OBS_DATA_PATH "../../data"
#define OBS_PLUGIN_PATH "../../obs-plugins/64bit"
#define OBS_PLUGIN_DESTINATION "obs-plugins/64bit"

#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
