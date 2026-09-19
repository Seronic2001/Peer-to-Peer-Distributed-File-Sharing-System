#pragma once
// Minimal, dependency-free unit test framework for the P2P project.
//
// Usage:
//   #include "test_assert.h"
//   TEST(protocol_roundtrip) { ... TEST_ASSERT(...); ... }
//   int main() { return run_all_tests(); }   // in exactly one test .cpp
//
// Each TEST body runs on its own thread; an uncaught exception or failed
// assertion marks that test failed and the suite continues.

#include <cmath>
#include <cstdio>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace testfw {

struct TestCase {
  const char* name;
  std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}

inline int& failures() {
  static int f = 0;
  return f;
}

inline bool register_test(const char* name, std::function<void()> fn) {
  registry().push_back({name, std::move(fn)});
  return true;
}

// RAII guard so a failed assertion inside a helper doesn't unwind through
// ASSERT macros that use `return` in void functions.
struct AssertError : std::exception {
  std::string msg;
  explicit AssertError(std::string m) : msg(std::move(m)) {}
  const char* what() const noexcept override { return msg.c_str(); }
};

template <typename T>
std::string repr(const T& v) {
  std::ostringstream ss;
  ss << v;
  return ss.str();
}

inline void run_all_tests() {
  int passed = 0;
  const auto& tests = registry();
  for (const auto& t : tests) {
    // Run each test on its own thread with an assertion flag; exceptions are
    // caught here so one failing test never aborts the suite.
    bool test_failed = false;
    std::string failure_msg;
    std::thread worker([&] {
      try {
        t.fn();
      } catch (const AssertError& e) {
        test_failed = true;
        failure_msg = e.msg;
      } catch (const std::exception& e) {
        test_failed = true;
        failure_msg = std::string("uncaught exception: ") + e.what();
      } catch (...) {
        test_failed = true;
        failure_msg = "uncaught non-standard exception";
      }
    });
    worker.join();

    if (test_failed) {
      failures()++;
      std::cout << "[FAIL] " << t.name << " -- " << failure_msg << std::endl;
    } else {
      passed++;
      std::cout << "[ ok ] " << t.name << std::endl;
    }
  }
  std::cout << "\n" << passed << "/" << tests.size() << " tests passed"
            << std::endl;
  if (failures() > 0) {
    std::cout << failures() << " test(s) FAILED" << std::endl;
  }
}

}  // namespace testfw

#define TEST(name)                                                        \
  static void test_##name();                                              \
  static const bool reg_##name =                                          \
      ::testfw::register_test(#name, test_##name);                        \
  static void test_##name()

#define TEST_FAIL_MSG(msg) \
  throw ::testfw::AssertError(std::string(__FILE__) + ":" +                \
                              std::to_string(__LINE__) + ": " + (msg))

#define TEST_ASSERT(cond)                                            \
  do {                                                               \
    if (!(cond)) {                                                   \
      TEST_FAIL_MSG("assertion failed: " #cond);                     \
    }                                                                \
  } while (0)

#define TEST_ASSERT_EQ(a, b)                                              \
  do {                                                                    \
    auto va = (a);                                                        \
    auto vb = (b);                                                        \
    if (!(va == vb)) {                                                    \
      TEST_FAIL_MSG("expected " + ::testfw::repr(vb) + " but got " +      \
                    ::testfw::repr(va) + " (" #a " == " #b ")");          \
    }                                                                     \
  } while (0)

// Floating-point comparison with tolerance.
#define TEST_ASSERT_NEAR(a, b, eps)                                   \
  do {                                                                \
    double va = (a);                                                  \
    double vb = (b);                                                  \
    if (std::fabs(va - vb) > (eps)) {                                 \
      TEST_FAIL_MSG("expected " + ::testfw::repr(vb) + " +/- " +      \
                    ::testfw::repr(eps) + " but got " +               \
                    ::testfw::repr(va));                              \
    }                                                                 \
  } while (0)

#define TEST_ASSERT_THROWS(expr)                                      \
  do {                                                                \
    bool threw = false;                                               \
    try {                                                             \
      (void)(expr);                                                   \
    } catch (const std::exception&) {                                 \
      threw = true;                                                   \
    } catch (...) {                                                   \
      threw = true;                                                   \
    }                                                                 \
    if (!threw) {                                                     \
      TEST_FAIL_MSG("expected exception from: " #expr);               \
    }                                                                 \
  } while (0)

// main() lives in a dedicated translation unit so any number of test files
// can include this header without link conflicts.
#define TEST_MAIN()                                        \
  int main() {                                             \
    std::cout << "Running test suite...\n" << std::endl;   \
    ::testfw::run_all_tests();                             \
    return ::testfw::failures() == 0 ? 0 : 1;              \
  }
