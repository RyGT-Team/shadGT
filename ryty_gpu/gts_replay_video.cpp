// GTS-015: video recording and clips, as the PS4's Share button and Capture Gallery do.
//
// A GT Sport replay file (download0/APP_DATA/replay/rpl##*.dat) is the game's own encrypted race
// recording, not video: only the game can turn it into pictures, by playing it back. On a PS4 the
// system records the screen, not the game, so the renderer does that here. The presenter hands
// every new game frame (without RyGT's overlays) to this file, and Media Foundation encodes them
// to H.264 MP4 in "<Videos>\Gran Turismo Sport" (the Capture Gallery):
//
//  - Clip buffer: the last RYGT_CLIP_MINUTES minutes (15 by default, as the PS4; 0 turns it off)
//    are kept in one-minute pieces in %TEMP%\RyGT GTS clips. Holding Share/Create (or F10) saves
//    them as one "Clip <date time>.mp4", like the PS4's "Save video clip".
//  - Recordings: a short press of Share/Create (or F9) starts and stops a "Video <date time>.mp4".
//    Playing a saved replay from the Replays menu (the race_replay project) records it by itself
//    into "Replay <date time>.mp4", until the replay ends.
//
// The videos have the game's sound (AAC), captured with Windows' process loopback, so other
// programs on the PC are not in them. Set RYGT_REPLAY_VIDEO=1 to enable recording and clips;
// they are disabled by default.

#include <windows.h>
#include <audioclient.h>
// Process loopback needs Windows 10 2004; the build targets older Windows, so the header is
// read as for 2004 (the program only uses it when Windows has it).
#pragma push_macro("NTDDI_VERSION")
#undef NTDDI_VERSION
#define NTDDI_VERSION NTDDI_WIN10_FE
#include <audioclientactivationparams.h>
#pragma pop_macro("NTDDI_VERSION")
#include <mmdeviceapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <codecapi.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/types.h"

namespace {

struct VideoFrame {
    std::vector<u8> bgra;
    u32 width{};
    u32 height{};
    LONGLONG time{}; // 100 ns, steady clock
};

// The game's sound: 48 kHz 16-bit stereo PCM from Windows' process loopback (only this process,
// not the other programs on the PC), stored as AAC.
struct AudioChunk {
    std::vector<u8> pcm;
    LONGLONG time{}; // 100 ns, steady clock, of the chunk's first sample
};
constexpr u32 AudioRate = 48000;
constexpr u32 AudioChannels = 2;
constexpr u32 AudioFrameBytes = AudioChannels * 2;
std::deque<AudioChunk> g_audio;          // under g_mutex
std::atomic<bool> g_audio_ok{false};     // capturing: the videos get a sound track

constexpr size_t MaxQueuedFrames = 12;
constexpr LONGLONG SegmentLength = 60LL * 10'000'000; // one minute
constexpr LONGLONG FrameDuration = 10'000'000 / 60;

std::atomic<bool> g_replay_running{false};
std::atomic<bool> g_manual{false};
std::atomic<bool> g_replay_stopped{false}; // Share pressed during a replay recording
std::atomic<bool> g_save_clip{false};
std::mutex g_mutex;
std::condition_variable g_cv;
std::deque<VideoFrame> g_queue;
std::vector<std::vector<u8>> g_spare; // reused frame buffers
std::once_flag g_worker_once;
u64 g_dropped = 0;

bool Enabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("RYGT_REPLAY_VIDEO");
        const bool enabled = env && std::strcmp(env, "1") == 0;
        std::fprintf(stderr, "[patch] GTS-015 video capture %s (RYGT_REPLAY_VIDEO=1 enables it)\n",
                     enabled ? "enabled" : "disabled");
        return enabled;
    }();
    return enabled;
}

int ClipMinutes() {
    static const int minutes = [] {
        const char* env = std::getenv("RYGT_CLIP_MINUTES");
        return env && env[0] ? std::max(0, std::atoi(env)) : 15;
    }();
    return minutes;
}

LONGLONG Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
               .count() /
           100;
}

