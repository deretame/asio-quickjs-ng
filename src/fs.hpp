#pragma once

#include "host.hpp"

namespace fs_api {

// Install the fs module into the host.
// Registers: fs.readFileSync, fs.writeFileSync, fs.readFile, fs.writeFile,
//            fs.existsSync, fs.statSync, fs.unlinkSync, fs.mkdirSync,
//            fs.rmdirSync, fs.readdirSync, fs.appendFileSync, fs.copyFileSync,
//            fs.renameSync, fs.rmSync, fs.chmodSync, fs.realpathSync,
//            fs.mkdtempSync
void install(Host& host);

// Install extended APIs: lstatSync, symlinkSync, readlinkSync, accessSync,
//                        truncateSync, utimesSync, createReadStream,
//                        createWriteStream, watch, openSync, closeSync,
//                        readSync, writeSync, fsyncSync, linkSync
void install_extended(Host& host);

}  // namespace fs_api
