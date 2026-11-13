// TvRock DTV Target.
#include <windows.h>
#include <shlwapi.h>
#include <dshow.h>
#include <tchar.h>
#include <vector>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <strsafe.h>
#define TVTEST_PLUGIN_VERSION TVTEST_PLUGIN_VERSION_(0,0,10)
#define TVTEST_PLUGIN_CLASS_IMPLEMENT	// プラグインをクラスとして実装
#include "TVTestPlugin.h"
#include "TvRockDTV.h"

#pragma comment(lib,"shlwapi.lib")

// TVTest SDK 0.0.14のstatus-item ABI。旧来のプラグインAPIを維持するため独立させている。
// TVTestPlugin.h（自由に再配布・改変可能）に基づく
namespace TvRockServiceItem {
enum : UINT { Register=96, Notify=99, DrawEvent=37, NotifyEvent=38 };
enum : int { ItemID=1 };
#pragma pack(push,1)
struct Info {
    DWORD Size,Flags,Style; int ID; LPCWSTR pszIDText,pszName;
    int MinWidth,MaxWidth,DefaultWidth,MinHeight;
};
struct DrawInfo {
    int ID; WORD Flags,State; LPCWSTR pszStyle; HDC hdc;
    RECT ItemRect,DrawRect; COLORREF Color;
};
struct EventInfo { int ID; UINT Event; LPARAM Param; };
#pragma pack(pop)
}

static void ServiceItemText(TVTest::CTVTestApp &app, WCHAR *text, size_t count, bool preview) {
    StringCchCopyW(text,count,preview?L"放送局：　番組名":L"サービス待ち");
    TVTest::ServiceInfo service={};
    const int index=app.GetService();
    if(index>=0 && app.GetServiceInfo(index,&service) && service.szServiceName[0]) {
        WCHAR eventName[512]={};
        TVTest::ProgramInfo program={};
        program.pszEventName=eventName;
        program.MaxEventName=static_cast<int>(sizeof(eventName)/sizeof(eventName[0]));
        const bool available=app.GetCurrentProgramInfo(&program);
        StringCchPrintfW(text,count,L"%ls：　%ls",service.szServiceName,available?eventName:L"");
    }
}

using namespace TvRock;

#define ArrayLength(a) (sizeof(a) / sizeof(a[0]))

// ファイルの拡張子
#define FILE_EXTENSION	L".ts"

// 地デジの物理チャンネルの範囲
#define TERRESTRIAL_CHANNEL_FIRST	13
#define TERRESTRIAL_CHANNEL_LAST	63

// 地デジのTSIDか?
inline bool IsTerrestrialTSID(WORD TSID) {
	return (TSID & 0xFF00) == 0x7F00 || (TSID & 0xFF00) == 0x7E00;
}
// BSのNetworkIDか?
inline bool IsBsNetworkID(WORD NetworkID) {
	return NetworkID == 4;
}
// BSのTSIDか?
inline bool IsBsTSID(WORD TSID) {
	return (TSID & 0xF000) == 0x4000;
}
// CSのNetworkIDか?
inline bool IsCsNetworkID(WORD NetworkID) {
	return NetworkID >= 6 && NetworkID <= 10;
}
// CSのTSIDか?
inline bool IsCsTSID(WORD TSID) {
	return !IsBsTSID(TSID) && !IsTerrestrialTSID(TSID);
}

// チャンネル
class CChannelSearch
{

	enum TuningSpaceType {
		SPACE_ERROR = -1,
		SPACE_UNKNOWN = 0,
		SPACE_TERRESTRIAL,
		SPACE_BS,
		SPACE_110CS
	};

	TVTest::CTVTestApp *m_pApp;
	bool m_fInitialized;
	bool m_fSetChannelServiceSupported;
	int m_TerrestrialIndex;
	int m_TerrestrialIndex2;
	int m_BSIndex;
	int m_CSIndex;
	DWORD m_SubService;
	bool m_fTerraIndexSpecified;
	bool m_fTerraIndex2Specified;
	bool m_fBSIndexSpecified;
	bool m_fCSIndexSpecified;

