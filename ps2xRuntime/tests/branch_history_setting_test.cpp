// Public diagnostic behavior: first-use selection, concurrent initialization,
// and the process-sticky setting must survive removal of the hot acquire guard.
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

void dumpBranchRing(const char *why);

static void require(bool ok, const char *message)
{
    if (!ok)
    {
        std::fprintf(stderr, "branch history setting test: %s\n", message);
        std::exit(1);
    }
}

int main(int argc, char **argv)
{
    require(argc == 2, "expected 0, 1, or unset");
    const std::string mode(argv[1]);
    require(mode == "0" || mode == "1" || mode == "unset", "invalid mode");
    // Deliberately change the inherited environment after library loading.
    if (mode == "unset") unsetenv("PS2X_BRANCH_RING");
    else setenv("PS2X_BRANCH_RING", mode.c_str(), 1);

    FILE *capture = std::tmpfile();
    require(capture != nullptr, "tmpfile failed");
    std::fflush(stderr);
    const int saved = dup(STDERR_FILENO);
    require(saved >= 0 && dup2(fileno(capture), STDERR_FILENO) >= 0, "capture failed");

    std::mutex mutex;
    std::condition_variable cv;
    unsigned ready = 0;
    bool go = false;
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 8; ++i)
        threads.emplace_back([&] {
            {
                std::unique_lock lock(mutex);
                ++ready;
                cv.notify_all();
                cv.wait(lock, [&] { return go; });
            }
            dumpBranchRing("concurrent first use");
        });
    {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return ready == 8; });
        go = true;
    }
    cv.notify_all();
    for (auto &thread : threads) thread.join();

    setenv("PS2X_BRANCH_RING", mode == "1" ? "0" : "1", 1);
    dumpBranchRing("setting remains sticky");
    std::fflush(stderr);
    require(dup2(saved, STDERR_FILENO) >= 0, "restore failed");
    close(saved);
    std::rewind(capture);
    std::string output;
    char buffer[512];
    while (const auto size = std::fread(buffer, 1, sizeof(buffer), capture))
        output.append(buffer, size);
    std::fclose(capture);
    unsigned headers = 0;
    for (std::size_t offset = 0; (offset = output.find("[branch-trace]", offset)) != std::string::npos; ++offset)
        ++headers;
    require(headers == (mode == "1" ? 9u : 0u), "wrong first-use/sticky setting");
    std::printf("PASS branch history mode=%s, eight concurrent callers and later env change\n", mode.c_str());
}
