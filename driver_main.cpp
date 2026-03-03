#include "runtime.hpp"

#include <exception>
#include <iostream>
#include <string_view>

namespace {

constexpr const char* kAnsiRed = "\033[0;31m";
constexpr const char* kAnsiGreen = "\033[0;32m";
constexpr const char* kAnsiReset = "\033[0m";
constexpr int kDeadlockExitCode = 2;

void print_regression_status(const char* color, const char* status) {
  std::cout << "BSG REGRESSION TEST "
            << color << status << kAnsiReset << '\n';
}

void print_deadlock_detected() {
  std::cerr << kAnsiRed << "HAMMER-SIM DEADLOCK DETECTED" << kAnsiReset << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  try {
    auto* program_main = hb_native_sim::program_main();
    if (program_main == nullptr) {
      std::cerr << "no program main registered\n";
      print_regression_status(kAnsiRed, "FAILED");
      return 1;
    }
    const int rc = program_main(argc, argv);
    if (rc == 0) {
      print_regression_status(kAnsiGreen, "PASSED");
    } else {
      print_regression_status(kAnsiRed, "FAILED");
    }
    return rc;
  } catch (const std::exception& ex) {
    if (std::string_view(ex.what()) == "deadlock detected") {
      print_deadlock_detected();
      print_regression_status(kAnsiRed, "DEADLOCK");
      return kDeadlockExitCode;
    } else {
      std::cerr << ex.what() << '\n';
    }
    print_regression_status(kAnsiRed, "FAILED");
    return 1;
  }
}
