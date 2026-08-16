#include "logging.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

namespace mv
{
namespace
{

std::string TempDir()
{
    char tempPath[MAX_PATH]{};
    GetTempPathA(MAX_PATH, tempPath);
    return std::string(tempPath);
}

std::mutex& LogMutex()
{
    static std::mutex m;
    return m;
}

std::string Timestamp()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const double seconds = std::chrono::duration<double>(now).count();
    char buf[32]{};
    snprintf(buf, sizeof(buf), "%.3f", seconds);
    return buf;
}

// Writes are flushed every kFlushEvery lines.
constexpr int kFlushEvery = 500;

// std::ofstream, not FILE_APPEND_DATA: single-injection is enforced at the
// launcher (refuses >1 matching process, refuses an already-loaded target),
// so only one process ever has a given log file open.
struct NamedFile
{
    std::ofstream stream;
    int unflushed = 0;
};

std::unordered_map<std::string, NamedFile>& NamedLogFiles()
{
    static std::unordered_map<std::string, NamedFile> files;
    return files;
}

// Caller must hold LogMutex().
NamedFile* FileFor(const std::string& name)
{
    auto& files = NamedLogFiles();
    auto it = files.find(name);
    if (it == files.end())
    {
        NamedFile nf;
        nf.stream.open(TempDir() + "mv_" + name + ".log", std::ios::app);
        it = files.emplace(name, std::move(nf)).first;
    }
    return it->second.stream.is_open() ? &it->second : nullptr;
}

} // namespace

// Low-volume log, flushed per-line
void Log(const std::string& message)
{
    std::lock_guard<std::mutex> lock(LogMutex());
    NamedFile* f = FileFor("hook");
    if (!f)
    {
        return;
    }
    f->stream << "[" << Timestamp() << "] " << message << "\n";
    f->stream.flush();
}

// High-volume log, flushed every kFlushEvery lines
void LogTo(const std::string& name, const std::string& message)
{
    std::lock_guard<std::mutex> lock(LogMutex());
    NamedFile* f = FileFor(name);
    if (!f)
    {
        return;
    }
    f->stream << "[" << Timestamp() << "] " << message << "\n";
    if (++f->unflushed >= kFlushEvery)
    {
        f->stream.flush();
        f->unflushed = 0;
    }
}

void FlushLogs()
{
    std::lock_guard<std::mutex> lock(LogMutex());
    for (auto& [name, file] : NamedLogFiles())
    {
        file.stream.flush();
    }
}

} // namespace mv
