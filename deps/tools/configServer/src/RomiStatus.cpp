// Copyright (c) FIRST and other WPILib contributors.
// Open Source Software; you can modify and/or share it under the terms of
// the WPILib BSD license file in the root directory of this project.

#include "RomiStatus.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <string_view>

#include <fmt/format.h>
#include <wpi/SmallString.h>
#include <wpi/fs.h>
#include <wpi/json.h>
#include <wpi/raw_istream.h>
#include <wpi/raw_ostream.h>
#include <wpi/uv/Buffer.h>
#include <wpi/uv/FsEvent.h>
#include <wpi/uv/Pipe.h>
#include <wpi/uv/Process.h>
#include <wpi/uv/Timer.h>
#include <wpi/uv/Work.h>

namespace uv = wpi::uv;

#define SERVICE "/service/wpilibws-romi"

std::shared_ptr<RomiStatus> RomiStatus::GetInstance() {
  static auto status = std::make_shared<RomiStatus>(private_init{});
  return status;
}

void RomiStatus::SetLoop(std::shared_ptr<wpi::uv::Loop> loop) {
  m_loop = std::move(loop);
}

void RomiStatus::RunSvc(const char* cmd,
                        std::function<void(std::string_view)> onFail) {
  struct SvcWorkReq : public uv::WorkReq {
    SvcWorkReq(const char* cmd_, std::function<void(std::string_view)> onFail_)
        : cmd(cmd_), onFail(onFail_) {}
    const char* cmd;
    std::function<void(std::string_view)> onFail;
    wpi::SmallString<128> err;
  };

  auto workReq = std::make_shared<SvcWorkReq>(cmd, onFail);
  workReq->work.connect([r = workReq.get()] {
    int fd = open(SERVICE "/supervise/control", O_WRONLY | O_NDELAY);
    if (fd == -1) {
      wpi::raw_svector_ostream os(r->err);
      if (errno == ENXIO)
        os << "unable to control service: supervise not running";
      else
        os << "unable to control service: " << std::strerror(errno);
    } else {
      fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
      if (write(fd, r->cmd, std::strlen(r->cmd)) == -1) {
        wpi::raw_svector_ostream os(r->err);
        os << "error writing command: " << std::strerror(errno);
      }
      close(fd);
    }
  });
  workReq->afterWork.connect([r = workReq.get()] {
    if (r->onFail && !r->err.empty()) r->onFail(r->err.str());
  });

  uv::QueueWork(m_loop, workReq);
}

void RomiStatus::Up(std::function<void(std::string_view)> onFail) {
  RunSvc("u", onFail);
  UpdateStatus();
}

void RomiStatus::Down(std::function<void(std::string_view)> onFail) {
  RunSvc("d", onFail);
  UpdateStatus();
}

void RomiStatus::Terminate(std::function<void(std::string_view)> onFail) {
  RunSvc("t", onFail);
  UpdateStatus();
}

void RomiStatus::Kill(std::function<void(std::string_view)> onFail) {
  RunSvc("k", onFail);
  UpdateStatus();
}

void RomiStatus::UpdateStatus() {
  struct StatusWorkReq : public uv::WorkReq {
    bool enabled = false;
    wpi::SmallString<128> status;
  };

  auto workReq = std::make_shared<StatusWorkReq>();

  workReq->work.connect([r = workReq.get()] {
    wpi::raw_svector_ostream os(r->status);

    // check to make sure supervise is running
    int fd = open(SERVICE "/supervise/ok", O_WRONLY | O_NDELAY);
    if (fd == -1) {
      if (errno == ENXIO)
        os << "supervise not running";
      else
        os << "unable to open supervise/ok: " << std::strerror(errno);
      return;
    }
    close(fd);

    // read the status data
    fd = open(SERVICE "/supervise/status", O_RDONLY | O_NDELAY);
    if (fd == -1) {
      os << "unable to open supervise/status: " << std::strerror(errno);
      return;
    }
    uint8_t status[18];
    int nr = read(fd, status, sizeof status);
    close(fd);
    if (nr < static_cast<int>(sizeof status)) {
      os << "unable to read supervise/status: ";
      if (nr == -1)
        os << std::strerror(errno);
      else
        os << "bad format";
      return;
    }

    // decode the status data (based on daemontools svstat.c)
    uint32_t pid = (static_cast<uint32_t>(status[15]) << 24) |
                   (static_cast<uint32_t>(status[14]) << 16) |
                   (static_cast<uint32_t>(status[13]) << 8) |
                   (static_cast<uint32_t>(status[12]));
    bool paused = status[16];
    auto want = status[17];
    uint64_t when = (static_cast<uint64_t>(status[0]) << 56) |
                    (static_cast<uint64_t>(status[1]) << 48) |
                    (static_cast<uint64_t>(status[2]) << 40) |
                    (static_cast<uint64_t>(status[3]) << 32) |
                    (static_cast<uint64_t>(status[4]) << 24) |
                    (static_cast<uint64_t>(status[5]) << 16) |
                    (static_cast<uint64_t>(status[6]) << 8) |
                    (static_cast<uint64_t>(status[7]));

    // constant is from daemontools tai.h
    uint64_t now =
        4611686018427387914ULL + static_cast<uint64_t>(std::time(nullptr));
    if (now >= when)
      when = now - when;
    else
      when = 0;

    // convert to status string
    if (pid)
      os << fmt::format("up (pid {}) ", pid);
    else
      os << "down ";
    os << fmt::format("{} seconds", when);
    if (pid && paused) os << ", paused";
    if (!pid && want == 'u') os << ", want up";
    if (pid && want == 'd') os << ", want down";

    if (pid) r->enabled = true;
  });

  workReq->afterWork.connect([this, r = workReq.get()] {
    wpi::json j = {{"type", "romiStatus"},
                   {"romiServiceEnabled", r->enabled},
                   {"romiServiceStatus", r->status.str()}};
    update(j);
  });

  uv::QueueWork(m_loop, workReq);

  {
    wpi::json j = {{"type", "romiFirmwareInterface"},
                   {"exists", access("/dev/ttyACM0", 0) == 0}};
    update(j);
  }
}

