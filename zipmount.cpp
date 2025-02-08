#include "stdafx.h"

const char * APP_NAME = "zipmount";
const char * APP_VERSION = "0.1.0";

using namespace std; namespace fs = filesystem;
using namespace ATL;

using path = fs::path;

static struct ok_type {
    string msg;

    ok_type & operator=(int rc) {
        if(!rc) {
            if(!msg.empty()) {
                println("[failed] {}, code: ", msg, rc); msg.clear();
            }
            else {
                println("[failed] code: {}", rc);
            }

            exit(rc);
        }

        return *this;
    }

    ok_type & operator=(bool b) {
        if(!b) {
            if(!msg.empty()) {
                println("[failed] {}", msg); msg.clear();
            }
            else {
                println("[failed] code: false");
            }

            exit(1);
        }

        return *this;
    }

    template<typename T>
    ok_type & operator()(T && s) { msg = std::forward<T>(s); return *this; }
} ok;

// fs callbacks
static NTSTATUS DOKAN_CALLBACK zmCreateFile(LPCWSTR FileName, PDOKAN_IO_SECURITY_CONTEXT SecurityContext, ACCESS_MASK DesiredAccess, ULONG FileAttributes, ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions, PDOKAN_FILE_INFO DokanFileInfo) {
    DWORD creationDisposition, fileAttributesAndFlags; ACCESS_MASK genericDesiredAccess; {
        DokanMapKernelToUserCreateFileFlags(
            DesiredAccess, FileAttributes, CreateOptions, CreateDisposition,
            &genericDesiredAccess, &fileAttributesAndFlags, &creationDisposition);
    }

    USES_CONVERSION;

    auto fpath = W2A(path(FileName).generic_wstring().c_str());

    auto fname = fpath; if(!PHYSFS_exists(fname)) {
        if((creationDisposition == CREATE_NEW) || (creationDisposition == OPEN_ALWAYS)) {
            return DokanNtStatusFromWin32(ERROR_ACCESS_DENIED);
        }

        return DokanNtStatusFromWin32(ERROR_FILE_NOT_FOUND);
    }

    if(creationDisposition == CREATE_NEW) {
        return DokanNtStatusFromWin32(ERROR_FILE_EXISTS);
    }

    bool is_dir = PHYSFS_isDirectory(fname); if(is_dir) {
        DokanFileInfo->IsDirectory = TRUE;

        if(creationDisposition == OPEN_ALWAYS) return STATUS_OBJECT_NAME_COLLISION;

        return STATUS_SUCCESS;
    }

    DokanFileInfo->IsDirectory = FALSE;

    PHYSFS_File * file = PHYSFS_openRead(fname); if(!file) {
        return PHYSFS_getLastErrorCode();
    }

    DokanFileInfo->Context = (intptr_t)file;

    return STATUS_SUCCESS;
}

static void DOKAN_CALLBACK zmCleanup(LPCWSTR FileName, PDOKAN_FILE_INFO DokanFileInfo) { return; }

static void DOKAN_CALLBACK zmCloseFile(LPCWSTR FileName, PDOKAN_FILE_INFO DokanFileInfo) {
    if(DokanFileInfo->IsDirectory) {
    }
    else {
        auto file = (PHYSFS_File *)DokanFileInfo->Context; PHYSFS_close(file);
    }
}

