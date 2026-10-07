#pragma once

#include "esphome/core/defines.h"

#ifdef USE_OCLEAN_CLOUD_RECEIVER

namespace esphome::oclean {

class OcleanHub;

// Registers a hub with the node-level cloud receiver and starts the http server
// on the hub's receiver port if one is not already listening there. Routing to
// the hub is by the brush MAC in each request body, so several hubs can share
// one server. Call once from the hub setup when the receiver is enabled.
void cloud_receiver_register(OcleanHub *hub);

}  // namespace esphome::oclean

#endif  // USE_OCLEAN_CLOUD_RECEIVER
