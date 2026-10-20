#include "settinggifscanner.h"

#include <algorithm>
#include <cctype>
#include <dirent.h>
#include <sys/stat.h>

namespace {

bool hasGifExtension(const std::string &fileName)
{
    static const char suffix[] = ".gif";
    const std::size_t suffixLength = sizeof(suffix) - 1;
    if (fileName.size() < suffixLength) {
        return false;
    }

    const std::size_t offset = fileName.size() - suffixLength;
    for (std::size_t i = 0; i < suffixLength; ++i) {
        const unsigned char actual =
            static_cast<unsigned char>(fileName[offset + i]);
        const unsigned char expected = static_cast<unsigned char>(suffix[i]);
        if (std::tolower(actual) != std::tolower(expected)) {
            return false;
        }
    }

    return true;
}

std::string joinPath(const std::string &directoryPath, const std::string &fileName)
{
    if (directoryPath.empty()) {
        return fileName;
    }

    const char last = directoryPath[directoryPath.size() - 1];
    if (last == '/' || last == '\\') {
        return directoryPath + fileName;
    }

    return directoryPath + "/" + fileName;
}

bool isRegularFile(const std::string &path)
{
    struct stat fileStat;
    if (stat(path.c_str(), &fileStat) != 0) {
        return false;
    }

#ifdef _WIN32
    return (fileStat.st_mode & _S_IFREG) != 0;
#else
    return S_ISREG(fileStat.st_mode);
#endif
}

class DirectoryHandle {
public:
    explicit DirectoryHandle(const std::string &directoryPath)
        : directory_(opendir(directoryPath.c_str()))
    {
    }

    ~DirectoryHandle()
    {
        if (directory_ != nullptr) {
            closedir(directory_);
        }
    }

    DIR *get() const
    {
        return directory_;
    }

private:
    DIR *directory_;

    DirectoryHandle(const DirectoryHandle &);
    DirectoryHandle &operator=(const DirectoryHandle &);
};

} // namespace

namespace vehiclesettings {

std::vector<std::string> collectGifFiles(const std::string &directoryPath)
{
    std::vector<std::string> files;
    DirectoryHandle directory(directoryPath);
    if (directory.get() == nullptr) {
        return files;
    }

    while (dirent *entry = readdir(directory.get())) {
        const std::string fileName(entry->d_name);
        if (!hasGifExtension(fileName)) {
            continue;
        }

        const std::string fullPath = joinPath(directoryPath, fileName);
        if (isRegularFile(fullPath)) {
            files.push_back(fullPath);
        }
    }

    std::sort(files.begin(), files.end());
    return files;
}

} // namespace vehiclesettings
