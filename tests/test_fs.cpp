#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <fstream>
#include <filesystem>

#include "host.hpp"

namespace fs = std::filesystem;

class FsTest : public ::testing::Test {
protected:
  static inline Host* host = nullptr;
  static inline std::thread host_thread;
  static inline fs::path test_dir;
  static inline int test_passed;
  static inline int test_failed;

  static void SetUpTestSuite() {
    test_dir = fs::temp_directory_path() / "asio_qjs_fs_test";
    fs::create_directories(test_dir);
    test_passed = 0;
    test_failed = 0;

    host_thread = std::thread([]() {
      host = new Host();
      host->install_runtime();

      // Build the test script with the correct test_dir path
      std::string dir = test_dir.string();
      // Replace backslashes with forward slashes for JS
      for (auto& c : dir) { if (c == '\\') c = '/'; }

      std::string script = R"(
        var passed = 0;
        var failed = 0;

        function assert(condition, msg) {
          if (condition) { passed++; }
          else { failed++; console.log('FAIL: ' + msg); }
        }

        // Test 1: writeFileSync + readFileSync
        globalThis.__nativeWriteFileSync(')" + dir + R"(/t1.txt', 'Hello World!');
        var data = globalThis.__nativeReadFileSync(')" + dir + R"(/t1.txt', 'utf8');
        assert(data === 'Hello World!', 'readFileSync utf8');

        // Test 2: existsSync
        assert(globalThis.__nativeExistsSync(')" + dir + R"(/t1.txt'), 'existsSync true');
        assert(!globalThis.__nativeExistsSync('/tmp/nonexistent_xyz'), 'existsSync false');

        // Test 3: binary write/read
        var bin = new Uint8Array([1, 2, 3, 4, 5]);
        globalThis.__nativeWriteFileSync(')" + dir + R"(/t3.bin', bin);
        var readBack = globalThis.__nativeReadFileSync(')" + dir + R"(/t3.bin');
        assert(readBack instanceof Uint8Array, 'binary read returns Uint8Array');
        assert(readBack.length === 5, 'binary length');
        assert(readBack[0] === 1 && readBack[4] === 5, 'binary data');

        // Test 4: mkdirSync + readdirSync
        globalThis.__nativeMkdirSync(')" + dir + R"(/subdir', true);
        globalThis.__nativeWriteFileSync(')" + dir + R"(/subdir/a.txt', 'a');
        globalThis.__nativeWriteFileSync(')" + dir + R"(/subdir/b.txt', 'b');
        var entries = globalThis.__nativeReaddirSync(')" + dir + R"(/subdir');
        assert(entries.length === 2, 'readdirSync count');

        // Test 5: unlinkSync
        globalThis.__nativeUnlinkSync(')" + dir + R"(/t1.txt');
        assert(!globalThis.__nativeExistsSync(')" + dir + R"(/t1.txt'), 'unlinkSync');

        // Test 6: error on nonexistent file
        try {
          globalThis.__nativeReadFileSync('/tmp/nonexistent_file_xyz');
          assert(false, 'should throw on nonexistent');
        } catch(e) {
          assert(true, 'throws on nonexistent');
        }

        // Test 7: async readFile
        globalThis.__nativeReadFile(')" + dir + R"(/subdir/a.txt', 'utf8', function(err, data) {
          assert(!err && data === 'a', 'async readFile');
        });

        // Test 8: async writeFile
        globalThis.__nativeWriteFile(')" + dir + R"(/async.txt', 'async data', function(err) {
          assert(!err, 'async writeFile');
        });

        globalThis.__testResults = { passed: passed, failed: failed };
      )";

      host->eval_source(script.c_str(), "<fs-test>", true);

      // Run loop to process async operations
      host->run_loop();

      // Get results
      auto g = host->global();
      auto results = g.get("__testResults");
      auto passed = results.get("passed");
      auto failed = results.get("failed");
      int p = 0, f = 0;
      passed.to_int32(p);
      failed.to_int32(f);
      globalLogPassed = p;
      globalLogFailed = f;
    });

    host_thread.join();
  }

  static void TearDownTestSuite() {
    if (host) {
      delete host;
      host = nullptr;
    }
    fs::remove_all(test_dir);
  }

  static inline int globalLogPassed = 0;
  static inline int globalLogFailed = 0;
};

TEST_F(FsTest, SyncWriteRead) {
  EXPECT_TRUE(fs::exists(test_dir / "t3.bin"));
  auto data = std::ifstream(test_dir / "t3.bin", std::ios::binary);
  char buf[5];
  data.read(buf, 5);
  EXPECT_EQ(data.gcount(), 5);
  EXPECT_EQ(static_cast<uint8_t>(buf[0]), 1);
  EXPECT_EQ(static_cast<uint8_t>(buf[4]), 5);
}

TEST_F(FsTest, MkdirReaddir) {
  EXPECT_TRUE(fs::exists(test_dir / "subdir"));
  EXPECT_TRUE(fs::exists(test_dir / "subdir" / "a.txt"));
  EXPECT_TRUE(fs::exists(test_dir / "subdir" / "b.txt"));
}

TEST_F(FsTest, UnlinkWorks) {
  EXPECT_FALSE(fs::exists(test_dir / "t1.txt"));
}

TEST_F(FsTest, AsyncWriteWorks) {
  EXPECT_TRUE(fs::exists(test_dir / "async.txt"));
  auto f = std::ifstream(test_dir / "async.txt");
  std::string content((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
  EXPECT_EQ(content, "async data");
}

TEST_F(FsTest, JsAssertionsPassed) {
  EXPECT_EQ(globalLogFailed, 0);
  EXPECT_GT(globalLogPassed, 5);
}
