#ifndef WINVER
#define WINVER 0x0A00
#endif

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include "color.h"
#include "httplib.h"
#include "json.hpp"
#include <cctype>
#include <chrono>
#include <deque>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <system_error>
#include <string>
#include <vector>

using nlohmann::json;

struct Client {
  httplib::ws::WebSocket *ws;
  std::string username;
  std::string color;
};

std::map<int, Client> clients;
std::mutex clients_mutex;
std::deque<std::string> message_history;
constexpr size_t MAX_HISTORY = 256;
constexpr long long IMAGE_EXPIRE_AFTER_MESSAGES = 30;
constexpr size_t MAX_TEXT_FILE_BYTES = 128 * 1024;
constexpr size_t MAX_PPTX_CONVERT_BYTES = 64 * 1024 * 1024;
long long chat_message_index = 0;

long long current_timestamp_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool is_valid_hex_color(const std::string &value) {
  if (value.size() != 7 || value[0] != '#') {
    return false;
  }

  for (size_t i = 1; i < value.size(); ++i) {
    if (!std::isxdigit(static_cast<unsigned char>(value[i]))) {
      return false;
    }
  }

  return true;
}

std::string base64_encode(const std::string &in) {
  static const char table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((in.size() + 2) / 3) * 4);

  int val = 0;
  int valb = -6;
  for (unsigned char c : in) {
    val = (val << 8) + c;
    valb += 8;
    while (valb >= 0) {
      out.push_back(table[(val >> valb) & 0x3F]);
      valb -= 6;
    }
  }
  if (valb > -6) {
    out.push_back(table[((val << 8) >> (valb + 8)) & 0x3F]);
  }
  while (out.size() % 4) {
    out.push_back('=');
  }
  return out;
}

void broadcast(const std::string &msg) {
  for (auto [id, client] : clients) {
    clients[id].ws->send(msg);
  }
}

void remember_message(const std::string &msg) {
  message_history.push_back(msg);
  if (message_history.size() > MAX_HISTORY) {
    message_history.pop_front();
  }
}

void expire_old_images_in_history() {
  for (auto &entry : message_history) {
    try {
      json evt = json::parse(entry);
      if (evt.value("event", "") != "photo") {
        continue;
      }
      if (!evt.contains("message_index") ||
          !evt["message_index"].is_number_integer()) {
        continue;
      }

      long long idx = evt["message_index"].get<long long>();
      if (chat_message_index - idx < IMAGE_EXPIRE_AFTER_MESSAGES) {
        continue;
      }

      json expired = {
          {"event", "msg"},
          {"id", evt.value("id", "")},
          {"username", evt.value("username", "unknown")},
          {"color", evt.value("color", "white")},
          {"timestamp", evt.value("timestamp", current_timestamp_ms())},
          {"msg", "(expired image)"},
          {"message_index", idx}};
      entry = expired.dump();
    } catch (...) {
    }
  }
}

void remember_chat_event(json evt) {
  chat_message_index++;
  evt["message_index"] = chat_message_index;
  remember_message(evt.dump());
  expire_old_images_in_history();
}

void broadcast_except(const std::string &msg, const int skip_id) {
  for (auto [id, client] : clients) {
    if (id == skip_id)
      continue;
    clients[id].ws->send(msg);
  }
}

std::string sanitize_filename_component(const std::string &name) {
  std::string cleaned;
  cleaned.reserve(name.size());
  for (unsigned char ch : name) {
    if (std::isalnum(ch) || ch == '.' || ch == '-' || ch == '_') {
      cleaned.push_back(static_cast<char>(ch));
    } else {
      cleaned.push_back('_');
    }
  }
  if (cleaned.empty()) {
    return "upload";
  }
  return cleaned;
}

