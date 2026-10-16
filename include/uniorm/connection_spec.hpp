#pragma once

// Naming a database without writing one backend's connection string: the
// pieces are handed to the backend that owns the scheme, which is the only
// place that knows how they have to be spelled. This layer because the backend
// contract names the type; the connection facade sits above that contract and
// a backend has no reason to carry it just to read four fields.

#include <string>

namespace uniorm {

// `scheme` is the only field the core reads: it picks the backend. The
// grammar of `target` belongs to that backend -- a registered DSN, a
// host[:port]/dbname, a service string -- and an empty `user` or `password`
// means none was supplied, which is not the same thing as an empty one. The
// pieces are owned because a pool keeps this recipe to open with later, after
// whatever the caller handed over is gone.
struct connection_spec {
  std::string scheme;
  std::string target;
  std::string user;
  std::string password;
};

}  // namespace uniorm
