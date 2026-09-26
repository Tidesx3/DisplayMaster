// This PC's long-term X25519 key for encrypted Wi-Fi connections (see dm/noise.h).
// Devices pin its public half when they pair; it must survive restarts and updates.
#pragma once

#include <filesystem>

#include "dm/noise.h"

namespace dm {

// Loads the key from `path` (DPAPI-protected for the current user), creating it on first use.
noise::KeyPair load_or_create_identity(const std::filesystem::path& path);

std::filesystem::path default_identity_path();

}  // namespace dm
