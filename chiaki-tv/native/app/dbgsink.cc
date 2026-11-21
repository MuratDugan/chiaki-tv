#include "dbgsink.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <deque>

#include "util.h"

namespace dbgsink {

namespace {

pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
std::deque<std::string> g_queue;
struct sockaddr_in g_addr;
volatile bool g_enabled = false;
bool g_thread_started = false;

volatile const char* g_step = "baslangic";
volatile int64_t g_beat_ms = 0;
void (*g_report)(const std::string&, void*) = NULL;
void* g_report_user = NULL;
int g_stall_ms = 2500;

void* SenderMain(void*) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  for (;;) {
    std::string line;
    struct sockaddr_in addr;
    {
      util::Lock l(&g_mu);
      while (g_queue.empty()) pthread_cond_wait(&g_cv, &g_mu);
      line.swap(g_queue.front());
      g_queue.pop_front();
      addr = g_addr;
    }
    if (sock >= 0 && g_enabled)
      sendto(sock, line.data(), line.size() > 1200 ? 1200 : line.size(), 0, reinterpret_cast<struct sockaddr*>(&addr),
             sizeof(addr));
  }
  return NULL;
}

void* WatchdogMain(void*) {
  bool reported = false;
  for (;;) {
    usleep(500 * 1000);
    int64_t beat = g_beat_ms;
    if (!beat) continue;
    int64_t idle = util::NowMs() - beat;
    if (idle > g_stall_ms) {
      if (!reported) {
        reported = true;
        char line[200];
        snprintf(line, sizeof(line), "! ANA IS PARCACIGI TAKILDI: %lld ms'dir isaret yok, son adim: %s",
                 static_cast<long long>(idle), const_cast<const char*>(g_step));
        if (g_report) g_report(line, g_report_user);
      }
    } else if (reported) {
      reported = false;
      if (g_report) g_report("> ana is parcacigi yeniden calisiyor", g_report_user);
    }
  }
  return NULL;
}

void StartDetached(void* (*fn)(void*)) {
  pthread_t thread;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_create(&thread, &attr, fn, NULL);
  pthread_attr_destroy(&attr);
}

}  // namespace

void SetTarget(const std::string& ip_port) {
  size_t colon = ip_port.rfind(':');
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  bool ok = colon != std::string::npos && inet_pton(AF_INET, ip_port.substr(0, colon).c_str(), &addr.sin_addr) == 1;
  if (ok) {
    // Yalnizca yerel ag adresleri (10/8, 172.16/12, 192.168/16, 169.254/16, 127/8): gunluk internete gonderilemez.
    uint32_t ip = ntohl(addr.sin_addr.s_addr);
    ok = (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 || (ip >> 16) == 0xA9FE || (ip >> 24) == 127;
  }
  if (ok) addr.sin_port = htons(static_cast<uint16_t>(atoi(ip_port.c_str() + colon + 1)));
  util::Lock l(&g_mu);
  g_addr = addr;
  g_enabled = ok && addr.sin_port != 0;
  if (g_enabled && !g_thread_started) {
    g_thread_started = true;
    StartDetached(SenderMain);
  }
}

void Send(const std::string& line) {
  if (!g_enabled) return;
  util::Lock l(&g_mu);
  if (g_queue.size() > 2000) return;  // dinleyici yoksa ya da ag tikaliysa bellek sismesin
  g_queue.push_back(line);
  pthread_cond_signal(&g_cv);
}

void MainStep(const char* step) {
  g_step = step;
  g_beat_ms = util::NowMs();
}

void StartWatchdog(void (*report)(const std::string&, void*), void* user, int stall_ms) {
  g_report = report;
  g_report_user = user;
  g_stall_ms = stall_ms;
  StartDetached(WatchdogMain);
}

}  // namespace dbgsink