	// チューニング空間のインデックスの取得
	void GetSpaceIndex() {
		if (m_fInitialized)
			return;

		bool fFound;
		WCHAR szLog[256];
		TVTest::ChannelInfo ChannelInfo;

		int NumSpaces = 0;
		m_pApp->GetTuningSpace(&NumSpaces);

		// 地デジのチューニング空間の取得
		if (m_TerrestrialIndex >= 0) {
			fFound = true;
			if (!m_fTerraIndexSpecified
					&& (!m_pApp->GetChannelInfo(m_TerrestrialIndex, 0, &ChannelInfo)
						|| !IsTerrestrialTSID(ChannelInfo.TransportStreamID))) {
				fFound = false;
				for (int i = 0; i < NumSpaces; i++) {
					if (m_pApp->GetChannelInfo(i, 0, &ChannelInfo)
							&& IsTerrestrialTSID(ChannelInfo.TransportStreamID)) {
						m_TerrestrialIndex = i;
						fFound = true;
						break;
					}
				}
			}
			::StringCchPrintfW(szLog, ArrayLength(szLog),
						fFound ? L"地デジのチューニング空間を取得しました。(%d)":
						         L"地デジのチューニング空間が取得できないため、%d に仮定しました。",
						m_TerrestrialIndex);
			m_pApp->AddLog(szLog);
			if (fFound && !m_fTerraIndex2Specified
					&& m_pApp->GetChannelInfo(m_TerrestrialIndex + 1, 0, &ChannelInfo)
					&& IsTerrestrialTSID(ChannelInfo.TransportStreamID)) {
				m_TerrestrialIndex2 = m_TerrestrialIndex + 1;
			}
			if (m_TerrestrialIndex2 >= 0) {
				::StringCchPrintfW(szLog, ArrayLength(szLog), L"地デジ2のチューニング空間を取得しました。(%d)",
							m_TerrestrialIndex2);
				m_pApp->AddLog(szLog);
			}
		}

		// BSのチューニング空間の取得
		if (m_BSIndex >= 0) {
			fFound = true;
			if (!m_fBSIndexSpecified
					&& (!m_pApp->GetChannelInfo(m_BSIndex, 0, &ChannelInfo)
						|| (ChannelInfo.NetworkID != 0
							&& !IsBsNetworkID(ChannelInfo.NetworkID))
						|| !IsBsTSID(ChannelInfo.TransportStreamID))) {
				// まず最初のチャンネルのみ調べる
				fFound = false;
				for (int i = 0; i < NumSpaces; i++) {
					if (m_pApp->GetChannelInfo(i, 0, &ChannelInfo)
							&& (IsBsNetworkID(ChannelInfo.NetworkID)
								|| IsBsTSID(ChannelInfo.TransportStreamID))) {
						m_BSIndex = i;
						fFound = true;
						break;
					}
				}
				// 見付からない場合、全てのチャンネルを調べる
				if (!fFound) {
					for (int Space = 0; !fFound && Space < NumSpaces; Space++) {
						for (int i = 1; m_pApp->GetChannelInfo(Space, i, &ChannelInfo); i++) {
							if (IsBsNetworkID(ChannelInfo.NetworkID)
									|| IsBsTSID(ChannelInfo.TransportStreamID)) {
								m_BSIndex = Space;
								fFound = true;
								break;
							}
						}
					}
				}
			}
			::StringCchPrintfW(szLog, ArrayLength(szLog),
						fFound ? L"BSのチューニング空間を取得しました。(%d)":
						         L"BSのチューニング空間が取得できないため、%d に仮定しました。",
						m_BSIndex);
			m_pApp->AddLog(szLog);
		}

		// CSのチューニング空間の取得
		if (m_CSIndex >= 0) {
			fFound = true;
			if (!m_fCSIndexSpecified
					&& (!m_pApp->GetChannelInfo(m_CSIndex, 0 ,&ChannelInfo)
						|| (ChannelInfo.NetworkID != 0
							&& !IsCsNetworkID(ChannelInfo.NetworkID))
						|| !IsCsTSID(ChannelInfo.TransportStreamID))) {
				// まず最初のチャンネルのみ調べる
				fFound = false;
				for (int i = 0; i < NumSpaces; i++) {
					if (m_pApp->GetChannelInfo(i, 0, &ChannelInfo)
							&& (IsCsNetworkID(ChannelInfo.NetworkID)
								|| IsCsTSID(ChannelInfo.TransportStreamID))) {
						m_CSIndex = i;
						fFound = true;
						break;
					}
				}
				// 見付からない場合、全てのチャンネルを調べる
				if (!fFound) {
					for (int Space = 0; !fFound && Space < NumSpaces; Space++) {
						for (int i = 1; m_pApp->GetChannelInfo(Space, i, &ChannelInfo); i++) {
							if (IsCsNetworkID(ChannelInfo.NetworkID)
									|| IsCsTSID(ChannelInfo.TransportStreamID)) {
								m_CSIndex = Space;
								fFound = true;
								break;
							}
						}
					}
				}
			}
			::StringCchPrintfW(szLog, ArrayLength(szLog),
						fFound ? L"CSのチューニング空間を取得しました。(%d)":
						         L"CSのチューニング空間が取得できないため、%d に仮定しました。",
						m_CSIndex);
			m_pApp->AddLog(szLog);
		}

		m_fInitialized = true;
	}

	// 視聴アプリのチャンネル変更
	bool SetChannel(int Space, int Channel, WORD ServiceID) {
		if (m_fSetChannelServiceSupported) {
			return m_pApp->SetChannel(Space, Channel, ServiceID);
		} else {
			return m_pApp->SetChannel(Space, Channel)
				&& m_pApp->SetService(ServiceID, true);
		}
	}

	// サービスIDの一致するチャンネルを探す
	int FindChannelByServiceID(int Space, WORD ServiceID) {
		TVTest::ChannelInfo ChannelInfo;

		for (int i = 0; m_pApp->GetChannelInfo(Space, i, &ChannelInfo); i++) {
			if (ChannelInfo.ServiceID == ServiceID)
				return i;
		}
		return -1;
	}

public:
	CChannelSearch(TVTest::CTVTestApp *pApp)
		: m_pApp(pApp)
		, m_fInitialized(false)
		, m_fSetChannelServiceSupported(false)
		, m_TerrestrialIndex(-1)
		, m_TerrestrialIndex2(-1)
		, m_BSIndex(-1)
		, m_CSIndex(-1)
		, m_SubService(-1)
		, m_fTerraIndexSpecified(false)
		, m_fTerraIndex2Specified(false)
		, m_fBSIndexSpecified(false)
		, m_fCSIndexSpecified(false)
	{
	}

	void Init(LPCTSTR pszIniFileName, LPCTSTR pszSectionName) {
		// プラグインバージョン 0.0.8 以降ではチャンネルとサービスIDを同時に指定できる
		TVTest::HostInfo HostInfo;
		if (m_pApp->GetHostInfo(&HostInfo))
			m_fSetChannelServiceSupported = HostInfo.SupportedPluginVersion >= TVTEST_PLUGIN_VERSION_(0,0,8);

		// 設定読み込み
		m_TerrestrialIndex = ::GetPrivateProfileInt(pszSectionName, TEXT("TerrestrialIndex"), -1, pszIniFileName);
		m_fTerraIndexSpecified = m_TerrestrialIndex >= 0;
		if (m_fTerraIndexSpecified) {
			m_TerrestrialIndex2 = ::GetPrivateProfileInt(pszSectionName, TEXT("TerrestrialIndex2"), -1, pszIniFileName);
			m_fTerraIndex2Specified = m_TerrestrialIndex2 >= 0;
		}
		m_BSIndex = ::GetPrivateProfileInt(pszSectionName, TEXT("BSIndex"), -1, pszIniFileName);
		m_fBSIndexSpecified = m_BSIndex >= 0;
		m_CSIndex = GetPrivateProfileInt(pszSectionName, TEXT("CSIndex"), -1, pszIniFileName);
		m_fCSIndexSpecified = m_CSIndex >= 0;

	}

	void Invalidate() {
		m_fInitialized = false;
		if (!m_fTerraIndex2Specified)
			m_TerrestrialIndex2 = -1;
	}

	void SetProperty(RockProperty *pProp) {
		int i = 0;

		// チューニング空間のインデックスを割り当てる(この段階ではまだ仮)
		if (pProp->UseTS()) {
			if (!m_fTerraIndexSpecified)
				m_TerrestrialIndex = i++;
			else
				i = m_TerrestrialIndex + 1;
		} else {
			m_TerrestrialIndex = -1;
		}
		if (pProp->UseBS()) {
			if (!m_fBSIndexSpecified)
				m_BSIndex = i++;
			else
				i = m_BSIndex + 1;
		} else {
			m_BSIndex = -1;
		}
		if (pProp->UseCS()) {
			if (!m_fCSIndexSpecified)
				m_CSIndex = i++;
		} else {
			m_CSIndex = -1;
		}
	}

