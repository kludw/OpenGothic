#include <Tempest/Window>
#include <Tempest/Application>
#include <Tempest/Log>

#include <zenkit/Logger.hh>

#include <Tempest/VulkanApi>

#if defined(_MSC_VER)
#include <Tempest/DirectX12Api>
#endif

#if defined(__APPLE__)
#include <Tempest/MetalApi>
#endif

#if defined(__IOS__) || defined(__ANDROID__)
#include "utils/installdetect.h"
#include <filesystem>
#endif

#include "utils/crashlog.h"
#include "mainwindow.h"
#include "gothic.h"
#include "build.h"
#include "commandline.h"
#include "og_main.h"
#include "physics/phystrace.h"
#include "capi/og_capi.h"
#include "capi/og_capi_internal.h"

#include <memory>
#include <optional>

#include <dmusic.h>

static std::string_view selectDevice(const Tempest::AbstractGraphicsApi& api) {
  auto d = api.devices();

  static Tempest::Device::Props p;
  for(auto& i:d)
    // if(i.type==Tempest::DeviceType::Integrated) {
    if(i.type==Tempest::DeviceType::Discrete) {
      p = i;
      return p.name;
      }
  if(d.size()>0) {
    p = d[0];
    return p.name;
    }
  return "";
  }

static std::unique_ptr<Tempest::AbstractGraphicsApi> mkApi(const CommandLine& g) {
  Tempest::ApiFlags flg = g.isValidationMode() ? Tempest::ApiFlags::Validation : Tempest::ApiFlags::NoFlags;
  switch(g.graphicsApi()) {
    case CommandLine::DirectX12:
#if defined(_MSC_VER)
      return std::make_unique<Tempest::DirectX12Api>(flg);
#else
      break;
#endif
    case CommandLine::Vulkan:
#if !defined(__APPLE__)
      return std::make_unique<Tempest::VulkanApi>(flg);
#else
      break;
#endif
    }

#if defined(__APPLE__)
  return std::make_unique<Tempest::MetalApi>(flg);
#else
  return std::make_unique<Tempest::VulkanApi>(flg);
#endif
  }

// opengothic-rs: logger levels for the host sink (keeps warnings as warnings).
static int zenkitLevel(zenkit::LogLevel lvl) {
  switch(lvl) {
    case zenkit::LogLevel::ERROR:   return OG_LOG_ERROR;
    case zenkit::LogLevel::WARNING: return OG_LOG_WARN;
    case zenkit::LogLevel::INFO:    return OG_LOG_INFO;
    case zenkit::LogLevel::DEBUG:   return OG_LOG_DEBUG;
    case zenkit::LogLevel::TRACE:   return OG_LOG_TRACE;
    }
  return OG_LOG_ERROR;
  }

static int dmusicLevel(DmLogLevel lvl) {
  switch(lvl) {
    case DmLogLevel_FATAL:
    case DmLogLevel_ERROR: return OG_LOG_ERROR;
    case DmLogLevel_WARN:  return OG_LOG_WARN;
    case DmLogLevel_INFO:  return OG_LOG_INFO;
    case DmLogLevel_DEBUG: return OG_LOG_DEBUG;
    case DmLogLevel_TRACE: return OG_LOG_TRACE;
    }
  return OG_LOG_ERROR;
  }

