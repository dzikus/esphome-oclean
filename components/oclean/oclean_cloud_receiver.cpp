#include "oclean_cloud_receiver.h"

#ifdef USE_OCLEAN_CLOUD_RECEIVER

#include <esp_http_server.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <vector>

#include "esphome/core/log.h"
#include "oclean.h"
#include "oclean_protocol.h"

namespace esphome::oclean {

static const char *const CLOUD_TAG = "oclean.cloud";

// Record body is a few hundred bytes (182 B record as hex plus the json). Read
// past this and the surplus is drained but dropped, so a bogus Content-Length
// cannot grow the buffer without bound.
static constexpr size_t CLOUD_MAX_BODY = 2048;

namespace {
struct CloudRegistry {
  std::vector<OcleanHub *> hubs;
  std::map<uint16_t, httpd_handle_t> servers;
};
}  // namespace

static CloudRegistry &registry() {
  static CloudRegistry reg;
  return reg;
}

static OcleanHub *find_hub_by_mac(uint64_t mac) {
  for (OcleanHub *hub : registry().hubs) {
    if (hub->brush_address() == mac)
      return hub;
  }
  return nullptr;
}

static esp_err_t send_json(httpd_req_t *req, const char *json) {
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, json);
}

// Reads the whole request body so the socket stays in sync, but keeps only the
// first CLOUD_MAX_BODY bytes.
static std::string read_body(httpd_req_t *req) {
  std::string body;
  char buf[256];
  size_t remaining = req->content_len;
  while (remaining > 0) {
    int const got = httpd_req_recv(req, buf, std::min(remaining, sizeof(buf)));
    if (got <= 0) {
      if (got == HTTPD_SOCK_ERR_TIMEOUT)
        continue;
      break;
    }
    size_t const n = static_cast<size_t>(got);
    remaining -= n;
    if (body.size() < CLOUD_MAX_BODY)
      body.append(buf, std::min(n, CLOUD_MAX_BODY - body.size()));
  }
  return body;
}

// A full brush record arrives as hex in "brushdata"; "mac" routes it to a hub.
// The reply decides the brush's store: "ok" erases the record, anything else
// keeps it. So a record is acked only once it has been published (the hub
// reports it captured); the first upload is published and kept, its re-upload
// is acked and dropped.
static esp_err_t handle_upload_record(httpd_req_t *req, const std::string &body) {
  static const char *const KEEP = R"({"data":"received"})";
  static const char *const ACK = R"({"data":"ok"})";
  std::string mac_text;
  std::string data_text;
  if (!cloud_body_field(body.data(), body.size(), "mac", &mac_text) ||
      !cloud_body_field(body.data(), body.size(), "brushdata", &data_text))
    return send_json(req, KEEP);
  uint64_t mac = 0;
  if (!parse_mac_u64(mac_text, &mac))
    return send_json(req, KEEP);
  OcleanHub *hub = find_hub_by_mac(mac);
  if (hub == nullptr) {
    ESP_LOGD(CLOUD_TAG, "record for unknown brush %s ignored", mac_text.c_str());
    return send_json(req, KEEP);
  }
  std::vector<uint8_t> bytes;
  SessionRecord rec{};
  if (!parse_hex_bytes(data_text, &bytes) || !decode_session_record_v20(bytes.data(), bytes.size(), &rec)) {
    ESP_LOGW(CLOUD_TAG, "record from %s did not decode (%u hex chars)", mac_text.c_str(),
             static_cast<unsigned>(data_text.size()));
    return send_json(req, KEEP);
  }
  uint32_t const epoch = session_record_epoch(rec);
  ESP_LOGD(CLOUD_TAG, "record from %s: %04u-%02u-%02u %02u:%02u:%02u scheme=%u score=%u raw=%s", mac_text.c_str(),
           rec.year, rec.month, rec.day, rec.hour, rec.minute, rec.second, static_cast<unsigned>(rec.scheme),
           static_cast<unsigned>(rec.score), data_text.c_str());
  if (hub->cloud_record_captured(epoch))
    return send_json(req, ACK);
  hub->enqueue_cloud_record(rec, epoch);
  return send_json(req, KEEP);
}

static esp_err_t handle_request(httpd_req_t *req) {
  std::string const body = read_body(req);
  const char *uri = req->uri;
  if (strstr(uri, "UploadBrushRecord") != nullptr)
    return handle_upload_record(req, body);
  if (strstr(uri, "UploadingMacWiFi") != nullptr) {
    // the brush takes its clock from currentTime; the wifi password in the body
    // is deliberately not read or stored
    int64_t now = 0;
    for (OcleanHub *hub : registry().hubs) {
      now = hub->cloud_now_epoch();
      if (now > 0)
        break;
    }
    if (now > 0) {
      char js[72];
      snprintf(js, sizeof(js), R"({"data":{"currentTime":"%lld"}})", static_cast<long long>(now));
      return send_json(req, js);
    }
    return send_json(req, "{}");
  }
  if (strstr(uri, "OTAUpGrade") != nullptr || strstr(uri, "GetOTACounterMode") != nullptr)
    return send_json(req, R"({"state":false})");  // no update is served from here
  return send_json(req, "{}");
}

static void start_server(uint16_t port) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = port;
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.lru_purge_enable = true;
  // json parse, hex decode and a record copy run on this task
  config.stack_size = 6144;
  httpd_handle_t server = nullptr;
  if (httpd_start(&server, &config) != ESP_OK) {
    ESP_LOGW(CLOUD_TAG, "http receiver failed to start on port %u", static_cast<unsigned>(port));
    return;
  }
  const httpd_method_t methods[] = {HTTP_POST, HTTP_GET, HTTP_PUT};
  for (httpd_method_t const method : methods) {
    httpd_uri_t handler = {};
    handler.uri = "/*";
    handler.method = method;
    handler.handler = handle_request;
    httpd_register_uri_handler(server, &handler);
  }
  registry().servers[port] = server;
  ESP_LOGI(CLOUD_TAG, "session receiver listening on port %u", static_cast<unsigned>(port));
}

void cloud_receiver_register(OcleanHub *hub) {
  if (hub == nullptr)
    return;
  registry().hubs.push_back(hub);
  uint16_t const port = hub->cloud_receiver_port();
  if (registry().servers.find(port) == registry().servers.end())
    start_server(port);
}

}  // namespace esphome::oclean

#endif  // USE_OCLEAN_CLOUD_RECEIVER