	// TvRockからのチャンネル変更要求の処理
	bool SetChannel(const DWORD dwChannel, const DWORD dwService) {
		if (!m_fInitialized)
			GetSpaceIndex();

		if (dwChannel == 0x08) {
			m_SubService = dwService;
			return false;
		}

		const WORD service = static_cast<WORD>(m_SubService == DWORD(-1) ? dwService : m_SubService);
		m_SubService = -1;

		if (m_TerrestrialIndex >= 0 && dwChannel == SPACE_TERRESTRIAL) {
			// 地上
			int SpaceIndex[2];
			int NumSpaces = 0;

			if (m_TerrestrialIndex >= 0) {
				SpaceIndex[NumSpaces++] = m_TerrestrialIndex;
				if (m_TerrestrialIndex2 >= 0 && m_TerrestrialIndex2 != m_TerrestrialIndex)
					SpaceIndex[NumSpaces++] = m_TerrestrialIndex2;
			}
			for (int i = 0; i < NumSpaces; i++) {
				const int Space = SpaceIndex[i];
				const int Channel = FindChannelByServiceID(Space, (WORD)dwService);
				if (Channel >= 0) {
					return SetChannel(Space, Channel, service);
				}
			}

		} else if (m_BSIndex >= 0 && dwChannel == SPACE_BS) {
			const int Channel = FindChannelByServiceID(m_BSIndex, (WORD)dwService);
			if (Channel >= 0)
				return SetChannel(m_BSIndex, Channel, service);
		} else if (m_CSIndex >= 0 && dwChannel == SPACE_110CS) {
			const int Channel = FindChannelByServiceID(m_CSIndex, (WORD)dwService);
			if (Channel >= 0)
				return SetChannel(m_CSIndex, Channel, (WORD)dwService);
		}

		m_pApp->AddLog(L"該当するチャンネルが見つかりません。");
		return false;
	}

	// 現在のチャンネルをTvRockGOへ通知する形式で取得する
	DWORD GetChannel() {
		if (!m_fInitialized)
			GetSpaceIndex();
		TVTest::ChannelInfo Info;
		if (!m_pApp->GetCurrentChannelInfo(&Info))
			return 0;
		if (Info.Space == m_TerrestrialIndex || Info.Space == m_TerrestrialIndex2)
			return (DWORD(SPACE_TERRESTRIAL) << 16) | DWORD(Info.Channel + TERRESTRIAL_CHANNEL_FIRST);
		const WORD TSID = Info.TransportStreamID == 0xffff ? 0 : Info.TransportStreamID;
		// TvRockGOがスキャンしたTSIDが利用できない場合でも帯域を特定できる。
		if (Info.Space == m_BSIndex)
			return (DWORD(SPACE_BS) << 16) | TSID;
		if (Info.Space == m_CSIndex)
			return (DWORD(SPACE_110CS) << 16) | TSID;
		return 0;
	}

};

// オプションのSDK 0.0.14 graph-event ABI。通常のSDK構造体は0.0.10としてコンパイルし、
// ChannelInfoやその他の旧ホスト向けリクエストのサイズが変わらないようにする。
// 定義はTVTestPlugin.h (CC0) に準拠。sdk_compat.cppにて新しいABIの検証を行う。
namespace TvRockPlaybackAPI {
enum : UINT { Initialized=33, Finalize=34 };
#pragma pack(push,1)
struct GraphInfo { DWORD Flags; BYTE VideoStreamType; BYTE Reserved[3]; IGraphBuilder *pGraphBuilder; };
#pragma pack(pop)
}
static_assert(sizeof(TVTest::ChannelInfo)==284,"keep SDK 0.0.10 ChannelInfo layout");

// SDK 0.0.14において、イベントはオプションです。DLLは引き続き従来のベースABIを公開しています。
// グラフは初期化通知から終了通知までの間のみ保持してください。
class PlaybackGraph {
    std::mutex mutex;
    IMediaControl *control = nullptr;
    bool supported = false;
    bool pausedByUs = false;
public:
    ~PlaybackGraph() { Clear(); }
    void SetSupported(bool value) { std::lock_guard<std::mutex> lock(mutex); supported=value; }
    void Clear() {
        IMediaControl *old;
        { std::lock_guard<std::mutex> lock(mutex); old=control; control=nullptr; pausedByUs=false; }
        if(old)old->Release();
    }
    void Initialized(IGraphBuilder *graph) {
        IMediaControl *next=nullptr;
        if(graph)graph->QueryInterface(__uuidof(IMediaControl),reinterpret_cast<void **>(&next));
        IMediaControl *old;
        { std::lock_guard<std::mutex> lock(mutex); old=control; control=next; pausedByUs=false; }
        if(old)old->Release();
    }
    bool Command(DWORD action) {
        IMediaControl *current=nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if(action==10)return supported;
            if(action==11)return supported && control;
            if(action==4 && !pausedByUs)return true;
            if((action!=2 && action!=3 && action!=4) || !supported || !control)return false;
            current=control;current->AddRef();
        }
        const HRESULT result=action==2?current->Pause():current->Run();
		// S_FALSE は非同期的な遷移を示す。UI スレッド上で数秒間にわたってブロックするような待機は避け、
		// 短時間の制限を設けた上で実際の状態を確認すること。
        bool completed=false;
        const ULONGLONG deadline=GetTickCount64()+250;
        if(SUCCEEDED(result)) do {
            OAFilterState state=State_Stopped;
            const HRESULT status=current->GetState(0,&state);
            completed=(status==S_OK || status==VFW_S_CANT_CUE) && state==(action==2?State_Paused:State_Running);
            if(completed || FAILED(status))break;
            Sleep(5);
        } while(GetTickCount64()<deadline);
        {
            std::lock_guard<std::mutex> lock(mutex);
            if(control==current) {
                if(action==2 && SUCCEEDED(result))pausedByUs=true;
                else if(action!=2 && completed)pausedByUs=false;
            }
        }
        current->Release();
        return completed;
    }
};

// TvRockからの要求を受け取るクラス
class CTvRockRequest : public RockRequest
{
	TVTest::CTVTestApp *m_pApp;
	CChannelSearch *m_pChannel;
    std::function<void()> m_PlaybackReset;
    std::function<bool(DWORD)> m_PlaybackGraph;
    bool m_PlaybackPreviewDisabled = false;

	struct Request {
		std::function<LRESULT()> action;
		HANDLE done = ::CreateEvent(nullptr, TRUE, FALSE, nullptr);
		LRESULT result = 0;
		~Request() { if (done) ::CloseHandle(done); }
	};
	std::mutex m_RequestMutex;
	std::deque<std::shared_ptr<Request>> m_Requests;
	bool m_Stopping = false;
	bool m_CloseRequested = false;
	DWORD m_MainThread = ::GetCurrentThreadId();

