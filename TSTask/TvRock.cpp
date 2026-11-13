// Modified from TSTask (GPL v2). See THIRD_PARTY_NOTICES.md and licenses/GPL-2.0.txt.
#include "stdafx.h"
#include "TSTask.h"
#include "TvRock.h"
#include "../Common/DebugDef.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// BEGIN TVROCK REMOTE UDP
// UDPの実装はTvRockDTV内にあります。入力はSI解析とは独立しています。
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

using namespace TvRock;

namespace TSTask
{

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

	// チャンネル検索クラス
	class CChannelSearch
	{

		enum TuningSpaceType {
			SPACE_ERROR = -1,
			SPACE_UNKNOWN = 0,
			SPACE_TERRESTRIAL,
			SPACE_BS,
			SPACE_110CS
		};

		CTSTaskCore &m_Core;
		bool m_fInitialized;
		int m_TerrestrialIndex;
		int m_TerrestrialIndex2;
		int m_BSIndex;
		int m_CSIndex;
		DWORD m_SubService;
		bool m_fTerraIndexSpecified;
		bool m_fTerraIndex2Specified;
		bool m_fBSIndexSpecified;
		bool m_fCSIndexSpecified;
		CTuningSpaceList m_TuningSpaceList;

		// チューニング空間のインデックスの取得
		void GetSpaceIndex()
		{
			if (m_fInitialized)
				return;

			m_Core.GetScannedChannelList(&m_TuningSpaceList);

			bool fFound;
			const CChannelInfo *pChannelInfo;

			// 地デジのチューニング空間の取得
			if (m_TerrestrialIndex >= 0) {
				fFound = true;
				if (!m_fTerraIndexSpecified
						&& ((pChannelInfo = m_TuningSpaceList.GetChannelInfo(m_TerrestrialIndex, 0)) == nullptr
							|| !IsTerrestrialTSID(pChannelInfo->GetTransportStreamID()))) {
					fFound = false;
					for (size_t i = 0; i < m_TuningSpaceList.GetSpaceCount(); i++) {
						pChannelInfo = m_TuningSpaceList.GetChannelInfo(i, 0);
						if (pChannelInfo!=nullptr
								&& IsTerrestrialTSID(pChannelInfo->GetTransportStreamID())) {
							m_TerrestrialIndex = (int)i;
							fFound = true;
							break;
						}
					}
				}
				OutLog(LOG_INFO,
					   fFound ? L"地デジのチューニング空間を取得しました。(%d)" :
					            L"地デジのチューニング空間が取得できないため、%d に仮定しました。",
					   m_TerrestrialIndex);
				if (fFound && !m_fTerraIndex2Specified
						&& (pChannelInfo = m_TuningSpaceList.GetChannelInfo(m_TerrestrialIndex + 1, 0)) != nullptr
						&& IsTerrestrialTSID(pChannelInfo->GetTransportStreamID())) {
					m_TerrestrialIndex2 = m_TerrestrialIndex + 1;
				}
				if (m_TerrestrialIndex2 >= 0) {
					OutLog(LOG_INFO, L"地デジ2のチューニング空間を取得しました。(%d)", m_TerrestrialIndex2);
				}
			}

			// BSのチューニング空間の取得
			if (m_BSIndex >= 0) {
				fFound = true;
				if (!m_fBSIndexSpecified
						&& ((pChannelInfo = m_TuningSpaceList.GetChannelInfo(m_BSIndex, 0)) == nullptr
							|| (pChannelInfo->GetNetworkID() != 0
								&& !IsBsNetworkID(pChannelInfo->GetNetworkID()))
							|| !IsBsTSID(pChannelInfo->GetTransportStreamID()))) {
					// まず最初のチャンネルのみ調べる
					fFound = false;
					for (size_t i = 0; i < m_TuningSpaceList.GetSpaceCount(); i++) {
						pChannelInfo = m_TuningSpaceList.GetChannelInfo(i, 0);
						if (pChannelInfo!=nullptr
								&& (IsBsNetworkID(pChannelInfo->GetNetworkID())
									|| IsBsTSID(pChannelInfo->GetTransportStreamID()))) {
							m_BSIndex = (int)i;
							fFound = true;
							break;
						}
					}
					// 見付からない場合、全てのチャンネルを調べる
					if (!fFound) {
						for (size_t Space = 0; !fFound && Space < m_TuningSpaceList.GetSpaceCount(); Space++) {
							for (size_t i = 1; (pChannelInfo = m_TuningSpaceList.GetChannelInfo(Space, i)) != nullptr; i++) {
								if (IsBsNetworkID(pChannelInfo->GetNetworkID())
										|| IsBsTSID(pChannelInfo->GetTransportStreamID())) {
									m_BSIndex = (int)Space;
									fFound = true;
									break;
								}
							}
						}
					}
				}
				OutLog(LOG_INFO,
					   fFound ? L"BSのチューニング空間を取得しました。(%d)" :
					            L"BSのチューニング空間が取得できないため、%d に仮定しました。",
					   m_BSIndex);
			}

			// CSのチューニング空間の取得
			if (m_CSIndex >= 0) {
				fFound = true;
				if (!m_fCSIndexSpecified
						&& ((pChannelInfo = m_TuningSpaceList.GetChannelInfo(m_CSIndex, 0)) == nullptr
							|| (pChannelInfo->GetNetworkID() != 0
								&& !IsCsNetworkID(pChannelInfo->GetNetworkID()))
							|| !IsCsTSID(pChannelInfo->GetTransportStreamID()))) {
					// まず最初のチャンネルのみ調べる
					fFound = false;
					for (size_t i = 0; i < m_TuningSpaceList.GetSpaceCount(); i++) {
						pChannelInfo = m_TuningSpaceList.GetChannelInfo(i, 0);
						if (pChannelInfo != nullptr
								&& (IsCsNetworkID(pChannelInfo->GetNetworkID())
									|| IsCsTSID(pChannelInfo->GetTransportStreamID()))) {
							m_CSIndex = (int)i;
							fFound = true;
							break;
						}
					}
					// 見付からない場合、全てのチャンネルを調べる
					if (!fFound) {
						for (size_t Space = 0; !fFound && Space < m_TuningSpaceList.GetSpaceCount(); Space++) {
							for (size_t i = 1; (pChannelInfo = m_TuningSpaceList.GetChannelInfo(Space, i)) != nullptr; i++) {
								if (IsCsNetworkID(pChannelInfo->GetNetworkID())
										|| IsCsTSID(pChannelInfo->GetTransportStreamID())) {
									m_CSIndex = (int)Space;
									fFound = true;
									break;
								}
							}
						}
					}
				}
				OutLog(LOG_INFO,
					   fFound ? L"CSのチューニング空間を取得しました。(%d)" :
					            L"CSのチューニング空間が取得できないため、%d に仮定しました。",
					   m_CSIndex);
			}

			m_fInitialized = true;
		}