// opengothic-rs: process-wide setup of og_main (cwd on mobile, loggers, crash handler).
static void setupProcess() {
#if defined(__ANDROID__)
  {
    auto appdir = InstallDetect::androidInternalDataPath();
    std::filesystem::current_path(appdir);
  }
#elif defined(__IOS__)
  {
    auto appdir = InstallDetect::applicationSupportDirectory();
    std::filesystem::current_path(appdir);
  }
#endif

  // opengothic-rs: a host log sink (og_run with on_log) replaces log.txt.
  if(OgCapi::hasLogSink()) {
    Tempest::Log::setOutputCallback([](Tempest::Log::Mode mode, const char* text) {
      const int level = mode==Tempest::Log::Error ? OG_LOG_ERROR :
                        mode==Tempest::Log::Debug ? OG_LOG_DEBUG : OG_LOG_INFO;
      // Once og_run has returned there is no sink; Tempest already echoed the line
      // to stdout/stderr, and the process is about to exit (one run per process).
      OgCapi::log(level,"tempest",text);
      });
    } else {
    try {
      static Tempest::WFile logFile("log.txt");
      Tempest::Log::setOutputCallback([](Tempest::Log::Mode mode, const char* text) {
        logFile.write(text,std::strlen(text));
        logFile.write("\n",1);
        if(mode==Tempest::Log::Error)
          logFile.flush();
        });
      }
    catch(...) {
      Tempest::Log::e("unable to setup logfile - fallback to console log");
      }
    }
  CrashLog::setup();

  zenkit::Logger::set(zenkit::LogLevel::INFO, [] (zenkit::LogLevel lvl, const char* cat, const char* message) {
    (void)cat;
    if(OgCapi::log(zenkitLevel(lvl),"zenkit",message))
      return;
    switch (lvl) {
      case zenkit::LogLevel::ERROR:
        Tempest::Log::e("[zenkit] ", message);
        break;
      case zenkit::LogLevel::WARNING:
        Tempest::Log::e("[zenkit] ", message);
        break;
      case zenkit::LogLevel::INFO:
        Tempest::Log::i("[zenkit] ", message);
        break;
      case zenkit::LogLevel::DEBUG:
      case zenkit::LogLevel::TRACE:
        Tempest::Log::d("[zenkit] ", message); // unused
        break;
      }
    });
  Dm_setLogger(DmLogLevel_INFO, [](void* ctx, DmLogLevel lvl, char const* msg) {
    if(OgCapi::log(dmusicLevel(lvl),"dmusic",msg))
      return;
    switch (lvl) {
      case DmLogLevel_FATAL:
      case DmLogLevel_ERROR:
      case DmLogLevel_WARN:
        Tempest::Log::e("[dmusic] ", msg);
        break;
      case DmLogLevel_INFO:
        Tempest::Log::i("[dmusic] ", msg);
        break;
      case DmLogLevel_DEBUG:
      case DmLogLevel_TRACE:
        Tempest::Log::d("[dmusic] ", msg);
        break;
      }
    }, nullptr);

  Tempest::Log::i(appBuild);
  Workers::setThreadName("Main thread");
  }

// opengothic-rs: everything og_main used to keep on its stack, in the same order, so
// members are destroyed in the order the stack unwound (task 5.2: a host that owns the
// loop keeps it between og_init and og_shutdown).
struct OgMain::Engine {
  CommandLine                                  cmd;
  std::unique_ptr<Tempest::AbstractGraphicsApi> api;
  Tempest::Device                              device;
  Resources                                    resources;
  Gothic                                       gothic;
  GameMusic                                    music;
  std::optional<MainWindow>                    wx;
  std::optional<Tempest::Application>          app;

  Engine(int argc, const char** argv)
    :cmd{argc,argv}, api(mkApi(cmd)), device{*api,selectGpu(*api)},
     // As og_main did: the device's own name goes to the crash log right after the device.
     resources{(CrashLog::setGpu(device.properties().name), device)} {
    // opengothic-rs (task 6.2): before any world exists, so the trace sees all of them.
    if(!cmd.physicsTrace().empty())
      PhysTrace::open(std::string(cmd.physicsTrace()));
    try {
      gothic.setupGlobalScripts();
      wx.emplace(device);
      app.emplace();
      }
    catch(...) {
      // No shutdown follows a failed init: close (and report) the trace here.
      PhysTrace::close();
      throw;
      }
    }

  static std::string_view selectGpu(const Tempest::AbstractGraphicsApi& api) {
    const auto gpuName = selectDevice(api);
    CrashLog::setGpu(gpuName);
    return gpuName;
    }
  };

OgMain::Engine* OgMain::init(int argc, const char** argv) {
  setupProcess();
  return new Engine(argc,argv);
  }

MainWindow& OgMain::window(Engine& e) {
  return *e.wx;
  }

void OgMain::shutdown(Engine* e) {
  // After the engine (also if destroying it throws): worlds destroyed with it still write
  // their records.
  struct TraceCloser { ~TraceCloser(){ PhysTrace::close(); } } closer;
  delete e;
  }

int og_main(int argc,const char** argv) {
  // Closes the physics trace after the engine, also when exec() throws (declared first,
  // destroyed last).
  struct TraceCloser { ~TraceCloser(){ PhysTrace::close(); } } closer;
  std::unique_ptr<OgMain::Engine> e{OgMain::init(argc,argv)};
  return e->app->exec();
  }