	LRESULT Invoke(std::function<LRESULT()> action) {
		if (::GetCurrentThreadId() == m_MainThread)
			return action();
		auto request = std::make_shared<Request>();
		if (!request->done)
			return 0;
		request->action = std::move(action);
		{
			std::lock_guard<std::mutex> lock(m_RequestMutex);
			if (m_Stopping)
				return 0;
			m_Requests.push_back(request);
		}
		::WaitForSingleObject(request->done, INFINITE);
		return request->result;
	}

public:
	CTvRockRequest(TVTest::CTVTestApp *pApp, CChannelSearch *pChannel, std::function<void()> playbackReset = {}, std::function<bool(DWORD)> playbackGraph = {})
		: m_pApp(pApp)
		, m_pChannel(pChannel), m_PlaybackReset(std::move(playbackReset)), m_PlaybackGraph(std::move(playbackGraph)) {
	}
	// 視聴アプリのAPI呼び出しはメインスレッドで行う。
	void DispatchPending() {
		std::deque<std::shared_ptr<Request>> requests;
		{
			std::lock_guard<std::mutex> lock(m_RequestMutex);
			requests.swap(m_Requests);
		}
		for (const auto &request : requests) {
			try { request->result = request->action(); }
			catch (...) { request->result = 0; }
			::SetEvent(request->done);
		}
	}
	bool TakeCloseRequest() {
		const bool requested = m_CloseRequested;
		m_CloseRequested = false;
		return requested;
	}
	void Stop() {
		std::lock_guard<std::mutex> lock(m_RequestMutex);
		m_Stopping = true;
		for (const auto &request : m_Requests)
			::SetEvent(request->done);
		m_Requests.clear();
	}
	void PowerOff() override { Invoke([this] { PowerOffImpl(); return 0; }); }
	bool ChannelChange(DWORD channel, DWORD service) override {
		return Invoke([=] { return ChannelChangeImpl(channel, service); }) != 0;
	}
	bool RecordStart(LPWSTR path) override {
		return path && Invoke([=] { return RecordStartImpl(path); }) != 0;
	}
	bool QuickRecord(LPWSTR path) override {
		return path && Invoke([=] { return QuickRecordImpl(path); }) != 0;
	}
	bool RecordStop() override { return Invoke([this] { return RecordStopImpl(); }) != 0; }
	void ScreenCapture() override { Invoke([this] { ScreenCaptureImpl(); return 0; }); }
	void OnOption() override { Invoke([this] { OnOptionImpl(); return 0; }); }

	// アプリ終了の要求
	void PowerOffImpl() {
		m_pApp->AddLog(L"TvRockから終了要求を受けました。");
		m_CloseRequested = true;
	}
	// チャンネル変更の要求
	bool ChannelChangeImpl(const DWORD dwChannel, const DWORD dwService) {
        if(dwChannel==0xfffffffdUL && dwService==2)
            return m_PlaybackReset && m_pApp->QueryMessage(TVTest::MESSAGE_SETPREVIEW);
        if(dwChannel==0xfffffffdUL)
            return m_PlaybackGraph && dwService<=1 && m_PlaybackGraph(dwService==0?10:11);
        if(dwChannel==0xfffffffeUL && (dwService==2 || dwService==3))
            return m_PlaybackGraph && m_PlaybackGraph(dwService);
        if(dwChannel==0xfffffffeUL) {
			// 一時停止中は、送信側が停止している間、最後にデコードされた画像が表示されます。
			// 再開/シーク中は新しいTSを受け入れる前にビューアとドライバのFIFOがリセットされます。
            if(!m_PlaybackReset)return false;
            if(dwService==4) {
                if(!m_pApp->Reset(TVTest::RESET_ALL))return false;
                m_PlaybackReset();
                if(!m_pApp->SetPreview(false))return false;
                m_PlaybackPreviewDisabled=true;
                return true;
            }
            if(dwService>1)return false;
            if(dwService==0) {
                if(!m_pApp->Reset(TVTest::RESET_ALL))return false;
                if(m_PlaybackPreviewDisabled) {
                    if(!m_pApp->SetPreview(true))return false;
                    m_PlaybackPreviewDisabled=false;
                }
				// RESET_ALLはTSをフラッシュするが、一時停止中のグラフを実行状態にする必要はない。
				// 一時停止状態からフォールバック（切り戻し）やシークを行う際は、グラフの一時停止状態を維持する。
                if(m_PlaybackGraph && !m_PlaybackGraph(4))
                    if(!m_pApp->SetPreview(false) || !m_pApp->SetPreview(true))return false;
            }
            m_PlaybackReset();
            return true;
        }
        WCHAR driver[MAX_PATH]={};
        if (dwChannel==0 && dwService>0 && m_pApp->GetDriverName(driver,ArrayLength(driver))>0 && (::StrStrIW(driver,L"UDP") || ::StrStrIW(driver,L"SRT")))
            return m_pApp->SetService(static_cast<int>(dwService),true);
		WCHAR szLog[128];
		::StringCchPrintfW(szLog, ArrayLength(szLog),L"TvRockからチャンネル変更要求を受けました。(Ch %lu / Sv %lu)",dwChannel,dwService);
		m_pApp->AddLog(szLog);

		return m_pChannel->SetChannel(dwChannel, dwService);
	}
	// 録画開始の要求
	bool RecordStartImpl(LPWSTR lpFileName) {
		WCHAR szLog[MAX_PATH+64];
		::StringCchPrintfW(szLog, ArrayLength(szLog),L"TvRockから録画開始要求を受けました。(%s)",lpFileName);
		m_pApp->AddLog(szLog);

		TVTest::RecordInfo RecordInfo = {};
		RecordInfo.Mask=TVTest::RECORD_MASK_FILENAME;
		RecordInfo.Flags=0;
		RecordInfo.pszFileName = lpFileName;
		RecordInfo.StartTimeSpec=TVTest::RECORD_START_NOTSPECIFIED;
		RecordInfo.StopTimeSpec=TVTest::RECORD_STOP_NOTSPECIFIED;
		return m_pApp->StartRecord(&RecordInfo);
	}
	// クイック録画開始の要求 (Rockバーからの直接録画の要求)
	bool QuickRecordImpl(LPWSTR lpFileName) {
		WCHAR szLog[MAX_PATH+256];
		::StringCchPrintfW(szLog, ArrayLength(szLog),L"TvRockからクイック録画開始要求を受けました。(%s)",lpFileName);
		m_pApp->AddLog(szLog);

		return RecordStartImpl(lpFileName);
	}
	// 録画停止の要求
	bool RecordStopImpl() {
		m_pApp->AddLog(L"TvRockから録画停止要求を受けました。");
		return m_pApp->StopRecord();
	}
	// 静止画保存の要求
	void ScreenCaptureImpl() {
		//m_pApp->AddLog(L"TvRockから静止画保存要求を受けました。");
		m_pApp->SaveImage();
	}
	// オプションボタンの要求
	void OnOptionImpl() {
		//m_pApp->AddLog(L"TvRockからオプションボタンの要求を受けました。");
		m_pApp->DoCommand(L"Options");
	}
	// ターゲット情報の取得
	void CollectSetting() {
	}

};