std::filesystem::path VideoFolder() {
    PWSTR videos = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_CREATE, nullptr, &videos))) {
        return {};
    }
    std::filesystem::path folder = std::filesystem::path{videos} / L"Gran Turismo Sport";
    CoTaskMemFree(videos);
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    return error ? std::filesystem::path{} : folder;
}

std::filesystem::path NewVideoPath(const wchar_t* kind) {
    const auto folder = VideoFolder();
    if (folder.empty()) {
        return {};
    }
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t name[96];
    std::swprintf(name, std::size(name), L"%ls %04d-%02d-%02d %02d-%02d-%02d", kind, now.wYear,
                  now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    std::filesystem::path path = folder / (std::wstring{name} + L".mp4");
    for (int i = 2; std::filesystem::exists(path); ++i) {
        path = folder / (std::wstring{name} + L" (" + std::to_wstring(i) + L").mp4");
    }
    return path;
}

std::filesystem::path ClipFolder() {
    wchar_t temp[MAX_PATH];
    const DWORD length = GetTempPathW(MAX_PATH, temp);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    return std::filesystem::path{temp} / L"RyGT GTS clips";
}

class Mp4Writer {
public:
    ~Mp4Writer() { Close(); }

    bool IsOpen() const { return writer != nullptr; }

    bool Open(const std::filesystem::path& path_, u32 width_, u32 height_, LONGLONG base_, bool quiet_) {
        IMFAttributes* attributes = nullptr;
        MFCreateAttributes(&attributes, 2);
        attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        HRESULT hr = MFCreateSinkWriterFromURL(path_.c_str(), nullptr, attributes, &writer);
        attributes->Release();
        if (FAILED(hr)) {
            std::fprintf(stderr, "[patch] GTS-015: cannot create %ls (0x%08lx)\n", path_.c_str(), hr);
            writer = nullptr;
            return false;
        }
        IMFMediaType* out = nullptr;
        MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        out->SetUINT32(MF_MT_AVG_BITRATE, width_ * height_ * 10); // ~20 Mbit/s at 1080p
        out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        MFSetAttributeSize(out, MF_MT_FRAME_SIZE, width_, height_);
        MFSetAttributeRatio(out, MF_MT_FRAME_RATE, 60, 1);
        MFSetAttributeRatio(out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = writer->AddStream(out, &stream);
        out->Release();
        if (FAILED(hr)) {
            std::fprintf(stderr, "[patch] GTS-015: no H.264 encoder (0x%08lx)\n", hr);
            Abandon();
            return false;
        }
        IMFMediaType* in = nullptr;
        MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        in->SetUINT32(MF_MT_DEFAULT_STRIDE, width_ * 4); // top-down rows
        MFSetAttributeSize(in, MF_MT_FRAME_SIZE, width_, height_);
        MFSetAttributeRatio(in, MF_MT_FRAME_RATE, 60, 1);
        MFSetAttributeRatio(in, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = writer->SetInputMediaType(stream, in, nullptr);
        in->Release();
        has_audio = SUCCEEDED(hr) && g_audio_ok.load() && AddAudio();
        if (FAILED(hr) || FAILED(writer->BeginWriting())) {
            std::fprintf(stderr, "[patch] GTS-015: encoder refused %ux%u (0x%08lx)\n", width_, height_, hr);
            Abandon();
            return false;
        }
        width = width_;
        height = height_;
        base = base_;
        path = path_;
        quiet = quiet_;
        frames = 0;
        return true;
    }

    void Write(const VideoFrame& frame) {
        const DWORD size = width * height * 4;
        IMFMediaBuffer* buffer = nullptr;
        if (FAILED(MFCreateMemoryBuffer(size, &buffer))) {
            return;
        }
        BYTE* data = nullptr;
        buffer->Lock(&data, nullptr, nullptr);
        for (u32 y = 0; y < height; ++y) {
            std::memcpy(data + static_cast<size_t>(y) * width * 4,
                        frame.bgra.data() + static_cast<size_t>(y) * frame.width * 4, width * 4);
        }
        buffer->Unlock();
        buffer->SetCurrentLength(size);
        IMFSample* sample = nullptr;
        MFCreateSample(&sample);
        sample->AddBuffer(buffer);
        last = std::max(frame.time - base, frames == 0 ? 0 : last + 1);
        sample->SetSampleTime(last);
        sample->SetSampleDuration(FrameDuration);
        writer->WriteSample(stream, sample);
        sample->Release();
        buffer->Release();
        ++frames;
    }

    // Sound from `chunk.time` on; what came before the video started is left out.
    void WriteAudio(const AudioChunk& chunk) {
        if (!has_audio || frames == 0) {
            return;
        }
        const u32 count = static_cast<u32>(chunk.pcm.size() / AudioFrameBytes);
        u32 skip = 0;
        LONGLONG time = chunk.time - base;
        if (time < audio_next) { // overlaps what was written: drop the overlap
            skip = static_cast<u32>(std::min<LONGLONG>(
                count, (audio_next - time) * AudioRate / 10'000'000));
            time = audio_next;
        }
        if (skip >= count || time < 0) {
            return;
        }
        const DWORD size = (count - skip) * AudioFrameBytes;
        IMFMediaBuffer* buffer = nullptr;
        if (FAILED(MFCreateMemoryBuffer(size, &buffer))) {
            return;
        }
        BYTE* data = nullptr;
        buffer->Lock(&data, nullptr, nullptr);
        std::memcpy(data, chunk.pcm.data() + static_cast<size_t>(skip) * AudioFrameBytes, size);
        buffer->Unlock();
        buffer->SetCurrentLength(size);
        IMFSample* sample = nullptr;
        MFCreateSample(&sample);
        sample->AddBuffer(buffer);
        const LONGLONG duration = static_cast<LONGLONG>(count - skip) * 10'000'000 / AudioRate;
        sample->SetSampleTime(time);
        sample->SetSampleDuration(duration);
        writer->WriteSample(audio_stream, sample);
        sample->Release();
        buffer->Release();
        audio_next = time + duration;
    }

    LONGLONG Length() const { return frames ? last + FrameDuration : 0; }

    std::filesystem::path Close() {
        std::filesystem::path closed;
        if (writer) {
            writer->Finalize();
            writer->Release();
            writer = nullptr;
            if (!quiet) {
                std::fprintf(stderr, "[patch] GTS-015 video saved: %ls (%llu frames)\n", path.c_str(),
                             static_cast<unsigned long long>(frames));
            }
            closed = path;
        }
        return closed;
    }

    u32 width{}, height{};

private:
    bool AddAudio() {
        IMFMediaType* out = nullptr;
        MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, AudioRate);
        out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, AudioChannels);
        out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000); // 192 kbit/s
        HRESULT hr = writer->AddStream(out, &audio_stream);
        out->Release();
        if (FAILED(hr)) {
            std::fprintf(stderr, "[patch] GTS-015: no AAC encoder, videos without sound (0x%08lx)\n", hr);
            return false;
        }
        IMFMediaType* in = nullptr;
        MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, AudioRate);
        in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, AudioChannels);
        in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, AudioFrameBytes);
        in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, AudioRate * AudioFrameBytes);
        hr = writer->SetInputMediaType(audio_stream, in, nullptr);
        in->Release();
        if (FAILED(hr)) {
            std::fprintf(stderr, "[patch] GTS-015: AAC encoder refused the sound (0x%08lx)\n", hr);
            return false; // the stream was added: the file still has an empty sound track
        }
        audio_next = 0;
        return true;
    }

    void Abandon() {
        if (writer) {
            writer->Release();
            writer = nullptr;
        }
    }

    IMFSinkWriter* writer = nullptr;
    DWORD stream = 0;
    std::filesystem::path path;
    LONGLONG base = 0;
    LONGLONG last = 0;
    u64 frames = 0;
    bool quiet = false;
    DWORD audio_stream = 0;
    bool has_audio = false;
    LONGLONG audio_next = 0;
};

