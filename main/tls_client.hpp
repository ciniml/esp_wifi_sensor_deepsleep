/**
 * @file tls_client.hpp
 * @brief TLS Client module header
 */
// Copyright 2017 Kenta IDA
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
 
#ifndef TLS_CLIENT_HPP__
#define TLS_CLIENT_HPP__

#include <mbedtls/platform.h>
#include <mbedtls/net.h>
#include <mbedtls/esp_debug.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <mbedtls/certs.h>

#include <cstdint>

class TlsClient
{
private:
	static const char* TAG;

	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context ctr_drbg;
	mbedtls_ssl_context ssl;
	mbedtls_x509_crt cacert;
	mbedtls_ssl_config ssl_config;
	mbedtls_net_context server_fd;

	enum class State
	{
		NotInitialized,
		NotConnected,
		Connected,
	};

	State state;

public:
	bool is_connected() const { return this->state == State::Connected; }

	TlsClient() : state(State::NotInitialized) {}
	bool initialize(const  std::uint8_t* cert_pem, std::size_t cert_pem_length);
	bool connect(const char* server, const char* port);
	void disconnect();
	int read(std::uint8_t* data, std::size_t length);
	int write(const std::uint8_t* data, std::size_t length);
	void terminate();
};


#endif //TLS_CLIENT_HPP__
