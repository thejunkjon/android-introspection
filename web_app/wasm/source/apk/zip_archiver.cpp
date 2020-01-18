//
// MIT License
//
// Copyright 2019-2020
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
#include <filesystem>
#include <fstream>
#include <memory>
#include <utility>

#include "scoped_minizip.h"
#include "utils/log.h"
#include "utils/utils.h"
#include "zip.h"
#include "zip_archiver.h"

using namespace ai;
using namespace ai::minizip;

namespace {

auto openZipFile(std::string const &path) {
  auto fileExists = std::filesystem::exists(path);
  auto zipMode = fileExists ? APPEND_STATUS_ADDINZIP : APPEND_STATUS_CREATE;
  auto openedZipFile = std::make_unique<ScopedZipOpen>(path.c_str(), zipMode);
  if (openedZipFile->get() == nullptr) {
    throw std::logic_error("unable to open zip file");
  }
  return openedZipFile;
}

auto writeStreamToOpenZipFile(std::istream &source, zipFile const zipFile) {
  auto result = ZIP_OK;
  std::vector<char> readBuffer;
  readBuffer.resize(BUFSIZ);
  size_t readCount = 0;
  do {
    source.read(readBuffer.data(), static_cast<uint32_t>(readBuffer.size()));
    readCount = static_cast<size_t>(source.gcount());
    if (readCount < readBuffer.size() && !source.eof() && !source.good()) {
      result = ZIP_ERRNO;
    } else if (readCount > 0) {
      result = zipWriteInFileInZip(zipFile, readBuffer.data(), static_cast<uint32_t>(readCount));
    }
  } while ((result == ZIP_OK) && (readCount > 0));
}

auto getAllEntriesInZipFile(std::string const &path) {
  using PathAndFileInfo = std::pair<std::string, unz_file_info64>;
  auto entries = std::vector<PathAndFileInfo>();
  auto openedZipFile = ScopedUnzOpenFile(path.c_str());
  if (openedZipFile.get() == nullptr) {
    LOGW("getAllEntriesInZipFile, path [%s]", path.c_str());
    return entries;
  }
  if (auto result = unzGoToFirstFile(openedZipFile.get()); result == UNZ_OK) {
    do {
      unz_file_info64 fileInfo = {};
      char fileNameInZip[256] = {};
      result = unzGetCurrentFileInfo64(openedZipFile.get(), &fileInfo, fileNameInZip, sizeof(fileNameInZip), nullptr, 0, nullptr, 0);
      if (result == UNZ_OK) {
        entries.push_back(std::make_pair(std::string(fileNameInZip), fileInfo));
      }
      result = unzGoToNextFile(openedZipFile.get());
    } while (UNZ_OK == result);
  }
  return entries;
}

} // namespace

auto ZipArchiver::add(std::istream &source, std::string_view const path) const -> void {
  LOGD("add, path [%s]", path);
  auto const zipFile = openZipFile(zipPath_);
  auto const pathString = std::string(path);
  if (auto result = zipOpenNewFileInZip_64(zipFile->get(), pathString.c_str(), nullptr, nullptr, 0, nullptr, 0, nullptr, 0, 0, false); result == ZIP_OK) {
    auto scopedZipCloseFileInZip = ScopedZipCloseFileInZip(zipFile->get());
    writeStreamToOpenZipFile(source, zipFile->get());
  }
}

auto ZipArchiver::contains(std::string_view path) const -> bool {
  LOGD("contains, path [%s]", path);
  if (auto const zipFile = ScopedUnzOpenFile(zipPath_.c_str()); zipFile.get() != nullptr) {
    auto const pathString = std::string(path);
    return unzLocateFile(zipFile.get(), pathString.c_str(), nullptr) == UNZ_OK;
  } else {
    return false;
  }
}

auto ZipArchiver::extractAll(std::string_view path) const -> void {
  LOGD("extractAll, path [%s]", path);
  using namespace std::filesystem;
  auto const isPathValid = is_directory(path) or !exists(path);
  if (!isPathValid) {
    throw std::logic_error("path must be a directory or must not exist");
  }
  auto const entries = getAllEntriesInZipFile(zipPath_);
  for (auto const &entry : entries) {
    extract(entry.first, path);
  }
}

auto ZipArchiver::extract(std::string_view pathToExtract, std::string_view path) const -> void {
  LOGD("extract, pathToExtract [%s] path [%s]", pathToExtract, path);
  namespace fs = std::filesystem;
  auto const isPathValid = fs::is_directory(path) or !fs::exists(path);
  if (!isPathValid) {
    throw std::logic_error("path must be a directory or must not exist");
  }
  auto const entries = getAllEntriesInZipFile(zipPath_);
  auto pathInZip = std::find_if(entries.cbegin(), entries.cend(),
                                [&pathToExtract = std::as_const(pathToExtract)](auto const &entry) { return entry.first == pathToExtract; });
  if (pathInZip == entries.cend()) {
    throw std::logic_error("path does not exist in archive");
  }
  if (auto const zipFile = ScopedUnzOpenFile(zipPath_.c_str()); zipFile.get() == nullptr) {
    throw std::logic_error("archive does not exist");
  } else {
    if (auto const result = unzLocateFile(zipFile.get(), pathInZip->first.c_str(), nullptr); result != UNZ_OK) {
      throw std::logic_error("path does not exist in archive");
    }
    unz_file_info zipFileInfo;
    if (auto const result = unzGetCurrentFileInfo(zipFile.get(), &zipFileInfo, nullptr, 0, nullptr, 0, nullptr, 0); result != UNZ_OK) {
      throw std::logic_error("unable to get file info in archive");
    }
    auto const openedZipFile = ScopedUnzOpenCurrentFile(zipFile.get());
    if (auto result = openedZipFile.result(); result != MZ_OK) {
      throw std::logic_error("unable to open zip entry in archive");
    }
    auto fileContents = std::vector<std::byte>(zipFileInfo.uncompressed_size);
    auto fileContentsSize = static_cast<uint32_t>(fileContents.size());
    if (auto const bytesRead = unzReadCurrentFile(zipFile.get(), &fileContents[0], fileContentsSize); static_cast<uint32_t>(bytesRead) != fileContentsSize) {
      throw std::logic_error("unable to read full file in archive");
    }
    auto const pathString = std::string(path);
    auto const fullPathToExtract = fs::path(pathString) / pathInZip->first.c_str();
    auto outputFile = std::ofstream(fullPathToExtract.string(), std::fstream::binary);
    outputFile.write(reinterpret_cast<char const *>(fileContents.data()), sizeof(std::byte) * fileContents.size());
    outputFile.close();
  }
}
