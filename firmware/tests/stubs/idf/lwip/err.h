#pragma once
// lwIP отображён на настоящие сокеты macOS: captive_dns.cpp пользуется обычным BSD-API, и
// подменять его незачем — привязка к адресу точки доступа (а не к INADDR_ANY) проверяется здесь
// по-настоящему. Это тот самый пункт, который закрывали 2026-09-18.
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
}
#endif