std::string url_encode_component(const std::string &value) {
  std::ostringstream escaped;
  escaped << std::uppercase << std::hex;
  for (unsigned char ch : value) {
    if (std::isalnum(ch) || ch == '.' || ch == '-' || ch == '_' || ch == '~') {
      escaped << static_cast<char>(ch);
    } else {
      escaped << '%' << std::setw(2) << std::setfill('0')
              << static_cast<int>(ch);
    }
  }
  return escaped.str();
}

std::string url_decode_component(const std::string &value) {
  std::string decoded;
  decoded.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2 < value.size()) {
      const std::string hex = value.substr(i + 1, 2);
      char *end = nullptr;
      const long code = std::strtol(hex.c_str(), &end, 16);
      if (end && *end == '\0') {
        decoded.push_back(static_cast<char>(code));
        i += 2;
        continue;
      }
    }
    decoded.push_back(value[i] == '+' ? ' ' : value[i]);
  }
  return decoded;
}

std::string shell_single_quote_escape(const std::string &value) {
  std::string escaped;
  escaped.reserve(value.size() + 8);
  for (char ch : value) {
    if (ch == '\'') {
      escaped += "'\\''";
    } else {
      escaped.push_back(ch);
    }
  }
  return escaped;
}

bool write_binary_file(const std::filesystem::path &path,
                       const std::string &data) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  return out.good();
}

bool read_binary_file(const std::filesystem::path &path, std::string &data) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  data = buffer.str();
  return in.good() || in.eof();
}

bool convert_pptx_to_pdf_local(const std::filesystem::path &pptx_path,
                               const std::filesystem::path &pdf_path,
                               std::string &error) {
#ifdef _WIN32
  std::ostringstream cmd;
  cmd << "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command ";
  cmd << "\"$ErrorActionPreference='Stop';";
  cmd << "$ppt='" << shell_single_quote_escape(pptx_path.string()) << "';";
  cmd << "$pdf='" << shell_single_quote_escape(pdf_path.string()) << "';";
  cmd << "$app=$null;$presentation=$null;";
  cmd << "try{";
  cmd << "$app=New-Object -ComObject PowerPoint.Application;";
  cmd << "$app.Visible=-1;";
  cmd << "$presentation=$app.Presentations.Open($ppt,$false,$false,$false);";
  cmd << "$presentation.SaveAs($pdf,32);";
  cmd << "}finally{";
  cmd << "if($presentation){$presentation.Close()|Out-Null};";
  cmd << "if($app){$app.Quit()|Out-Null};";
  cmd << "}";
  cmd << "\"";

  int rc = std::system(cmd.str().c_str());
  if (rc != 0) {
    error = "PPTX conversion failed. Make sure Microsoft PowerPoint is installed on this Windows machine.";
    return false;
  }

  if (!std::filesystem::exists(pdf_path) ||
      std::filesystem::file_size(pdf_path) == 0) {
    error = "PowerPoint conversion did not produce a PDF file.";
    return false;
  }

  return true;
#else
  std::ostringstream cmd;
  const auto output_dir = pdf_path.parent_path();
  cmd << "soffice ";
  cmd << "--headless --nologo --nolockcheck --nodefault --nofirststartwizard ";
  cmd << "--convert-to pdf --outdir ";
  cmd << "'" << shell_single_quote_escape(output_dir.string()) << "' ";
  cmd << "'" << shell_single_quote_escape(pptx_path.string()) << "'";

  int rc = std::system(cmd.str().c_str());
  if (rc != 0) {
    error = "PPTX conversion failed. Make sure LibreOffice or soffice is installed on the Linux server.";
    return false;
  }

  if (!std::filesystem::exists(pdf_path) ||
      std::filesystem::file_size(pdf_path) == 0) {
    error = "LibreOffice conversion did not produce a PDF file.";
    return false;
  }

  return true;
#endif
}

