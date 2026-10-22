#pragma once
#include <cstdio>
#include <cstdlib>
#define CHECK(...) do { if (!(__VA_ARGS__)) { std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__); std::abort(); } } while(false)
#define CHECK_ERROR(expr, expected) do { auto result_ = (expr); CHECK(!result_); CHECK(result_.error().code == (expected)); } while(false)
