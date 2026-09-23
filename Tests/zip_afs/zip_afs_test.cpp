// Standalone test for the read-only ZIP AbstractFileSystem (FreeFileSync/Source/afs/zip.cpp)
//
// usage: zip_afs_test <archive.zip> <reference folder>          => archive content must equal reference folder
//        zip_afs_test --expect-read-error <archive.zip> <item>  => reading <item> must fail (e.g. CRC mismatch)

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <sys/stat.h>
#include <FreeFileSync/Source/afs/zip.h>

using namespace zen;
using namespace fff;
using AFS = AbstractFileSystem;
namespace fs = std::filesystem;

namespace
{
int failures = 0;

void check(bool condition, const std::string& msg)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << msg << '\n';
        ++failures;
    }
}

struct ItemDetails
{
    bool isFolder = false;
    uint64_t fileSize = 0;
    time_t modTime = 0;
};

class CollectingCallback : public AFS::TraverserCallback
{
public:
    CollectingCallback(std::map<std::string, ItemDetails>& items, const std::string& parentRelPath) : items_(items), parentRelPath_(parentRelPath) {}

    void onFile(const AFS::FileInfo& fi) override { items_[relPath(fi.itemName)] = {false, fi.fileSize, fi.modTime}; }

    HandleLink onSymlink(const AFS::SymlinkInfo& si) override { return HandleLink::skip; }

    std::shared_ptr<TraverserCallback> onFolder(const AFS::FolderInfo& fi) override
    {
        items_[relPath(fi.itemName)] = {true};
        return std::make_shared<CollectingCallback>(items_, relPath(fi.itemName));
    }

    HandleError reportDirError(const ErrorInfo& errorInfo) override
    {
        std::cerr << "dir error: " << utfTo<std::string>(errorInfo.msg) << '\n';
        ++failures;
        return HandleError::ignore;
    }

    HandleError reportItemError(const ErrorInfo& errorInfo, const Zstring& itemName) override
    {
        std::cerr << "item error: " << utfTo<std::string>(errorInfo.msg) << '\n';
        ++failures;
        return HandleError::ignore;
    }

private:
    std::string relPath(const Zstring& itemName) const { return parentRelPath_.empty() ? utfTo<std::string>(itemName) : parentRelPath_ + '/' + utfTo<std::string>(itemName); }

    std::map<std::string, ItemDetails>& items_;
    const std::string parentRelPath_;
};


std::map<std::string, ItemDetails> traverse(const AbstractPath& folderPath)
{
    std::map<std::string, ItemDetails> items;
    AFS::traverseFolderRecursive(folderPath.afsDevice, {{folderPath.afsPath, std::make_shared<CollectingCallback>(items, "")}}, 1);
    return items;
}


std::string readAll(const AbstractPath& filePath) //throw FileError
{
    std::unique_ptr<AFS::InputStream> in = AFS::getInputStream(filePath); //throw FileError
    std::string content;
    std::vector<char> buf(in->getBlockSize());
    for (;;)
    {
        const size_t bytesRead = in->tryRead(buf.data(), buf.size(), nullptr); //throw FileError
        if (bytesRead == 0)
            return content;
        content.append(buf.data(), bytesRead);
    }
}


std::string readNativeFile(const fs::path& filePath)
{
    std::ifstream file(filePath, std::ios::binary);
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}


