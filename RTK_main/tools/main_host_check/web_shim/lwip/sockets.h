// host 替身：lwip/sockets.h 最小桩（仅 web_module_check 使用，避免绑定宿主 53 端口或与 libc close 冲突）。
#pragma once

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

int lwip_socket(int domain, int type, int protocol);
int lwip_setsockopt(int s, int level, int optname, const void *optval, socklen_t optlen);
int lwip_fcntl(int s, int cmd, int val);
int lwip_bind(int s, const struct sockaddr *name, socklen_t namelen);
ssize_t lwip_recvfrom(int s, void *mem, size_t len, int flags, struct sockaddr *from,
                      socklen_t *fromlen);
ssize_t lwip_recvmsg(int s, struct msghdr *message, int flags);
ssize_t lwip_sendto(int s, const void *dataptr, size_t size, int flags, const struct sockaddr *to,
                    socklen_t tolen);
int lwip_close(int s);

#ifdef __cplusplus
}
#endif
