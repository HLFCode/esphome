#include "http_request_idf.h"
#ifdef USE_ESP_IDF

#include "esphome/components/network/util.h"
#include "esphome/components/watchdog/watchdog.h"

#include "esphome/core/application.h"
#include "esphome/core/log.h"

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

namespace esphome {
namespace http_request {

static const char *const TAG = "http_request.idf";

void HttpRequestIDF::dump_config() {
  HttpRequestComponent::dump_config();
  ESP_LOGCONFIG(TAG, "  Buffer Size RX: %u", this->buffer_size_rx_);
  ESP_LOGCONFIG(TAG, "  Buffer Size TX: %u", this->buffer_size_tx_);
}
//#define DEBUG_IDF 1
#if DEBUG_IDF
esp_err_t _http_event_handle(esp_http_client_event_t *evt) {
  switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
      ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
      break;
    case HTTP_EVENT_ON_CONNECTED:
      ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
      break;
    case HTTP_EVENT_HEADER_SENT:
      ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
      break;
    case HTTP_EVENT_ON_HEADER:
      ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
      break;
    case HTTP_EVENT_ON_DATA:
      if (esp_http_client_is_chunked_response(evt->client)) {
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, chunked, len=%d", evt->data_len);
      } else {
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, not chunked, len=%d, '%s'", evt->data_len, (char *) evt->data);
      }
      break;
    case HTTP_EVENT_ON_FINISH:
      ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
      break;
    case HTTP_EVENT_DISCONNECTED:
      ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
      break;
      // case HTTP_EVENT_REDIRECT:
      //     ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
      //     break;
  }
  return ESP_OK;
}
#endif

std::shared_ptr<HttpContainer> HttpRequestIDF::start(std::string url, std::string method, std::string body,
                                                     std::list<Header> headers) {
  if (!network::is_connected()) {
    this->status_momentary_error("failed", 1000);
    ESP_LOGE(TAG, "HTTP Request failed; Not connected to network");
    return nullptr;
  }

  esp_http_client_method_t method_idf;
  if (method == "GET") {
    method_idf = HTTP_METHOD_GET;
  } else if (method == "POST") {
    method_idf = HTTP_METHOD_POST;
  } else if (method == "PUT") {
    method_idf = HTTP_METHOD_PUT;
  } else if (method == "DELETE") {
    method_idf = HTTP_METHOD_DELETE;
  } else if (method == "PATCH") {
    method_idf = HTTP_METHOD_PATCH;
  } else {
    this->status_momentary_error("failed", 1000);
    ESP_LOGE(TAG, "HTTP Request failed; Unsupported method");
    return nullptr;
  }

  bool secure = url.find("https:") != std::string::npos;

  esp_http_client_config_t config = {};

  config.url = url.c_str();
  config.method = method_idf;
  config.timeout_ms = this->timeout_;
  config.disable_auto_redirect = !this->follow_redirects_;
  config.max_redirection_count = this->redirect_limit_;
  config.auth_type = HTTP_AUTH_TYPE_BASIC;
#if DEBUG_IDF
  config.event_handler = _http_event_handle;
#endif
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
  if (secure) {
    config.crt_bundle_attach = esp_crt_bundle_attach;
  }
#endif

  if (this->useragent_ != nullptr) {
    config.user_agent = this->useragent_;
  }

  config.buffer_size = this->buffer_size_rx_;
  config.buffer_size_tx = this->buffer_size_tx_;

  const uint32_t start = millis();
  watchdog::WatchdogManager wdm(this->get_watchdog_timeout());

  esp_http_client_handle_t client = esp_http_client_init(&config);

  std::shared_ptr<HttpContainerIDF> container = std::make_shared<HttpContainerIDF>(client);
  container->set_parent(this);

  container->set_secure(secure);

  for (const auto &header : headers) {
    esp_http_client_set_header(client, header.name, header.value);
  }

  const int body_len = body.length();

  esp_err_t err = esp_http_client_open(client, body_len);
  if (err != ESP_OK) {
    this->status_momentary_error("failed", 1000);
    ESP_LOGE(TAG, "HTTP Request failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return nullptr;
  }

  if (body_len > 0) {
    int write_left = body_len;
    int write_index = 0;
    const char *buf = body.c_str();
    while (write_left > 0) {
      int written = esp_http_client_write(client, buf + write_index, write_left);
      if (written < 0) {
        err = ESP_FAIL;
        break;
      }
      write_left -= written;
      write_index += written;
    }
  }

  if (err != ESP_OK) {
    this->status_momentary_error("failed", 1000);
    ESP_LOGE(TAG, "HTTP Request failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return nullptr;
  }

  container->feed_wdt();
  // calling esp_http_client_fetch_headers can result in a
  // "HTTP_CLIENT: Body received in fetch header state, 0xXXXXXXX, nnn" message
  // nnn is the size of the body retrieved
  // Even though this client already has the response body it still
  // needs to be "read"
  container->content_length = esp_http_client_fetch_headers(client);
  container->feed_wdt();
  container->status_code = esp_http_client_get_status_code(client);
  container->feed_wdt();

  // Check for a chunked response where the content length could be 0 or negative
  if (esp_http_client_is_chunked_response(client)) {
    container->response_chunked = true;
  }

  if (is_success(container->status_code)) {
    container->duration_ms = millis() - start;
    return container;
  }

  if (this->follow_redirects_) {
    auto num_redirects = this->redirect_limit_;
    while (is_redirect(container->status_code) && num_redirects > 0) {
      err = esp_http_client_set_redirection(client);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_http_client_set_redirection failed: %s", esp_err_to_name(err));
        this->status_momentary_error("failed", 1000);
        esp_http_client_cleanup(client);
        return nullptr;
      }
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE
      char redirect_url[256]{};
      if (esp_http_client_get_url(client, redirect_url, sizeof(redirect_url) - 1) == ESP_OK) {
        ESP_LOGV(TAG, "redirecting to url: %s", redirect_url);
      }
#endif
      err = esp_http_client_open(client, 0);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_http_client_open failed: %s", esp_err_to_name(err));
        this->status_momentary_error("failed", 1000);
        esp_http_client_cleanup(client);
        return nullptr;
      }

      container->feed_wdt();
      container->content_length = esp_http_client_fetch_headers(client);
      container->feed_wdt();
      container->status_code = esp_http_client_get_status_code(client);
      container->feed_wdt();

      // Check for a chunked response where the content length could be 0 or negative
      if (esp_http_client_is_chunked_response(client)) {
        container->response_chunked = true;
      }
      if (is_success(container->status_code)) {
        container->duration_ms = millis() - start;
        return container;
      }

      num_redirects--;
    }

    if (num_redirects == 0) {
      ESP_LOGW(TAG, "Reach redirect limit count=%d", this->redirect_limit_);
    }
  }

  ESP_LOGE(TAG, "HTTP Request failed; URL: %s; Code: %d", url.c_str(), container->status_code);
  this->status_momentary_error("failed", 1000);
  return container;
}

int HttpContainerIDF::read(uint8_t *buf, size_t max_len) {
  const uint32_t start = millis();
  watchdog::WatchdogManager wdm(this->parent_->get_watchdog_timeout());

  int max_chars_to_read = 0;
  if (this->response_chunked) {
    // we don't know how many bytes the server will send with trandfer-encoding chunked
    // so set the read limit as large as possible
    max_chars_to_read = max_len;
  } else {
    max_chars_to_read = std::min(max_len, this->content_length - this->bytes_read_);
  }

  if (max_chars_to_read == 0) {
    this->duration_ms += (millis() - start);
    return 0;
  }

  this->feed_wdt();
  int read_len = esp_http_client_read(this->client_, (char *) buf, max_chars_to_read);
  this->bytes_read_ += read_len;

  this->duration_ms += (millis() - start);

  return read_len;
}

void HttpContainerIDF::end() {
  watchdog::WatchdogManager wdm(this->parent_->get_watchdog_timeout());

  esp_http_client_close(this->client_);
  esp_http_client_cleanup(this->client_);
}

}  // namespace http_request
}  // namespace esphome

#endif  // USE_ESP_IDF
