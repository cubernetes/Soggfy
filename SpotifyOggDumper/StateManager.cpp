#include "pch.h"
#include "StateManager.h"
#include "Utils/Log.h"
#include "Utils/Http.h"

namespace fs = std::filesystem;
using namespace nlohmann;

struct PlaybackInfo;
struct OggStream;

struct PlaybackInfo
{
    std::string TrackId;
    bool WasSeeked;
    int StartPos;
    bool Closed;
    bool PlayedToEnd; //whether closeReason == "trackdone"
    
    //Map of <Stream, Freq> that were alive during this playback.
    //Used to determine stream source playbacks.
    std::unordered_map<std::shared_ptr<OggStream>, int> LiveStreams;
};
struct OggStream
{
    fs::path FileName;
    std::ofstream FileStream;
    int NumPages;
    bool EOS;
};

struct StateManagerImpl : public StateManager
{
    fs::path _dataDir;
    std::string _accessToken;

    std::unordered_map<std::string, std::shared_ptr<PlaybackInfo>> _playbacks;
    std::shared_ptr<PlaybackInfo> _currPlayback;

    std::unordered_map<uintptr_t, std::shared_ptr<OggStream>> _oggs;
    int _nextStreamId;

    json _config;

    StateManagerImpl(const fs::path& dataDir) : 
        _dataDir(dataDir),
        _nextStreamId(0)
    {
        std::ifstream configFile(dataDir / "config.json");
        if (configFile.good()) {
            _config = json::parse(configFile, nullptr, true, true);
        } else {
            _config["track_path_fmt"] = "%userprofile%/Music/Soggfy/{artist_name}/{album_name}/{track_num}. {track_name}.ogg";
            _config["cover_path_fmt"] = "%userprofile%/Music/Soggfy/{artist_name}/{album_name}/cover.jpg";
        }

        std::error_code deleteError;
        fs::remove_all(dataDir / "temp", deleteError);
    }

    void OnTrackCreated(const std::string& playbackId, const std::string& trackId)
    {
        LogDebug("TrackCreated: id={} playId={}", trackId, playbackId);

        if (_playbacks.contains(playbackId)) {
            LogWarn("Playback with the same id already exists, overwriting.");
        }
        auto info = std::make_shared<PlaybackInfo>();
        info->TrackId = trackId;
        _playbacks[playbackId] = info;
    }
    void OnTrackOpened(const std::string& playbackId, int positionMs)
    {
        auto itr = _playbacks.find(playbackId);
        if (itr == _playbacks.end()) {
            LogWarn("Track opened for unknown playback, ignoring.");
            _currPlayback = nullptr;
            return;
        }
        _currPlayback = itr->second;
        _currPlayback->StartPos = positionMs;
        LogInfo("New track detected: {}", _currPlayback->TrackId);
    }
    void OnTrackClosed(const std::string& playbackId, const std::string& reason)
    {
        LogDebug("TrackClosed: playId={} reason={}", playbackId, reason);
        
        auto itr = _playbacks.find(playbackId);
        if (itr != _playbacks.end()) {
            auto playback = itr->second;

            playback->Closed = true;
            playback->PlayedToEnd = reason == "trackdone";

            if (playback == _currPlayback) {
                _currPlayback = nullptr;
            }
            FlushClosedTracks();
        }
    }
    void OnTrackSeeked(const std::string& playbackId)
    {
#if !_DEBUG
        auto itr = _playbacks.find(playbackId);
        if (itr != _playbacks.end()) {
            itr->second->WasSeeked = true;
        }
#endif
    }
    void ReceiveOggPage(uintptr_t syncId, ogg_page* page)
    {
        auto state = GetOggStream(syncId);

        if (!state->EOS) {
            state->FileStream.write((char*)page->header, page->header_len);
            state->FileStream.write((char*)page->body, page->body_len);
            state->NumPages++;

            if (_currPlayback != nullptr) {
                _currPlayback->LiveStreams[state]++;
            }
            if (page->header[5] & 0x04) { //ogg_page_eos
                state->EOS = true;
                state->FileStream.close();
                _oggs.erase(syncId);

                FlushClosedTracks();
            }
        }
    }
    void UpdateAccToken(const std::string& token)
    {
        LogDebug("Update access token: {}", token);
        _accessToken = token;
    }
    void Shutdown()
    {
        for (auto& [id, stream] : _oggs) {
            stream->FileStream.close();
        }
    }

