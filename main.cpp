#include "httplib.h"
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <string>

struct Client {
  httplib::ws::WebSocket *ws;
  std::string username;
};

std::map<int, Client> clients;
std::mutex clients_mutex;

void broadcast(const std::string &msg) {
  std::lock_guard<std::mutex> l(clients_mutex);
  for (auto [id, client] : clients) {
    clients[id].ws->send(msg);
  }
}

int main() {
  httplib::Server svr;

  svr.WebSocket("/ws",
                [](const httplib::Request &req, httplib::ws::WebSocket &ws) {
                  std::string msg;
                  int c_id;
                  {
                    std::lock_guard<std::mutex> l(clients_mutex);

                    bool taken = true;
                    while (taken) {
                      c_id = rand();
                      taken = false;
                      for (auto [id, client] : clients) {
                        if (id == c_id) {
                          taken = true;
                        }
                      }
                    }
                    clients[c_id] = (Client{&ws, ""});
                  }
                  while (ws.read(msg)) {
                    std::cout << msg << std::endl;
                    if (msg[0] == '&') // command
                    {
                      switch (msg[1]) {
                      case 'u': {
                        std::lock_guard<std::mutex> l(clients_mutex);
                        std::string uname = msg.substr(2);
                        clients[c_id].username = uname;
                        msg = "<" + uname + "> joined.";
                        break;
                      }
                      }
                    }
                    broadcast(msg);
                  }

                  // disconnect
                  msg = "<" + clients[c_id].username + "> left.";
                  {
                    std::lock_guard<std::mutex> l(clients_mutex);
                    clients.erase(c_id);
                  }
                  broadcast(msg);
                });

  svr.set_mount_point("/", "./");

  svr.listen("0.0.0.0", 8080);
}
