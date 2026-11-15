#include "sdmc.hpp"

#include <cstring>

namespace sdmc {

    namespace {

        FsFileSystem sdmc;

    }

    Result Open() {
        return fsOpenSdCardFileSystem(&sdmc);
    }

    void Close() {
        fsFsClose(&sdmc);
    }

    Result OpenFile(FsFile *file, const char *path, int open_mode) {
        char path_buffer[FS_MAX_PATH]{};
        if (std::strlen(path) >= sizeof(path_buffer))
            return MAKERESULT(2, 6001);
        std::strcpy(path_buffer, path);
        return fsFsOpenFile(&sdmc, path_buffer, open_mode, file);
    }

    Result OpenDir(FsDir *dir, const char *path, int open_mode) {
        char path_buffer[FS_MAX_PATH]{};
        if (std::strlen(path) >= sizeof(path_buffer))
            return MAKERESULT(2, 6001);
        std::strcpy(path_buffer, path);
        return fsFsOpenDirectory(&sdmc, path_buffer, open_mode, dir);
    }

    Result GetType(const char* path, FsDirEntryType* type) {
        char path_buffer[FS_MAX_PATH]{};
        if (std::strlen(path) >= sizeof(path_buffer))
            return MAKERESULT(2, 6001);
        std::strcpy(path_buffer, path);
        return fsFsGetEntryType(&sdmc, path_buffer, type);;
    }

    bool FileExists(const char* path) {
        FsDirEntryType type;
        return R_SUCCEEDED(GetType(path, &type)) && type == FsDirEntryType_File;
    }

    Result CreateFolder(const char* path) {
        char path_buffer[FS_MAX_PATH]{};
        if (std::strlen(path) >= sizeof(path_buffer))
            return MAKERESULT(2, 6001);
        std::strcpy(path_buffer, path);
        return fsFsCreateDirectory(&sdmc, path_buffer);
    }

}
