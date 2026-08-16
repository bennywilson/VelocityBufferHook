#pragma once
#include <string>

namespace mv
{

// Minimal file logger.
void Log(const std::string& message);

// Used for higher-volume investigative dumps (e.g. render-target binding info) kept
// out of the main log so it stays readable.
void LogTo(const std::string& name, const std::string& message);

void FlushLogs();

} // namespace mv