void RomiStatus::ConsoleLog(uv::Buffer& buf, size_t len) {
  wpi::json j = {{"type", "romiLog"},
                 {"data", std::string_view(buf.base, len)}};
  log(j);
}

void RomiStatus::FirmwareUpdate(std::function<void(std::string_view)> onFail) {
  // create pipe to capture stdout
  auto pipe = uv::Pipe::Create(m_loop);
  if (auto proc = uv::Process::Spawn(
          m_loop, "/usr/bin/python",
          pipe ? uv::Process::StdioCreatePipe(1, *pipe, UV_WRITABLE_PIPE)
               : uv::Process::Option(),
          "/usr/bin/python", "/usr/src/wpilib-ws-romi/scripts/uploadRomi.py")) {
    // send stdout output to firmware log
    if (pipe) {
      pipe->StartRead();
      pipe->data.connect([=](uv::Buffer& buf, size_t len) {
        wpi::json j = {{"type", "romiFirmwareLog"},
                       {"data", std::string_view(buf.base, len)}};
        update(j);
      });
      pipe->end.connect([p = pipe.get()] { p->Close(); });
    }

    // on exit, report
    proc->exited.connect(
        [this, p = proc.get(), onFail](int64_t status, int sig) {
          wpi::json j = {{"type", "romiFirmwareComplete"}};
          update(j);
          if (status != EXIT_SUCCESS) {
            onFail("firmware update failed");
          }
          p->Close();
        });
  } else {
    onFail("could not spawn process");
  }
}

void RomiStatus::UpdateConfig(std::function<void(std::string_view)> onFail) {
  config(GetConfigJson(onFail));
}

wpi::json RomiStatus::ReadRomiConfigFile(std::function<void(std::string_view)> onFail) {
  // Read config file
  std::error_code ec;
  wpi::raw_fd_istream is(ROMI_JSON, ec);

  if (ec) {
    onFail("Could not read romi config file");
    fmt::print(stderr, "could not read {}\n", ROMI_JSON);
    wpi::json();
  }

  wpi::json j;
  try {
    j = wpi::json::parse(is);
  } catch(const wpi::json::parse_error& e) {
    onFail("Parse error in config file");
    fmt::print(stderr, "Parse error in {}: byte {}: {}\n", ROMI_JSON,
               e.byte, e.what());
    return wpi::json();
  }

  if (!j.is_object()) {
    onFail("Top level must be a JSON object");
    fmt::print(stderr, "{}", "must be a JSON object\n");
    return wpi::json();
  }

  return j;
}

wpi::json RomiStatus::GetConfigJson(std::function<void(std::string_view)> onFail) {
  wpi::json j = ReadRomiConfigFile(onFail);
  return {{"type", "romiConfig"},
          {"romiConfig", j}};
}

void RomiStatus::SaveConfig(const wpi::json& data, bool restartService, std::function<void(std::string_view)> onFail) {
  // We should first read in the file
  wpi::json configFileJson = ReadRomiConfigFile(onFail);

  // Verify that it is an object
  if (!data.is_object()) {
    onFail("invalid romi config format. Should be an object");
    return;
  }

  // Iterate through all the keys in `data`
  // Add them to configFileJson
  for (auto it = data.begin(); it != data.end(); ++it) {
    configFileJson[it.key()] = it.value();
  }

  {
    // write file
    std::error_code ec;
    wpi::raw_fd_ostream os(ROMI_JSON, ec, fs::F_Text);
    if (ec) {
      onFail("could not write to romi config");
      return;
    }
    configFileJson.dump(os, 4);
    os << "\n";
  }

  if (restartService) {
    // Terminate Romi process so it reloads the file
    Terminate(onFail);
  }
  UpdateConfig(onFail);
}