static NTSTATUS DOKAN_CALLBACK zmReadFile(LPCWSTR FileName, LPVOID Buffer, DWORD BufferLength, LPDWORD ReadLength, LONGLONG Offset, PDOKAN_FILE_INFO DokanFileInfo) {
    auto file = (PHYSFS_File *)DokanFileInfo->Context; if(!file) {
        return STATUS_INVALID_PARAMETER;
    }

    if(Offset) {
        auto rc = PHYSFS_seek(file, Offset); if(rc == 0) {
            return PHYSFS_getLastErrorCode();
        }
    }

    auto read = PHYSFS_read(file, Buffer, 1, BufferLength); if(read == -1) {
        return PHYSFS_getLastErrorCode();
    }

    *ReadLength = read; return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK zmGetFileInformation(LPCWSTR FileName, LPBY_HANDLE_FILE_INFORMATION HandleFileInformation, PDOKAN_FILE_INFO DokanFileInfo) {
    if(DokanFileInfo->IsDirectory) {
        HandleFileInformation->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY; return STATUS_SUCCESS;
    }

    auto file = (PHYSFS_File *)DokanFileInfo->Context; if(!file) {
        return STATUS_INVALID_PARAMETER;
    }

    USES_CONVERSION;

    auto fpath = W2A(path(FileName).generic_wstring().c_str());

    PHYSFS_Stat stat; if(!PHYSFS_stat(fpath, &stat)) {
        return PHYSFS_getLastErrorCode();
    }

    HandleFileInformation->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    HandleFileInformation->nFileSizeLow = stat.filesize;
    HandleFileInformation->nFileSizeHigh = stat.filesize >> 32;
    HandleFileInformation->ftCreationTime = (FILETIME)stat.createtime;
    HandleFileInformation->ftLastWriteTime = (FILETIME)stat.modtime;
    HandleFileInformation->ftLastAccessTime = (FILETIME)stat.accesstime;
    HandleFileInformation->nNumberOfLinks = 0;
    HandleFileInformation->nFileIndexHigh = 0;
    HandleFileInformation->nFileIndexLow = 0;
    HandleFileInformation->dwVolumeSerialNumber = 0;

    return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK zmFindFiles(LPCWSTR FileName, PFillFindData FillFindData, PDOKAN_FILE_INFO DokanFileInfo) {
    USES_CONVERSION;

    auto dname = W2A(path(FileName).generic_wstring().c_str());

    auto params = std::make_pair(FillFindData, DokanFileInfo);

    PHYSFS_enumerate(
        dname, [](void * userp, const char * dname, const char * fname, int isdir) -> PHYSFS_EnumerateCallbackResult {
            USES_CONVERSION;

            auto ctx = (std::pair<PFillFindData, PDOKAN_FILE_INFO> *)userp;

            auto FillFindData = ctx->first; auto DokanFileInfo = ctx->second;

            WIN32_FIND_DATAW find_data {0}; if(isdir) {
                find_data.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
            }
            else {
                auto fpath = W2A((path(dname) / fname).generic_wstring().c_str());

                PHYSFS_Stat stat; PHYSFS_stat(fpath, &stat);

                find_data.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
                find_data.nFileSizeLow = stat.filesize;
                find_data.nFileSizeHigh = stat.filesize >> 32;
                find_data.ftCreationTime = (FILETIME)stat.createtime;
                find_data.ftLastWriteTime = (FILETIME)stat.modtime;
                find_data.ftLastAccessTime = (FILETIME)stat.accesstime;
            }

            wcscpy(find_data.cFileName, A2W(fname));

            FillFindData(&find_data, DokanFileInfo);

            return PHYSFS_ENUM_OK;
        }, &params);

    return STATUS_SUCCESS;
}

static PHYSFS_Allocator physfs_allocator =
    {0, 0, mi_malloc, mi_realloc, mi_free};

struct zipmount_options {
    string archive_fname; optional<string> mount_point {"m:\\"};
};

STRUCTOPT(zipmount_options, archive_fname, mount_point);

static wstring mount_point;

int main(int argc, char ** argv) {
    USES_CONVERSION; try {
        // Line of code that does all the work:
        auto options = structopt::app(APP_NAME, APP_VERSION).parse<zipmount_options>(argc, argv);

        ok(format("locate archive file '{}'", options.archive_fname)) = fs::exists(options.archive_fname);

        ok = PHYSFS_setAllocator(&physfs_allocator);
        ok("init physfs") = PHYSFS_init(APP_NAME);
        ok(format("mount archive '{}' to '/'", options.archive_fname)) = PHYSFS_mount(options.archive_fname.c_str(), "/", 1);

        mount_point = A2W(options.mount_point.value().c_str());

        SetConsoleCtrlHandler([](DWORD type) {
            switch(type) {
                case CTRL_C_EVENT:
                case CTRL_BREAK_EVENT:
                case CTRL_CLOSE_EVENT:
                case CTRL_LOGOFF_EVENT:
                case CTRL_SHUTDOWN_EVENT: {
                    DokanRemoveMountPoint(mount_point.c_str()); exit(0);
                }
            }

            return FALSE;
        }, true);

        DOKAN_OPTIONS dokanOptions {0}; {
            dokanOptions.Version = DOKAN_VERSION;
            dokanOptions.SingleThread = TRUE;
            dokanOptions.MountPoint = mount_point.c_str();
            dokanOptions.Options =
                DOKAN_OPTION_REMOVABLE | DOKAN_OPTION_WRITE_PROTECT | DOKAN_OPTION_MOUNT_MANAGER;
            // dokanOptions.UNCName = unc_name.c_str();
            // dokanOptions.Options = DOKAN_OPTION_NETWORK | DOKAN_OPTION_ENABLE_UNMOUNT_NETWORK_DRIVE;
        }

        DOKAN_OPERATIONS dokanOperations {0}; {
            dokanOperations.ZwCreateFile = zmCreateFile;
            dokanOperations.Cleanup = zmCleanup;
            dokanOperations.CloseFile = zmCloseFile;
            dokanOperations.ReadFile = zmReadFile;
            dokanOperations.GetFileInformation = zmGetFileInformation;
            dokanOperations.FindFiles = zmFindFiles;
        }

        DokanInit();

        auto rc = DokanMain(&dokanOptions, &dokanOperations); switch(rc) {
            case DOKAN_SUCCESS: break;
            case DOKAN_ERROR: println("Error"); break;
            case DOKAN_DRIVE_LETTER_ERROR: println("Bad Drive letter"); break;
            case DOKAN_DRIVER_INSTALL_ERROR: println("Can't install driver"); break;
            case DOKAN_START_ERROR: println("Driver something wrong"); break;
            case DOKAN_MOUNT_ERROR: println("Can't assign a drive letter"); break;
            case DOKAN_MOUNT_POINT_ERROR: println("Mount point error"); break;
            case DOKAN_VERSION_ERROR: println("Version error"); break;
            default: println("Unknown error: {}", rc); break;
        }

        DokanShutdown();
    }
    catch(structopt::exception & e) {
        println("{}", e.what()); println("{}", e.help());
    }

    return 0;
}