bool is_pptx_conversion_available() {
#ifdef _WIN32
  const int rc = std::system(
      "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "
      "\"try{$app=New-Object -ComObject PowerPoint.Application;$app.Quit();exit 0}catch{exit 1}\"");
  return rc == 0;
#else
  const int rc = std::system("command -v soffice >/dev/null 2>&1");
  return rc == 0;
#endif
}

int main() {
  httplib::Server svr;

  svr.Post("/convert-pptx", [](const httplib::Request &req,
                               httplib::Response &res) {
    if (req.body.empty()) {
      res.status = 400;
      res.set_content("missing PPTX body", "text/plain");
      return;
    }

    if (req.body.size() > MAX_PPTX_CONVERT_BYTES) {
      res.status = 413;
      res.set_content("PPTX file too large", "text/plain");
      return;
    }

    const auto raw_name = req.get_header_value("X-Filename");
    std::string decoded_name = raw_name.empty() ? "upload.pptx" : url_decode_component(raw_name);
    std::string safe_name = sanitize_filename_component(decoded_name);
    if (safe_name.size() < 5 || safe_name.substr(safe_name.size() - 5) != ".pptx") {
      safe_name += ".pptx";
    }

    std::error_code ec;
    const auto temp_root = std::filesystem::temp_directory_path(ec);
    if (ec) {
      res.status = 500;
      res.set_content("unable to access temp directory", "text/plain");
      return;
    }

    auto unique_dir = temp_root / ("textchat-pptx-" + std::to_string(current_timestamp_ms()));
    std::filesystem::create_directories(unique_dir, ec);
    if (ec) {
      res.status = 500;
      res.set_content("unable to create temp workspace", "text/plain");
      return;
    }

    const auto pptx_path = unique_dir / safe_name;
    auto pdf_name = safe_name.substr(0, safe_name.size() - 5) + ".pdf";
    const auto pdf_path = unique_dir / pdf_name;

    struct Cleanup {
      std::filesystem::path path;
      ~Cleanup() {
        std::error_code cleanup_ec;
        std::filesystem::remove_all(path, cleanup_ec);
      }
    } cleanup{unique_dir};

    if (!write_binary_file(pptx_path, req.body)) {
      res.status = 500;
      res.set_content("unable to save uploaded PPTX", "text/plain");
      return;
    }

    std::string convert_error;
    if (!convert_pptx_to_pdf_local(pptx_path, pdf_path, convert_error)) {
      res.status = 500;
      res.set_content(convert_error, "text/plain");
      return;
    }

    std::string pdf_bytes;
    if (!read_binary_file(pdf_path, pdf_bytes)) {
      res.status = 500;
      res.set_content("unable to read converted PDF", "text/plain");
      return;
    }

    res.set_header("X-Converted-Filename", url_encode_component(pdf_name));
    res.set_content(std::move(pdf_bytes), "application/pdf");
  });

  svr.WebSocket("/ws", [](const httplib::Request &req,
                          httplib::ws::WebSocket &ws) {
    std::string msg;
    int c_id;
    bool expecting_photo_binary = false;
    bool expecting_file_binary = false;
    bool expecting_document_binary = false;
    std::string pending_file_name;
    std::string pending_file_language;
    std::string pending_document_name;
    std::string pending_document_type;
    std::vector<std::string> history_snapshot;
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
      clients[c_id] = (Client{&ws, "", randomColor()});
    }

    while (true) {
      auto read_result = ws.read(msg);
      if (read_result == httplib::ws::Fail) {
        break;
      }

      if (read_result == httplib::ws::Binary) {
        if (expecting_file_binary) {
          expecting_file_binary = false;

          if (msg.empty() || msg.size() > MAX_TEXT_FILE_BYTES) {
            pending_file_name.clear();
            pending_file_language.clear();
            continue;
          }

          if (msg.find('\0') != std::string::npos) {
            pending_file_name.clear();
            pending_file_language.clear();
            continue;
          }

          std::string payload;
          {
            std::lock_guard<std::mutex> l(clients_mutex);
            json jmsg = {{"event", "file"},
                         {"id", std::to_string(c_id)},
                         {"username", clients[c_id].username},
                         {"color", clients[c_id].color},
                         {"timestamp", current_timestamp_ms()},
                         {"filename", pending_file_name},
                         {"language", pending_file_language},
                         {"content", msg}};
            remember_chat_event(jmsg);
            payload = jmsg.dump();
          }
          pending_file_name.clear();
          pending_file_language.clear();
          broadcast(payload);
          continue;
        }

        if (expecting_document_binary) {
          expecting_document_binary = false;

          if (msg.empty()) {
            pending_document_name.clear();
            pending_document_type.clear();
            continue;
          }

          std::string document_base64 = base64_encode(msg);
          std::string payload;
          {
            std::lock_guard<std::mutex> l(clients_mutex);
            json jmsg = {{"event", "document"},
                         {"id", std::to_string(c_id)},
                         {"username", clients[c_id].username},
                         {"color", clients[c_id].color},
                         {"timestamp", current_timestamp_ms()},
                         {"filename", pending_document_name},
                         {"doctype", pending_document_type},
                         {"data", document_base64}};
            remember_chat_event(jmsg);
            payload = jmsg.dump();
          }
          pending_document_name.clear();
          pending_document_type.clear();
          broadcast(payload);
          continue;
        }

        if (!expecting_photo_binary) {
          continue;
        }
        expecting_photo_binary = false;

        if (msg.empty()) {
          continue;
        }

        std::string image_base64 = base64_encode(msg);
        std::string payload;
        {
          std::lock_guard<std::mutex> l(clients_mutex);
          json jmsg = {{"event", "photo"},
                       {"id", std::to_string(c_id)},
                       {"username", clients[c_id].username},
                       {"color", clients[c_id].color},
                       {"timestamp", current_timestamp_ms()},
                       {"mime", "image/jpeg"},
                       {"data", image_base64}};
          remember_chat_event(jmsg);
          payload = jmsg.dump();
        }
        broadcast(payload);
        continue;
      }

      std::cout << msg << std::endl;
      if (!msg.empty() && msg[0] == '&') // command
      {
        switch (msg[1]) {
        case 'u': {
          std::string uname = msg.substr(2);
          std::string requested_color;
          if (!uname.empty() && uname[0] == '{') {
            try {
              json login = json::parse(uname);
              uname = login.value("username", "");
              requested_color = login.value("color", "");
            } catch (...) {
              uname = "";
            }
          }
          std::string color;
          {
            std::lock_guard<std::mutex> l(clients_mutex);
            bool taken = false;
            for (auto &[id, client] : clients) {
              if (client.username == uname)
                taken = true;
            }
            if (taken) {
              json jmsg = {{"event", "uname-eval"}, {"result", "taken"}};
              ws.send(jmsg.dump());
              break;
            } else {
              json jmsg = {{"event", "uname-eval"}, {"result", "ok"}};
              ws.send(jmsg.dump());
              history_snapshot.assign(message_history.begin(),
                                      message_history.end());
              for (const auto &old_msg : history_snapshot) {
                ws.send(old_msg);
              }
            }
            clients[c_id].username = uname;
            if (is_valid_hex_color(requested_color)) {
              clients[c_id].color = requested_color;
            }
            color = clients[c_id].color;
          }
          json jmsg = {{"event", "userjoin"},
                       {"id", std::to_string(c_id)},
                       {"username", uname},
                       {"color", color},
                       {"timestamp", current_timestamp_ms()}};

          broadcast(jmsg.dump());

          remember_message((json){{"event", "msg"},
                                  {"id", std::to_string(c_id)},
                                  {"username", uname},
                                  {"color", clients[c_id].color},
                                  {"timestamp", current_timestamp_ms()},
                                  {"msg", " joined."}}
                               .dump());
          break;
        }
        case 'i': { // get users i̲n chat
          std::lock_guard<std::mutex> l(clients_mutex);
          json uja = json::object();
          for (auto [id, client] : clients) {
            if (id == c_id)
              continue;
            uja[std::to_string(id)] = {{"username", client.username},
                                       {"color", client.color}};
          }
          json jmsg = {{"event", "sendusers"}, {"users", uja}};
          clients[c_id].ws->send(jmsg.dump());
          break;
        }
        case 't': { // typing
          json jmsg = {{"event", "typing"}, {"id", std::to_string(c_id)}};
          broadcast(jmsg.dump());
          break;
        }
        case 's': { // stop typing
          json jmsg = {{"event", "stoptyping"}, {"id", std::to_string(c_id)}};
          broadcast(jmsg.dump());
          break;
        }
        case 'p': { // photo marker; next binary frame contains image bytes
          expecting_file_binary = false;
          expecting_document_binary = false;
          pending_file_name.clear();
          pending_file_language.clear();
          pending_document_name.clear();
          pending_document_type.clear();
          expecting_photo_binary = true;
          break;
        }
        case 'f': { // file marker; next binary frame contains UTF-8 text bytes
          expecting_photo_binary = false;
          expecting_document_binary = false;
          pending_file_name.clear();
          pending_file_language.clear();

          try {
            json file_meta = json::parse(msg.substr(2));
            pending_file_name = file_meta.value("filename", "");
            pending_file_language = file_meta.value("language", "");
            if (!pending_file_name.empty()) {
              expecting_file_binary = true;
            }
          } catch (...) {
            expecting_file_binary = false;
          }
          break;
        }
        case 'd': { // document marker; next binary frame contains document bytes
          expecting_photo_binary = false;
          expecting_file_binary = false;
          pending_document_name.clear();
          pending_document_type.clear();

          try {
            json doc_meta = json::parse(msg.substr(2));
            pending_document_name = doc_meta.value("filename", "");
            pending_document_type = doc_meta.value("doctype", "");
            if (!pending_document_name.empty() && !pending_document_type.empty()) {
              expecting_document_binary = true;
            }
          } catch (...) {
            expecting_document_binary = false;
          }
          break;
        }
        }
      } else {
        std::string payload;
        {
          std::lock_guard<std::mutex> l(clients_mutex);
          json jmsg = {{"event", "msg"},
                       {"id", std::to_string(c_id)},
                       {"username", clients[c_id].username},
                       {"color", clients[c_id].color},
                       {"timestamp", current_timestamp_ms()},
                       {"msg", msg}};
          remember_chat_event(jmsg);
          payload = jmsg.dump();
        }
        broadcast(payload);
      }
    }

    // disconnect
    json jmsg = {{"event", "userleft"}, {"id", std::to_string(c_id)}};
    remember_message((json){{"event", "msg"},
                            {"id", std::to_string(c_id)},
                            {"username", clients[c_id].username},
                            {"color", clients[c_id].color},
                            {"timestamp", current_timestamp_ms()},
                            {"msg", " left."}}
                         .dump());
    {
      std::lock_guard<std::mutex> l(clients_mutex);
      clients.erase(c_id);
    }
    broadcast(jmsg.dump());
  });

  svr.set_mount_point("/", "./cli/");

  if (!is_pptx_conversion_available()) {
#ifdef _WIN32
    std::cout << "[warn] PPTX to PDF conversion is unavailable. Install Microsoft PowerPoint on this Windows machine to enable .pptx uploads."
              << std::endl;
#else
    std::cout << "[warn] PPTX to PDF conversion is unavailable. Install LibreOffice/soffice on PATH to enable .pptx uploads."
              << std::endl;
#endif
  }

  std::cout << "Listening on " << "0.0.0.0:" << 8080 << std::endl;
  svr.listen("0.0.0.0", 8080);
}