// クリティカルセクション
class CCriticalLock
{
public:
	CCriticalLock();
	virtual ~CCriticalLock();
	void Lock(void);
	void Unlock(void);
	bool TryLock(DWORD TimeOut=0);
private:
	CRITICAL_SECTION m_CriticalSection;
};

CCriticalLock::CCriticalLock()
{
	::InitializeCriticalSection(&m_CriticalSection);
}

CCriticalLock::~CCriticalLock()
{
	::DeleteCriticalSection(&m_CriticalSection);
}

void CCriticalLock::Lock(void)
{
	::EnterCriticalSection(&m_CriticalSection);
}

void CCriticalLock::Unlock(void)
{
	::LeaveCriticalSection(&m_CriticalSection);
}

bool CCriticalLock::TryLock(DWORD TimeOut)
{
	bool bLocked=false;
	if (TimeOut==0) {
		if (::TryEnterCriticalSection(&m_CriticalSection))
			bLocked=true;
	} else {
		for (DWORD i=TimeOut;i>0;i--) {
			if (::TryEnterCriticalSection(&m_CriticalSection)) {
				bLocked=true;
				break;
			}
			Sleep(1);
		}
	}
	return bLocked;
}

// CTvRockDTVTarget (TVBridgeより抜粋)
// BEGIN TVROCK REMOTE UDP
// UDP implementation lives in the Rust DLL. Input is independent of SI analysis.
namespace {
class CDllStreamer {
    std::atomic<TvRock::TvRockDTV *> m_Receiver{nullptr};
    TvRock::StreamTSProc m_Feed=nullptr;
    TvRock::StreamResetProc m_Reset=nullptr;
public:
    bool Enable(HMODULE library,TvRock::TvRockDTV *receiver) {
        auto mode=reinterpret_cast<TvRock::SetStreamModeProc>(GetProcAddress(library,"TvRockDTVSetStreamMode"));
        m_Feed=reinterpret_cast<TvRock::StreamTSProc>(GetProcAddress(library,"TvRockDTVStreamTS"));
        m_Reset=reinterpret_cast<TvRock::StreamResetProc>(GetProcAddress(library,"TvRockDTVStreamReset"));
        if(!mode||!m_Feed||!m_Reset||!mode(receiver,TvRock::STREAM_MODE_DIRECT_TS))return false;
        m_Receiver=receiver;return true;
    }
    void Stop(){m_Receiver=nullptr;}
    void Feed(const BYTE *data,size_t size) {auto receiver=m_Receiver.load();if(receiver&&m_Feed&&size<=MAXDWORD)m_Feed(receiver,data,static_cast<DWORD>(size));}
    void Reset(){auto receiver=m_Receiver.load();if(receiver&&m_Reset)m_Reset(receiver);}
};
CDllStreamer &LibraryStream(){static CDllStreamer stream;return stream;}
}
// END TVROCK REMOTE UDP

class CTvRockDTVTarget : public TVTest::CTVTestPlugin
{
	enum {
		DEFAULT_BUFFER_LENGTH      = 20000UL,
		MIN_BUFFER_LENGTH          = 10000UL,
		MAX_BUFFER_LENGTH          = 100000UL,
		DEFAULT_SEND_BUFFER_LENGTH = 256UL
	};
	static LRESULT CALLBACK EventCallback(UINT Event,LPARAM lParam1,LPARAM lParam2,void *pClientData);
	static BOOL CALLBACK StreamCallback(BYTE *pData,void *pClientData);
	HINSTANCE m_hPlug;
	TvRockDTV *m_pTvRock;
	CTvRockRequest *m_pRequest;
	CChannelSearch *m_pChannel;
	int m_DeviceID;
	WCHAR m_szDriverName[MAX_PATH];

	CCriticalLock m_StreamLock;
	HANDLE m_hProcessTSEvent;
	SIZE_T m_StreamBufferLength;
	BYTE *m_pStreamBuffer;
	HANDLE m_hStreamThread;
	std::atomic<bool> m_fStreamEnd;
	CCriticalLock m_ProcessLock;
	HWND m_hDispatchWindow = nullptr;
	ULONGLONG m_LastStatusTime = 0;
    PlaybackGraph m_PlaybackGraph;
	bool m_StreamCallbackRegistered = false;
    bool m_ServiceItemRegistered = false;
	static LRESULT CALLBACK DispatchWindowProc(HWND, UINT, WPARAM, LPARAM);
	DWORD m_DropCount;
	SIZE_T m_StreamBufferUsed;
	SIZE_T m_StreamBufferPos;

	bool GetStream(BYTE *pBuffer,SIZE_T *pBufferLength);
	static DWORD WINAPI StreamThread(LPVOID lpParameter);
	bool InitializeTvRockDTV(LPCWSTR pszCommandLine);
	static int GetDeviceID(LPCWSTR pszCommandLine,LPWSTR pszDriverName);

public:
	CTvRockDTVTarget()
		: m_hPlug(NULL)
		, m_pTvRock(NULL)
		, m_pRequest(NULL)
		, m_pChannel(NULL)
		, m_DeviceID(-1)
		, m_hProcessTSEvent(NULL)
		, m_StreamBufferLength(DEFAULT_BUFFER_LENGTH)
		, m_pStreamBuffer(NULL)
		, m_hStreamThread(NULL)
		, m_fStreamEnd(false)
		, m_DropCount(0)
		, m_StreamBufferUsed(0)
		, m_StreamBufferPos(0)
	{
		m_szDriverName[0]=L'\0';
	}

	// 元の最小プラグインABIを維持する。オプション機能は実行時に照会される。
    DWORD GetVersion() override { return TVTEST_PLUGIN_VERSION_(0,0,10); }

	// プラグインの情報
	bool GetPluginInfo(TVTest::PluginInfo *pInfo)
	{
		pInfo->Type           = TVTest::PLUGIN_TYPE_NORMAL;
		pInfo->Flags          = 0;
		pInfo->pszPluginName  = L"TvRock DTV Target";
		pInfo->pszCopyright   = L"Public Domain";
		pInfo->pszDescription = L"TvRock DTV Target";
		return true;
	}

