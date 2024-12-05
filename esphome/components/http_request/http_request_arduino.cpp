#include "http_request_arduino.h"

#ifdef USE_ARDUINO

#include "esphome/components/network/util.h"
#include "esphome/components/watchdog/watchdog.h"

#include "esphome/core/application.h"
#include "esphome/core/defines.h"
#include "esphome/core/log.h"

namespace esphome {
namespace http_request {

static const char *const TAG = "http_request.arduino";

std::shared_ptr<HttpContainer> HttpRequestArduino::start(std::string url, std::string method, std::string body,
                                                         std::list<Header> headers) {
  if (!network::is_connected()) {
    this->status_momentary_error("failed", 1000);
    ESP_LOGW(TAG, "HTTP Request failed; Not connected to network");
    return nullptr;
  }

  std::shared_ptr<HttpContainerArduino> container = std::make_shared<HttpContainerArduino>();
  container->set_parent(this);

  const uint32_t start = millis();

  bool secure = url.find("https:") != std::string::npos;
  container->set_secure(secure);

  watchdog::WatchdogManager wdm(this->get_watchdog_timeout());

  if (this->follow_redirects_) {
    container->client_.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    container->client_.setRedirectLimit(this->redirect_limit_);
  } else {
    container->client_.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  }

#if defined(USE_ESP8266)
  std::unique_ptr<WiFiClient> stream_ptr;
#ifdef USE_HTTP_REQUEST_ESP8266_HTTPS
  if (secure) {
    ESP_LOGV(TAG, "ESP8266 HTTPS connection with WiFiClientSecure");
    stream_ptr = std::make_unique<WiFiClientSecure>();
    WiFiClientSecure *secure_client = static_cast<WiFiClientSecure *>(stream_ptr.get());
    secure_client->setBufferSizes(512, 512);
    secure_client->setInsecure();
  } else {
    stream_ptr = std::make_unique<WiFiClient>();
  }
#else
  ESP_LOGV(TAG, "ESP8266 HTTP connection with WiFiClient");
  if (secure) {
    ESP_LOGE(TAG, "Can't use HTTPS connection with esp8266_disable_ssl_support");
    return nullptr;
  }
  stream_ptr = std::make_unique<WiFiClient>();
#endif  // USE_HTTP_REQUEST_ESP8266_HTTPS

#if USE_ARDUINO_VERSION_CODE >= VERSION_CODE(3, 1, 0)  // && USE_ARDUINO_VERSION_CODE < VERSION_CODE(?, ?, ?)
  if (!secure) {
    ESP_LOGW(TAG, "Using HTTP on Arduino version >= 3.1 is **very** slow. Consider setting framework version to 3.0.2 "
                  "in your YAML, or use HTTPS");
  }
#endif  // USE_ARDUINO_VERSION_CODE
  bool status = container->client_.begin(*stream_ptr, url.c_str());

#elif defined(USE_RP2040)
  if (secure) {
    container->client_.setInsecure();
  }
  bool status = container->client_.begin(url.c_str());
#elif defined(USE_ESP32)
  bool status = container->client_.begin(url.c_str());
#endif

  App.feed_wdt();

  if (!status) {
    ESP_LOGW(TAG, "HTTP Request failed; URL: %s", url.c_str());
    container->end();
    this->status_momentary_error("failed", 1000);
    return nullptr;
  }

  container->client_.setReuse(true);
  container->client_.setTimeout(this->timeout_);
#if defined(USE_ESP32)
  container->client_.setConnectTimeout(this->timeout_);
#endif

  if (this->useragent_ != nullptr) {
    container->client_.setUserAgent(this->useragent_);
  }
  for (const auto &header : headers) {
    container->client_.addHeader(header.name, header.value, false, true);
  }

  // returned needed headers must be collected before the requests
  static const char *header_keys[] = {"Content-Length", "Content-Type"};
  static const size_t HEADER_COUNT = sizeof(header_keys) / sizeof(header_keys[0]);
  container->client_.collectHeaders(header_keys, HEADER_COUNT);

  App.feed_wdt();
  container->status_code = container->client_.sendRequest(method.c_str(), body.c_str());
  App.feed_wdt();
  if (container->status_code < 0) {
    ESP_LOGW(TAG, "HTTP Request failed; URL: %s; Error: %s", url.c_str(),
             HTTPClient::errorToString(container->status_code).c_str());
    this->status_momentary_error("failed", 1000);
    container->end();
    return nullptr;
  }

  if (!is_success(container->status_code)) {
    ESP_LOGE(TAG, "HTTP Request failed; URL: %s; Code: %d", url.c_str(), container->status_code);
    this->status_momentary_error("failed", 1000);
    // Still return the container, so it can be used to get the status code and error message
  }

  int content_length = container->client_.getSize();
  container->response_chunked = (bool) (content_length < 0);
  ESP_LOGD(TAG, "Content-Length: %d", content_length);
  container->content_length = (size_t) content_length;
  container->duration_ms = millis() - start;

  return container;
}

int HttpContainerArduino::read(uint8_t *buf, size_t max_len) {
  /* 
  This is repeatedly called by 'play' until buf is full (max_len=0) or this returns 0
  Chunked data:
    max_len will be as large as possible so buf can be filled as defined by this method
    The stream data has the length at the start of the chunk (so we need to read some of it to find out how long it is)
    The length information is one or two byes, ascii encoded, terminated by cr,lf
  Non-chunked data:
    max_len is always sized so that stream reads won't be larger than the server will be sending
    Non-chunked data has a known length (container->content_length)
  
  For both chunked and non-chunked the data might already be in the stream or be sent in delayed packets
  Either way stream_ptr->readBytes needs to be called with the right buffer start address and exactly the tight number of bytes to read
  */
  const uint32_t start = millis();
  watchdog::WatchdogManager wdm(this->parent_->get_watchdog_timeout());

  WiFiClient *stream_ptr = this->client_.getStreamPtr();
  if (stream_ptr == nullptr) {
    ESP_LOGE(TAG, "Stream pointer vanished!");
    return -1;
  }
  
  int bytes_to_read = 0;
  int chunk_length = 0;
  int available_data = stream_ptr->available();
  const uint8_t cr = 0x0D;
  const uint8_t lf = 0x0A;
  if (this->response_chunked) {
    // The data is chunked so we don't know how much to read from the stream
    // There's nothing waiting to be read in the stream so we need to wait until the server sends 
    // at least 4 bytes to find out how long the chunk is
    uint32_t available_start = millis();
    int stream_read_count = stream_ptr->readBytes(buf, 4);
    if (stream_read_count < 4) {
      ESP_LOGE(TAG, "Server did no send enough data to decode the chunk size");
      return -1;
    }
    // If the chunk size is less than 255 bytes (0xF) the 4th byte will be data as the chunk
    // length only occupies the first byte returned
    // Decode the chunk length (stoi ignores cr/lf)
    chunk_length = std::stoi((char*) buf, 0, 16);
    if ((chunk_length < 0) || (chunk_length > 0xFF)) {
      ESP_LOGE(TAG, "Invalid chunk length %d", chunk_length);
      return -1;
    }
    if (chunk_length > max_len) {
      ESP_LOGE(TAG, "Buffer too small (%d bytes) for chunk of %d bytes", max_len, chunk_length);
      return -1;
    }
    if (chunk_length == 0) {
      // All the data is read
      // empty the buffer as there is one unread byte (lf)
      int z = stream_ptr->readBytes(buf + stream_read_count, 1);
      return 0;
    }
    // Now we have the chunk length we know how many bytes to read
    // Unfortunately this is not fixed as the chunk length header can be 0ne or two bytes (plus cr/lf)
    int crlf_offset = 2;
    bytes_to_read = chunk_length + 2; // two extra bytes for the chunk terminator
    if (chunk_length <= 0xF) {
      // Only one byte for the chunk length
      // One needed response databyte is in the buffer but in the wrong place
      this->bytes_read_+= 1;
      // shift the needed data byte to the start of the buffer
      *(buf) = *(buf + 3);
      // need to shift the pointer for the start of the next read as we've read one byte of data we actually need
      buf += 1;
      // Adjust the crlf offset to align with the changes to the pointer buf
      crlf_offset = 0;
      // available space in the buffer is one byte fewer
      max_len -= 1;
      bytes_to_read -= 1;
    }
    // Check the cunk terminator is correct
    if (*(buf + crlf_offset) != cr || *(buf + crlf_offset + 1) != lf) {
      ESP_LOGE(TAG, "Chunk length terminator not found");
      return -1;
    }
  } else {
    // Not chunked so we can safely read a known amount of data as defined by content_length and the bytes already read
    bytes_to_read = std::min(max_len, this->content_length - this->bytes_read_);
  }

  if (bytes_to_read == 0) {
    this->duration_ms += (millis() - start);
    return 0;
  }

  App.feed_wdt();
  int read_len = stream_ptr->readBytes(buf, bytes_to_read);
  this->duration_ms += (millis() - start);
  if (this->response_chunked) {
    // need to check for and discard the chunk terminator
    if (read_len < bytes_to_read) {
      ESP_LOGE(TAG, "Response too short, expected %d, received %d", bytes_to_read, read_len);
      return -1;
    }
    if (*(buf + read_len - 2) != cr || *(buf + read_len - 1) != lf) {
      ESP_LOGE(TAG, "Invalid chunk termiator");
      return -1;
    }
    // discard the terminator by using chunk_length not read_len
    this->bytes_read_ += chunk_length;
    return chunk_length;
  } else {
    // not chunked so just keep track of the number of bytes read
    this->bytes_read_ += read_len;
    return read_len;
  }
  

}

void HttpContainerArduino::end() {
  watchdog::WatchdogManager wdm(this->parent_->get_watchdog_timeout());
  this->client_.end();
}

}  // namespace http_request
}  // namespace esphome

#endif  // USE_ARDUINO
