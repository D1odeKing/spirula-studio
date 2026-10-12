#pragma once

// A reconstruction watched from a browser: the --progress-dir snapshots
// (app/SfmProgress.h), the workspace's reports and the run's log lines, served
// over HttpServer behind a token. Read-only: nothing here can steer the run.
// Routes, formats and the SSH recipe: docs/notes/remote-monitoring.md.

#include <memory>
#include <string>

namespace app {

struct SfmMonitorOptions {
    std::string host = "127.0.0.1";
    int port = 7008;
    std::string token;          // empty: one is made
    std::string progress_dir;   // what the run snapshots into
    std::string workspace;      // progressive.txt, image_stats.bin, matches.bin
    // The run's error gates, which the fit colours are measured against.
    bool progressive = false;
    float error_start = 20.0f, error_end = 0.0f;
    bool log = false;           // the caller feeds log(); the page says when not
};

class SfmMonitor {
public:
    SfmMonitor();
    ~SfmMonitor();

    // Throws when the port cannot be bound. Returns the token in effect.
    std::string start(SfmMonitorOptions opt);
    void stop();

    // One line of the run's log as the terminal shows it; any thread.
    void log(const char* level, std::string text);

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}  // namespace app