	// 初期化
	bool Initialize()
	{
		InitializeTvRockDTV(::GetCommandLineW());

        m_PlaybackGraph.SetSupported(m_pApp->QueryMessage(TVTest::MESSAGE_QUERYEVENT) &&
            m_pApp->QueryEvent(TvRockPlaybackAPI::Initialized) &&
            m_pApp->QueryEvent(TvRockPlaybackAPI::Finalize));
		m_pApp->SetEventCallback(EventCallback, this);
        if(!m_ServiceItemRegistered && m_pApp->QueryMessage(TvRockServiceItem::Register)) {
            // 自動拡張を無効にし、視聴アプリのステータスバー設定で指定した幅を使用する。
            TvRockServiceItem::Info info={sizeof(info),1,0,TvRockServiceItem::ItemID,L"TvRockServiceName",L"TvRock サービス名",40,-1,-14000,0};
            m_ServiceItemRegistered=m_pPluginParam->Callback(m_pPluginParam,TvRockServiceItem::Register,reinterpret_cast<LPARAM>(&info),0)!=FALSE;
        }

		return true;
	}

	// 終了
	bool Finalize()
	{
        m_PlaybackGraph.Clear();
		if (m_pRequest)
			m_pRequest->Stop();
		if (m_hDispatchWindow) {
			::DestroyWindow(m_hDispatchWindow);
			m_hDispatchWindow = nullptr;
		}
		::UnregisterClassW(L"TvRockOnTVTest.Dispatch", g_hinstDLL);
		LibraryStream().Stop();
		if (m_StreamCallbackRegistered) {
			m_pApp->SetStreamCallback(TVTest::STREAM_CALLBACK_REMOVE,StreamCallback);
			m_StreamCallbackRegistered = false;
		}
		if (m_hStreamThread) {
			m_fStreamEnd=true;
			::SetEvent(m_hProcessTSEvent);
			::WaitForSingleObject(m_hStreamThread, INFINITE);
			::CloseHandle(m_hStreamThread);
			m_hStreamThread=NULL;
		}
		if (m_hProcessTSEvent) {
			::CloseHandle(m_hProcessTSEvent);
			m_hProcessTSEvent=NULL;
		}
		if (m_pStreamBuffer) {
			delete [] m_pStreamBuffer;
			m_pStreamBuffer=NULL;
		}
		// TvRockDTVTargetを廃棄
		if (m_pTvRock) {
			m_pTvRock->Dispose();
			delete m_pTvRock;
			delete m_pRequest;
			m_pTvRock = NULL;
			m_pRequest = NULL;
			m_pApp->AddLog(L"TvRock DTV Targetの終了処理を行いました。");
		}
		if (m_hPlug) {
			::FreeLibrary(m_hPlug);
			m_hPlug=NULL;
		}
		if (m_pChannel) {
			delete m_pChannel;
			m_pChannel = NULL;
		}
		m_DeviceID=-1;
		m_szDriverName[0]=L'\0';
		return true;
	}

private:
	// TS処理
	void ProcessTS(BYTE *pData, const DWORD dwLeng)
	{
		if (m_pTvRock)
			m_pTvRock->ProcessTS(pData, dwLeng);
	}

	void UpdateStatus() {
		TVTest::StatusInfo info = {};
		if (!m_pTvRock || !m_pApp->GetStatus(&info))
			return;
        auto quality=reinterpret_cast<TvRock::ReceiverQualityProc>(::GetProcAddress(m_hPlug,"TvRockDTVReceiverQuality"));
        if(quality)quality(m_pTvRock,info.DropPacketCount);
		m_ProcessLock.Lock();
		RockStatus *status = m_pTvRock->GetStatus();
		if (status) {
			status->SetStatusSignalLevel(info.SignalLevel);
			// 視聴アプリはビット毎秒で報告する。
			status->SetStatusBitrate(info.BitRate);
			status->SetStatusErrorPacket(info.ErrorPacketCount);
			status->SetStatusScramblingPacket(info.ScramblePacketCount);
		}
		m_ProcessLock.Unlock();
	}

	// キューをクリアしてストリームを切り替える前に現在のバッチを完了させる
	void StreamChange(const DWORD dwChannel, const DWORD dwService)
	{
		if (!m_pTvRock)
			return;
		LibraryStream().Reset();
		m_ProcessLock.Lock();
		m_StreamLock.Lock();
		m_StreamBufferUsed = 0;
		m_StreamBufferPos = 0;
		m_StreamLock.Unlock();
		m_pTvRock->OnStreamChange(dwChannel, dwService);
		m_ProcessLock.Unlock();
	}

	// チャンネルの取得
	DWORD GetChannel()
	{
		return (m_pChannel ? m_pChannel->GetChannel() : 0);
	}

	// サービスの取得
	DWORD GetServiceID()
	{
		int Service = m_pApp->GetService();
		TVTest::ServiceInfo ServiceInfo;
		if (Service < 0 || !m_pApp->GetServiceInfo(Service, &ServiceInfo))
			return 0;
		return ServiceInfo.ServiceID;
	}
};

// イベント通知
LRESULT CALLBACK CTvRockDTVTarget::EventCallback(UINT Event,LPARAM lParam1,LPARAM lParam2,void *pClientData)
{
	CTvRockDTVTarget *pThis=static_cast<CTvRockDTVTarget*>(pClientData);
	switch (Event) {
    case TvRockPlaybackAPI::Initialized: {
        auto info=reinterpret_cast<TvRockPlaybackAPI::GraphInfo *>(lParam1);
        pThis->m_PlaybackGraph.Initialized(info?info->pGraphBuilder:nullptr);
        return TRUE;
    }
    case TvRockPlaybackAPI::Finalize:
        pThis->m_PlaybackGraph.Clear();
        return TRUE;
    case TvRockServiceItem::DrawEvent: {
        auto info=reinterpret_cast<TvRockServiceItem::DrawInfo *>(lParam1);
        if(!info || info->ID!=TvRockServiceItem::ItemID)return FALSE;
        WCHAR text[640];
        ServiceItemText(*pThis->m_pApp,text,ArrayLength(text),(info->Flags&1)!=0);
        const int saved=SaveDC(info->hdc);
        SetTextColor(info->hdc,info->Color);SetBkMode(info->hdc,TRANSPARENT);
        RECT rect=info->DrawRect;
        DrawTextW(info->hdc,text,-1,&rect,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX|DT_END_ELLIPSIS);
        if(saved)RestoreDC(info->hdc,saved);
        return TRUE;
    }
    case TvRockServiceItem::NotifyEvent: {
        auto info=reinterpret_cast<TvRockServiceItem::EventInfo *>(lParam1);
        return info && info->ID==TvRockServiceItem::ItemID && info->Event==6;
    }
	case TVTest::EVENT_DRIVERCHANGE:
		if (pThis->m_pChannel)
			pThis->m_pChannel->Invalidate();
		[[fallthrough]];
	case TVTest::EVENT_CHANNELCHANGE:
	case TVTest::EVENT_SERVICECHANGE:
	case TVTest::EVENT_SERVICEUPDATE:
		pThis->StreamChange(pThis->GetChannel(), pThis->GetServiceID());
		return TRUE;

	case TVTest::EVENT_EXECUTE:
		{
			LPCWSTR pszCommandLine=reinterpret_cast<LPCWSTR>(lParam1);
			WCHAR szDriverName[MAX_PATH];
			int DeviceID=GetDeviceID(pszCommandLine,szDriverName);
			if (DeviceID>=0
					&& (pThis->m_DeviceID<0 || pThis->m_DeviceID!=DeviceID)) {
				// 新しいデバイスIDを設定
				pThis->Finalize();
				pThis->InitializeTvRockDTV(pszCommandLine);
			} else if (DeviceID<0 && pThis->m_DeviceID>=0
					&& szDriverName[0]!='\0') {
				pThis->Finalize();
			}
		}
		return TRUE;
	}
	return 0;
}

