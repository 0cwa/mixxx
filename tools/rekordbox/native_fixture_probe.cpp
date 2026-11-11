// Prepared by ChatGPT for human review.
#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

#include "library/rekordbox/rekordboxdatabasesnapshot.h"
#include "library/rekordbox/rekordboxnativereader.h"

namespace {

using namespace mixxx::rekordbox;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string hashFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(file.good(), "Cannot read an independent fixture file.");
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(
            EVP_MD_CTX_new(), EVP_MD_CTX_free);
    require(digest && EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) == 1,
            "Cannot initialize the fixture checksum.");
    std::array<char, 65536> buffer;
    while (file) {
        file.read(buffer.data(), buffer.size());
        require(EVP_DigestUpdate(digest.get(), buffer.data(), file.gcount()) == 1,
                "Cannot update the fixture checksum.");
    }
    require(file.eof(), "Cannot finish reading an independent fixture file.");
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes;
    unsigned int size = 0;
    require(EVP_DigestFinal_ex(digest.get(), bytes.data(), &size) == 1,
            "Cannot finish the fixture checksum.");
    std::ostringstream result;
    for (unsigned int i = 0; i < size; ++i) {
        result << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<unsigned int>(bytes[i]);
    }
    return result.str();
}

std::map<std::string, std::string> sourceManifest(const std::filesystem::path& directory) {
    std::map<std::string, std::string> result;
    result.emplace(".",
            std::to_string(std::filesystem::last_write_time(directory)
                            .time_since_epoch()
                            .count()) +
                    ":" +
                    std::to_string(static_cast<unsigned int>(
                            std::filesystem::status(directory).permissions())) +
                    ":directory");
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
        require(!entry.is_symlink(),
                "The independent fixture tree contains an unexpected symbolic "
                "link.");
        auto state = std::to_string(entry.last_write_time().time_since_epoch().count());
        state += ":" + std::to_string(static_cast<unsigned int>(entry.status().permissions()));
        if (entry.is_regular_file()) {
            state += ":" + std::to_string(entry.file_size()) + ":" + hashFile(entry.path());
        } else {
            require(entry.is_directory(),
                    "The independent fixture tree contains an unexpected "
                    "special file.");
            state += ":directory";
        }
        result.emplace(std::filesystem::relative(entry.path(), directory)
                               .generic_string(),
                std::move(state));
    }
    return result;
}

class KeyBytes {
  public:
    ~KeyBytes() {
        clear();
    }

    void clear() {
        if (!bytes.empty()) {
            OPENSSL_cleanse(bytes.data(), bytes.size());
            bytes.clear();
        }
    }

    std::vector<unsigned char> bytes;
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: native-fixture-probe master_locked.db private-temporary-directory\n"
                     "Provide the independent fixture's key bytes on stdin, without a newline.\n";
        return 2;
    }
    try {
        const auto source = std::filesystem::absolute(argv[1]);
        const auto temporaryRoot = std::filesystem::absolute(argv[2]);
        require(hashFile(source) ==
                        "d388444b6e86ab537613d03297a57e1c0d99ccfb7c8fbd2955a66b"
                        "34947d4ac2",
                "The native fixture does not match the pinned pyrekordbox "
                "master_locked.db checksum.");
        const auto relativeTemporary = temporaryRoot.lexically_relative(source.parent_path());
        require(relativeTemporary.empty() || *relativeTemporary.begin() == "..",
                "Use a temporary directory outside the fixture source tree.");
        const auto before = sourceManifest(source.parent_path());
        KeyBytes key;
        key.bytes.reserve(4096);
        char byte = 0;
        while (std::cin.get(byte)) {
            require(key.bytes.size() < 4096, "The key exceeds the probe's input bound.");
            key.bytes.push_back(static_cast<unsigned char>(byte));
        }
        require(!key.bytes.empty(), "A fixture key must be supplied through stdin.");
        std::filesystem::path privateDatabase;
        {
            DatabaseSnapshotRequest request;
            request.sourcePath = source;
            request.temporaryRoot = temporaryRoot;
            request.profile = DatabaseCipherProfile::Native6SqlCipher4;
            request.keyProvider = [&](DatabaseCipherProfile profile) {
                require(profile == DatabaseCipherProfile::Native6SqlCipher4,
                        "The probe was asked for an unrelated format key.");
                return key.bytes;
            };
            const auto result = snapshotDatabase(request);
            require(result.outcome == DatabaseSnapshotOutcome::Success && result.snapshot,
                    "The encrypted native fixture could not be safely snapshotted.");
            privateDatabase = result.snapshot->path();
            DatabaseReadContext context;
            context.sourceId = "independent-native-fixture-binding";
            context.generation = 1;
            context.fingerprint = result.snapshot->fingerprint();
            context.originalDatabasePath = source;
            context.assetRoot = source.parent_path();
            context.accessLease = result.snapshot;
            const auto decoded = readNativeLibrary(result.snapshot->database(), context);
            require(decoded.library &&
                            decoded.library->databaseIdentity == "3546525630" &&
                            decoded.library->tracks.size() == 6 &&
                            decoded.library->playlists.size() == 2 &&
                            decoded.library->annotations.empty(),
                    "The encrypted fixture did not match its independent "
                    "structural expectations.");
            const auto track = std::find_if(decoded.library->tracks.begin(),
                    decoded.library->tracks.end(),
                    [](const auto& item) {
                        return item.sourceTrackId == "178162577";
                    });
            require(track != decoded.library->tracks.end() && track->title == "Demo Track 1" &&
                            track->artist == "Loopmasters" && track->bpm == 128.0 &&
                            track->durationMilliseconds == 172000,
                    "The encrypted fixture did not match independently observed metadata.");
            key.bytes.front() ^= 0x01;
            const auto wrongKey = snapshotDatabase(request);
            require(wrongKey.outcome != DatabaseSnapshotOutcome::Success && !wrongKey.snapshot,
                    "The encrypted native fixture accepted an incorrect key.");
            key.clear();
        }
        require(!std::filesystem::exists(privateDatabase),
                "Private plaintext snapshot cleanup failed.");
        require(before == sourceManifest(source.parent_path()),
                "The complete independent fixture directory manifest changed.");
        std::cout << "PASS encrypted native snapshot and metadata: 6 tracks, 2 "
                     "playlist records, 0 cue records\n"
                     "PASS native profile rejects wrong key without publishing "
                     "records\n"
                     "PASS private plaintext snapshot cleanup\n"
                  << "PASS source directory integrity: " << before.size()
                  << " entries unchanged\n"
                     "Qualification: DBVersion 6000; no native7, cue/grid, "
                     "application, or hardware claim\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
// End of code prepared by ChatGPT for human review and submission.
