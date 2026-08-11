#pragma once

#include <string>

#include <esp_transport.h>

// Creates an HTTP CONNECT transport. The caller owns the returned handle.
// Accepts http://host:port and http://user:password@host:port proxy URLs.
esp_transport_handle_t CreateQuotaProxyTransport(const std::string& proxy_url,
                                                  bool use_tls,
                                                  std::string& error);

// Returns a sanitized stage error. It never includes proxy credentials.
std::string GetQuotaProxyTransportError(esp_transport_handle_t transport);

// Performs the same HTTP CONNECT + TLS route used by quota refresh and returns
// a sanitized JSON diagnostic. It never includes proxy credentials.
std::string DiagnoseQuotaProxy(const std::string& proxy_url);
