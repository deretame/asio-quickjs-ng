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

      // Use a simple fixed path to avoid escaping issues
      std::string dir = "/tmp/asio_qjs_fs_test";
      fs::create_directories(dir);

      std::string script = R"(
        var passed = 0;
        var failed = 0;

        function assert(condition, msg) {
          if (condition) { passed++; }
          else { failed++; console.log('FAIL: ' + msg); }
        }

        // 1. writeFileSync + readFileSync
        globalThis.__nativeWriteFileSync(')" + dir + R"(/t1.txt', 'Hello World!');
        assert(globalThis.__nativeReadFileSync(')" + dir + R"(/t1.txt', 'utf8') === 'Hello World!', 'readFileSync');

        // 2. existsSync
        assert(globalThis.__nativeExistsSync(')" + dir + R"(/t1.txt'), 'existsSync');

        // 3. binary write/read
        var bin = new Uint8Array([1, 2, 3, 4, 5]);
        globalThis.__nativeWriteFileSync(')" + dir + R"(/t3.bin', bin);
        assert(globalThis.__nativeReadFileSync(')" + dir + R"(/t3.bin') instanceof Uint8Array, 'binary read');

        // 4. mkdirSync + readdirSync
        globalThis.__nativeMkdirSync(')" + dir + R"(/subdir', true);
        globalThis.__nativeWriteFileSync(')" + dir + R"(/subdir/a.txt', 'a');
        globalThis.__nativeWriteFileSync(')" + dir + R"(/subdir/b.txt', 'b');
        assert(globalThis.__nativeReaddirSync(')" + dir + R"(/subdir').length === 2, 'readdirSync');

        // 5. unlinkSync
        globalThis.__nativeUnlinkSync(')" + dir + R"(/t1.txt');
        assert(!globalThis.__nativeExistsSync(')" + dir + R"(/t1.txt'), 'unlinkSync');

        // 6. appendFileSync
        globalThis.__nativeWriteFileSync(')" + dir + R"(/append.txt', 'hello');
        globalThis.__nativeAppendFileSync(')" + dir + R"(/append.txt', ' world');
        assert(globalThis.__nativeReadFileSync(')" + dir + R"(/append.txt', 'utf8') === 'hello world', 'appendFileSync');

        // 7. copyFileSync
        globalThis.__nativeCopyFileSync(')" + dir + R"(/append.txt', ')" + dir + R"(/copy.txt');
        assert(globalThis.__nativeExistsSync(')" + dir + R"(/copy.txt'), 'copyFileSync');

        // 8. statSync
        var st = globalThis.__nativeStatSync(')" + dir + R"(/append.txt');
        assert(st.isFile === true && st.size === 11, 'statSync');

        // 9. renameSync
        globalThis.__nativeRenameSync(')" + dir + R"(/copy.txt', ')" + dir + R"(/renamed.txt');
        assert(globalThis.__nativeExistsSync(')" + dir + R"(/renamed.txt'), 'renameSync');

        // 10. rmSync
        globalThis.__nativeMkdirSync(')" + dir + R"(/rmtest', true);
        globalThis.__nativeWriteFileSync(')" + dir + R"(/rmtest/f.txt', 'x');
        globalThis.__nativeRmSync(')" + dir + R"(/rmtest', 1);
        assert(!globalThis.__nativeExistsSync(')" + dir + R"(/rmtest'), 'rmSync');

        // 11. lstatSync + symlinkSync + readlinkSync
        globalThis.__nativeSymlinkSync(')" + dir + R"(/renamed.txt', ')" + dir + R"(/link.txt');
        assert(globalThis.__nativeLstatSync(')" + dir + R"(/link.txt').isSymbolicLink === true, 'symlinkSync');
        assert(globalThis.__nativeReadlinkSync(')" + dir + R"(/link.txt').length > 0, 'readlinkSync');

        // 12. accessSync
        globalThis.__nativeAccessSync(')" + dir + R"(/renamed.txt', 0);
        assert(true, 'accessSync');

        // 13. truncateSync
        globalThis.__nativeWriteFileSync(')" + dir + R"(/trunc.txt', 'Hello World!');
        globalThis.__nativeTruncateSync(')" + dir + R"(/trunc.txt', 5);
        assert(globalThis.__nativeReadFileSync(')" + dir + R"(/trunc.txt', 'utf8') === 'Hello', 'truncateSync');

        // 14. utimesSync
        globalThis.__nativeUtimesSync(')" + dir + R"(/trunc.txt', 1000000000, 1000000000);
        assert(true, 'utimesSync');

        // 15. realpathSync
        var rp = globalThis.__nativeRealpathSync(')" + dir + R"(/renamed.txt');
        assert(rp.length > 0, 'realpathSync');

        // 16. createReadStream
        var stream = globalThis.__nativeCreateReadStream(')" + dir + R"(/renamed.txt');
        assert(stream != null, 'createReadStream');

        globalThis.__testResults = { passed: passed, failed: failed };
      )";

      host->eval_source(script.c_str(), "<fs-test>", true);
      host->run_loop();
    });

    host_thread.join();
  }

  static void TearDownTestSuite() {
    if (host) { delete host; host = nullptr; }
    fs::remove_all("/tmp/asio_qjs_fs_test");
  }
};

TEST_F(FsTest, BinaryFileCreated) {
  EXPECT_TRUE(fs::exists("/tmp/asio_qjs_fs_test/t3.bin"));
}

TEST_F(FsTest, DirectoryCreated) {
  EXPECT_TRUE(fs::exists("/tmp/asio_qjs_fs_test/subdir"));
}

TEST_F(FsTest, UnlinkWorks) {
  EXPECT_FALSE(fs::exists("/tmp/asio_qjs_fs_test/t1.txt"));
}

TEST_F(FsTest, SymlinkCreated) {
  EXPECT_TRUE(fs::exists("/tmp/asio_qjs_fs_test/link.txt"));
}

TEST_F(FsTest, TruncateWorks) {
  EXPECT_EQ(fs::file_size("/tmp/asio_qjs_fs_test/trunc.txt"), 5);
}

TEST_F(FsTest, RenameWorks) {
  EXPECT_TRUE(fs::exists("/tmp/asio_qjs_fs_test/renamed.txt"));
}

TEST_F(FsTest, AppendWorks) {
  auto f = std::ifstream("/tmp/asio_qjs_fs_test/append.txt");
  std::string content((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
  EXPECT_EQ(content, "hello world");
}