// Joins the clip pieces, without encoding them again, into one video.
void SaveClip(std::vector<std::filesystem::path> pieces) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    const auto path = NewVideoPath(L"Clip");
    IMFSinkWriter* writer = nullptr;
    DWORD video_out = 0;
    DWORD audio_out = 0;
    bool writer_audio = false;
    LONGLONG offset = 0;
    for (const auto& piece : pieces) {
        IMFSourceReader* reader = nullptr;
        if (FAILED(MFCreateSourceReaderFromURL(piece.c_str(), nullptr, &reader))) {
            continue;
        }
        IMFMediaType* video = nullptr;
        IMFMediaType* audio = nullptr;
        if (FAILED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &video))) {
            reader->Release();
            continue;
        }
        const bool piece_audio =
            SUCCEEDED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &audio));
        if (!writer && !path.empty() &&
            SUCCEEDED(MFCreateSinkWriterFromURL(path.c_str(), nullptr, nullptr, &writer))) {
            bool ok = SUCCEEDED(writer->AddStream(video, &video_out)) &&
                      SUCCEEDED(writer->SetInputMediaType(video_out, video, nullptr));
            writer_audio = ok && piece_audio && SUCCEEDED(writer->AddStream(audio, &audio_out)) &&
                           SUCCEEDED(writer->SetInputMediaType(audio_out, audio, nullptr));
            if (!ok || FAILED(writer->BeginWriting())) {
                writer->Release();
                writer = nullptr;
            }
        }
        video->Release();
        if (audio) {
            audio->Release();
        }
        if (!writer) {
            reader->Release();
            break;
        }
        // Video and sound of the piece, both moved to start where the last piece ended.
        LONGLONG end = offset;
        const DWORD streams[2] = {static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                                  static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM)};
        const DWORD outputs[2] = {video_out, audio_out};
        for (int s = 0; s < (writer_audio && piece_audio ? 2 : 1); ++s) {
            for (;;) {
                DWORD flags = 0;
                LONGLONG time = 0;
                IMFSample* sample = nullptr;
                if (FAILED(reader->ReadSample(streams[s], 0, nullptr, &flags, &time, &sample)) ||
                    (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
                    if (sample) {
                        sample->Release();
                    }
                    break;
                }
                if (!sample) {
                    continue;
                }
                LONGLONG duration = FrameDuration;
                sample->GetSampleDuration(&duration);
                sample->SetSampleTime(offset + time);
                if (s == 0) {
                    end = std::max(end, offset + time + duration);
                }
                writer->WriteSample(outputs[s], sample);
                sample->Release();
            }
        }
        offset = end;
        reader->Release();
    }
    if (writer) {
        writer->Finalize();
        writer->Release();
        std::fprintf(stderr, "[patch] GTS-015 clip saved: %ls (%.0f s)\n", path.c_str(),
                     static_cast<double>(offset) / 10'000'000.0);
    } else {
        std::fprintf(stderr, "[patch] GTS-015: clip not saved (nothing recorded yet)\n");
    }
    for (const auto& piece : pieces) { // they were copies kept for this save
        std::error_code error;
        std::filesystem::remove(piece, error);
    }
    MFShutdown();
    CoUninitialize();
}

