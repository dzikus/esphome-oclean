#include "oclean_cloud_receiver.h"

#ifdef USE_OCLEAN_CLOUD_RECEIVER

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "esphome/core/log.h"
#include "oclean.h"
#include "oclean_protocol.h"

namespace esphome::oclean {

static const char *const CLOUD_TAG = "oclean.cloud";

// bound the kept body; a bogus Content-Length must not grow it
static constexpr size_t CLOUD_MAX_BODY = 2048;

namespace {

// runs on the web server task; records cross to the main loop via the hub queue
class OcleanCloudReceiver : public AsyncWebHandler {
 public:
  void add_hub(OcleanHub *hub) { this->hubs_.push_back(hub); }

  bool canHandle(AsyncWebServerRequest *request) const override {
    httpd_req_t *r = *request;
    return r != nullptr && std::strncmp(r->uri, "/OTA/", 5) == 0;
  }

  bool isRequestHandlerTrivial() const override { return false; }  // need the POST body

  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override {
    (void)request;
    (void)total;
    if (index == 0)
      this->body_.clear();
    if (this->body_.size() < CLOUD_MAX_BODY)
      this->body_.append(reinterpret_cast<const char *>(data), std::min(len, CLOUD_MAX_BODY - this->body_.size()));
  }

  void handleRequest(AsyncWebServerRequest *request) override {
    httpd_req_t *r = *request;
    const char *uri = (r != nullptr) ? r->uri : "";
    if (std::strstr(uri, "UploadBrushRecord") != nullptr) {
      this->handle_upload_(request);
    } else if (std::strstr(uri, "UploadingMacWiFi") != nullptr) {
      // currentTime sets the brush clock; the wifi password in the body is ignored
      std::string now;
      for (OcleanHub *hub : this->hubs_) {
        now = hub->cloud_current_time();
        if (!now.empty())
          break;
      }
      if (now.empty()) {
        request->send(200, "application/json", "{}");
      } else {
        std::string const js = R"({"data":{"currentTime":")" + now + R"("}})";
        request->send(200, "application/json", js.c_str());
      }
    } else if (std::strstr(uri, "OTAUpGrade") != nullptr || std::strstr(uri, "GetOTACounterMode") != nullptr) {
      request->send(200, "application/json", R"({"state":false})");
    } else {
      request->send(200, "application/json", "{}");
    }
    this->body_.clear();
  }

 protected:
  OcleanHub *find_hub_(uint64_t mac) {
    for (OcleanHub *hub : this->hubs_) {
      if (hub->brush_address() == mac)
        return hub;
    }
    return nullptr;
  }

  // "ok" erases the brush's record, so ack only what was published: keep on the
  // first sight, ok the re-upload once the hub reports it captured
  void handle_upload_(AsyncWebServerRequest *request) {
    static const char *const KEEP = R"({"data":"received"})";
    static const char *const ACK = R"({"data":"ok"})";
    std::string mac_text;
    std::string data_text;
    if (!cloud_body_field(this->body_.data(), this->body_.size(), "mac", &mac_text) ||
        !cloud_body_field(this->body_.data(), this->body_.size(), "brushdata", &data_text)) {
      request->send(200, "application/json", KEEP);
      return;
    }
    uint64_t mac = 0;
    if (!parse_mac_u64(mac_text, &mac)) {
      request->send(200, "application/json", KEEP);
      return;
    }
    OcleanHub *hub = this->find_hub_(mac);
    if (hub == nullptr) {
      ESP_LOGD(CLOUD_TAG, "record for unknown brush %s ignored", mac_text.c_str());
      request->send(200, "application/json", KEEP);
      return;
    }
    std::vector<uint8_t> bytes;
    SessionRecord rec{};
    if (!parse_hex_bytes(data_text, &bytes) || !decode_session_record_v20(bytes.data(), bytes.size(), &rec)) {
      ESP_LOGW(CLOUD_TAG, "record from %s did not decode (%u hex chars)", mac_text.c_str(),
               static_cast<unsigned>(data_text.size()));
      request->send(200, "application/json", KEEP);
      return;
    }
    uint32_t const epoch = session_record_epoch(rec);
    ESP_LOGD(CLOUD_TAG, "record from %s: %04u-%02u-%02u %02u:%02u:%02u scheme=%u score=%u raw=%s", mac_text.c_str(),
             rec.year, rec.month, rec.day, rec.hour, rec.minute, rec.second, static_cast<unsigned>(rec.scheme),
             static_cast<unsigned>(rec.score), data_text.c_str());
    if (hub->cloud_record_captured(epoch)) {
      request->send(200, "application/json", ACK);
      return;
    }
    hub->enqueue_cloud_record(rec, epoch);
    request->send(200, "application/json", KEEP);
  }

  std::vector<OcleanHub *> hubs_;
  std::string body_;
};

OcleanCloudReceiver *attach_receiver(web_server_base::WebServerBase *base) {
  static OcleanCloudReceiver *receiver = nullptr;
  if (receiver == nullptr) {
    receiver = new OcleanCloudReceiver();  // NOLINT(cppcoreguidelines-owning-memory)
    base->add_handler_without_auth(receiver);
    // add_handler is only honored at init(); if the server already started, add it live
    if (base->get_server() != nullptr)
      base->get_server()->addHandler(receiver);
    ESP_LOGI(CLOUD_TAG, "session receiver attached to the web server on port %u",
             static_cast<unsigned>(base->get_port()));
  }
  return receiver;
}

}  // namespace

void cloud_receiver_register(OcleanHub *hub, web_server_base::WebServerBase *base) {
  if (hub == nullptr || base == nullptr)
    return;
  attach_receiver(base)->add_hub(hub);
}

}  // namespace esphome::oclean

#endif  // USE_OCLEAN_CLOUD_RECEIVER
