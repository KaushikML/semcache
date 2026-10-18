#pragma once

// Transport-neutral sink for a response to the client. The request pipeline only talks to this
// interface, so it can be tested without sockets and reused for other front-ends.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "net/asio.hpp"

namespace semcache {

using HeaderList = std::vector<std::pair<std::string, std::string>>;

// Named empty list: GCC 13 ICEs on braced-init temporaries inside co_await expressions, so call
// sites pass named lists instead of `{}`.
inline const HeaderList kNoHeaders{};

class ResponseWriter {
 public:
  virtual ~ResponseWriter() = default;

  // Each returns false if the client is gone; callers should stop producing output.
  virtual Task<bool> send(unsigned status, std::string_view content_type, std::string body,
                          const HeaderList& headers) = 0;
  virtual Task<bool> begin_stream(unsigned status, std::string_view content_type,
                                  const HeaderList& headers) = 0;
  virtual Task<bool> write_chunk(std::string_view data) = 0;
  virtual Task<bool> end_stream() = 0;
};

}  // namespace semcache