// Completion of ActivateAudioInterfaceAsync for the process loopback device.
class ActivateHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    IAudioClient* client = nullptr;
    HRESULT result = E_FAIL;

    STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        } else if (riid == __uuidof(IAgileObject)) {
            *out = static_cast<IAgileObject*>(this);
        } else {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs; }
    STDMETHODIMP_(ULONG) Release() override { return --refs; } // lives on the capture thread's stack
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        IUnknown* unknown = nullptr;
        HRESULT activated = E_FAIL;
        operation->GetActivateResult(&activated, &unknown);
        result = activated;
        if (SUCCEEDED(activated) && unknown) {
            result = unknown->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(&client));
        }
        if (unknown) {
            unknown->Release();
        }
        SetEvent(done);
        return S_OK;
    }

private:
    std::atomic<ULONG> refs{1};
};

// Captures what this process plays (the game's sound) and queues it for the videos.
void AudioCapture() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = GetCurrentProcessId();
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT activation{};
    activation.vt = VT_BLOB;
    activation.blob.cbSize = sizeof(params);
    activation.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
    ActivateHandler handler;
    IActivateAudioInterfaceAsyncOperation* operation = nullptr;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
                                             &activation, &handler, &operation);
    if (SUCCEEDED(hr)) {
        WaitForSingleObject(handler.done, 10'000);
        hr = handler.result;
    }
    if (operation) {
        operation->Release();
    }
    IAudioClient* client = handler.client;
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = AudioChannels;
    format.nSamplesPerSec = AudioRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = AudioFrameBytes;
    format.nAvgBytesPerSec = AudioRate * AudioFrameBytes;
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    IAudioCaptureClient* capture = nullptr;
    if (SUCCEEDED(hr) && client) {
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                2'000'000, 0, &format, nullptr);
    }
    if (SUCCEEDED(hr)) hr = client->SetEventHandle(ready);
    if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture));
    if (SUCCEEDED(hr)) hr = client->Start();
    if (FAILED(hr) || !client) {
        std::fprintf(stderr, "[patch] GTS-015: cannot capture the game's sound, videos without sound (0x%08lx)\n", hr);
        CoUninitialize();
        return;
    }
    g_audio_ok = true;
    std::fprintf(stderr, "[patch] GTS-015 game sound captured for the videos\n");
    for (;;) {
        WaitForSingleObject(ready, 100);
        UINT32 packet = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data = nullptr;
            UINT32 count = 0;
            DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &count, &flags, nullptr, nullptr))) {
                break;
            }
            AudioChunk chunk;
            chunk.pcm.resize(static_cast<size_t>(count) * AudioFrameBytes);
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data) {
                std::memcpy(chunk.pcm.data(), data, chunk.pcm.size());
            }
            chunk.time = Now() - static_cast<LONGLONG>(count) * 10'000'000 / AudioRate;
            capture->ReleaseBuffer(count);
            std::scoped_lock lock{g_mutex};
            if (g_audio.size() < 2000) { // the worker takes them with each frame
                g_audio.push_back(std::move(chunk));
            }
        }
    }
}

