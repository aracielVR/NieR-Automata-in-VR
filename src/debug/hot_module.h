// hot_module.h - [Debug] loads NieRVR_debug.dll (src/hot/) and reloads it whenever it's rebuilt.
#pragma once

void HotModulePoll();  // each game frame (Present): reloads if needed, then runs the module's tools
