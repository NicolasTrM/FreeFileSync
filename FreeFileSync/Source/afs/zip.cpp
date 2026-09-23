// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#include "zip.h"
#include <cerrno>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <zlib.h>
#include <zen/globals.h>
#include <zen/resolve_path.h>
#include <zen/scope_guard.h>
#include <zen/sys_error.h>
#include <zen/time.h>
#include <zen/utf.h>
#include "abstract_impl.h"

#include <fcntl.h>    //open
#include <unistd.h>   //pread, close
#include <sys/stat.h> //fstat, stat

using namespace zen;
using namespace fff;
using AFS = AbstractFileSystem;


namespace
{
constexpr ZstringView zipPrefix = Zstr("zip:");
constexpr Zchar zipInnerPathSeparator = Zstr('|'); //zip:/path/archive.zip|inner/folder

constexpr size_t ZIP_STREAM_BLOCK_SIZE = 128 * 1024;

constexpr uint32_t SIG_LOCAL_HEADER     = 0x04034b50;
constexpr uint32_t SIG_CENTRAL_HEADER   = 0x02014b50;
constexpr uint32_t SIG_EOCD             = 0x06054b50;
constexpr uint32_t SIG_ZIP64_EOCD       = 0x06064b50;
constexpr uint32_t SIG_ZIP64_EOCD_LOCATOR = 0x07064b50;

constexpr uint16_t ZIP_METHOD_STORED  = 0;
constexpr uint16_t ZIP_METHOD_DEFLATE = 8;

constexpr uint16_t ZIP_FLAG_ENCRYPTED = 1 << 0;
constexpr uint16_t ZIP_FLAG_UTF8      = 1 << 11;


uint16_t readLE16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t readLE32(const unsigned char* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint64_t readLE64(const unsigned char* p) { return static_cast<uint64_t>(readLE32(p)) | (static_cast<uint64_t>(readLE32(p + 4)) << 32); }

//-----------------------------------------------------------------------------------------------------------

class ArchiveFile //thread-safe: pread() only
{
public:
    explicit ArchiveFile(const Zstring& archivePath) //throw SysError
    {
        fd_ = ::open(archivePath.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd_ == -1)
            THROW_LAST_SYS_ERROR("open");
    }

    ~ArchiveFile() { ::close(fd_); }

    ArchiveFile           (const ArchiveFile&) = delete;
    ArchiveFile& operator=(const ArchiveFile&) = delete;

    struct Details
    {
        uint64_t fileSize;
        time_t modTime;
    };
    Details getDetails() const //throw SysError
    {
        struct stat fileInfo = {};
        if (::fstat(fd_, &fileInfo) != 0)
            THROW_LAST_SYS_ERROR("fstat");
        return {static_cast<uint64_t>(fileInfo.st_size), fileInfo.st_mtime};
    }

    //read exactly "len" bytes, fail otherwise
    void readAt(uint64_t offset, void* buffer, size_t len) const //throw SysError
    {
        auto it = static_cast<char*>(buffer);
        while (len > 0)
        {
            const ssize_t bytesRead = ::pread(fd_, it, len, static_cast<off_t>(offset));
            if (bytesRead < 0)
            {
                if (errno == EINTR)
                    continue;
                THROW_LAST_SYS_ERROR("pread");
            }
            if (bytesRead == 0)
                throw SysError(_("Unexpected end of file.") + L" [ZIP]");

            it     += bytesRead;
            offset += bytesRead;
            len    -= bytesRead;
        }
    }

    std::string readAt(uint64_t offset, size_t len) const //throw SysError
    {
        std::string buf(len, '\0');
        readAt(offset, buf.data(), len); //throw SysError
        return buf;
    }

private:
    int fd_ = -1;
};

//-----------------------------------------------------------------------------------------------------------

struct ZipFileEntry
{
    uint64_t localHeaderOffset = 0;
    uint64_t compressedSize    = 0;
    uint64_t uncompressedSize  = 0;
    uint32_t crc32  = 0;
    uint16_t method = 0;
    uint16_t flags  = 0;
    time_t   modTime = 0;
};

struct ZipFolderContent
{
    std::map<Zstring, ZipFileEntry> files;   //item name => entry
    std::set<Zstring>               folders; //item names
};

struct ZipIndex
{
    std::map<Zstring, ZipFolderContent> folders; //relative folder path ("" == archive root) => content
    uint64_t archiveSize = 0;
    time_t   archiveModTime = 0;
};


time_t dosTimeToTimeT(uint16_t dosTime, uint16_t dosDate) //DOS time stamps are local time, 2 sec precision
{
    TimeComp tc;
    tc.year   = ((dosDate >> 9) & 0x7f) + 1980;
    tc.month  =  (dosDate >> 5) & 0x0f;
    tc.day    =   dosDate       & 0x1f;
    tc.hour   =   dosTime >> 11;
    tc.minute =  (dosTime >> 5) & 0x3f;
    tc.second =  (dosTime       & 0x1f) * 2;

    if (tc.month < 1 || tc.month > 12 || tc.day < 1)
        return 0;

    const auto [modTime, success] = localToTimeT(tc);
    return success ? modTime : 0;
}


Zstring decodeCp437(const std::string& str) //ZIP default encoding if flag bit 11 is not set
{
    static constexpr wchar_t cp437Upper[128] =
    {
        0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
        0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
        0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
        0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
        0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
        0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
        0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
        0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
    };
    std::wstring wideStr;
    for (const char c : str)
    {
        const auto b = static_cast<unsigned char>(c);
        wideStr += b < 0x80 ? static_cast<wchar_t>(b) : cp437Upper[b - 0x80];
    }
    return utfTo<Zstring>(wideStr);
}


//returns empty on failure/unsafe path (e.g. containing "..")
Zstring sanitizeArchiveItemPath(const Zstring& rawPath)
{
    Zstring relPath;
    bool unsafe = false;

    split(replaceCpy(rawPath, Zstr('\\'), Zstr('/')), Zstr('/'), [&](ZstringView part)
    {
        if (part.empty() || part == Zstr("."))
            return;
        if (part == Zstr(".."))
        {
            unsafe = true;
            return;
        }
        if (!relPath.empty())
            relPath += FILE_NAME_SEPARATOR;
        relPath += part;
    });
    return unsafe ? Zstring() : relPath;
}


void addFolderRecursively(ZipIndex& index, const Zstring& folderRelPath)
{
    for (Zstring relPath = folderRelPath; !relPath.empty();)
    {
        const Zstring parentPath = beforeLast(relPath, FILE_NAME_SEPARATOR, IfNotFoundReturn::none);
        const Zstring itemName   =  afterLast(relPath, FILE_NAME_SEPARATOR, IfNotFoundReturn::all);

        index.folders[relPath]; //create if missing
        if (!index.folders[parentPath].folders.insert(itemName).second)
            break; //parents are already registered
        relPath = parentPath;
    }
    index.folders[Zstring()]; //root is always existing
}


ZipIndex parseZipArchive(const Zstring& archivePath) //throw SysError
{
    const ArchiveFile archive(archivePath); //throw SysError
    const ArchiveFile::Details details = archive.getDetails(); //throw SysError

    ZipIndex index;
    index.archiveSize    = details.fileSize;
    index.archiveModTime = details.modTime;
    index.folders[Zstring()]; //root

    if (details.fileSize < 22)
        throw SysError(_("File is not a ZIP archive.") + L" (EOCD)");

    //---------- locate "end of central directory" record ----------
    const size_t tailSize = static_cast<size_t>(std::min<uint64_t>(details.fileSize, 22 + 0xffff));
    const uint64_t tailOffset = details.fileSize - tailSize;
    const std::string tail = archive.readAt(tailOffset, tailSize); //throw SysError
    const auto tailBytes = reinterpret_cast<const unsigned char*>(tail.data());

    std::optional<size_t> eocdPosTail;
    for (size_t i = tailSize - 22 + 1; i-- > 0;)
        if (readLE32(tailBytes + i) == SIG_EOCD)
        {
            eocdPosTail = i;
            break;
        }
    if (!eocdPosTail)
        throw SysError(_("File is not a ZIP archive.") + L" (EOCD)");

    const unsigned char* eocd = tailBytes + *eocdPosTail;
    const uint64_t eocdOffset = tailOffset + *eocdPosTail;

    if (readLE16(eocd + 4) != 0 || readLE16(eocd + 6) != 0)
        throw SysError(_("Multi-volume ZIP archives are not supported."));

    uint64_t entryCount = readLE16(eocd + 10);
    uint64_t cdSize     = readLE32(eocd + 12);
    uint64_t cdOffset   = readLE32(eocd + 16);
    uint64_t prefixSize = 0; //e.g. self-extracting archive

    //---------- Zip64? ----------
    bool isZip64 = false;
    if (eocdOffset >= 20)
    {
        unsigned char locator[20] = {};
        archive.readAt(eocdOffset - 20, locator, sizeof(locator)); //throw SysError
        if (readLE32(locator) == SIG_ZIP64_EOCD_LOCATOR)
        {
            const uint64_t zip64EocdOffset = readLE64(locator + 8);
            unsigned char zip64Eocd[56] = {};
            if (zip64EocdOffset + sizeof(zip64Eocd) > details.fileSize)
                throw SysError(_("ZIP archive is corrupted.") + L" (Zip64 EOCD)");
            archive.readAt(zip64EocdOffset, zip64Eocd, sizeof(zip64Eocd)); //throw SysError

            if (readLE32(zip64Eocd) != SIG_ZIP64_EOCD)
                throw SysError(_("ZIP archive is corrupted.") + L" (Zip64 EOCD)");

            entryCount = readLE64(zip64Eocd + 32);
            cdSize     = readLE64(zip64Eocd + 40);
            cdOffset   = readLE64(zip64Eocd + 48);
            isZip64 = true;
        }
    }

    if (!isZip64 && eocdOffset > cdOffset + cdSize)
        prefixSize = eocdOffset - (cdOffset + cdSize);

    if (cdOffset + prefixSize + cdSize > details.fileSize)
        throw SysError(_("ZIP archive is corrupted.") + L" (central directory)");

    //---------- parse central directory ----------
    const std::string cd = archive.readAt(cdOffset + prefixSize, static_cast<size_t>(cdSize)); //throw SysError
    const auto cdBytes = reinterpret_cast<const unsigned char*>(cd.data());

    size_t pos = 0;
    for (uint64_t i = 0; i < entryCount; ++i)
    {
        if (pos + 46 > cd.size() || readLE32(cdBytes + pos) != SIG_CENTRAL_HEADER)
            throw SysError(_("ZIP archive is corrupted.") + L" (central directory entry " + numberTo<std::wstring>(i) + L')');

        const unsigned char* hdr = cdBytes + pos;
        const uint16_t versionMadeBy = readLE16(hdr + 4);
        ZipFileEntry entry;
        entry.flags  = readLE16(hdr + 8);
        entry.method = readLE16(hdr + 10);
        const uint16_t dosTime = readLE16(hdr + 12);
        const uint16_t dosDate = readLE16(hdr + 14);
        entry.crc32            = readLE32(hdr + 16);
        entry.compressedSize   = readLE32(hdr + 20);
        entry.uncompressedSize = readLE32(hdr + 24);
        const uint16_t nameLen    = readLE16(hdr + 28);
        const uint16_t extraLen   = readLE16(hdr + 30);
        const uint16_t commentLen = readLE16(hdr + 32);
        const uint32_t externalAttr = readLE32(hdr + 38);
        entry.localHeaderOffset = readLE32(hdr + 42);

        if (pos + 46 + nameLen + extraLen + commentLen > cd.size())
            throw SysError(_("ZIP archive is corrupted.") + L" (central directory entry " + numberTo<std::wstring>(i) + L')');

        const std::string rawName(reinterpret_cast<const char*>(hdr + 46), nameLen);
        const unsigned char* extra = hdr + 46 + nameLen;
        pos += 46 + nameLen + extraLen + commentLen;

        entry.modTime = dosTimeToTimeT(dosTime, dosDate);
        std::optional<Zstring> unicodeName;

        //---------- extra fields ----------
        for (size_t ep = 0; ep + 4 <= extraLen;)
        {
            const uint16_t tag  = readLE16(extra + ep);
            const uint16_t size = readLE16(extra + ep + 2);
            const unsigned char* data = extra + ep + 4;
            if (ep + 4 + size > extraLen)
                break;

            if (tag == 0x0001) //Zip64 extended information: only fields set to 0xffffffff in main header are present
            {
                size_t dp = 0;
                auto readZip64 = [&](uint64_t& val)
                {
                    if (val == 0xffffffff && dp + 8 <= size)
                    {
                        val = readLE64(data + dp);
                        dp += 8;
                    }
                };
                readZip64(entry.uncompressedSize);
                readZip64(entry.compressedSize);
                readZip64(entry.localHeaderOffset);
            }
            else if (tag == 0x5455) //extended timestamp (UTC)
            {
                if (size >= 5 && (data[0] & 1))
                    entry.modTime = static_cast<int32_t>(readLE32(data + 1));
            }
            else if (tag == 0x000a) //NTFS timestamps (UTC, FILETIME)
            {
                for (size_t tp = 4; tp + 4 <= size;)
                {
                    const uint16_t attrTag  = readLE16(data + tp);
                    const uint16_t attrSize = readLE16(data + tp + 2);
                    if (attrTag == 1 && attrSize >= 8 && tp + 4 + 8 <= size)
                    {
                        const uint64_t fileTime = readLE64(data + tp + 4);
                        if (fileTime > 116444736000000000ULL)
                            entry.modTime = static_cast<time_t>((fileTime - 116444736000000000ULL) / 10000000);
                        break;
                    }
                    tp += 4 + attrSize;
                }
            }
            else if (tag == 0x7075) //Info-ZIP Unicode Path: version (1) + CRC32 of header name (4) + UTF-8 name
            {
                if (size >= 5 && data[0] == 1 &&
                    readLE32(data + 1) == ::crc32(0, reinterpret_cast<const Bytef*>(rawName.data()), static_cast<uInt>(rawName.size())))
                    unicodeName = Zstring(reinterpret_cast<const char*>(data + 5), size - 5);
            }
            ep += 4 + size;
        }

        //---------- item name ----------
        Zstring itemName;
        if (unicodeName)
            itemName = *unicodeName;
        else if ((entry.flags & ZIP_FLAG_UTF8) || isValidUtf(rawName))
            itemName = rawName;
        else
            itemName = decodeCp437(rawName);

        const bool isFolder = endsWith(itemName, Zstr('/')) || endsWith(itemName, Zstr('\\')) ||
                              ((versionMadeBy >> 8) == 0 /*MS-DOS*/ && (externalAttr & 0x10 /*FILE_ATTRIBUTE_DIRECTORY*/));

        const bool isSymlink = (versionMadeBy >> 8) == 3 /*Unix*/ && ((externalAttr >> 16) & S_IFMT) == S_IFLNK;
        if (isSymlink)
            continue; //not supported: don't extract link target as file content!

        const Zstring relPath = sanitizeArchiveItemPath(itemName);
        if (relPath.empty())
            continue; //root or unsafe path => skip

        if (isFolder)
            addFolderRecursively(index, relPath);
        else
        {
            const Zstring parentPath = beforeLast(relPath, FILE_NAME_SEPARATOR, IfNotFoundReturn::none);
            const Zstring fileName   =  afterLast(relPath, FILE_NAME_SEPARATOR, IfNotFoundReturn::all);
            addFolderRecursively(index, parentPath);

            entry.localHeaderOffset += prefixSize;
            index.folders[parentPath].files[fileName] = entry; //duplicate entries: last one wins (same as unzip)
        }
    }
    return index;
}

//-----------------------------------------------------------------------------------------------------------

class ZipIndexBuffer //cache parsed central directory: comparison and synchronization access the same archive many times
{
public:
    std::shared_ptr<const ZipIndex> getIndex(const Zstring& archivePath) //throw SysError
    {
        std::lock_guard dummy(lockIndex_); //parse each archive once, even for concurrent access

        struct stat fileInfo = {};
        if (::stat(archivePath.c_str(), &fileInfo) != 0)
            THROW_LAST_SYS_ERROR("stat");

        if (auto it = indexes_.find(archivePath);
            it != indexes_.end() &&
            it->second->archiveSize    == static_cast<uint64_t>(fileInfo.st_size) &&
            it->second->archiveModTime == fileInfo.st_mtime)
            return it->second;

        auto index = std::make_shared<const ZipIndex>(parseZipArchive(archivePath)); //throw SysError
        indexes_[archivePath] = index;
        return index;
    }

private:
    std::mutex lockIndex_;
    std::map<Zstring, std::shared_ptr<const ZipIndex>> indexes_;
};

constinit Global<ZipIndexBuffer> globalZipIndexBuffer;


std::shared_ptr<const ZipIndex> getZipIndex(const Zstring& archivePath) //throw SysError
{
    const std::shared_ptr<ZipIndexBuffer> buf = globalZipIndexBuffer.get();
    if (!buf)
        throw SysError(formatSystemError("getZipIndex", L"", L"Function call not allowed during init/shutdown."));

    return buf->getIndex(archivePath); //throw SysError
}

//-----------------------------------------------------------------------------------------------------------

class InputStreamZip : public AFS::InputStream
{
public:
    InputStreamZip(const Zstring& archivePath, const ZipFileEntry& entry, const std::wstring& displayPath) : //throw SysError
        archive_(archivePath), //throw SysError
        entry_(entry),
        displayPath_(displayPath)
    {
        if (entry.flags & ZIP_FLAG_ENCRYPTED)
            throw SysError(_("Encrypted ZIP entries are not supported."));

        if (entry.method != ZIP_METHOD_STORED &&
            entry.method != ZIP_METHOD_DEFLATE)
            throw SysError(replaceCpy<std::wstring>(L"Unsupported ZIP compression method: %x", L"%x", numberTo<std::wstring>(entry.method)));

        unsigned char localHeader[30] = {};
        archive_.readAt(entry.localHeaderOffset, localHeader, sizeof(localHeader)); //throw SysError
        if (readLE32(localHeader) != SIG_LOCAL_HEADER)
            throw SysError(_("ZIP archive is corrupted.") + L" (local file header)");

        dataOffset_ = entry.localHeaderOffset + sizeof(localHeader) + readLE16(localHeader + 26) + readLE16(localHeader + 28);

        if (entry.method == ZIP_METHOD_DEFLATE)
        {
            if (::inflateInit2(&zs_, -MAX_WBITS /*raw deflate*/) != Z_OK)
                throw SysError(formatSystemError("inflateInit2", L"", L"zlib initialization failed."));
            zsInitialized_ = true;
            inBuf_.resize(ZIP_STREAM_BLOCK_SIZE);
        }
    }

    ~InputStreamZip()
    {
        if (zsInitialized_)
            ::inflateEnd(&zs_);
    }

    size_t getBlockSize() override { return ZIP_STREAM_BLOCK_SIZE; } //throw FileError

    //may return short; only 0 means EOF! CONTRACT: bytesToRead > 0!
    size_t tryRead(void* buffer, size_t bytesToRead, const IoCallback& notifyUnbufferedIO /*throw X*/) override //throw FileError, X
    {
        try
        {
            const size_t bytesRead = entry_.method == ZIP_METHOD_STORED ?
                                     readStored  (buffer, bytesToRead) : //throw SysError
                                     readInflated(buffer, bytesToRead);  //

            if (bytesRead == 0)
                verifyChecksum(); //throw SysError
            else
            {
                crc_ = ::crc32(crc_, static_cast<const Bytef*>(buffer), static_cast<uInt>(bytesRead));
                totalOut_ += bytesRead;
            }

            if (notifyUnbufferedIO) notifyUnbufferedIO(bytesRead); //throw X
            return bytesRead;
        }
        catch (const SysError& e) { throw FileError(replaceCpy(_("Cannot read file %x."), L"%x", fmtPath(displayPath_)), e.toString()); }
    }

    std::optional<AFS::StreamAttributes> tryGetAttributesFast() override //throw FileError
    {
        return AFS::StreamAttributes{entry_.modTime, entry_.uncompressedSize, 0 /*filePrint*/};
    }

private:
    size_t readStored(void* buffer, size_t bytesToRead) //throw SysError
    {
        const size_t len = static_cast<size_t>(std::min<uint64_t>(bytesToRead, entry_.compressedSize - compIn_));
        if (len > 0)
        {
            archive_.readAt(dataOffset_ + compIn_, buffer, len); //throw SysError
            compIn_ += len;
        }
        return len;
    }

    size_t readInflated(void* buffer, size_t bytesToRead) //throw SysError
    {
        if (streamEnd_)
            return 0;

        zs_.next_out  = static_cast<Bytef*>(buffer);
        zs_.avail_out = static_cast<uInt>(std::min<size_t>(bytesToRead, std::numeric_limits<uInt>::max()));
        const uInt availOutStart = zs_.avail_out;

        for (;;)
        {
            if (zs_.avail_in == 0 && compIn_ < entry_.compressedSize)
            {
                const size_t len = static_cast<size_t>(std::min<uint64_t>(inBuf_.size(), entry_.compressedSize - compIn_));
                archive_.readAt(dataOffset_ + compIn_, inBuf_.data(), len); //throw SysError
                compIn_ += len;
                zs_.next_in  = reinterpret_cast<Bytef*>(inBuf_.data());
                zs_.avail_in = static_cast<uInt>(len);
            }

            const int rv = ::inflate(&zs_, Z_NO_FLUSH);
            const size_t produced = availOutStart - zs_.avail_out;

            if (rv == Z_STREAM_END)
            {
                streamEnd_ = true;
                return produced;
            }
            if (rv != Z_OK && rv != Z_BUF_ERROR)
                throw SysError(formatSystemError("inflate", L"", utfTo<std::wstring>(std::string(zs_.msg ? zs_.msg : ""))));

            if (produced > 0)
                return produced;

            if (zs_.avail_in == 0 && compIn_ >= entry_.compressedSize)
                throw SysError(_("Unexpected end of file.") + L" [deflate]");
        }
    }

    void verifyChecksum() const //throw SysError
    {
        if (totalOut_ != entry_.uncompressedSize)
            throw SysError(_("Unexpected size of data stream:") + L' ' + formatNumber(totalOut_) + L'\n' +
                           _("Expected:") + L' ' + formatNumber(entry_.uncompressedSize));
        if (crc_ != entry_.crc32)
            throw SysError(L"CRC32 mismatch: ZIP archive is corrupted.");
    }

    const ArchiveFile archive_;
    const ZipFileEntry entry_;
    const std::wstring displayPath_;
    uint64_t dataOffset_ = 0;

    uint64_t compIn_   = 0; //compressed bytes consumed
    uint64_t totalOut_ = 0; //uncompressed bytes delivered
    uLong crc_ = ::crc32(0, nullptr, 0);

    z_stream zs_ = {};
    bool zsInitialized_ = false;
    bool streamEnd_ = false;
    std::vector<char> inBuf_;
};

//===========================================================================================================================

class ZipFileSystem : public AbstractFileSystem
{
public:
    explicit ZipFileSystem(const Zstring& archivePath) : archivePath_(archivePath) {}

private:
    Zstring getInitPathPhrase(const AfsPath& itemPath) const override
    {
        Zstring phrase = Zstring(zipPrefix) + archivePath_;
        if (!itemPath.value.empty())
            phrase += zipInnerPathSeparator + itemPath.value;
        return phrase;
    }

    std::vector<Zstring> getPathPhraseAliases(const AfsPath& itemPath) const override { return {getInitPathPhrase(itemPath)}; }

    std::wstring getDisplayPath(const AfsPath& itemPath) const override { return utfTo<std::wstring>(appendPath(archivePath_, itemPath.value)); }

    bool isNullFileSystem() const override { return archivePath_.empty(); }

    std::weak_ordering compareDeviceSameAfsType(const AbstractFileSystem& afsRhs) const override
    {
        return compareNativePath(archivePath_, static_cast<const ZipFileSystem&>(afsRhs).archivePath_);
    }

    //----------------------------------------------------------------------------------------------------------------
    std::optional<ItemType> getItemTypeIfExistsImpl(const AfsPath& itemPath) const //throw SysError
    {
        if (itemPath.value.empty()) //archive root
        {
            struct stat fileInfo = {};
            if (::stat(archivePath_.c_str(), &fileInfo) != 0)
            {
                if (errno == ENOENT)
                    return std::nullopt;
                THROW_LAST_SYS_ERROR("stat");
            }
            getZipIndex(archivePath_); //throw SysError => access test
            return ItemType::folder;
        }

        const std::shared_ptr<const ZipIndex> index = getZipIndex(archivePath_); //throw SysError

        const Zstring parentPath = beforeLast(itemPath.value, FILE_NAME_SEPARATOR, IfNotFoundReturn::none);
        const Zstring itemName   = getItemName(itemPath);

        auto it = index->folders.find(parentPath);
        if (it == index->folders.end())
            return std::nullopt;

        if (it->second.folders.contains(itemName))
            return ItemType::folder;
        if (it->second.files.contains(itemName))
            return ItemType::file;
        return std::nullopt;
    }

    std::optional<ItemType> getItemTypeIfExists(const AfsPath& itemPath) const override //throw FileError
    {
        try { return getItemTypeIfExistsImpl(itemPath); } //throw SysError
        catch (const SysError& e) { throw FileError(replaceCpy(_("Cannot read file attributes of %x."), L"%x", fmtPath(getDisplayPath(itemPath))), e.toString()); }
    }

    ItemType getItemType(const AfsPath& itemPath) const override //throw FileError
    {
        if (const std::optional<ItemType> type = getItemTypeIfExists(itemPath)) //throw FileError
            return *type;

        throw FileError(replaceCpy(_("Cannot read file attributes of %x."), L"%x", fmtPath(getDisplayPath(itemPath))),
                        replaceCpy(_("%x does not exist."), L"%x", fmtPath(itemPath.value.empty() ? archivePath_ : getItemName(itemPath))));
    }

    //----------------------------------------------------------------------------------------------------------------
    [[noreturn]] void throwReadOnly(const std::wstring& msg) const //throw FileError
    {
        throw FileError(msg, _("Operation not supported by device.") + L" (ZIP archives are read-only)");
    }

    void createFolderPlain(const AfsPath& folderPath) const override //throw FileError
    { throwReadOnly(replaceCpy(_("Cannot create directory %x."), L"%x", fmtPath(getDisplayPath(folderPath)))); }

    void removeFilePlain(const AfsPath& filePath) const override //throw FileError
    { throwReadOnly(replaceCpy(_("Cannot delete file %x."), L"%x", fmtPath(getDisplayPath(filePath)))); }

    void removeSymlinkPlain(const AfsPath& linkPath) const override //throw FileError
    { throwReadOnly(replaceCpy(_("Cannot delete symbolic link %x."), L"%x", fmtPath(getDisplayPath(linkPath)))); }

    void removeFolderPlain(const AfsPath& folderPath) const override //throw FileError
    { throwReadOnly(replaceCpy(_("Cannot delete directory %x."), L"%x", fmtPath(getDisplayPath(folderPath)))); }

    void removeFolderIfExistsRecursion(const AfsPath& folderPath, //throw FileError
                                       const std::function<void(const std::wstring& displayPath)>& onBeforeFileDeletion   /*throw X*/,
                                       const std::function<void(const std::wstring& displayPath)>& onBeforeSymlinkDeletion/*throw X*/,
                                       const std::function<void(const std::wstring& displayPath)>& onBeforeFolderDeletion /*throw X*/) const override
    { throwReadOnly(replaceCpy(_("Cannot delete directory %x."), L"%x", fmtPath(getDisplayPath(folderPath)))); }

    //----------------------------------------------------------------------------------------------------------------
    AbstractPath getSymlinkResolvedPath(const AfsPath& linkPath) const override //throw FileError
    {
        throw FileError(replaceCpy(_("Cannot determine final path for %x."), L"%x", fmtPath(getDisplayPath(linkPath))), _("Operation not supported by device."));
    }

    bool equalSymlinkContentForSameAfsType(const AfsPath& linkPathL, const AbstractPath& linkPathR) const override //throw FileError
    {
        throw FileError(replaceCpy(_("Cannot resolve symbolic link %x."), L"%x", fmtPath(getDisplayPath(linkPathL))), _("Operation not supported by device."));
    }

    //----------------------------------------------------------------------------------------------------------------
    //return value always bound:
    std::unique_ptr<InputStream> getInputStream(const AfsPath& filePath) const override //throw FileError, (ErrorFileLocked)
    {
        try
        {
            const std::shared_ptr<const ZipIndex> index = getZipIndex(archivePath_); //throw SysError

            const Zstring parentPath = beforeLast(filePath.value, FILE_NAME_SEPARATOR, IfNotFoundReturn::none);
            if (auto itFolder = index->folders.find(parentPath);
                itFolder != index->folders.end())
                if (auto itFile = itFolder->second.files.find(getItemName(filePath));
                    itFile != itFolder->second.files.end())
                    return std::make_unique<InputStreamZip>(archivePath_, itFile->second, getDisplayPath(filePath)); //throw SysError

            throw SysError(replaceCpy(_("%x does not exist."), L"%x", fmtPath(getItemName(filePath))));
        }
        catch (const SysError& e) { throw FileError(replaceCpy(_("Cannot read file %x."), L"%x", fmtPath(getDisplayPath(filePath))), e.toString()); }
    }

    std::unique_ptr<OutputStreamImpl> getOutputStream(const AfsPath& filePath, //throw FileError
                                                      std::optional<uint64_t> streamSize,
                                                      std::optional<time_t> modTime) const override
    { throwReadOnly(replaceCpy(_("Cannot write file %x."), L"%x", fmtPath(getDisplayPath(filePath)))); }

    //----------------------------------------------------------------------------------------------------------------
    static void traverseZipFolder(const ZipIndex& index, const Zstring& folderRelPath, AFS::TraverserCallback& cb) //throw X
    {
        auto it = index.folders.find(folderRelPath);
        if (it == index.folders.end())
            return;

        for (const auto& [fileName, entry] : it->second.files)
            cb.onFile({fileName, entry.uncompressedSize, entry.modTime, 0 /*filePrint*/, false /*isFollowedSymlink*/}); //throw X

        for (const Zstring& folderName : it->second.folders)
            if (std::shared_ptr<AFS::TraverserCallback> cbSub = cb.onFolder({folderName, false /*isFollowedSymlink*/})) //throw X
                traverseZipFolder(index, appendPath(folderRelPath, folderName), *cbSub); //throw X
    }

    void traverseFolderRecursive(const TraverserWorkload& workload /*throw X*/, size_t parallelOps) const override
    {
        for (const auto& [folderPath, cb] : workload)
        {
            std::shared_ptr<const ZipIndex> index;

            tryReportingDirError([&] //throw X
            {
                try
                {
                    index = getZipIndex(archivePath_); //throw SysError
                    if (!index->folders.contains(folderPath.value))
                        throw SysError(replaceCpy(_("%x does not exist."), L"%x", fmtPath(folderPath.value)));
                }
                catch (const SysError& e) { throw FileError(replaceCpy(_("Cannot open directory %x."), L"%x", fmtPath(getDisplayPath(folderPath))), e.toString()); }
            }, *cb);

            if (index && index->folders.contains(folderPath.value))
                traverseZipFolder(*index, folderPath.value, *cb); //throw X
        }
    }

    //----------------------------------------------------------------------------------------------------------------
    FileCopyResult copyFileForSameAfsType(const AfsPath& sourcePath, const StreamAttributes& sourceAttr, //throw FileError, (ErrorFileLocked), X
                                          const AbstractPath& targetPath, bool copyFilePermissions, const IoCallback& notifyUnbufferedIO /*throw X*/) const override
    {
        if (copyFilePermissions)
            throw FileError(replaceCpy(_("Cannot write permissions of %x."), L"%x", fmtPath(AFS::getDisplayPath(targetPath))), _("Operation not supported by device."));

        return copyFileAsStream(sourcePath, sourceAttr, targetPath, notifyUnbufferedIO); //throw FileError, (ErrorFileLocked), X
    }

    void copyNewFolderForSameAfsType(const AfsPath& sourcePath, const AbstractPath& targetPath, bool copyFilePermissions) const override //throw FileError
    {
        AFS::createFolderPlain(targetPath); //throw FileError

        if (copyFilePermissions)
            throw FileError(replaceCpy(_("Cannot write permissions of %x."), L"%x", fmtPath(AFS::getDisplayPath(targetPath))), _("Operation not supported by device."));
    }

    void copySymlinkForSameAfsType(const AfsPath& sourcePath, const AbstractPath& targetPath, bool copyFilePermissions) const override //throw FileError
    {
        throw FileError(replaceCpy(replaceCpy(_("Cannot copy symbolic link %x to %y."),
                                              L"%x", L'\n' + fmtPath(getDisplayPath(sourcePath))),
                                   L"%y", L'\n' + fmtPath(AFS::getDisplayPath(targetPath))), _("Operation not supported by device."));
    }

    void moveAndRenameItemForSameAfsType(const AfsPath& pathFrom, const AbstractPath& pathTo) const override //throw FileError, ErrorMoveUnsupported
    {
        throw ErrorMoveUnsupported(generateMoveErrorMsg(pathFrom, pathTo), _("Operation not supported by device."));
    }

    bool supportsPermissions(const AfsPath& folderPath) const override { return false; } //throw FileError

    //----------------------------------------------------------------------------------------------------------------
    FileIconHolder getFileIcon      (const AfsPath& filePath, int pixelSize) const override { return {}; } //throw FileError; optional return value
    ImageHolder    getThumbnailImage(const AfsPath& filePath, int pixelSize) const override { return {}; } //throw FileError; optional return value

    void authenticateAccess(const RequestPasswordFun& requestPassword /*throw X*/) const override {} //throw FileError, X

    bool hasNativeTransactionalCopy() const override { return false; }
    //----------------------------------------------------------------------------------------------------------------

    int64_t getFreeDiskSpace(const AfsPath& folderPath) const override { return -1; } //throw FileError, returns < 0 if not available

    std::unique_ptr<RecycleSession> createRecyclerSession(const AfsPath& folderPath) const override //throw FileError, RecycleBinUnavailable
    {
        throw RecycleBinUnavailable(replaceCpy(_("The recycle bin is not available for %x."), L"%x", fmtPath(getDisplayPath(folderPath))));
    }

    void moveToRecycleBin(const AfsPath& itemPath) const override //throw FileError, RecycleBinUnavailable
    {
        throw RecycleBinUnavailable(replaceCpy(_("The recycle bin is not available for %x."), L"%x", fmtPath(getDisplayPath(itemPath))));
    }

    const Zstring archivePath_;
};
}


void fff::zipInit()
{
    assert(!globalZipIndexBuffer.get());
    globalZipIndexBuffer.set(std::make_unique<ZipIndexBuffer>());
}


void fff::zipTeardown()
{
    assert(globalZipIndexBuffer.get());
    globalZipIndexBuffer.set(nullptr);
}


bool fff::acceptsItemPathPhraseZip(const Zstring& itemPathPhrase) //noexcept
{
    Zstring path = expandMacros(itemPathPhrase); //expand before trimming!
    trim(path);
    return startsWithAsciiNoCase(path, zipPrefix);
}


/* syntax: zip:<archive path>[|<folder path inside archive>]

   e.g. zip:/home/user/backup.zip
        zip:/home/user/backup.zip|project/src      */
AbstractPath fff::createItemPathZip(const Zstring& itemPathPhrase) //noexcept
{
    Zstring pathPhrase = expandMacros(itemPathPhrase); //expand before trimming!
    trim(pathPhrase);

    if (startsWithAsciiNoCase(pathPhrase, zipPrefix))
        pathPhrase = pathPhrase.c_str() + zipPrefix.size();

    const Zstring archivePhrase = beforeFirst(pathPhrase, zipInnerPathSeparator, IfNotFoundReturn::all);
    const Zstring innerPath     =  afterFirst(pathPhrase, zipInnerPathSeparator, IfNotFoundReturn::none);

    const Zstring archivePath = trimCpy(archivePhrase).empty() ? Zstring() : getResolvedFilePath(archivePhrase);

    return AbstractPath(makeSharedRef<ZipFileSystem>(archivePath), sanitizeDeviceRelativePath(innerPath));
}
