#pragma once

#include "host.hpp"

namespace fs_api {

// Install the fs module into the host.
// Registers: fs.readFileSync, fs.writeFileSync, fs.readFile, fs.writeFile,
//            fs.existsSync, fs.statSync, fs.unlinkSync, fs.mkdirSync,
//            fs.rmdirSync, fs.readdirSync, fs.promises (Promise-based API)
void install(Host& host);

}  // namespace fs_api
