#ifndef CORO_CLOUDSTORAGE_TEST_UTILS_H
#define CORO_CLOUDSTORAGE_TEST_UTILS_H

#include <string>
#include <string_view>

namespace coro::cloudstorage::test {

inline constexpr std::string_view kTestDataDirectory = TEST_DATA_DIRECTORY;
inline constexpr std::string_view kTestRunDirectory = BUILD_DIRECTORY "/test";

std::string GetTestFileContent(std::string_view filename);

void WriteTestFileContent(std::string_view filename, std::string_view content);

bool AreVideosEquiv(std::string_view video1, std::string_view video2,
                    std::string_view format);

}  // namespace coro::cloudstorage::test

#endif  // CORO_CLOUDSTORAGE_TEST_UTILS_H