// ストリームの取得
bool CTvRockDTVTarget::GetStream(BYTE *pBuffer,SIZE_T *pBufferLength)
{
	m_StreamLock.Lock();
	if (m_StreamBufferUsed == 0) {
		m_StreamLock.Unlock();
		return false;
	}
	SIZE_T Size = min(*pBufferLength,m_StreamBufferUsed);
	if (Size > m_StreamBufferLength-m_StreamBufferPos)
		Size = m_StreamBufferLength-m_StreamBufferPos;
	::CopyMemory(pBuffer,m_pStreamBuffer+m_StreamBufferPos*188,Size*188);
	*pBufferLength = Size;
	m_StreamBufferPos += Size;
	if (m_StreamBufferPos == m_StreamBufferLength)
		m_StreamBufferPos = 0;
	m_StreamBufferUsed -= Size;
	m_StreamLock.Unlock();
	return true;
}

// DLLリクエストおよびステータス収集のためのメインスレッド・ディスパッチャー
LRESULT CALLBACK CTvRockDTVTarget::DispatchWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (message == WM_NCCREATE) {
		auto cs = reinterpret_cast<CREATESTRUCT *>(lParam);
		::SetWindowLongPtr(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
	}
	auto self = reinterpret_cast<CTvRockDTVTarget *>(::GetWindowLongPtr(window, GWLP_USERDATA));
	if (message == WM_TIMER && self && wParam == 1) {
		if (self->m_pRequest) {
			self->m_pRequest->DispatchPending();
			if (self->m_pRequest->TakeCloseRequest())
				::PostMessage(window, WM_APP + 1, 0, 0);
		}
		const ULONGLONG now = ::GetTickCount64();
		if (now - self->m_LastStatusTime >= 1000) {
			self->m_LastStatusTime = now;
            self->UpdateStatus();
            if (self->m_hPlug && self->m_pTvRock) {
                using NotifyCapabilities=void (*)(TvRockDTV *,BOOL,BOOL);
                auto capabilities=reinterpret_cast<NotifyCapabilities>(::GetProcAddress(self->m_hPlug,"TvRockDTVPlaybackCapabilities"));
                if(capabilities)capabilities(self->m_pTvRock,self->m_PlaybackGraph.Command(10),self->m_PlaybackGraph.Command(11));
                using NotifyReady=void (*)(TvRockDTV *,BOOL,DWORD);
                auto notify=reinterpret_cast<NotifyReady>(::GetProcAddress(self->m_hPlug,"TvRockDTVReceiverReady"));
                if (notify) {
                    WCHAR driver[MAX_PATH]={};
                    const bool ready=self->m_pApp->GetDriverName(driver,ArrayLength(driver))>0
                        && (::StrStrIW(driver,L"UDP") || ::StrStrIW(driver,L"SRT")) && self->m_pApp->GetTuningSpace()>=0;
                    notify(self->m_pTvRock,ready,self->GetServiceID());
                }
            }
		}
		return 0;
	}
	if (message == WM_APP + 1 && self) {
		// 視聴アプリに終了を求める前にDLLコールバックを返す
		self->m_pApp->Close(TVTest::CLOSE_EXIT);
		return 0;
	}
	return ::DefWindowProc(window, message, wParam, lParam);
}

// イベント駆動型のストリーム処理。ポーリングや強制的なスレッド終了は行わない
DWORD WINAPI CTvRockDTVTarget::StreamThread(LPVOID lpParameter)
{
	auto self = static_cast<CTvRockDTVTarget *>(lpParameter);
	BYTE buffer[DEFAULT_SEND_BUFFER_LENGTH * 188];
	while (!self->m_fStreamEnd.load()) {
		SIZE_T length = DEFAULT_SEND_BUFFER_LENGTH;
		self->m_ProcessLock.Lock();
		const bool available = self->GetStream(buffer, &length);
		if (available)
			self->ProcessTS(buffer, static_cast<DWORD>(length * 188));
		self->m_ProcessLock.Unlock();
		if (!available)
			::WaitForSingleObject(self->m_hProcessTSEvent, INFINITE);
	}
	return 0;
}

// ストリーム処理
BOOL CALLBACK CTvRockDTVTarget::StreamCallback(BYTE *pData,void *pClientData)
{
	CTvRockDTVTarget *pThis = static_cast<CTvRockDTVTarget*>(pClientData);
    LibraryStream().Feed(pData,188);
	pThis->m_StreamLock.Lock();
	if (pThis->m_StreamBufferUsed == pThis->m_StreamBufferLength) {
		pThis->m_DropCount++;
		pThis->m_StreamLock.Unlock();
		return TRUE;
	}
	::CopyMemory(pThis->m_pStreamBuffer+(pThis->m_StreamBufferPos+pThis->m_StreamBufferUsed)%pThis->m_StreamBufferLength*188,pData,188);
	const bool wake = pThis->m_StreamBufferUsed == 0;
	pThis->m_StreamBufferUsed++;
	pThis->m_StreamLock.Unlock();
	if (wake)
		::SetEvent(pThis->m_hProcessTSEvent);
	return TRUE;
}

