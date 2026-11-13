#include "../TvRockOnTVTest.cpp"
#include <thread>
#include <cstdio>
#include <cstdlib>
#pragma comment(lib,"ole32.lib")
#pragma comment(lib,"strmiids.lib")

static DWORD mainThread;
static TVTest::EventCallbackFunc eventCallback;
static void *eventData;
static TVTest::StreamCallbackInfo stream = {};
static bool rejectStream = false;
static int recordCount = 0;
static bool closeRequested = false;
static int playbackViewerResets = 0;
static bool previewEnabled = true;
static int previewChanges = 0;
static DWORD supportedVersion = TVTEST_PLUGIN_VERSION_(0,0,10);
static WORD selectedService = 0;
static const wchar_t *currentDriver=L"BonDriver.dll";
static int channelChanges=0;
static bool statusSupport=false,serviceNameAvailable=false;
static bool graphEventSupport=false;
static bool programNameAvailable=false;
static const wchar_t *currentProgramName=L"テスト番組";
static int statusRegistrations=0;
static int currentSpace = 1;
static WORD bsTSID = 0x4ffe, csTSID = 0x8ffe;
static bool currentChannelAvailable = true;
static void Check(bool condition, const char *message) {
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
static LRESULT CALLBACK Host(TVTest::PluginParam *, UINT message, LPARAM p1, LPARAM p2) {
    Check(GetCurrentThreadId() == mainThread, "viewer API called outside main thread");
    switch (message) {
    case TVTest::MESSAGE_RESET:
        Check(p1==TVTest::RESET_ALL,"Playback resets viewer and driver state");
        ++playbackViewerResets;return TRUE;
    case TVTest::MESSAGE_SETPREVIEW:
        previewEnabled=p1!=0;++previewChanges;return TRUE;
    case TVTest::MESSAGE_QUERYMESSAGE:
        if(p1==TVTest::MESSAGE_SETPREVIEW)return TRUE;
        if(p1==TVTest::MESSAGE_QUERYEVENT)return graphEventSupport;
        return statusSupport && (p1==TvRockServiceItem::Register || p1==TvRockServiceItem::Notify);
    case TVTest::MESSAGE_QUERYEVENT:
        return graphEventSupport && (p1==TvRockPlaybackAPI::Initialized || p1==TvRockPlaybackAPI::Finalize);
    case TvRockServiceItem::Register: {
        auto info=reinterpret_cast<TvRockServiceItem::Info *>(p1);
        Check(info && info->Size==sizeof(*info) && info->ID==1 && info->Flags==1,"status registration ABI");
        Check(wcscmp(info->pszIDText,L"TvRockServiceName")==0,"status item identity");
        ++statusRegistrations;return TRUE;
    }
    case TVTest::MESSAGE_GETSERVICEINFO: {
        if(!serviceNameAvailable)return FALSE;
        auto info=reinterpret_cast<TVTest::ServiceInfo *>(p2);
        wcscpy_s(info->szServiceName,L"テスト放送");return TRUE;
    }
    case TVTest::MESSAGE_GETCURRENTPROGRAMINFO: {
        auto info=reinterpret_cast<TVTest::ProgramInfo *>(p1);
        Check(info->Size==sizeof(*info) && p2==0,"current program request");
        lstrcpynW(info->pszEventName,currentProgramName,info->MaxEventName);
        return programNameAvailable;
    }
    case TVTest::MESSAGE_SETEVENTCALLBACK:
        eventCallback = reinterpret_cast<TVTest::EventCallbackFunc>(p1);
        eventData = reinterpret_cast<void *>(p2);
        return TRUE;
    case TVTest::MESSAGE_SETSTREAMCALLBACK: {
        auto info = reinterpret_cast<TVTest::StreamCallbackInfo *>(p1);
        if (info->Flags & TVTest::STREAM_CALLBACK_REMOVE) { stream = {}; return TRUE; }
        if (rejectStream) return FALSE;
        stream = *info;
        return TRUE;
    }
    case TVTest::MESSAGE_GETHOSTINFO:
        reinterpret_cast<TVTest::HostInfo *>(p1)->SupportedPluginVersion = supportedVersion;
        return TRUE;
    case TVTest::MESSAGE_GETDRIVERNAME: lstrcpynW(reinterpret_cast<LPWSTR>(p1),currentDriver,static_cast<int>(p2)); return lstrlenW(currentDriver);
    case TVTest::MESSAGE_GETCHANNELINFO: {
        if (LOWORD(p2) > 2 || HIWORD(p2) != 0) return FALSE;
        auto info = reinterpret_cast<TVTest::ChannelInfo *>(p1);
        const int space = LOWORD(p2);
        info->NetworkID = space == 1 ? 4 : space == 2 ? 6 : 0;
        info->TransportStreamID = space == 1 ? bsTSID : space == 2 ? csTSID : 0x7f00;
        info->ServiceID = space == 2 ? 333 : 101;
        return TRUE;
    }
    case TVTest::MESSAGE_GETCURRENTCHANNELINFO: {
        if (!currentChannelAvailable) return FALSE;
        auto info = reinterpret_cast<TVTest::ChannelInfo *>(p1);
        info->Space = currentSpace;
        info->Channel = 14;
        info->TransportStreamID = currentSpace == 1 ? bsTSID : csTSID;
        return TRUE;
    }
    case TVTest::MESSAGE_SETCHANNEL:
        ++channelChanges;
        selectedService = HIWORD(p2);
        return TRUE;
    case TVTest::MESSAGE_SETSERVICE:
        selectedService = static_cast<WORD>(p1);
        return TRUE;
    case TVTest::MESSAGE_GETSTATUS: {
        auto info = reinterpret_cast<TVTest::StatusInfo *>(p1);
        info->BitRate = 20000000;
        info->SignalLevel = 30.0f;
        return TRUE;
    }
    case TVTest::MESSAGE_GETTUNINGSPACE:
        if (p1) *reinterpret_cast<int *>(p1) = 0;
        return -1;
    case TVTest::MESSAGE_GETSERVICE: return serviceNameAvailable?0:-1;
    case TVTest::MESSAGE_STARTRECORD: ++recordCount; return TRUE;
    case TVTest::MESSAGE_STOPRECORD: return TRUE;
    case TVTest::MESSAGE_CLOSE:
        Check(p1 == TVTest::CLOSE_EXIT, "power-off must exit viewer");
        closeRequested = true;
        return TRUE;
    case TVTest::MESSAGE_ADDLOG: return TRUE;
    default: return FALSE;
    }
}
static void PumpUntil(const std::function<bool()> &done) {
    const auto deadline = GetTickCount64() + 5000;
    while (!done() && GetTickCount64() < deadline) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        Sleep(1);
    }
    Check(done(), "operation timed out");
}
int wmain(int argc, wchar_t **argv) {
    mainThread = GetCurrentThreadId();
    Check(SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)),"initialize COM for graph test");
    {
        PlaybackGraph playback;
        Check(!playback.Command(10) && !playback.Command(11),"old host reports unsupported");
        playback.SetSupported(true);
        Check(playback.Command(10) && !playback.Command(11),"supported graph can be uninitialized");
        IGraphBuilder *graph=nullptr;
        Check(SUCCEEDED(CoCreateInstance(CLSID_FilterGraph,nullptr,CLSCTX_INPROC_SERVER,__uuidof(IGraphBuilder),reinterpret_cast<void **>(&graph))),"create DirectShow graph");
        playback.Initialized(graph);graph->Release();
        Check(playback.Command(11),"new graph is ready");
        Check(playback.Command(2),"DirectShow graph pauses");
        Check(playback.Command(3),"DirectShow graph resumes");
        Check(playback.Command(2) && playback.Command(4),"legacy fallback restores a paused graph");
        playback.Clear();
        Check(!playback.Command(11) && !playback.Command(2),"finalized graph cannot be controlled");
        CTvRockDTVTarget plugin;
        Check(plugin.GetVersion()==TVTEST_PLUGIN_VERSION_(0,0,10),"new SDK preserves old minimum ABI");
    }
    CoUninitialize();

    g_hinstDLL = GetModuleHandle(nullptr);
    HMODULE mock = LoadLibraryW(L"TvRockDTV.dll");
    Check(mock != nullptr, "load mock DLL");
    auto bytes = reinterpret_cast<DWORD (*)()>(GetProcAddress(mock, "TestBytes"));
    auto bitrate = reinterpret_cast<DWORD (*)()>(GetProcAddress(mock, "TestBitrate"));
    auto request = reinterpret_cast<TvRock::RockRequest *(*)()>(GetProcAddress(mock, "TestRequest"));
    Check(bytes && bitrate && request, "mock exports");
    TVTest::PluginParam param = {};
    param.Callback = Host;
    {
        TVTest::CTVTestApp app(&param);
        int captionResets=0;
        CTvRockRequest control(&app,nullptr,[&] {Check(GetCurrentThreadId()==mainThread,"Playback core reset on main thread");++captionResets;});
        Check(control.ChannelChange(0xfffffffeUL,1),"Playback pause callback");
        Check(captionResets==1 && playbackViewerResets==0,"pause preserves last viewer picture");
        Check(control.ChannelChange(0xfffffffeUL,0),"Playback resume/seek callback");
        Check(captionResets==2 && playbackViewerResets==1,"resume/seek resets core and viewer");
        Check(control.ChannelChange(0xfffffffdUL,2),"idle supported independently of graph SDK");
        Check(control.ChannelChange(0xfffffffeUL,4),"idle callback");
        Check(!previewEnabled && previewChanges==1,"idle disables preview");
        Check(control.ChannelChange(0xfffffffeUL,1),"pause while idle");
        Check(!previewEnabled && previewChanges==1,"pause does not reenable preview");
        Check(control.ChannelChange(0xfffffffeUL,0),"play after idle");
        Check(previewEnabled && previewChanges==2,"play restores preview disabled by idle");
        Check(control.ChannelChange(0xfffffffeUL,0),"ordinary seek");
        Check(previewChanges==2,"ordinary seeks do not toggle preview");
        Check(!control.ChannelChange(0xfffffffeUL,2),"old callback does not acknowledge direct pause");
        int graphActions=0;
        CTvRockRequest modern(&app,nullptr,{},[&](DWORD action) { Check(GetCurrentThreadId()==mainThread,"graph control on main thread"); ++graphActions; return action==10 || action==11 || action==2 || action==3; });
        Check(modern.ChannelChange(0xfffffffdUL,0),"query graph support");
        Check(modern.ChannelChange(0xfffffffdUL,1),"query graph readiness");
        Check(modern.ChannelChange(0xfffffffeUL,2),"request direct pause");
        Check(modern.ChannelChange(0xfffffffeUL,3),"request direct resume");
        Check(graphActions==4,"graph actions dispatched");
        CTvRockRequest unsupported(&app,nullptr);
        Check(!unsupported.ChannelChange(0xfffffffeUL,0),"unsupported host does not acknowledge Playback");
    }

    { TVTest::CTVTestApp app(&param);WCHAR text[96];
      ServiceItemText(app,text,96,false);Check(wcscmp(text,L"サービス待ち")==0,"waiting service label");
      serviceNameAvailable=true;
      ServiceItemText(app,text,96,false);Check(wcscmp(text,L"テスト放送：　")==0,"unavailable program stays blank");
      programNameAvailable=true;
      ServiceItemText(app,text,96,false);Check(wcscmp(text,L"テスト放送：　テスト番組")==0,"current program name");
      currentProgramName=L"次の番組";
      ServiceItemText(app,text,96,false);Check(wcscmp(text,L"テスト放送：　次の番組")==0,"program name updates");
      currentProgramName=L"";
      ServiceItemText(app,text,96,false);Check(wcscmp(text,L"テスト放送：　")==0,"empty program stays blank");
      currentProgramName=L"テスト番組";programNameAvailable=false;
      ServiceItemText(app,text,96,false);Check(wcscmp(text,L"テスト放送：　")==0,"previous program cleared on failure");
      serviceNameAvailable=false;
      ServiceItemText(app,text,96,true);Check(wcscmp(text,L"放送局：　番組名")==0,"preview service label");
    }

    {
        wchar_t ini[MAX_PATH];
        Check(GetFullPathNameW(L"channel-test.ini", MAX_PATH, ini, nullptr) != 0, "INI path");
        Check(WritePrivateProfileStringW(L"DID_A", L"TerrestrialIndex", L"0", ini), "write test INI");
        Check(WritePrivateProfileStringW(L"DID_A", L"BSIndex", L"1", ini), "write BS index");
        Check(WritePrivateProfileStringW(L"DID_A", L"CSIndex", L"2", ini), "write CS index");
        TVTest::CTVTestApp app(&param);
        CChannelSearch channels(&app);
        channels.Init(ini, L"DID_A");
        Check(channels.SetChannel(1, 101) && selectedService == 101, "default terrestrial service");
        channels.SetChannel(8, 102);
        Check(channels.SetChannel(1, 101) && selectedService == 102, "sub-service selection");
        Check(channels.SetChannel(1, 101) && selectedService == 101, "consume sub-service once");
        supportedVersion = TVTEST_PLUGIN_VERSION_(0,0,7);
        channels.Init(ini, L"DID_A");
        Check(channels.SetChannel(1, 101) && selectedService == 101, "legacy service selection");
        const DWORD versions[] = {TVTEST_PLUGIN_VERSION_(0,0,10), TVTEST_PLUGIN_VERSION_(0,0,7)};
        for (DWORD version : versions) {
            supportedVersion = version;
            channels.Init(ini, L"DID_A");
            Check(channels.SetChannel(2, 101) && selectedService == 101, "default BS service");
            channels.SetChannel(8, 102);
            Check(channels.SetChannel(2, 101) && selectedService == 102, "BS sub-service selection");
            Check(channels.SetChannel(2, 101) && selectedService == 101, "consume BS sub-service once");
            channels.SetChannel(8, 102);
            Check(!channels.SetChannel(2, 999), "missing BS parent fails");
            Check(channels.SetChannel(2, 101) && selectedService == 101, "failed BS request consumes sub-service");
            Check(channels.SetChannel(3, 333) && selectedService == 333, "CS selection remains unchanged");
            Check(channels.SetChannel(1, 101) && selectedService == 101, "terrestrial selection after BS");
        }
        currentSpace = 1;
        Check(channels.GetChannel() == (2u << 16 | bsTSID), "BS notification without frequency table");
        currentSpace = 2;
        Check(channels.GetChannel() == (3u << 16 | csTSID), "CS notification without frequency table");
        bsTSID = 0;
        currentSpace = 1;
        Check(channels.GetChannel() == (2u << 16), "BS band survives missing scanned TSID");
        bsTSID = 0xffff;
        Check(channels.GetChannel() == (2u << 16), "invalid TSID becomes unknown");
        currentSpace = 0;
        Check(channels.GetChannel() == (1u << 16 | 27u), "terrestrial notification includes band");
        currentChannelAvailable = false;
        Check(channels.GetChannel() == 0, "no current channel notification");
        currentChannelAvailable = true;
        bsTSID = 0x4ffe;
        supportedVersion = TVTEST_PLUGIN_VERSION_(0,0,10);
        DeleteFileW(ini);
    }
    statusSupport=true;
    { CTvRockDTVTarget legacy;legacy.SetPluginParam(&param);
      Check(legacy.Initialize(),"initialize on old event API");legacy.Finalize(); }
    statusRegistrations=0;
    graphEventSupport=true;
    CTvRockDTVTarget plugin;
    plugin.SetPluginParam(&param);
    Check(plugin.Initialize(), "initialize plugin without DID");
      Check(statusRegistrations==1,"register service status item on supported viewer");
    auto execute = [&](LPCWSTR command) {
        eventCallback(TVTest::EVENT_EXECUTE, reinterpret_cast<LPARAM>(command), 0, eventData);
    };
    execute(L"TVTest.exe /DID AB");
    Check(!stream.Callback && !request(), "reject multi-character DID");
    execute(L"TVTest.exe /DID");
    Check(!request(), "reject missing DID");
    rejectStream = true;
    execute(L"TVTest.exe /DID A");
    Check(!request() && !stream.Callback, "clean up failed stream registration");
    rejectStream = false;
    for (int n = 0; n < 3; ++n) {
        execute(n % 2 ? L"TVTest.exe /DID B" : L"TVTest.exe /DID A");
        Check(stream.Callback && request(), "initialize or reinitialize streaming");
        DWORD before = bytes();
        BYTE packet[188] = {0x47};
        for (int i = 0; i < 12000; ++i) stream.Callback(packet, stream.pClientData);
        PumpUntil([&] { return bytes() >= before + 12000 * 188; });
        PumpUntil([&] { return bitrate() == 20000000; });
        auto remote = request();
        std::atomic<bool> done{false};
        bool result = false;
        std::thread worker([&] {
            wchar_t path[] = L"test.ts";
            result = remote->RecordStart(path);
            done = true;
        });
        PumpUntil([&] { return done.load(); });
        worker.join();
        Check(result, "record result returned to worker");
        eventCallback(TVTest::EVENT_CHANNELCHANGE, 0, 0, eventData);
    }
    Check(recordCount == 3, "record requests dispatched exactly once");
    for(const wchar_t *driver:{L"BonDriver_UDP.dll",L"BonDriver_SRT.dll"}) {
        currentDriver=driver;const int before=channelChanges;
        Check(request()->ChannelChange(0,555)&&selectedService==555,"network service selection");
        Check(channelChanges==before,"network service selection must preserve listener channel");
    }
    currentDriver=L"BonDriver.dll";
    {
        Check(SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)),"initialize COM for event path");
        auto capabilities=reinterpret_cast<DWORD (*)()>(GetProcAddress(mock,"TestCapabilities"));
        Check(capabilities!=nullptr,"capability report mock");
        PumpUntil([&] {return capabilities()==1;});
        IGraphBuilder *graph=nullptr;
        Check(SUCCEEDED(CoCreateInstance(CLSID_FilterGraph,nullptr,CLSCTX_INPROC_SERVER,__uuidof(IGraphBuilder),reinterpret_cast<void **>(&graph))),"create event graph");
        TvRockPlaybackAPI::GraphInfo info={};info.pGraphBuilder=graph;
        eventCallback(TvRockPlaybackAPI::Initialized,reinterpret_cast<LPARAM>(&info),0,eventData);
        graph->Release();
        PumpUntil([&] {return capabilities()==3;});
        Check(request()->ChannelChange(0xfffffffeUL,2),"event graph direct pause");
        Check(request()->ChannelChange(0xfffffffeUL,0),"legacy reset restores direct-paused graph");
        IMediaControl *media=nullptr;
        Check(SUCCEEDED(info.pGraphBuilder->QueryInterface(__uuidof(IMediaControl),reinterpret_cast<void **>(&media))),"get graph after reset");
        OAFilterState graphState=State_Stopped;
        Check(SUCCEEDED(media->GetState(0,&graphState)) && graphState==State_Running,"fallback reset leaves viewer running");
        media->Release();
        Check(request()->ChannelChange(0xfffffffeUL,2),"pause after legacy recovery");
        Check(request()->ChannelChange(0xfffffffeUL,3),"event graph direct resume");
        eventCallback(TvRockPlaybackAPI::Finalize,0,0,eventData);
        Check(!request()->ChannelChange(0xfffffffeUL,2),"released event graph rejects pause");
        PumpUntil([&] {return capabilities()==1;});
        CoUninitialize();
    }
    std::atomic<bool> poweredOff{false};
    std::thread power([&] { request()->PowerOff(); poweredOff = true; });
    PumpUntil([&] { return poweredOff.load() && closeRequested; });
    power.join();
    // Stop while a DLL callback is waiting for the main-thread dispatcher.
    auto remote = request();
    std::atomic<bool> started{false};
    bool stoppedResult = true;
    std::thread pending([&] { started = true; stoppedResult = remote->RecordStop(); });
    while (!started) Sleep(1);
    Sleep(20);
    plugin.Finalize();
    pending.join();
    Check(!stoppedResult && !request() && !stream.Callback, "cancel callback during shutdown");
    plugin.Finalize();
    execute(L"TVTest.exe /DID C");
    Check(request(), "restart after finalization");
    execute(L"TVTest.exe /d BonDriver.dll");
    Check(!request(), "driver-only command stops DTV");
    plugin.Finalize();
    FreeLibrary(mock);
    if (argc > 1) {
        HMODULE actual = LoadLibraryW(argv[1]);
        Check(actual != nullptr, "load actual Rust-backed DLL");
        auto create = reinterpret_cast<TvRock::TvRockDTV *(*)(HINSTANCE)>(GetProcAddress(actual, TRDTV_MODULE_FUNC));
        Check(create != nullptr, "actual DLL factory export");
        auto object = create(actual);
        Check(object && strcmp(object->GetClassName(), "TvRockDTV_V100") == 0, "actual DLL C++ ABI");
        Check(!object->LaterThan(static_cast<LPCSTR>(nullptr)), "null class comparison");
        object->Dispose();
        delete object;
        FreeLibrary(actual);
    }
    puts("PASS: lifecycle, TS queue, main-thread callbacks, bitrate, shutdown, DLL ABI");
}