		// サービスIDの一致するチャンネルを探す
		int FindChannelByServiceID(int Space, WORD ServiceID)
		{
			const CChannelList *pChannelList=m_TuningSpaceList.GetChannelList(Space);
			if (pChannelList==nullptr)
				return -1;
			return pChannelList->FindByIDs(0, 0, ServiceID);
		}

	public:
		CChannelSearch(CTSTaskCore &Core)
			: m_Core(Core)
			, m_fInitialized(false)
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

		void Initialize(CSettings *pSettings,LPCWSTR pszDevice)
		{
			// 設定読み込み
			String Keyword;

			if (pSettings!=nullptr) {
				StringUtility::Format(Keyword,L"TvRock.%s.TerrestrialIndex",pszDevice);
				m_fTerraIndexSpecified=
					pSettings->Read(Keyword.c_str(),&m_TerrestrialIndex) && m_TerrestrialIndex>=0;
				if (m_fTerraIndexSpecified) {
					StringUtility::Format(Keyword,L"TvRock.%s.TerrestrialIndex2",pszDevice);
					m_fTerraIndex2Specified=
						pSettings->Read(Keyword.c_str(),&m_TerrestrialIndex2) && m_TerrestrialIndex2>=0;
				} else {
					m_fTerraIndex2Specified=false;
				}
				StringUtility::Format(Keyword,L"TvRock.%s.BSIndex",pszDevice);
				m_fBSIndexSpecified=pSettings->Read(Keyword.c_str(),&m_BSIndex) && m_BSIndex>=0;
				StringUtility::Format(Keyword,L"TvRock.%s.CSIndex",pszDevice);
				m_fCSIndexSpecified=pSettings->Read(Keyword.c_str(),&m_CSIndex) && m_CSIndex>=0;
			} else {
				m_fTerraIndexSpecified=false;
				m_fTerraIndex2Specified=false;
				m_fBSIndexSpecified=false;
				m_fCSIndexSpecified=false;
			}

		}

