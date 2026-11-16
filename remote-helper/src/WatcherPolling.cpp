/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2026 Kate Code contributors
*/

// Systems without a directory notification backend here (macOS, the BSDs):
// Watcher polls every file.

#include "Watcher.h"

namespace helper
{

std::unique_ptr<DirBackend> createDirBackend(DirBackend::Callback)
{
    return nullptr;
}

}
