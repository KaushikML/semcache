#pragma once

// Single place for Asio/Beast includes and namespace aliases.
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

namespace semcache {

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

template <class T>
using Task = net::awaitable<T>;

// Completion token returning (error_code, results...) instead of throwing.
inline constexpr auto use_nothrow = net::as_tuple(net::use_awaitable);

}  // namespace semcache
