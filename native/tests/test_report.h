// test_report.h -- the one result-line / summary / exit-code implementation
// for C++ test harnesses (design-B-refactor.md §2 grammar, frozen):
//   result line : "[<Prefix>] <name> -> PASS|FAIL|SKIP reason=<token>" [" (<detail>)"]
//   summary line: "[<Prefix> SUMMARY] executed=<n> skipped=<m> failed=<k>"
//                 [" skipped_cases=<comma-list>"]   -- exactly once, last line
//   exit code   : 1 if failed > 0, else 2 if executed == 0, else 0
// A SKIP never shares a line with PASS and never increments `executed`.
#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace test_report {

inline int failures = 0;
inline int executed = 0;
inline int skipped = 0;
inline std::vector<std::string> skipped_case_names;

inline void report(const char* prefix, const char* name, bool ok, const char* detail) {
    ++executed;
    if (detail != nullptr && detail[0] != '\0') {
        std::printf("[%s] %s -> %s (%s)\n", prefix, name, ok ? "PASS" : "FAIL", detail);
    } else {
        std::printf("[%s] %s -> %s\n", prefix, name, ok ? "PASS" : "FAIL");
    }
    if (!ok) ++failures;
}

inline void reportSkip(const char* prefix, const char* name, const char* reason,
                       const char* detail = nullptr) {
    ++skipped;
    skipped_case_names.emplace_back(name);
    if (detail != nullptr && detail[0] != '\0') {
        std::printf("[%s] %s -> SKIP reason=%s (%s)\n", prefix, name, reason, detail);
    } else {
        std::printf("[%s] %s -> SKIP reason=%s\n", prefix, name, reason);
    }
}

inline int finish(const char* prefix) {
    std::printf("[%s SUMMARY] executed=%d skipped=%d failed=%d", prefix, executed,
                skipped, failures);
    if (!skipped_case_names.empty()) {
        std::printf(" skipped_cases=");
        for (size_t index = 0; index < skipped_case_names.size(); ++index) {
            std::printf("%s%s", index == 0 ? "" : ",", skipped_case_names[index].c_str());
        }
    }
    std::printf("\n");
    std::fflush(stdout);
    if (failures > 0) return 1;
    if (executed == 0) return 2;
    return 0;
}

}  // namespace test_report

// Each adopter defines `constexpr const char kReportPrefix[] = "<Prefix>";`
// before the first CHECK use.
#define CHECK(name, cond, detail) \
    ::test_report::report(kReportPrefix, (name), (cond), (detail))