void Worker() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    const int minutes = ClipMinutes();
    const auto clip_folder = minutes > 0 ? ClipFolder() : std::filesystem::path{};
    if (!clip_folder.empty()) {
        std::error_code error;
        std::filesystem::remove_all(clip_folder, error); // pieces of an earlier game
        std::filesystem::create_directories(clip_folder, error);
        std::fprintf(stderr, "[patch] GTS-015 clip buffer: the last %d minutes (hold Share or F10 to save)\n",
                     minutes);
    }
    std::deque<std::filesystem::path> pieces;
    u64 piece_number = 0;
    Mp4Writer piece;
    LONGLONG piece_start = 0;
    Mp4Writer recording;
    bool recording_failed = false;
    bool recording_manual = false;

    const auto close_piece = [&] {
        if (auto closed = piece.Close(); !closed.empty()) {
            pieces.push_back(std::move(closed));
            while (pieces.size() > static_cast<size_t>(minutes) + 1) {
                std::error_code error;
                std::filesystem::remove(pieces.front(), error);
                pieces.pop_front();
            }
        }
    };

    for (;;) {
        VideoFrame frame;
        bool have_frame = false;
        std::deque<AudioChunk> audio;
        {
            std::unique_lock lock{g_mutex};
            g_cv.wait_for(lock, std::chrono::milliseconds{250}, [] { return !g_queue.empty(); });
            if (!g_queue.empty()) {
                frame = std::move(g_queue.front());
                g_queue.pop_front();
                have_frame = true;
            }
            audio.swap(g_audio);
        }
        for (const auto& chunk : audio) {
            if (piece.IsOpen()) {
                piece.WriteAudio(chunk);
            }
            if (recording.IsOpen()) {
                recording.WriteAudio(chunk);
            }
        }
        const bool manual = g_manual.load();
        const bool record = (g_replay_running.load() && !g_replay_stopped.load()) || manual;
        if (recording.IsOpen() && manual != recording_manual) {
            recording.Close(); // a replay recording ended by Share, then a new one started
        }
        if (!record) {
            recording.Close();
            recording_failed = false;
        }
        if (g_save_clip.exchange(false)) {
            close_piece();
            std::vector<std::filesystem::path> copies;
            for (const auto& source : pieces) { // copies, so the buffer can drop them meanwhile
                auto copy = source;
                copy += L".save";
                std::error_code error;
                if (std::filesystem::copy_file(source, copy,
                                               std::filesystem::copy_options::overwrite_existing, error)) {
                    copies.push_back(std::move(copy));
                }
            }
            std::thread{SaveClip, std::move(copies)}.detach();
        }
        if (!have_frame) {
            continue;
        }
        const u32 width = frame.width & ~1u; // H.264 wants even sizes
        const u32 height = frame.height & ~1u;
        if (width >= 16 && height >= 16) {
            if (!clip_folder.empty()) {
                if (piece.IsOpen() &&
                    (piece.width != width || piece.height != height || frame.time - piece_start >= SegmentLength)) {
                    close_piece();
                }
                if (!piece.IsOpen()) {
                    piece_start = frame.time;
                    piece.Open(clip_folder / (L"piece-" + std::to_wstring(piece_number++) + L".mp4"),
                               width, height, piece_start, true);
                }
                if (piece.IsOpen()) {
                    piece.Write(frame);
                }
            }
            if (record) {
                if (recording.IsOpen() && (recording.width != width || recording.height != height)) {
                    recording.Close(); // the game changed resolution: continue in a new file
                }
                if (!recording.IsOpen() && !recording_failed) {
                    recording_manual = manual;
                    const auto path = NewVideoPath(manual ? L"Video" : L"Replay");
                    recording_failed = path.empty() || !recording.Open(path, width, height, frame.time, false);
                    if (recording_failed) {
                        std::fprintf(stderr, "[patch] GTS-015: cannot record\n");
                    }
                }
                if (recording.IsOpen()) {
                    recording.Write(frame);
                }
            }
        }
        std::scoped_lock lock{g_mutex};
        if (g_spare.size() < MaxQueuedFrames) {
            g_spare.push_back(std::move(frame.bgra));
        }
    }
}

} // namespace

