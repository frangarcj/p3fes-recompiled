#pragma once

#include <string>

namespace p3fes_native_movie
{
enum class Status { Idle, Pending, Playing, Finished, Failed };
bool request(const std::string &path);
Status status();
void cancel();
void reset();
bool renderHostFrame();
void shutdownHost();
}
