#include "stdafx.h"
#include <algorithm>

// a cache which evicts the least recently used item when it is full
template<class Key, class Value>
class lru_cache {
public:
    typedef Key key_type;
    typedef Value value_type;
    typedef std::list<key_type> list_type;
    typedef std::pair<value_type, typename list_type::iterator> xvalue_type;
    typedef std::map<key_type, xvalue_type> map_type;

    lru_cache(size_t capacity) : m_capacity(capacity) {}

    ~lru_cache() {}

    size_t size() const { return m_map.size(); }

    size_t capacity() const { return m_capacity; }

    bool empty() const { return m_map.empty(); }

    bool contains(const key_type & key) { return m_map.find(key) != m_map.end(); }

    template<typename K, typename V>
    void insert(K && key, V && value) {
        typename map_type::iterator i = m_map.find(key); if(i == m_map.end()) {
            // insert item into the cache, but first check if it is full
            if(size() >= m_capacity) {
                // cache is full, evict the least recently used item
                evict();
            }

            // insert the new item
            m_list.push_front(std::forward<K>(key));
            m_map.emplace(std::forward<K>(key), xvalue_type {std::forward<V>(value), m_list.begin()});
        }
    }

    const value_type * get(const key_type & key) {
        // lookup value in the cache
        typename map_type::iterator i = m_map.find(key);

        if(i == m_map.end()) return nullptr;

        // return the value, but first update its place in the most recently used list
        typename list_type::iterator j = i->second.second; if(j != m_list.begin()) {
            // move item to the front of the most recently used list
            m_list.erase(j); m_list.push_front(key);

            // update iterator in map
            j = m_list.begin(); const value_type & value = i->second.first; {
                m_map[key] = std::make_pair(value, j);
            }

            // return the value
            return &value;
        }
        else {
            // the item is already at the front of the most recently
            // used list so just return it
            return &i->second.first;
        }
    }

    void clear() { m_map.clear(); m_list.clear(); }

private:
    void evict() {
        // evict item from the end of most recently used list
        typename list_type::iterator i = --m_list.end(); {
            m_map.erase(*i); m_list.erase(i);
        }
    }

private:
    map_type m_map; list_type m_list; size_t m_capacity;
};

const char * APP_NAME = "zipmount";
const char * APP_VERSION = "0.1.0";

using namespace std; using namespace ATL; namespace fs = filesystem; using fs::path;

static struct ok_type {
    bool epilogue {false};

    void failed(int rc = 1) { if(epilogue) { print("failed\n"); epilogue = false; } exit(rc); }

    void succeeded() { if(epilogue) { print("\n"); epilogue = false; } }

    ok_type & operator=(int rc) {
        if(rc) failed(rc); else succeeded(); ; return *this;
    }

    template<typename T, std::enable_if_t<std::is_same_v<T, bool>, int> = 0>
    ok_type & operator=(T b) {
        if(!b) failed(); else succeeded(); ; return *this;
    }

    template<typename T>
    ok_type & operator()(T && s) {
        auto now = std::chrono::system_clock::now();
        auto time_point = std::chrono::floor<std::chrono::seconds>(now);
        auto time_of_day = std::chrono::hh_mm_ss {time_point - std::chrono::floor<std::chrono::days>(time_point)};

        epilogue = true; print("[{:%T}] {}", time_of_day, s); return *this;
    }
} ok;

static struct {
    enum { NONE, FILE, DIR };

    struct entry_t {
        int type {0}; int index {0};

        operator bool() const { return !type; }

        bool is_file() const { return type == FILE; }

        bool is_dir() const { return type == DIR; }
    };

    struct stat_t {
        string fpath; size_t size; int64_t mtime; int type;

        bool is_file() const { return type == FILE; }

        bool is_dir() const { return type == DIR; }
    };

    mz_zip_archive zipf {0}; size_t size {0}; lru_cache<int, std::string> cache {128}; CAtlFileMappingBase fmapping;

    string canonicalize(LPCWSTR FileName) {
        USES_CONVERSION; auto ws = W2A(path(FileName).generic_wstring().c_str()); return ws[0] == '/' ? ++ws : ws;
    }