		void SetProperty(RockProperty *pProp)
		{
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
		bool SetChannel(const DWORD dwChannel, const DWORD dwService)
		{
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
					if (Channel >= 0)
						return m_Core.SetChannelByScanned(Space, Channel, service);
				}

			} else if (m_BSIndex >= 0 && dwChannel == SPACE_BS) {
				const int Channel = FindChannelByServiceID(m_BSIndex, (WORD)dwService);
				if (Channel >= 0)
					return m_Core.SetChannelByScanned(m_BSIndex, Channel, service);
			} else if (m_CSIndex >= 0 && dwChannel == SPACE_110CS) {
				const int Channel = FindChannelByServiceID(m_CSIndex, (WORD)dwService);
				if (Channel >= 0)
					return m_Core.SetChannelByScanned(m_CSIndex, Channel, service);
			}

			OutLog(LOG_ERROR,L"該当するチャンネルが見つかりません。");
			return false;
		}

		// 現在のチャンネルをTvRockGOへ通知する形式で取得する
		DWORD GetChannel()
		{
			if (!m_fInitialized)
				GetSpaceIndex();
			ChannelInfo Info;
			if (!m_Core.GetChannel(&Info) || Info.Channel < 0)
				return 0;
			if (Info.Space == m_TerrestrialIndex || Info.Space == m_TerrestrialIndex2)
				return (DWORD(SPACE_TERRESTRIAL) << 16) | DWORD(Info.Channel + TERRESTRIAL_CHANNEL_FIRST);
			ChannelStreamIDs IDs;
			if (m_Core.GetStreamIDs(&IDs) && IDs.TransportStreamID != 0 && IDs.TransportStreamID != 0xffff)
				Info.TransportStreamID = IDs.TransportStreamID;
			const WORD TSID = Info.TransportStreamID == 0xffff ? 0 : Info.TransportStreamID;
			// TvRockGO: upper word is broadcast type, lower word is TSID.
			if (Info.Space == m_BSIndex)
				return (DWORD(SPACE_BS) << 16) | TSID;
			if (Info.Space == m_CSIndex)
				return (DWORD(SPACE_110CS) << 16) | TSID;
			return 0;
		}

	};

	// TvRockからの要求を受け取るクラス
	class CTvRockRequest : public RockRequest
	{
		CTSTaskCore &m_Core;
		CTSTaskAppCore &m_AppCore;
		CChannelSearch &m_ChannelSearch;

	public:
		CTvRockRequest(CTSTaskCore &Core,CTSTaskAppCore &AppCore,CChannelSearch &ChannelSearch)
			: m_Core(Core)
			, m_AppCore(AppCore)
			, m_ChannelSearch(ChannelSearch)
		{
		}

		// アプリ終了の要求
		void PowerOff() override
		{
			OutLog(LOG_INFO,L"TvRockから終了要求を受けました。");
			m_AppCore.Quit();
		}

		// チャンネル変更の要求
		bool ChannelChange(const DWORD dwChannel,const DWORD dwService) override
		{
			OutLog(LOG_INFO,L"TvRockからチャンネル変更要求を受けました。(Ch %lu / Sv %lu)",dwChannel,dwService);
			return m_ChannelSearch.SetChannel(dwChannel,dwService);
		}

		// 録画開始の要求
		bool RecordStart(LPWSTR lpFileName) override
		{
			OutLog(LOG_INFO,L"TvRockから録画開始要求を受けました。(%s)",lpFileName);

			RecordingSettings Settings;
			m_AppCore.GetCurSettings().Recording.GetRecordingSettings(&Settings);

			String DefaultDir;
			PathUtility::Split(String(lpFileName),&DefaultDir,&Settings.FileName);
			Settings.Directories.clear();
			Settings.Directories.push_back(DefaultDir);

			std::vector<String> Directories;
			if (m_AppCore.GetCurSettings().Recording.GetDirectories(&Directories)) {
				String Dir;
				PathUtility::AppendDelimiter(&DefaultDir);
				for (const auto &e:Directories) {
					Dir=e;
					PathUtility::AppendDelimiter(&Dir);
					if (StringUtility::CompareNoCase(Dir,DefaultDir)!=0)
						Settings.Directories.push_back(e);
				}
			}

			return m_Core.StartRecording(Settings);
		}

		// クイック録画開始の要求 (Rockバーからの直接録画の要求)
		bool QuickRecord(LPWSTR lpFileName) override
		{
			return RecordStart(lpFileName);
		}

		// 録画停止の要求
		bool RecordStop() override
		{
			OutLog(LOG_INFO,L"TvRockから録画停止要求を受けました。");
			return m_Core.StopRecording();
		}

		// 静止画保存の要求
		void ScreenCapture() override
		{
			OutLog(LOG_VERBOSE,L"TvRockから静止画保存要求を受けました。");
		}

		// オプションボタンの要求
		void OnOption() override
		{
			OutLog(LOG_VERBOSE,L"TvRockからオプションボタンの要求を受けました。");
		}

		// ターゲット情報の取得
		void CollectSetting() override
		{
		}
	};

	CTvRockDTVTarget::CTvRockDTVTarget(CTSTaskAppCore &AppCore)
		: m_AppCore(AppCore)
		, m_Core(AppCore.GetCore())
		, m_hDTVModule(nullptr)
		, m_pTvRock(nullptr)
		, m_pRequest(nullptr)
		, m_pChannel(nullptr)
		, m_DeviceID(-1)
		, m_StreamQueueMaxBlocks(MAX_QUEUE_BLOCKS_DEFAULT)
		, m_StreamSendBufferLength(DEFAULT_SEND_BUFFER_LENGTH)
		, m_InitialChannelThreadID(0)
	{
	}

	CTvRockDTVTarget::~CTvRockDTVTarget()
	{
		Finalize();
	}

	// 初期化
	bool CTvRockDTVTarget::Initialize(int DeviceID,CSettings *pSettings)
	{
		if (DeviceID>=0)
			return InitializeTvRockDTV(DeviceID,pSettings);

		return true;
	}

	// 終了
	void CTvRockDTVTarget::Finalize()
	{
		CancelInitialChannelSelection();
		if (m_InitialChannelThread.IsOpened()) {
			m_InitialChannelThread.Wait(INFINITE);
			m_InitialChannelThread.Close();
		}
		m_InitialChannelEndEvent.Close();

		m_Core.RemoveEventHandler(this);
		m_Core.RemoveTsGrabber(this);

		if (m_StreamThread.IsOpened()) {
			OutLog(LOG_VERBOSE,L"TvRockストリームスレッドを終了します。");
			m_StreamEndEvent.Set();
			if (m_StreamThread.Wait(5000)==WAIT_TIMEOUT) {
				OutLog(LOG_WARNING,L"TvRockストリームスレッドを強制終了させます。");
				m_StreamThread.Terminate();
			}
			m_StreamThread.Close();
			OutLog(LOG_VERBOSE,L"TvRockストリームスレッドを終了しました。");
		}
		m_ProcessTSEvent.Close();
		m_StreamEndEvent.Close();
		m_StreamQueue.Clear();

		// TvRockDTVTargetを廃棄
		if (m_pTvRock!=nullptr) {
			OutLog(LOG_VERBOSE,L"TvRock DTV Targetを終了します。");
			m_pTvRock->Dispose();
			SafeDelete(m_pTvRock);
			SafeDelete(m_pRequest);
			OutLog(LOG_INFO,L"TvRock DTV Targetを終了しました。");
		}

        LibraryStream().Stop();

		if (m_hDTVModule!=nullptr) {
			OutLog(LOG_VERBOSE,L"TvRockDTVライブラリを解放します。");
			::FreeLibrary(m_hDTVModule);
			m_hDTVModule=nullptr;
			OutLog(LOG_VERBOSE,L"TvRockDTVライブラリを解放しました。");
		}

		SafeDelete(m_pChannel);
		m_DeviceID=-1;
	}


	// 固定の周波数テーブルがなくてもアプリケーションのコールバックを呼び出せるようにしておく。
	// 0は、周波数の補完が利用できないことを意味する。
	int CTvRockDTVTarget::GetFrequencyByTSID(WORD)
	{
		return 0;
	}

	// TvRockDTVの初期化
	bool CTvRockDTVTarget::InitializeTvRockDTV(int DeviceID,CSettings *pSettings)
	{
		if (DeviceID<0)
			return false;

		if (m_hDTVModule!=nullptr)
			return false;

		OutLog(LOG_IMPORTANT,L"TvRock DTV Targetを初期化します。(DID %c)",DeviceID+L'A');

		// TvRockDTVライブラリの読み込み
		m_hDTVModule=LoadDTVModule();
		if (m_hDTVModule==nullptr) {
			OutLog(LOG_ERROR,L"TvRockDTVライブラリを読み込めませんでした。");
			return false;
		}

		OutLog(LOG_VERBOSE,L"TvRockDTVライブラリを読み込みました。(%p)",m_hDTVModule);

		m_DeviceID = DeviceID;

		typedef TvRockDTV *(*CreateTvRockDTV)(HINSTANCE);
		CreateTvRockDTV pFunc=reinterpret_cast<CreateTvRockDTV>(::GetProcAddress(m_hDTVModule,"CreateTvRockDTV"));
		if (pFunc==nullptr) {
			OutLog(LOG_ERROR,L"TvRockDTVライブラリの関数を取得できませんでした。");
			Finalize();
			return false;
		}

		m_pTvRock=pFunc(m_hDTVModule);
		if (m_pTvRock==nullptr) {
			OutLog(LOG_ERROR,L"TvRockDTVを作成できませんでした。");
			Finalize();
			return false;
		}

		LibraryStream().Enable(m_hDTVModule,m_pTvRock);
		m_pChannel=new CChannelSearch(m_Core);
		m_pRequest=new CTvRockRequest(m_Core,m_AppCore,*m_pChannel);
		if (!m_pTvRock->Initialize(DeviceID,m_pRequest)) {
			OutLog(LOG_ERROR,L"TvRockDTVの初期化エラーです。");
			Finalize();
			return false;
		}

		// バッファの設定
		if (pSettings!=nullptr) {
			unsigned int BufferLength;
			if (pSettings->Read(L"TvRock.MaxBufferLength",&BufferLength)
					&& BufferLength>=MAX_QUEUE_BLOCKS_MIN
					&& BufferLength<=MAX_QUEUE_BLOCKS_MAX) {
				m_StreamQueueMaxBlocks=BufferLength;
			}
		}

		WCHAR szDevice[8];
		FormatString(szDevice,_countof(szDevice),L"DID_%c",DeviceID+L'A');
		m_pChannel->Initialize(pSettings,szDevice);

		// 設定の確認
		RockProperty *pProp=m_pTvRock->GetProperty();
		if (pProp!=nullptr) {
			// 録画ファイル拡張子の設定
			WCHAR szExtension[MAX_PATH];
			::ZeroMemory(szExtension,sizeof(szExtension));
			pProp->GetFileExtension(szExtension,_countof(szExtension));
			if (::lstrcmpiW(szExtension,L".ts")!=0) {
				pProp->SetFileExtension(L".ts");
				pProp->Notify();
			}
			m_pChannel->SetProperty(pProp);
		}

		m_StreamQueue.Initialize(QUEUE_BLOCK_SIZE,m_StreamQueueMaxBlocks);

		if (!m_ProcessTSEvent.Create() || !m_StreamEndEvent.Create()) {
			OutLog(LOG_ERROR,L"TvRock stream events could not be created.");
			Finalize();
			return false;
		}

		CThread::CFunctorBase *pFunctor=
			new CThread::CFunctor<CTvRockDTVTarget>(this,&CTvRockDTVTarget::StreamThread);
		if (!m_StreamThread.Begin(pFunctor)) {
			delete pFunctor;
			OutLog(LOG_ERROR,L"TvRock stream thread could not be started.");
			Finalize();
			return false;
		}

		m_Core.AddEventHandler(this);
		m_Core.AddTsGrabber(this);

		return true;
	}

	// ストリームの破棄
	void CTvRockDTVTarget::PurgeStream()
	{
		m_StreamQueue.Clear();

		if (m_pTvRock!=nullptr) {
			if (m_ProcessTSEvent.IsOpened()) {
				m_ProcessTSEvent.Reset();
				m_ProcessTSEvent.Wait(5000);
			}
		}
	}

	// ストリーム処理スレッド
	unsigned int CTvRockDTVTarget::StreamThread()
	{
		const size_t BufferSize=m_StreamSendBufferLength*188;
		BYTE *pBuffer=new BYTE[BufferSize];
		size_t ProcessSize;

		do {
			ProcessSize=BufferSize;
			if (m_StreamQueue.GetData(pBuffer,&ProcessSize)) {
				if (m_pTvRock!=nullptr)
					m_pTvRock->ProcessTS(pBuffer,DWORD(ProcessSize));
			} else {
				ProcessSize=0;
			}
			m_ProcessTSEvent.Set();

			if (m_pTvRock!=nullptr) {
				static DWORD SetStatusTime=0;
				const DWORD CurTime=::GetTickCount();
				if (CurTime-SetStatusTime>=1000UL) {
					RockStatus *pStatus=m_pTvRock->GetStatus();
					if (pStatus!=nullptr) {
						StreamStatistics Statistics;
						if (m_Core.GetStreamStatistics(&Statistics)) {
							pStatus->SetStatusSignalLevel(Statistics.SignalLevel);
							pStatus->SetStatusBitrate(Statistics.BitRate*8);
							pStatus->SetStatusErrorPacket((DWORD)(Statistics.ErrorPacketCount+
																  Statistics.DiscontinuityCount));
							pStatus->SetStatusScramblingPacket((DWORD)Statistics.ScramblePacketCount);
						}
					}
					SetStatusTime=CurTime;
				}
			}
		} while (m_StreamEndEvent.Wait(ProcessSize<BufferSize?50:0)==WAIT_TIMEOUT);

		delete [] pBuffer;

		return 0;
	}

	// ストリームの変更通知
	void CTvRockDTVTarget::NotifyStreamChange()
	{

		if (m_pTvRock!=nullptr) {
			ChannelStreamIDs StreamIDs;
			DWORD Channel=m_pChannel!=nullptr ? m_pChannel->GetChannel() : 0;
			DWORD Service=m_Core.GetStreamIDs(&StreamIDs) ? StreamIDs.ServiceID : 0;

			OutLog(LOG_VERBOSE,L"TvRock にストリームの変更を通知します。(Ch %u / Sv %u)",Channel,Service);
			m_pTvRock->OnStreamChange(Channel,Service);
		}
	}

	// 起動時の選局指定を処理しても未選局なら、受信できるチャンネルを探す。
	bool CTvRockDTVTarget::StartInitialChannelSelection()
	{
		if (m_pTvRock==nullptr || !m_Core.IsTunerOpened() || m_Core.IsRecording()
				|| m_InitialChannelThread.IsOpened())
			return false;
		ChannelInfo Current;
		if (!m_Core.GetChannel(&Current) || Current.Channel>=0)
			return false;
		if (!m_InitialChannelEndEvent.Create(nullptr,true,false))
			return false;
		auto pFunctor=new CThread::CFunctor<CTvRockDTVTarget>(this,&CTvRockDTVTarget::InitialChannelThread);
		if (!m_InitialChannelThread.Begin(pFunctor)) {
			delete pFunctor;
			m_InitialChannelEndEvent.Close();
			OutLog(LOG_ERROR,L"初期チャンネル選局スレッドを開始できません。");
			return false;
		}
		return true;
	}

	void CTvRockDTVTarget::CancelInitialChannelSelection()
	{
		if (m_InitialChannelEndEvent.IsOpened())
			m_InitialChannelEndEvent.Set();
	}

	unsigned int CTvRockDTVTarget::InitialChannelThread()
	{
		::InterlockedExchange(&m_InitialChannelThreadID,LONG(::GetCurrentThreadId()));
		// 全ての終了経路でIDを戻し、スレッドIDの再利用による誤判定を防ぐ。
		struct ThreadIDReset {
			LONG *pID;
			~ThreadIDReset() { ::InterlockedExchange(pID,0); }
		} Reset={&m_InitialChannelThreadID};
		OutLog(LOG_INFO,L"起動時のチャンネルが未選局のため、受信可能な初期チャンネルを探します。");
		std::vector<TuningChannelInfo> Candidates;
		auto AddCandidate=[&Candidates](DWORD Space,DWORD Channel) {
			for (const auto &Info:Candidates) {
				if (Info.Space==Space && Info.Channel==Channel)
					return;
			}
			TuningChannelInfo Info;
			Info.Space=Space;
			Info.Channel=Channel;
			Candidates.push_back(Info);
		};
		// スキャン済みのチャンネルがあれば優先する。SIDは指定しない。
		CTuningSpaceList Scanned;
		if (m_Core.GetScannedChannelList(&Scanned)) {
			for (size_t Space=0;Space<Scanned.GetSpaceCount();Space++) {
				const CChannelList *pList=Scanned.GetChannelList(Space);
				if (pList==nullptr)
					continue;
				for (size_t Channel=0;Channel<pList->GetChannelCount();Channel++) {
					const CChannelInfo *pInfo=pList->GetChannelInfo(Channel);
					if (pInfo!=nullptr && pInfo->IsEnabled())
						AddCandidate(pInfo->GetSpace(),pInfo->GetChannel());
				}
			}
		}
		// .ch2がない場合も、BonDriver自身の一覧から選局できる。
		BonDriverTuningSpaceList Spaces;
		if (m_Core.GetBonDriverChannelList(&Spaces)) {
			for (size_t Space=0;Space<Spaces.size();Space++) {
				for (size_t Channel=0;Channel<Spaces[Space].ChannelList.size();Channel++)
					AddCandidate(DWORD(Space),DWORD(Channel));
			}
		}
		for (const auto &Info:Candidates) {
			if (m_InitialChannelEndEvent.Wait(0)!=WAIT_TIMEOUT || !m_Core.IsTunerOpened()
					|| m_Core.IsRecording())
				return 0;
			if (!m_Core.SetChannelByIndex(Info.Space,Info.Channel))
				continue;
			// 外部の選局や終了要求で中断する。各チャンネルで最大3秒待つ。
			const DWORD Start=::GetTickCount();
			while (::GetTickCount()-Start<3000) {
				if (m_InitialChannelEndEvent.Wait(100)!=WAIT_TIMEOUT)
					return 0;
				ChannelStreamIDs IDs;
				if (m_Core.GetStreamIDs(&IDs) && IDs.ServiceID!=0
						&& IDs.ServiceID!=SERVICE_ID_INVALID) {
					OutLog(LOG_INFO,L"初期チャンネルを選局しました。(空間 %u / チャンネル %u / SID %u)",
						   Info.Space,Info.Channel,IDs.ServiceID);
					return 0;
				}
			}
		}
		OutLog(LOG_WARNING,L"受信可能な初期チャンネルが見付かりませんでした。");
		return 0;
	}

	bool CTvRockDTVTarget::OnBonDriverUnload()
	{
		CancelInitialChannelSelection();
		return true;
	}

	bool CTvRockDTVTarget::OnTunerClose()
	{
		CancelInitialChannelSelection();
		return true;
	}

	bool CTvRockDTVTarget::OnChannelChange(DWORD Space,DWORD Channel,WORD ServiceID)
	{
		if (DWORD(::InterlockedCompareExchange(&m_InitialChannelThreadID,0,0))==::GetCurrentThreadId())
			return m_InitialChannelEndEvent.Wait(0)==WAIT_TIMEOUT && !m_Core.IsRecording();
		CancelInitialChannelSelection();
		return true;
	}

	bool CTvRockDTVTarget::OnServiceChange(WORD ServiceID)
	{
		CancelInitialChannelSelection();
		return true;
	}

	bool CTvRockDTVTarget::OnRecordingStart(const RecordingSettings &Settings)
	{
		// Called with the core lock held: signal cancellation without joining the worker.
		CancelInitialChannelSelection();
		return true;
	}

	void CTvRockDTVTarget::OnTunerOpened()
	{
		NotifyStreamChange();
	}

	void CTvRockDTVTarget::OnChannelChanged(DWORD Space,DWORD Channel,WORD ServiceID)
	{
		NotifyStreamChange();
	}

	void CTvRockDTVTarget::OnStreamChanged()
	{
		PurgeStream();
		NotifyStreamChange();
	}

	void CTvRockDTVTarget::OnServiceChanged(WORD ServiceID)
	{
		NotifyStreamChange();
	}

	bool CTvRockDTVTarget::OnPacket(const ::CTsPacket *pPacket)
	{
		LibraryStream().Feed(pPacket->GetData(),pPacket->GetSize());
		m_StreamQueue.AddData(pPacket->GetData(),pPacket->GetSize());

		return true;
	}

	void CTvRockDTVTarget::OnReset()
	{
        LibraryStream().Reset();
		PurgeStream();
	}

}