// From NoteProjectScript: whether the race_replay project (replay playback) runs.
extern "C" void RytyReplayVideoSetReplay(bool replay) {
    if (!Enabled() || g_replay_running.exchange(replay) == replay) {
        return;
    }
    g_replay_stopped = false;
    std::fprintf(stderr, "[patch] GTS-015 replay %s\n",
                 replay ? "playing: recording it to the Videos folder" : "ended");
}

// A short press of Share/Create or F9: starts or stops a recording. While a replay records by
// itself, the first press stops that recording, so each press starts or ends a video.
extern "C" void RytyReplayVideoToggle(const char* source) {
    if (!Enabled()) {
        return;
    }
    if (g_replay_running.load() && !g_replay_stopped.load() && !g_manual.load()) {
        g_replay_stopped = true;
        std::fprintf(stderr, "[patch] GTS-015 %s: replay recording stopped\n", source);
        return;
    }
    const bool manual = !g_manual.load();
    g_manual = manual;
    std::fprintf(stderr, "[patch] GTS-015 %s: recording %s\n", source, manual ? "started" : "stopped");
}

// Holding Share/Create or F10: saves the clip buffer.
extern "C" void RytyClipSave(const char* source) {
    if (!Enabled() || ClipMinutes() == 0) {
        return;
    }
    std::fprintf(stderr, "[patch] GTS-015 %s: saving the last %d minutes as a clip\n", source, ClipMinutes());
    g_save_clip = true;
    g_cv.notify_one();
}