    std::shared_ptr<OggStream> GetOggStream(uintptr_t syncId)
    {
        auto itr = _oggs.find(syncId);
        if (itr != _oggs.end()) {
            if (itr->second->EOS) {
                LogWarn("Detected a possible collision with ogg stream ids, last few tracks could become corrupted.");
            }
            return itr->second;
        }
        auto stream = std::make_shared<OggStream>();

        stream->FileName = _dataDir / "temp" / ("stream_" + std::to_string(_nextStreamId++) + ".ogg");
        fs::create_directories(stream->FileName.parent_path());

        stream->FileStream.open(stream->FileName, std::ios::out | std::ios::binary);

        LogDebug("Detected new ogg stream, dumping to {}", stream->FileName.string());

        _oggs[syncId] = stream;
        return stream;
    }
    
    void FlushClosedTracks()
    {
        if (_accessToken.empty()) {
            LogDebug("FlushTracks: access token not available yet, deferring...");
            return;
        }
        //Iterate and remove finished playbacks
        auto numFlushed = std::erase_if(_playbacks, [&](const auto& elem) {
            auto playback = elem.second;
            if (!playback->Closed) return false;

            auto stream = FindSourceStream(playback);
            if (stream == nullptr) return false; //defer until a the stream is closed
            
            if (playback->StartPos == 0 && playback->PlayedToEnd && !playback->WasSeeked) {
                std::thread t(&StateManagerImpl::TagAndMoveToOutput, this, playback, stream);
                t.detach();
            } else {
                LogInfo("Discarding track {} because it was not fully played.", playback->TrackId);
            }
            return true;
        });
        LogDebug("FlushTracks: flushed {} tracks. Alive: {} playbacks, {} streams.", numFlushed, _playbacks.size(), _oggs.size());
    }
    std::shared_ptr<OggStream> FindSourceStream(std::shared_ptr<PlaybackInfo> playback)
    {
        std::shared_ptr<OggStream> bestStream;
        int bestFreq = 8; //ignore short streams

        for (auto& [stream, freq] : playback->LiveStreams) {
            if (freq > bestFreq) {
                bestStream = stream;
                bestFreq = freq;
            }
        }
        return bestStream;
    }

    void TagAndMoveToOutput(std::shared_ptr<PlaybackInfo> playback, std::shared_ptr<OggStream> stream)
    {
        auto& trackId = playback->TrackId;

        try {
            auto meta = FetchTrackMetadata(trackId);

            LogInfo("Saving track {}", trackId);
            LogInfo("  title: {} - {}", meta["artists"][0]["name"].get<std::string>(), meta["name"].get<std::string>());
            LogInfo("  stream: {}", stream->FileName.filename().string());
            LogDebug("  meta: {}", meta.dump());

            auto trackPath = RenderTrackPath("track_path_fmt", meta);
            auto coverPath = RenderTrackPath("cover_path_fmt", meta);

            auto tmpCoverPath = _dataDir / "temp" / (meta["album"]["id"].get<std::string>() + "_cover.jpg");

            if (!fs::exists(tmpCoverPath)) {
                DownloadFile(tmpCoverPath, meta["album"]["images"][0]["url"]);
            }
            if (!coverPath.empty() && !fs::exists(coverPath)) {
                fs::create_directories(coverPath.parent_path());
                fs::copy_file(tmpCoverPath, coverPath);
            }
            WriteTags(stream->FileName, tmpCoverPath, meta);

            fs::create_directories(trackPath.parent_path());
            fs::rename(stream->FileName, trackPath);
        } catch (std::exception& ex) {
            LogError("Failed to save track {}: {}", trackId, ex.what());
        }
    }

