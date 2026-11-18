// SPDX-License-Identifier: MIT
// Copyright © 2026 Germán Méndez Bravo (Kronuz)

// The tests' one assertion: counts failures and keeps going, live in every build type.
#pragma once

#include <cstdio>
#include <exception>
#include <functional>
#include <system_error>

namespace faultlab_test {

inline int& failures() {
	static int n = 0;
	return n;
}

inline bool check(bool ok, const char* what, const char* file, int line) {
	if (!ok) {
		++failures();
		std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", file, line, what);
	}
	return ok;
}

// Runs one named case; an escaping exception is a failure.
inline void run(const char* name, const std::function<void()>& body) {
	const int before = failures();
	try {
		body();
	} catch (const std::exception& e) {
		++failures();
		std::fprintf(stderr, "%s: threw %s\n", name, e.what());
	}
	std::printf("%s %s\n", failures() == before ? "ok:  " : "FAIL:", name);
}

inline int verdict() {
	std::printf("=== %s (%d failures)\n", failures() ? "RED" : "GREEN", failures());
	return failures() ? 1 : 0;
}

}  // namespace faultlab_test

#define CHECK(cond) ::faultlab_test::check(static_cast<bool>(cond), #cond, __FILE__, __LINE__)

// Passes when `expr` throws std::system_error with `code`.
#define CHECK_ERRNO(expr, code)                                                     \
	do {                                                                            \
		int caught_ = 0;                                                            \
		try {                                                                       \
			(void)(expr);                                                           \
		} catch (const std::system_error& e_) {                                     \
			caught_ = e_.code().value();                                            \
		}                                                                           \
		::faultlab_test::check(caught_ == (code), #expr " fails with " #code, __FILE__, __LINE__); \
	} while (0)