    int open(string const & fname) {
        CAtlFile f; {
            ok = f.Create(fname.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
            ok = fmapping.MapFile(f);
        }

        ok = (mz_zip_reader_init_mem(&zipf, fmapping.GetData(), fmapping.GetMappingSize(), 0) == MZ_TRUE);

        size = mz_zip_reader_get_num_files(&zipf);

        return 0;
    }

    stat_t stat(int findex) {
        mz_zip_archive_file_stat st; ok = (mz_zip_reader_file_stat(&zipf, findex, &st) == MZ_TRUE);

        stat_t r; {
            r.fpath = st.m_filename; r.size = st.m_uncomp_size; r.mtime = st.m_time; r.type = (st.m_is_directory) ? DIR : FILE;
        }

        return r;
    }

    entry_t locate(string const & fname) {
        if(fname.empty() || fname == "/") return {DIR, -1};

        auto index = mz_zip_reader_locate_file(&zipf, fname.c_str(), 0, 0); if(index < 0) {
            string dname = fname + '/';

            index = mz_zip_reader_locate_file(&zipf, dname.c_str(), 0, 0); {
                if(!(index < 0)) return {DIR, index + 1};

                index = mz_zip_reader_locate_dir(&zipf, dname.c_str(), 0, 0); {
                    if(!(index < 0)) {
                        while(index > 0) {
                            auto st = stat(index - 1); {
                                if(!st.fpath.starts_with(dname)) break; else --index;
                            }
                        }

                        return {DIR, index};
                    }
                }
            }

            return {};
        }

        return {stat(index).type, index};
    }

    const string * read(int findex) {
        { // try cache first
            auto r = cache.get(findex); if(r) {
                return r;
            }
        }

        auto st = stat(findex); std::string s(st.size, 0); {
            ok = (mz_zip_reader_extract_to_mem(&zipf, findex, (void *)s.data(), s.size(), 0) == MZ_TRUE);
        }

        cache.insert(findex, std::move(s));

        return read(findex);
    }

    template<typename F>
    void each(string const & fname, F && f) {
        auto ent = locate(fname); if(ent.is_dir()) {
            auto findex = ent.index;

            auto is_root = (findex == -1); if(is_root) {
                ++findex;
            }

            stat_t st; scan: while(findex < this->size) {
                st = stat(findex++); string & fpath = st.fpath;

                size_t offset = fname.size(); if(!is_root) {
                    if(!fpath.starts_with(fname)) return;
                    if(fpath[offset] != '/') return;

                    ++offset;
                }

                if(auto pos = fpath.find('/', offset); pos != string::npos) {
                    // dir found
                    string_view dname {fpath.data() + offset, pos - offset};

                    {
                        stat_t st2; {
                            st2.fpath = dname; st2.size = 0; st2.mtime = 0; st2.type = DIR;
                        }

                        f(st2);
                    }

                    // skip this dir
                    dname = {fpath.data(), offset + dname.size() + 1};

                    while(findex < this->size) {
                        auto st2 = stat(findex); {
                            if(st2.fpath.starts_with(dname)) {
                                ++findex; continue;
                            }
                        }

                        goto scan;
                    }

                    return;
                }

                st.fpath = st.fpath.substr(offset); f(st);
            }
        }
    }
} $archive;

// fs callbacks
static NTSTATUS DOKAN_CALLBACK zmCreateFile(LPCWSTR FileName, PDOKAN_IO_SECURITY_CONTEXT SecurityContext, ACCESS_MASK DesiredAccess, ULONG FileAttributes, ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions, PDOKAN_FILE_INFO DokanFileInfo) {
    DWORD creationDisposition, fileAttributesAndFlags; ACCESS_MASK genericDesiredAccess; {
        DokanMapKernelToUserCreateFileFlags(
            DesiredAccess, FileAttributes, CreateOptions, CreateDisposition,
            &genericDesiredAccess, &fileAttributesAndFlags, &creationDisposition);
    }

    USES_CONVERSION;

    string fpath = $archive.canonicalize(FileName);

    auto [ftype, findex] = $archive.locate(fpath);

    auto fname = fpath; if(!ftype) {
        if((creationDisposition == CREATE_NEW) || (creationDisposition == OPEN_ALWAYS)) {
            return DokanNtStatusFromWin32(ERROR_ACCESS_DENIED);
        }

        return DokanNtStatusFromWin32(ERROR_FILE_NOT_FOUND);
    }

    if(creationDisposition == CREATE_NEW) {
        return DokanNtStatusFromWin32(ERROR_FILE_EXISTS);
    }

    DokanFileInfo->Context = findex;

    bool is_dir = (ftype == 2); if(is_dir) {
        DokanFileInfo->IsDirectory = TRUE;

        if(creationDisposition == OPEN_ALWAYS) return STATUS_OBJECT_NAME_COLLISION;

        return STATUS_SUCCESS;
    }

    DokanFileInfo->IsDirectory = FALSE;

    return STATUS_SUCCESS;
}

static void DOKAN_CALLBACK zmCloseFile(LPCWSTR FileName, PDOKAN_FILE_INFO DokanFileInfo) {}

static NTSTATUS DOKAN_CALLBACK zmReadFile(LPCWSTR FileName, LPVOID Buffer, DWORD BufferLength, LPDWORD ReadLength, LONGLONG Offset, PDOKAN_FILE_INFO DokanFileInfo) {
    int findex = DokanFileInfo->Context;

    auto s = $archive.read(findex);

    auto size = s->size();
    auto toread = std::min(size - Offset, (size_t)BufferLength);

    memcpy(Buffer, s->data() + Offset, toread);

    *ReadLength = toread; return STATUS_SUCCESS;
}

FILETIME time64_to_filetime(__time64_t t) {
    ULARGE_INTEGER time_value; FILETIME ft;
    // FILETIME represents time in 100-nanosecond intervals since January 1, 1601 (UTC).
    // _time64_t represents seconds since January 1, 1970 (UTC).
    // The difference in seconds is 11644473600.
    // Multiply by 10,000,000 to convert seconds to 100-nanosecond intervals.
    time_value.QuadPart = (t * 10000000LL) + 116444736000000000LL;

    ft.dwLowDateTime = time_value.LowPart; ft.dwHighDateTime = time_value.HighPart;

    return ft;
}

static NTSTATUS DOKAN_CALLBACK zmGetFileInformation(LPCWSTR FileName, LPBY_HANDLE_FILE_INFORMATION HandleFileInformation, PDOKAN_FILE_INFO DokanFileInfo) {
    if(DokanFileInfo->IsDirectory) {
        HandleFileInformation->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY; return STATUS_SUCCESS;
    }

    int findex = DokanFileInfo->Context;

    auto stat = $archive.stat(findex); {
        FILETIME mtime = time64_to_filetime(stat.mtime);

        HandleFileInformation->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        HandleFileInformation->nFileSizeLow = stat.size;
        HandleFileInformation->nFileSizeHigh = stat.size >> 32;
        HandleFileInformation->ftCreationTime = mtime;
        HandleFileInformation->ftLastWriteTime = mtime;
        HandleFileInformation->ftLastAccessTime = mtime;
        HandleFileInformation->nNumberOfLinks = 0;
        HandleFileInformation->nFileIndexHigh = 0;
        HandleFileInformation->nFileIndexLow = 0;
        HandleFileInformation->dwVolumeSerialNumber = 0;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK zmFindFiles(LPCWSTR FileName, PFillFindData FillFindData, PDOKAN_FILE_INFO DokanFileInfo) {
    USES_CONVERSION;

    auto dname = $archive.canonicalize(FileName);

    $archive.each(dname, [&](auto const & stat) {
        WIN32_FIND_DATAW find_data {0}; if(stat.is_dir()) {
            find_data.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
        }
        else {
            auto fpath = W2A((path(dname) / stat.fpath).generic_wstring().c_str());
            auto ftime = time64_to_filetime(stat.mtime);

            find_data.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
            find_data.nFileSizeLow = stat.size;
            find_data.nFileSizeHigh = stat.size >> 32;
            find_data.ftCreationTime = ftime;
            find_data.ftLastWriteTime = ftime;
            find_data.ftLastAccessTime = ftime;
        }

        wcscpy(find_data.cFileName, A2W(stat.fpath.c_str()));

        FillFindData(&find_data, DokanFileInfo);
    });

    return STATUS_SUCCESS;
}

struct zipmount_options {
    string archive_fname; optional<string> mount_point {"m:\\"};
};

STRUCTOPT(zipmount_options, archive_fname, mount_point);

static wstring mount_point;

int main(int argc, char ** argv) {
    USES_CONVERSION; try {
        // Line of code that does all the work:
        auto options = structopt::app(APP_NAME, APP_VERSION).parse<zipmount_options>(argc, argv);

        ok(format("check {}", options.archive_fname)) =
            fs::exists(options.archive_fname);

        ok(format("open  {}", options.archive_fname)) =
            $archive.open(options.archive_fname);

#if 0
        {
            string fname = "/";

            auto ent = $archive.locate(fname); if(ent.is_dir()) {
                auto findex = ent.index; auto is_root = (findex == -1);

                while(++findex < $archive.size) {
                    auto st = $archive.stat(findex); print("{}\n", st.fpath); continue;
                }
            }

            exit(0);
        }
#endif

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
            dokanOptions.Timeout = 3000 * 1000;
            dokanOptions.MountPoint = mount_point.c_str();
            dokanOptions.Options =
                DOKAN_OPTION_REMOVABLE | DOKAN_OPTION_WRITE_PROTECT | DOKAN_OPTION_MOUNT_MANAGER;
            // dokanOptions.UNCName = unc_name.c_str();
            // dokanOptions.Options = DOKAN_OPTION_NETWORK | DOKAN_OPTION_ENABLE_UNMOUNT_NETWORK_DRIVE;
        }

        DOKAN_OPERATIONS dokanOperations {0}; {
            dokanOperations.ZwCreateFile = zmCreateFile;
            dokanOperations.CloseFile = zmCloseFile;
            dokanOperations.ReadFile = zmReadFile;
            dokanOperations.GetFileInformation = zmGetFileInformation;
            dokanOperations.FindFiles = zmFindFiles;
        }

        DokanInit(); ok("ready, (CTRL + C) to quit");

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