    void WriteTags(const fs::path& path, const fs::path& coverPath, const json& meta)
    {
        std::ifstream coverFile(coverPath, std::ios::binary);
        auto coverData = std::vector<char>(std::istreambuf_iterator<char>(coverFile), std::istreambuf_iterator<char>());

        std::string artists;
        for (auto& artist : meta["artists"]) {
            if (artists.size() > 0) artists += ", ";
            artists += artist["name"];
        }
        auto releaseDate = meta["album"]["release_date"].get<std::string>();
        int releaseYear = std::stoi(releaseDate.substr(0, releaseDate.find('-')));

        TagLib::Ogg::Vorbis::File ogg(path.string().c_str());
        auto* tag = ogg.tag();
        
        auto coverArt = new TagLib::FLAC::Picture();
        coverArt->setType(TagLib::FLAC::Picture::Type::FrontCover);
        coverArt->setMimeType("image/jpeg");
        coverArt->setDescription("Front Cover");
        coverArt->setData(TagLib::ByteVector(coverData.data(), (uint32_t)coverData.size()));

        tag->addPicture(coverArt);
        tag->setTitle(TagLib::String(meta["name"].get<std::string>(), TagLib::String::UTF8));
        tag->setArtist(TagLib::String(artists, TagLib::String::UTF8));
        tag->setAlbum(TagLib::String(meta["album"]["name"].get<std::string>(), TagLib::String::UTF8));
        tag->setTrack(meta["track_number"].get<int>());
        tag->setYear(releaseYear);

        tag->addField("DATE", releaseDate);
        tag->addField("DISCNUMBER", std::to_string(meta["disc_number"].get<int>()));
        tag->addField("ISRC", meta["external_ids"]["isrc"].get<std::string>());
        tag->addField("RELEASETYPE", meta["album"]["album_type"].get<std::string>());
        tag->addField("TOTALTRACKS", std::to_string(meta["album"]["total_tracks"].get<int>()));

        ogg.save();
    }

    json FetchTrackMetadata(const std::string& trackId)
    {
        std::string path;

        if (trackId.starts_with("spotify:track:")) {
            path = "/v1/tracks/" + trackId.substr(14);
        } else if (trackId.starts_with("spotify:episode:")) {
            path = "/v1/episodes/" + trackId.substr(16);
        } else {
            throw std::runtime_error("Unknown track type (id=" + trackId + ")");
        }
        Http::Request req;
        req.Url = "https://api.spotify.com" + path;
        req.Headers.emplace("Accept", "application/json");
        req.Headers.emplace("Authorization", "Bearer " + _accessToken);
        //FIXME: is it good to fake user-agent headers?

        auto resp = Http::Fetch(req);
        auto& rawJson = resp.Content;
        return json::parse(rawJson.begin(), rawJson.end());
    }
    void DownloadFile(const fs::path& path, const std::string& url)
    {
        Http::Request req;
        req.Url = url;
        req.Headers.emplace("Accept", "*/*");

        auto resp = Http::Fetch(req);

        std::ofstream file(path, std::ios::out | std::ios::binary);
        file.write((char*)resp.Content.data(), resp.Content.size());
    }

    fs::path RenderTrackPath(const std::string& fmtKey, const json& metadata)
    {
        auto releaseDate = metadata["album"]["release_date"].get<std::string>();
        int releaseYear = std::stoi(releaseDate.substr(0, releaseDate.find('-')));
        
        std::string fmt = _config[fmtKey].get<std::string>();

        auto FillArg = [&](const std::string& needle, const std::string& replacement) {
            auto repl = SanitizeFilename(replacement);
            //https://stackoverflow.com/a/4643526
            size_t index = 0;
            while (true) {
                index = fmt.find(needle, index);
                if (index == std::string::npos) break;

                fmt.replace(index, needle.length(), repl);
                index += repl.length();
            }
        };
        FillArg("{artist_name}", metadata["artists"][0]["name"]);
        FillArg("{album_name}", metadata["album"]["name"]);
        FillArg("{track_name}", metadata["name"]);
        FillArg("{track_num}", std::to_string(metadata["track_number"].get<int>()));
        FillArg("{release_year}", std::to_string(releaseYear));
        
        return fs::u8path(ExpandEnvVars(fmt));
    }
    //Call WINAPI ExpandEnvironmentStrings(), assuming str is UTF8.
    std::string ExpandEnvVars(const std::string& str)
    {
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> utfConv;
        std::wstring srcW = utfConv.from_bytes(str);

        DWORD len = ExpandEnvironmentStrings(srcW.c_str(), NULL, 0);

        std::wstring dstW(len, '\0');
        ExpandEnvironmentStrings(srcW.c_str(), dstW.data(), len);

        return utfConv.to_bytes(dstW);
    }
    std::string SanitizeFilename(const std::string& src)
    {
        std::string dst;
        dst.reserve(src.length());

        for (char ch : src) {
            if ((ch >= 0x00 && ch < 0x20) || strchr("\\/:*?\"<>|", ch)) {
                continue;
            }
            dst += ch;
        }
        return dst;
    }
};

std::unique_ptr<StateManager> StateManager::New(const fs::path& dataDir)
{
    return std::make_unique<StateManagerImpl>(dataDir);
}