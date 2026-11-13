#include "TvRockDTV.h"
#include <atomic>
using namespace TvRock;
static std::atomic<DWORD> bytes{0};
static std::atomic<DWORD> bitrate{0};
static RockRequest *request;
class Status : public RockStatus {
public:
    DWORD GetStatusSmartCard() const override { return 0; }
    void SetStatusSmartCard(DWORD) override {}
    float GetStatusSignalLevel() const override { return 0; }
    void SetStatusSignalLevel(float) override {}
    DWORD GetStatusBitrate() const override { return bitrate; }
    void SetStatusBitrate(DWORD value) override { bitrate = value; }
    DWORD GetStatusErrorPacket() const override { return 0; }
    void SetStatusErrorPacket(DWORD) override {}
    DWORD GetStatusScramblingPacket() const override { return 0; }
    void SetStatusScramblingPacket(DWORD) override {}
};
class MockDtv : public TvRockDTV {
    Status status;
public:
    bool Initialize(DWORD, RockRequest *value) override { request = value; return true; }
    void Dispose() override { request = nullptr; }
    bool ProcessTS(BYTE *, DWORD length) override { bytes += length; return true; }
    bool OnStreamChange(DWORD, DWORD) override { return true; }
    void GetBroadCastTime(SYSTEMTIME *) override {}
    RockProperty *GetProperty() override { return nullptr; }
    RockStatus *GetStatus() override { return &status; }
    void Verbose(BOOL) override {}
    void Trace(LPCWSTR, ...) override {}
    void Trace(LPCSTR, ...) override {}
};
extern "C" __declspec(dllexport) TvRockDTV *CreateTvRockDTV(HINSTANCE) { return new MockDtv; }
extern "C" __declspec(dllexport) DWORD TestBytes() { return bytes; }
extern "C" __declspec(dllexport) DWORD TestBitrate() { return bitrate; }
extern "C" __declspec(dllexport) RockRequest *TestRequest() { return request; }

static std::atomic<DWORD> playbackCapabilities{0};
extern "C" __declspec(dllexport) void TvRockDTVPlaybackCapabilities(TvRockDTV *,BOOL supported,BOOL ready) { playbackCapabilities=(supported?1:0)|(ready?2:0); }
extern "C" __declspec(dllexport) DWORD TestCapabilities() { return playbackCapabilities; }
