// Command-line diagnostics: monitor listing, encoder probing, capture self-test.
#pragma once

#include <string>

#include "session/session.h"

namespace dm {

int list_monitors();
int probe_encoders();
int selftest(int seconds, const std::string& out_path, const HostOptions& opts);

}  // namespace dm