extern "C" bool RytyReplayVideoWanted() {
    if (!Enabled()) {
        return false;
    }
    static bool f9_was_down = false;
    static bool f10_was_down = false;
    DWORD process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    const bool focused = process == GetCurrentProcessId();
    const bool f9 = focused && (GetAsyncKeyState(VK_F9) & 0x8000);
    const bool f10 = focused && (GetAsyncKeyState(VK_F10) & 0x8000);
    if (f9 && !f9_was_down) {
        RytyReplayVideoToggle("F9");
    }
    if (f10 && !f10_was_down) {
        RytyClipSave("F10");
    }
    f9_was_down = f9;
    f10_was_down = f10;
    return ClipMinutes() > 0 || g_replay_running.load(std::memory_order_relaxed) ||
           g_manual.load(std::memory_order_relaxed);
}

// GTS-019: "Export this video" in the Replays menu. The gallery script creates
// /APP_DATA/video_export and plays the replay; it is recorded like any replay (GTS-015) while the
// screen shows only "Exporting video" (the presenter draws that over the swapchain, after the
// frame copy, so the video stays clean). The script removes the folder when the gallery opens
// again. Returns 0 (no export), 1 (exporting; `seconds` = time so far) or 2 ("Video exported",
// shown for 5 s after an export).
extern "C" int RytyVideoExportState(int* seconds) {
    using clock = std::chrono::steady_clock;
    static std::wstring marker = [] {
        wchar_t exe[MAX_PATH];
        if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) {
            return std::wstring{};
        }
        std::wstring base{exe};
        base.resize(base.find_last_of(L"\\/") + 1);
        return base + L"download0\\APP_DATA\\video_export";
    }();
    static clock::time_point started{}, finished{};
    static bool exporting = false;
    static clock::time_point checked{};
    static bool present = false;
    const auto now = clock::now();
    if (!Enabled() || marker.empty()) {
        return 0;
    }
    if (now - checked > std::chrono::milliseconds(250)) {
        checked = now;
        present = GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    const bool active = present && g_replay_running.load(std::memory_order_relaxed);
    if (active && !exporting) {
        exporting = true;
        started = now;
        std::fprintf(stderr, "[patch] GTS-019 exporting a replay to video\n");
    } else if (!active && exporting && !present) {
        exporting = false;
        finished = now;
        std::fprintf(stderr, "[patch] GTS-019 replay exported\n");
    }
    if (exporting) {
        if (seconds) {
            *seconds = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(now - started).count());
        }
        return 1;
    }
    if (present) {
        return 1; // the replay is loading
    }
    if (finished != clock::time_point{} && now - finished < std::chrono::seconds(5)) {
        return 2;
    }
    return 0;
}

// From the presenter, once the copy of a new game frame is done. `rgba` is width*height RGBA8.
extern "C" void RytyReplayVideoFrame(const u8* rgba, u32 width, u32 height) {
    if (!Enabled()) {
        return;
    }
    std::call_once(g_worker_once, [] {
        std::thread{AudioCapture}.detach();
        std::thread{Worker}.detach();
    });
    std::unique_lock lock{g_mutex};
    if (g_queue.size() >= MaxQueuedFrames) {
        if ((++g_dropped & 1023) == 1) {
            std::fprintf(stderr, "[patch] GTS-015: frames dropped (encoder too slow): %llu\n",
                         static_cast<unsigned long long>(g_dropped));
        }
        return;
    }
    VideoFrame frame;
    if (!g_spare.empty()) {
        frame.bgra = std::move(g_spare.back());
        g_spare.pop_back();
    }
    lock.unlock();
    const size_t pixels = static_cast<size_t>(width) * height;
    frame.bgra.resize(pixels * 4);
    for (size_t i = 0; i < pixels; ++i) { // RGBA -> BGRA
        frame.bgra[i * 4 + 0] = rgba[i * 4 + 2];
        frame.bgra[i * 4 + 1] = rgba[i * 4 + 1];
        frame.bgra[i * 4 + 2] = rgba[i * 4 + 0];
        frame.bgra[i * 4 + 3] = 255;
    }
    frame.width = width;
    frame.height = height;
    frame.time = Now();
    lock.lock();
    g_queue.push_back(std::move(frame));
    lock.unlock();
    g_cv.notify_one();
}
