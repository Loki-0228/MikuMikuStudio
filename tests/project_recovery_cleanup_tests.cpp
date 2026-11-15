#include "io/project_recovery_cleanup.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/wait.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using mikumikustudio::project_recovery::CompleteCleanup;

void Check(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}

struct Directory {
    fs::path path;
    Directory() {
        auto pattern = (fs::temp_directory_path() / "mms-journal-cleanup-XXXXXX").string();
        const auto* result = mkdtemp(pattern.data());
        if (!result) throw std::system_error(errno, std::generic_category(), "mkdtemp");
        path = result;
    }
    ~Directory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};
void Write(const fs::path& path, const char* bytes) {
    std::ofstream file(path, std::ios::binary);
    file << bytes;
    file.close();
    Check(static_cast<bool>(file), "write fixture");
}
std::string Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    Check(static_cast<bool>(file), "read fixture");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

template<class Body>
int RunChild(Body body) {
    const auto pid = fork();
    if (pid < 0) throw std::system_error(errno, std::generic_category(), "fork");
    if (!pid) {
        try { body(); }
        catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            _exit(1);
        }
        _exit(0);
    }
    int status = 0;
    pid_t waited;
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    Check(waited == pid, "wait for fixture process");
    return status;
}

void CheckCleanupInterruption() {
    const Directory fixture;
    const auto active = fixture.path / "active.pmm";
    const auto one = fixture.path / "active.pmm1", two = fixture.path / "active.pmm11";
    const auto untouched = fixture.path / "active.pmm111";
    const auto journal = fixture.path / "active.pmm.mms-save.json";
    const auto duplicate = fixture.path / "staged.new-pmm";
    const auto last = fixture.path / "staged.old-pmm";
    constexpr char decision[] = "durable decision fixture bytes";
    Write(active, "fresh");
    Write(one, "previous");
    Write(two, "oldest");
    Write(duplicate, "fresh");
    Write(last, "previous");
    Write(journal, decision);

    // A real directory entry disappears before the child dies. This exercises
    // production's cleanup order with POSIX operations, not Win32 file identity
    // checks, journal encoding, or archive alignment.
    std::array<fs::path, 2> stages{duplicate, last};
    const auto cleanup = [&](bool interrupt) {
        CompleteCleanup(stages, [&](const fs::path& path) {
            const bool removed = fs::remove(path);
            if (interrupt) {
                Check(path == duplicate && removed, "kill only after actual first stage removal");
                Check(kill(getpid(), SIGKILL) == 0, "kill interrupted recovery");
                _exit(2);
            }
        }, [&] {
            for (const auto& path : stages) Check(!fs::exists(path), "stage still present at continuation");
            Check(fs::exists(journal) && Read(journal) == decision, "journal removed before continuation");
        }, [&] { fs::remove(journal); });
    };
    const auto checkChain = [&] {
        Check(Read(active) == "fresh" && Read(one) == "previous" && Read(two) == "oldest" &&
              !fs::exists(untouched), "retained chain changed");
    };
    const int killed = RunChild([&] { cleanup(true); });
    Check(WIFSIGNALED(killed) && WTERMSIG(killed) == SIGKILL, "child did not reach hard interruption");
    Check(!fs::exists(duplicate) && Read(last) == "previous", "unexpected deletion prefix");
    Check(fs::exists(journal) && Read(journal) == decision, "journal did not survive the interrupted deletion");
    checkChain();

    const int resumed = RunChild([&] {
        cleanup(false);
        Check(!fs::exists(journal), "retry did not complete the journal");
        checkChain();
    });
    Check(WIFEXITED(resumed) && WEXITSTATUS(resumed) == 0, "fresh recovery process failed");
    Check(!fs::exists(duplicate) && !fs::exists(last) && !fs::exists(journal), "retry left cleanup artifacts");
    checkChain();
}
}

int main() {
    try {
        CheckCleanupInterruption();
        std::cout << "PASS project recovery cleanup: actual stage deletion, hard interruption, fresh retry\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL project recovery cleanup: " << error.what() << '\n';
        return 1;
    }
}
