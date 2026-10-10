#pragma once

#include "esphome/core/defines.h"

#ifdef USE_OCLEAN_CLOUD_RECEIVER

#include "esphome/components/web_server_base/web_server_base.h"

namespace esphome::oclean {

class OcleanHub;

// Attaches the cloud session receiver to the shared ESPHome web server (first
// call) and registers the hub for MAC routing. Call once from the hub setup.
void cloud_receiver_register(OcleanHub *hub, web_server_base::WebServerBase *base);

}  // namespace esphome::oclean

#endif  // USE_OCLEAN_CLOUD_RECEIVER
