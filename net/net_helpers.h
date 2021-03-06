#ifndef WEBRTC_BASE_NETHELPERS_H_
#define WEBRTC_BASE_NETHELPERS_H_

#include <netdb.h>
#include <stddef.h>

#include <list>

#include "net/async_resolver_interface.h"
#include "net/signal_thread.h"
#include "net/sigslot.h"
#include "net/socket_address.h"

namespace net {

class AsyncResolverTest;

// AsyncResolver will perform async DNS resolution, signaling the result on
// the SignalDone from AsyncResolverInterface when the operation completes.
class AsyncResolver : public SignalThread, public AsyncResolverInterface {
 public:
  AsyncResolver();
  ~AsyncResolver() override;

  void Start(const SocketAddress& addr) override;
  bool GetResolvedAddress(int family, SocketAddress* addr) const override;
  int GetError() const override;
  void Destroy(bool wait) override;

  const std::vector<IPAddress>& addresses() const { return addresses_; }
  void set_error(int error) { error_ = error; }

 protected:
  void DoWork() override;
  void OnWorkDone() override;

 private:
  SocketAddress addr_;
  std::vector<IPAddress> addresses_;
  int error_;
};

// rtc namespaced wrappers for inet_ntop and inet_pton so we can avoid
// the windows-native versions of these.
const char* inet_ntop(int af, const void *src, char* dst, socklen_t size);
int inet_pton(int af, const char* src, void *dst);

bool HasIPv6Enabled();
}  // namespace rtc

#endif  // WEBRTC_BASE_NETHELPERS_H_