// TvRockDTVの初期化
bool CTvRockDTVTarget::InitializeTvRockDTV(LPCWSTR pszCommandLine) try
{
	// TvRockはデバイス識別番号が必要
	m_DeviceID = GetDeviceID(pszCommandLine, m_szDriverName);
	if (m_DeviceID < 0) {
		if (m_DeviceID == -2)
			m_pApp->AddLog(L"DIDが不正です。");
		else
			m_pApp->AddLog(L"DIDが指定されていません。");
		return false;
	}

	WCHAR szLog[256];
	::StringCchPrintfW(szLog, ArrayLength(szLog), L"TvRock DTV Targetを初期化します。(DID %c)", m_DeviceID + 'A');
	m_pApp->AddLog(szLog);

	// TvRockDTVライブラリの読み込み
	typedef TvRockDTV *(*CreateTvRockDTV)(HINSTANCE);
	m_hPlug = LoadDTVModule(g_hinstDLL);
	if (m_hPlug==NULL) {
		m_pApp->AddLog(L"TvRockDTVライブラリが見つかりません。");
		Finalize();
		return false;
	}
	CreateTvRockDTV pFunc = (CreateTvRockDTV)::GetProcAddress(m_hPlug, "CreateTvRockDTV");
	if (pFunc == NULL) {
		m_pApp->AddLog(L"TvRockDTVライブラリが一致しません。");
		Finalize();
		return false;
	}

	// このプラグインはデバイス一つですが
	// 一度に複数デバイス作成してもかまいません
	m_pTvRock = pFunc(m_hPlug);
	if (m_pTvRock == NULL) {
		m_pApp->AddLog(L"TvRockDTVが作成できません。");
		Finalize();
		return false;
	}

	// TvRock DTV Targetを初期化 (既にデバイスが存在した場合エラー)
	m_pChannel = new CChannelSearch(m_pApp);
	m_pRequest = new CTvRockRequest(m_pApp, m_pChannel, [this] {
        using ResetProc=void (*)(TvRockDTV *);
        auto reset=reinterpret_cast<ResetProc>(GetProcAddress(m_hPlug,"TvRockDTVPlaybackReset"));
        if(reset)reset(m_pTvRock);
    }, [this](DWORD action) { return m_PlaybackGraph.Command(action); });
	LibraryStream().Enable(m_hPlug,m_pTvRock);
	if (!m_pTvRock->Initialize(m_DeviceID, m_pRequest)) {
		m_pApp->AddLog(L"TvRockDTVの初期化エラーです。");
		Finalize();
		return false;
	}

	// 設定の読み込み
	TCHAR szIniFileName[MAX_PATH], szSectionName[8];
	const DWORD pathLength = ::GetModuleFileName(g_hinstDLL, szIniFileName, MAX_PATH);
	if (!pathLength || pathLength >= MAX_PATH ||
			!::PathRenameExtension(szIniFileName, TEXT(".ini"))) {
		m_pApp->AddLog(L"プラグインの設定ファイルのパスを取得できません。");
		Finalize();
		return false;
	}
	m_StreamBufferLength = ::GetPrivateProfileInt(TEXT("Settings"), TEXT("BufferLength"), (INT)m_StreamBufferLength, szIniFileName);
	if (m_StreamBufferLength < MIN_BUFFER_LENGTH)
		m_StreamBufferLength = MIN_BUFFER_LENGTH;
	else if (m_StreamBufferLength > MAX_BUFFER_LENGTH)
		m_StreamBufferLength = MAX_BUFFER_LENGTH;
	wsprintf(szSectionName, TEXT("DID_%c"), m_DeviceID + 'A');
	m_pChannel->Init(szIniFileName, szSectionName);

	// 設定の確認
	RockProperty *pProp = m_pTvRock->GetProperty();
	if (pProp) {
		// 録画ファイル拡張子の調査
		WCHAR ext[MAX_PATH];
		memset(ext,0,sizeof(ext));
		pProp->GetFileExtension(ext, MAX_PATH);
		if (wcscmp(ext,FILE_EXTENSION)) {
			pProp->SetFileExtension(FILE_EXTENSION);
			pProp->Notify();
		}
		m_pChannel->SetProperty(pProp);
	}

	WNDCLASSW wc = {};
	wc.lpfnWndProc = DispatchWindowProc;
	wc.hInstance = g_hinstDLL;
	wc.lpszClassName = L"TvRockOnTVTest.Dispatch";
	if (!::RegisterClassW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
		Finalize();
		return false;
	}
	m_hDispatchWindow = ::CreateWindowExW(0, wc.lpszClassName, L"", 0,
		0, 0, 0, 0, HWND_MESSAGE, nullptr, g_hinstDLL, this);
	if (!m_hDispatchWindow || !::SetTimer(m_hDispatchWindow, 1, 50, nullptr)) {
		Finalize();
		return false;
	}
	m_LastStatusTime = 0;

	// バッファの初期化
	m_pStreamBuffer = new BYTE[m_StreamBufferLength * 188];
	m_StreamBufferUsed = 0;
	m_StreamBufferPos = 0;
	m_hProcessTSEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);
	m_fStreamEnd = false;
	m_DropCount = 0;
	if (!m_hProcessTSEvent) {
		Finalize();
		return false;
	}
	m_hStreamThread = ::CreateThread(NULL, 0, StreamThread, this, 0, NULL);
	if (!m_hStreamThread) {
		Finalize();
		return false;
	}
	m_StreamCallbackRegistered = m_pApp->SetStreamCallback(0, StreamCallback, this);
	if (!m_StreamCallbackRegistered) {
		Finalize();
		return false;
	}
	StreamChange(GetChannel(), GetServiceID());

	return true;
}

catch (...) {
	m_pApp->AddLog(L"TvRock DTV Targetの初期化中にエラーが発生しました。");
	Finalize();
	return false;
}

// コマンドラインからデバイスIDを取得
int CTvRockDTVTarget::GetDeviceID(LPCWSTR pszCommandLine,LPWSTR pszDriverName)
{
	pszDriverName[0]='\0';
	if (pszCommandLine == NULL || pszCommandLine[0] == '\0')
		return -1;

	int argc;
	LPWSTR *argv = ::CommandLineToArgvW(pszCommandLine, &argc);

	if (argv == NULL)
		return -1;
	int DeviceID = -1;
	for (int i=0; i<argc; i++) {
		if (::lstrcmpiW(argv[i], L"/DID") == 0) {
			if (i + 1 >= argc || ::lstrlenW(argv[i+1]) != 1) {
				DeviceID = -2;
				continue;
			}
			if (argv[i+1][0] >= L'A' && argv[i+1][0] <= L'Z') {
				DeviceID = (argv[i+1][0] - L'A');
			} else if (argv[i+1][0] >= L'a' && argv[i+1][0] <= L'z') {
				DeviceID = (argv[i+1][0] - L'a');
			} else {
				DeviceID = -2;
			}
		} else if (::lstrcmpiW(argv[i], L"/d") == 0 && i + 1 < argc) {
			if (::lstrlenW(argv[i+1]) < MAX_PATH)
				::lstrcpyW(pszDriverName,argv[i+1]);
		}
	}
	LocalFree(argv);
	return DeviceID;
}

// プラグイン・エントリ
TVTest::CTVTestPlugin *CreatePluginClass()
{
	return new CTvRockDTVTarget;
}