void compareWithReference(const Zstring& archivePath, const fs::path& refRoot)
{
    const AbstractPath rootPath = createItemPathZip(Zstr("zip:") + archivePath);

    check(AFS::getItemTypeIfExists(rootPath) == AFS::ItemType::folder, "archive root must be a folder");
    check(!AFS::getItemTypeIfExists(AFS::appendRelPath(rootPath, Zstr("does/not/exist"))), "missing item must not exist");

    const std::map<std::string, ItemDetails> items = traverse(rootPath);

    //expected items
    std::map<std::string, ItemDetails> refItems;
    for (const fs::directory_entry& de : fs::recursive_directory_iterator(refRoot))
    {
        const std::string relPath = fs::relative(de.path(), refRoot).generic_string();
        if (de.is_directory())
            refItems[relPath] = {true};
        else
        {
            struct stat fileInfo = {};
            ::stat(de.path().c_str(), &fileInfo);
            refItems[relPath] = {false, static_cast<uint64_t>(fileInfo.st_size), fileInfo.st_mtime};
        }
    }

    for (const auto& [relPath, ref] : refItems)
    {
        auto it = items.find(relPath);
        if (it == items.end())
        {
            check(false, "missing in archive listing: " + relPath);
            continue;
        }
        const ItemDetails& item = it->second;
        check(item.isFolder == ref.isFolder, "item type mismatch: " + relPath);

        if (!ref.isFolder)
        {
            const AbstractPath filePath = AFS::appendRelPath(rootPath, utfTo<Zstring>(relPath));
            check(AFS::getItemType(filePath) == AFS::ItemType::file, "getItemType() != file: " + relPath);
            check(item.fileSize == ref.fileSize, "file size mismatch: " + relPath);
            //FFS default tolerance is 2 seconds (DOS time stamp precision)
            check(std::abs(item.modTime - ref.modTime) <= 2, "modification time mismatch: " + relPath +
                  " (" + std::to_string(item.modTime) + " vs " + std::to_string(ref.modTime) + ')');
            try
            {
                check(readAll(filePath) == readNativeFile(refRoot / relPath), "content mismatch: " + relPath);
            }
            catch (const FileError& e) { check(false, "read error: " + utfTo<std::string>(e.toString())); }
        }
    }
    for (const auto& [relPath, item] : items)
        check(refItems.contains(relPath), "unexpected item in archive: " + relPath);

    //sub folder syntax: zip:<archive>|<inner folder>
    for (const auto& [relPath, ref] : refItems)
        if (ref.isFolder)
        {
            const AbstractPath subPath = createItemPathZip(Zstr("zip:") + archivePath + Zstr('|') + utfTo<Zstring>(relPath));
            check(AFS::getInitPathPhrase(subPath) == Zstr("zip:") + archivePath + Zstr('|') + utfTo<Zstring>(relPath), "path phrase round trip: " + relPath);
            size_t expectedCount = 0;
            for (const auto& [relPath2, ref2] : refItems)
                if (relPath2.starts_with(relPath + '/'))
                    ++expectedCount;
            check(traverse(subPath).size() == expectedCount, "sub folder traversal: " + relPath);
            break;
        }

    //archive must be read-only
    try
    {
        AFS::createFolderPlain(AFS::appendRelPath(rootPath, Zstr("new folder")));
        check(false, "createFolderPlain() must fail");
    }
    catch (const FileError&) {}

    try
    {
        AFS::removeFilePlain(AFS::appendRelPath(rootPath, Zstr("any file")));
        check(false, "removeFilePlain() must fail");
    }
    catch (const FileError&) {}

    std::cout << items.size() << " items checked in " << utfTo<std::string>(archivePath) << '\n';
}
}


int main(int argc, char* argv[])
{
    zipInit();

    if (argc == 4 && std::string(argv[1]) == "--expect-read-error")
    {
        const AbstractPath filePath = AFS::appendRelPath(createItemPathZip(Zstr("zip:") + Zstring(argv[2])), argv[3]);
        try
        {
            readAll(filePath);
            check(false, std::string("reading corrupted item must fail: ") + argv[3]);
        }
        catch (const FileError& e) { std::cout << "expected error: " << utfTo<std::string>(e.toString()) << '\n'; }
    }
    else if (argc == 3)
        compareWithReference(argv[1], argv[2]);
    else
    {
        std::cerr << "usage: zip_afs_test <archive.zip> <reference folder>\n"
                  "       zip_afs_test --expect-read-error <archive.zip> <item>\n";
        return EXIT_FAILURE;
    }

    zipTeardown();

    if (failures > 0)
    {
        std::cerr << failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "OK\n";
    return EXIT_SUCCESS;
}
