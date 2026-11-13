// Modified from RecTest (GPL; version unspecified in original notice).
// See LICENSE.md for the original GPL and warranty notices.
﻿#include "stdafx.h"
#include <string>
#include <cstring> 
#include "RecTest.h"
#include "AppMain.h"
#include "TvRock.h"
#include "resource.h"
#include "UdpOptions.h"
#include <functional>
#include <deque>
#include <memory>
#include <mutex>
#include <atomic>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// BEGIN TVROCK REMOTE UDP
// UDPの実装はRustのDLL内にあります。入力はSI解析とは独立しています。
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
        if(!mode||!m_Feed||!m_Reset||!mode(receiver,TvRock::STREAM_MODE_DIRECT_TS|TvRock::STREAM_MODE_RECORDER_PORT_LOCK))return false;
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

namespace {

// TvRock.cppだけで/addressに対応するため、最初のメッセージ取得時に
// メインウィンドウをサブクラス化する。/tvrockなしでも適用される。
struct UDPAddressOption {
	HHOOK Hook;
	char Address[16];
	bool Valid;
	UDPAddressOption() : Hook(NULL), Valid(false) { Address[0] = '\0'; }
};

UDPAddressOption& GetUDPAddressOption()
{
	static UDPAddressOption Option;
	return Option;
}

// ヘッダーを変更せず、既存オブジェクトのprotectedメンバーを
// メンバーポインターで参照する。派生型へのダウンキャストは行わない。
class CUDPAddressAccess : public CTsNetworkSender {
public:
	static CCriticalLock& GetLock(CTsNetworkSender& Sender) {
		const auto Member = &CUDPAddressAccess::m_DecoderLock;
		return Sender.*Member;
	}
	static void Apply(CTsNetworkSender& Sender, const char* Address) {
		const auto Member = &CUDPAddressAccess::m_SockList;
		auto& Sockets = Sender.*Member;
		for (size_t i = 0; i < Sockets.size(); i++) {
			if (Sockets[i].Type == CTsNetworkSender::SOCKET_UDP) {
				Sockets[i].addr.sin_addr.s_addr = ::inet_addr(Address);
				GetAppClass().AddLog(TEXT("UDP destination: %hs:%u"),
					Address, static_cast<unsigned int>(::ntohs(Sockets[i].addr.sin_port)));
			}
		}
	}
};

LRESULT CALLBACK UDPAddressSubclass(HWND hwnd, UINT Message, WPARAM wParam,
	LPARAM lParam, UINT_PTR ID, DWORD_PTR Data)
{
	if (Message == WM_NCDESTROY) {
		::RemoveWindowSubclass(hwnd, UDPAddressSubclass, ID);
	} else if (Message == WM_COMMAND
			&& (LOWORD(wParam) == CM_UDPSEND || LOWORD(wParam) == CM_UDPVIEW)) {
		CTsNetworkSender& Sender = GetAppClass().GetCoreEngine()->m_DtvEngine.m_TsNetworkSender;
		if (!Sender.IsOpen()) {
			const UDPAddressOption& Option = GetUDPAddressOption();
			if (!Option.Valid) {
				GetAppClass().AddLog(TEXT("Invalid /address: specify an IPv4 address (e.g. 192.168.0.1)."));
				return 0;
			}
			// Open()による送信スレッドの起動から宛先変更まで、入力と
			// GetStream()を止める。既定の宛先にはパケットを送らない。
			// 送信停止時はこのロックを取らず、スレッドの終了を妨げない。
			CBlockLock Lock(&CUDPAddressAccess::GetLock(Sender));
			const LRESULT Result = ::DefSubclassProc(hwnd, Message, wParam, lParam);
			CUDPAddressAccess::Apply(Sender, Option.Address);
			return Result;
		}
	}
	return ::DefSubclassProc(hwnd, Message, wParam, lParam);
}

LRESULT CALLBACK UDPAddressMessageHook(int Code, WPARAM wParam, LPARAM lParam)
{
	if (Code >= 0 && wParam == PM_REMOVE) {
		const MSG* pMessage = reinterpret_cast<const MSG*>(lParam);
		const HWND hwnd = GetAppClass().GetMainWindow()->GetHandle();
		if (hwnd != NULL && pMessage->hwnd == hwnd) {
			UDPAddressOption& Option = GetUDPAddressOption();
			if (::SetWindowSubclass(hwnd, UDPAddressSubclass,
				reinterpret_cast<UINT_PTR>(&Option), 0)) {
				::UnhookWindowsHookEx(Option.Hook);
				Option.Hook = NULL;
			}
		}
	}
	return ::CallNextHookEx(NULL, Code, wParam, lParam);
}

bool ParseUDPAddress(LPCWSTR Text, char* Address)
{
	// inet_addrの省略表記や名前解決は受け付けず、4オクテットを検証する。
	unsigned int Parts[4];
	for (int i = 0; i < 4; i++) {
		unsigned int Value = 0;
		int Digits = 0;
		while (*Text >= L'0' && *Text <= L'9') {
			Value = Value * 10 + (*Text++ - L'0');
			if (++Digits > 3 || Value > 255)
				return false;
		}
		if (Digits == 0 || (i < 3 ? *Text++ != L'.' : *Text != L'\0'))
			return false;
		Parts[i] = Value;
	}
	// 未指定宛先と、既存送信クラスが対応しないブロードキャストは除外する。
	if ((Parts[0] | Parts[1] | Parts[2] | Parts[3]) == 0
			|| (Parts[0] == 255 && Parts[1] == 255 && Parts[2] == 255 && Parts[3] == 255))
		return false;
	::wsprintfA(Address, "%u.%u.%u.%u", Parts[0], Parts[1], Parts[2], Parts[3]);
	return true;
}

void InitializeUDPAddressOption()
{
	UDPAddressOption& Option = GetUDPAddressOption();
	int Count = 0;
	LPWSTR* Args = ::CommandLineToArgvW(::GetCommandLineW(), &Count);
	if (Args == NULL)
		return;
	bool Specified = false;
	for (int i = 1; i < Count; i++) {
		if ((Args[i][0] == L'/' || Args[i][0] == L'-')
				&& ::lstrcmpiW(Args[i] + 1, L"address") == 0) {
			Specified = true;
			Option.Valid = i + 1 < Count && ParseUDPAddress(Args[i + 1], Option.Address);
			if (i + 1 < Count && Args[i + 1][0] != L'/' && Args[i + 1][0] != L'-')
				i++;
		}
	}
	::LocalFree(Args);
	if (Specified) {
		Option.Hook = ::SetWindowsHookEx(WH_GETMESSAGE, UDPAddressMessageHook,
			NULL, ::GetCurrentThreadId());
	}
}

} // namespace


#define _FILE_EXTENSION		L".ts"


// チャンネル
class CChannelSearch {
private:
	int m_TerrestrialIndex;
	int m_TerrestrialIndex2;
	int m_BSIndex;
	int m_CSIndex;
	DWORD m_SubService;

	bool GetChannelInfo(int Space, int Channel, const CChannelInfo** ppChannelInfo) {
		const CChannelList* pChannelList = GetAppClass().GetChannelManager()->GetChannelList(Space);

		if (pChannelList == NULL)
			return false;
		*ppChannelInfo = pChannelList->GetChannelInfo(Channel);
		if (*ppChannelInfo == NULL)
			return false;
		return true;
	}

public:
	CChannelSearch()
		: m_TerrestrialIndex(-1)
		, m_TerrestrialIndex2(-1)
		, m_BSIndex(-1)
		, m_CSIndex(-1)
		, m_SubService(-1)
	{
	}

	void SetProperty(RockProperty* pProp, LPCTSTR pszTerrestrialSpaceName) {
		CAppMain& AppClass = GetAppClass();
		int n = 0;

		m_TerrestrialIndex = (pProp->UseTS() ? n++ : -1);
		m_BSIndex = (pProp->UseBS() ? n++ : -1);
		m_CSIndex = (pProp->UseCS() ? n++ : -1);


		AppClass.AddLog(TEXT("チューニング空間のインデックスを取得しています..."));

		const CChannelInfo* pChannelInfo;
		if (m_TerrestrialIndex >= 0) {
			bool fFound = false;
			if (pszTerrestrialSpaceName != NULL && pszTerrestrialSpaceName[0] != '\0') {
				const CTuningSpaceList* pTuningSpaceList = AppClass.GetChannelManager()->GetDriverTuningSpaceList();
				for (int i = 0; i < pTuningSpaceList->NumSpaces(); i++) {
					if (::StrStrI(pTuningSpaceList->GetTuningSpaceName(i), pszTerrestrialSpaceName) != NULL) {
						m_TerrestrialIndex = i;
						fFound = true;
						AppClass.AddLog(TEXT("地デジのインデックスを取得しました。(%d)"), i);
						break;
					}
				}
				if (!fFound && ::lstrcmpi(pszTerrestrialSpaceName, TEXT("CATV")) == 0) {
					for (int i = 0; i < pTuningSpaceList->NumSpaces(); i++) {
						if (::StrStr(pTuningSpaceList->GetTuningSpaceName(i), TEXT("P[u")) != NULL) {
							m_TerrestrialIndex = i;
							fFound = true;
							AppClass.AddLog(TEXT("地デジのインデックスを取得しました。(%d)"), i);
							break;
						}
					}
				}
			}
			if (!fFound) {
				if (GetChannelInfo(m_TerrestrialIndex, 0, &pChannelInfo)
					&& ((pChannelInfo->GetTransportStreamID() & 0xFF00) == 0x7F00
						|| (pChannelInfo->GetTransportStreamID() & 0xFF00) == 0x7E00)) {
					fFound = true;
				}
				else {
					for (int i = 0; GetChannelInfo(i, 0, &pChannelInfo); i++) {
						if ((pChannelInfo->GetTransportStreamID() & 0xFF00) == 0x7F00
							|| (pChannelInfo->GetTransportStreamID() & 0xFF00) == 0x7E00) {
							m_TerrestrialIndex = i;
							fFound = true;
							break;
						}
					}
				}
				if (fFound)
					AppClass.AddLog(TEXT("地デジのインデックスをTSIDから取得しました。(%d)"), m_TerrestrialIndex);
			}
			if (fFound
				&& GetChannelInfo(m_TerrestrialIndex + 1, 0, &pChannelInfo)
				&& ((pChannelInfo->GetTransportStreamID() & 0xFF00) == 0x7F00
					|| (pChannelInfo->GetTransportStreamID() & 0xFF00) == 0x7E00)) {
				m_TerrestrialIndex2 = m_TerrestrialIndex + 1;
				AppClass.AddLog(TEXT("地デジ2のインデックスを取得しました。(%d)"), m_TerrestrialIndex2);
			}
			if (!fFound) {
				AppClass.AddLog(TEXT("地デジのインデックスを取得できませんでした。%d に設定しました。"), m_TerrestrialIndex);
			}
		}
		if (m_BSIndex >= 0) {
			bool fFound = false;
			if (GetChannelInfo(m_BSIndex, 0, &pChannelInfo)
				&& (pChannelInfo->GetTransportStreamID() & 0xF000) == 0x4000) {
				fFound = true;
			}
			else {
				for (int i = 0; GetChannelInfo(i, 0, &pChannelInfo); i++) {
					if ((pChannelInfo->GetTransportStreamID() & 0xF000) == 0x4000) {
						m_BSIndex = i;
						fFound = true;
						break;
					}
				}
			}
			if (fFound) {
				AppClass.AddLog(TEXT("BSのインデックスをTSIDから取得しました。(%d)"), m_BSIndex);
			}
			else {
				const CTuningSpaceList* pTuningSpaceList = AppClass.GetChannelManager()->GetTuningSpaceList();

				for (int i = 0; i < pTuningSpaceList->NumSpaces(); i++) {
					if (pTuningSpaceList->GetTuningSpaceType(i) == CTuningSpaceInfo::SPACE_BS) {
						m_BSIndex = i;
						fFound = true;
						AppClass.AddLog(TEXT("BSのインデックスを取得しました。(%d)"), i);
						break;
					}
				}
				if (!fFound) {
					AppClass.AddLog(TEXT("BSのインデックスを取得できませんでした。%d に設定しました。"), m_BSIndex);
				}
			}
		}
		if (m_CSIndex >= 0) {
			bool fFound = false;
			if (GetChannelInfo(m_CSIndex, 0, &pChannelInfo)
				&& ((pChannelInfo->GetTransportStreamID() & 0xF000) == 0x6000
					|| ((pChannelInfo->GetTransportStreamID() & 0xF000) == 0x7000
						&& (pChannelInfo->GetTransportStreamID() & 0x0F00) < 0x0E00))) {
				fFound = true;
			}
			else {
				for (int i = 0; GetChannelInfo(i, 0, &pChannelInfo); i++) {
					WORD TSID = pChannelInfo->GetTransportStreamID();
					if ((TSID & 0xF000) == 0x6000
						|| ((TSID & 0xF000) == 0x7000 && (TSID & 0x0F00) < 0x0E00)) {
						m_CSIndex = i;
						fFound = true;
						break;
					}
				}
			}
			if (fFound) {
				AppClass.AddLog(TEXT("CSのインデックスをTSIDから取得しました。(%d)"), m_CSIndex);
			}
			else {
				const CTuningSpaceList* pTuningSpaceList = AppClass.GetChannelManager()->GetTuningSpaceList();

				for (int i = 0; i < pTuningSpaceList->NumSpaces(); i++) {
					if (pTuningSpaceList->GetTuningSpaceType(i) == CTuningSpaceInfo::SPACE_110CS) {
						m_CSIndex = i;
						fFound = true;
						AppClass.AddLog(TEXT("CSのインデックスを取得しました。(%d)"), i);
						break;
					}
				}
				if (!fFound) {
					AppClass.AddLog(TEXT("CSのインデックスを取得できませんでした。%d に設定しました。"), m_CSIndex);
				}
			}
		}
	}


	bool SetChannel(const DWORD dwChannel, const DWORD dwService) {
		const CChannelManager* pChannelManager = GetAppClass().GetChannelManager();
		const CChannelList* pChannelList;
		const CChannelInfo* pChannelInfo;

		TCHAR strx[MAX_PATH + 256];
		::wsprintf(strx, TEXT("0:%d,%d,%d,%d,%d"), m_TerrestrialIndex, m_BSIndex, m_CSIndex, dwChannel, dwService);
		GetAppClass().AddLog(strx);

		if (dwChannel == 0x08) {
			m_SubService = dwService;
			return false;
		}

		const int service = static_cast<int>(m_SubService == DWORD(-1) ? dwService : m_SubService);
		m_SubService = -1;

		if (m_TerrestrialIndex >= 0 && dwChannel == CTuningSpaceInfo::SPACE_TERRESTRIAL) {
			TCHAR str[MAX_PATH + 256];
			::wsprintf(str, TEXT("1:%d,%d"), dwChannel, dwService);
			GetAppClass().AddLog(str);

			for (int j = 0; j < 2; j++) {
				const int Space = j == 0 ? m_TerrestrialIndex : m_TerrestrialIndex2;
				if (Space < 0)
					break;
				pChannelList = pChannelManager->GetChannelList(Space);
				if (pChannelList == NULL)
					break;
				for (int i = 0; i < pChannelList->NumChannels(); i++) {
					pChannelInfo = pChannelList->GetChannelInfo(i);
					if (pChannelInfo->GetServiceID() == dwService) {
						return GetAppClass().SetChannel(Space, i, service);
					}
				}
			}
		}

		if (m_BSIndex >= 0 && dwChannel == CTuningSpaceInfo::SPACE_BS) {
			TCHAR str[MAX_PATH + 256];
			::wsprintf(str, TEXT("2:%d,%d"), dwChannel, dwService);
			GetAppClass().AddLog(str);

			pChannelList = pChannelManager->GetChannelList(m_BSIndex);
			if (pChannelList == NULL)
				return false;
			for (int j = 0; j < pChannelList->NumChannels(); j++) {
				pChannelInfo = pChannelList->GetChannelInfo(j);
				if (pChannelInfo->GetServiceID() == dwService)
					return GetAppClass().SetChannel(m_BSIndex, j, service);
			}
		}

		if (m_CSIndex >= 0 && dwChannel == CTuningSpaceInfo::SPACE_110CS) {
			TCHAR str[MAX_PATH + 256];
			::wsprintf(str, TEXT("3:%d,%d"), dwChannel, dwService);
			GetAppClass().AddLog(str);

			pChannelList = pChannelManager->GetChannelList(m_CSIndex);
			if (pChannelList == NULL)
				return false;
			for (int j = 0; j < pChannelList->NumChannels(); j++) {
				pChannelInfo = pChannelList->GetChannelInfo(j);
				if (pChannelInfo->GetServiceID() == dwService)
					return GetAppClass().SetChannel(m_CSIndex, j);
			}
		}

		//	NotFound:
		GetAppClass().AddLog(TEXT("該当するチャンネルが見つかりません。"));
		return false;
	}

	DWORD GetChannel() {
		const CChannelInfo* Info = GetAppClass().GetChannelManager()->GetCurrentChannelInfo();
		if (Info == NULL)
			return 0;
		if (Info->GetSpace() == m_TerrestrialIndex || Info->GetSpace() == m_TerrestrialIndex2)
			return (DWORD(CTuningSpaceInfo::SPACE_TERRESTRIAL) << 16) | DWORD(Info->GetChannelIndex() + 13);
		WORD TSID = GetAppClass().GetCoreEngine()->m_DtvEngine.m_ProgManager.GetTransportStreamID();
		if (TSID == 0 || TSID == 0xffff)
			TSID = Info->GetTransportStreamID();
		if (TSID == 0xffff)
			TSID = 0;
		// TvRockGO: upper word is broadcast type, lower word is TSID.
		if (Info->GetSpace() == m_BSIndex)
			return (DWORD(CTuningSpaceInfo::SPACE_BS) << 16) | TSID;
		if (Info->GetSpace() == m_CSIndex)
			return (DWORD(CTuningSpaceInfo::SPACE_110CS) << 16) | TSID;
		return 0;
	}
};

// TvRockからの要求を受け取るクラス
class CTvRockRequest : public RockRequest
{
	CChannelSearch* m_pChannel;
public:
    CTvRockRequest(CChannelSearch *channel):m_pChannel(channel){}
	// アプリ終了の要求
	void PowerOff() {
		GetAppClass().AddLog(TEXT("TvRockから終了要求を受けました。"));
		GetAppClass().GetMainWindow()->SendCommand(CM_EXIT);
	}
	// チャンネル変更の要求
	bool ChannelChange(const DWORD dwChannel, const DWORD dwService) {
		if (dwService == 0x0) {
			return false;
		}
		GetAppClass().AddLog(TEXT("TvRockチャンネル変更要求を受けました。(Ch %lu Sv %lu)"), dwChannel, dwService);
		return (m_pChannel ? m_pChannel->SetChannel(dwChannel, dwService) : false);
	}
	// 録画開始の要求
	bool RecordStart(LPWSTR lpFileName) {
		GetAppClass().AddLog(TEXT("TvRockから録画要求を受けました。(%s)"), lpFileName);

		return GetAppClass().StartRecord(lpFileName);
	}
	// いますぐ録画の要求 (Rockバーの録画要求)
	bool QuickRecord(LPWSTR lpFileName) {
		GetAppClass().AddLog(TEXT("TvRockいますぐ録画要求を受けました。(%s)"), lpFileName);
		return RecordStart(lpFileName);
	}
	// 録画停止の要求
	bool RecordStop() {
		GetAppClass().AddLog(TEXT("TvRockから録画停止要求を受けました。"));
		GetAppClass().GetMainWindow()->SendCommand(CM_RECORD_STOP);
		return true;
	}

	// 画像保存の要求
	void ScreenCapture() {
		GetAppClass().AddLog(TEXT("TvRockから静止画保存要求を受けました。"));
	}
	// オプションボタンの要求
	void OnOption() {
		GetAppClass().AddLog(TEXT("TvRockからオプションボタンの要求を受けました。"));
	}
	// ターゲット名の取得
	void CollectSetting() {
	}
};


CTvRockDTVTarget::CTvRockDTVTarget()
	: m_hPlugin(NULL)
	, m_pTvRock(NULL)
	, m_pRequest(NULL)
	, m_pChannel(NULL)
	, m_DeviceID(-1)
	, m_hProcessTSEvent(NULL)
	, m_StreamBufferLength(DEFAULT_BUFFER_LENGTH)
	, m_pStreamBuffer(NULL)
	, m_StreamSendBufferLength(DEFAULT_SEND_BUFFER_LENGTH)
	, m_hStreamThread(NULL)
	, m_fStreamEnd(false)
	, m_StreamBufferUsed(0)
	, m_StreamBufferPos(0)
	, m_DropCount(0)
	, m_hwndInitialChannel(NULL)
	, m_InitialChannelTimer(0)
	, m_InitialChannelIndex(0)
	, m_InitialChannelSetTime(0)
	, m_fInitialChannelStarted(false)
	, m_fInitialChannelWaiting(false)
	, m_fSelectingInitialChannel(false)
{
	m_szTerrestrialSpaceName[0] = '\0';
	InitializeUDPAddressOption();
}


CTvRockDTVTarget::~CTvRockDTVTarget()
{
	UDPAddressOption& Option = GetUDPAddressOption();
	if (Option.Hook != NULL) {
		::UnhookWindowsHookEx(Option.Hook);
		Option.Hook = NULL;
	}
	Finalize();
}


bool CTvRockDTVTarget::Initialize()
{
	if (m_hPlugin != NULL)
		return false;
	m_DeviceID = -1;
	typedef TvRockDTV* (*CreateTvRockDTV)(HINSTANCE);

	Trace(TEXT("TvRockDTVTargetの初期化を実行しています..."));

	// TvRockDTVライブラリの読み込み
	Trace(TEXT("TvRockDTVライブラリを読み込んでいます..."));
	m_hPlugin = LoadDTVModule();
	if (m_hPlugin == NULL) {
		SetError(TEXT("TvRockDTVライブラリが見つかりません。"));
		return false;
	}

	Trace(TEXT("TvRockDTVクラスを取得しています..."));
	CreateTvRockDTV pFunc = (CreateTvRockDTV)GetProcAddress(m_hPlugin, "CreateTvRockDTV");
	if (pFunc == NULL) {
		FreeLibrary(m_hPlugin);
		m_hPlugin = NULL;
		SetError(TEXT("TvRockDTVライブラリが古いようです。"));
		return false;
	}

	// TvRockはデバイス識別番号が必要
	Trace(TEXT("デバイス識別番号を取得しています..."));
	int argc;
	LPTSTR* argv = CommandLineToArgvW(GetCommandLine(), &argc);

	bool fFindDID = false;
	if (argv != NULL) {
		for (int i = 0; i < (argc - 1); i++) {
			if (::lstrcmpiW(argv[i], L"/DID") == 0) {
				fFindDID = true;
				if (argv[i + 1][0] >= L'A' && argv[i + 1][0] <= L'Z') {
					m_DeviceID = (argv[i + 1][0] - L'A');
					break;
				}
				else if (argv[i + 1][0] >= L'a' && argv[i + 1][0] <= L'z') {
					m_DeviceID = (argv[i + 1][0] - L'a');
					break;
				}
			}
		}
		LocalFree(argv);
	}
	if (m_DeviceID < 0) {
		FreeLibrary(m_hPlugin);
		m_hPlugin = NULL;
		SetError(fFindDID ? TEXT("デバイス識別番号が不正です。") :
			TEXT("デバイス識別番号が設定されていません。"));
		return false;
	}

	m_pTvRock = pFunc(m_hPlugin);
	if (m_pTvRock == NULL) {
		Finalize();
		SetError(TEXT("TvRockDTV could not be created."));
		return false;
	}

	m_pChannel = new CChannelSearch();
	// TvRock DTV Targetを初期化 (既にデバイスが存在する場合エラー)
	Trace(TEXT("TvRockの初期化を実行しています(DID = %c)..."), m_DeviceID + 'A');
	m_pRequest = new CTvRockRequest(m_pChannel);
	LibraryStream().Enable(m_hPlugin,m_pTvRock);
	if (!m_pTvRock->Initialize(m_DeviceID, m_pRequest)) {
		TCHAR szText[128];
		::wsprintf(szText, TEXT("TvRockの初期化ができません。"));
		SetError(szText);
		Finalize();
		return false;
	}

	// 設定の確認
	Trace(TEXT("設定の確認を行っています..."));
	RockProperty* m_pProp = m_pTvRock->GetProperty();
	if (m_pProp) {
		WCHAR ext[MAX_PATH];
		memset(ext, 0, sizeof(ext));
		m_pProp->GetFileExtension(ext, MAX_PATH);
		if (wcscmp(ext, _FILE_EXTENSION)) {
			m_pProp->SetFileExtension(_FILE_EXTENSION);
			m_pProp->Notify();
		}
		if (m_pChannel)
			m_pChannel->SetProperty(m_pProp, m_szTerrestrialSpaceName);
	}

	Trace(TEXT("TvRockストリーミングスレッドを開始しています..."));
	m_pStreamBuffer = new BYTE[m_StreamBufferLength * 188];
	m_StreamBufferUsed = 0;
	m_StreamBufferPos = 0;
	m_fStreamEnd = false;
	m_hProcessTSEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);
	if (m_hProcessTSEvent == NULL) {
		Finalize();
		SetError(TEXT("TvRock stream event could not be created."));
		return false;
	}
	m_hStreamThread = ::CreateThread(NULL, 0, StreamThread, this, 0, NULL);
	if (m_hStreamThread == NULL) {
		Finalize();
		SetError(TEXT("TvRock stream thread could not be started."));
		return false;
	}
	GetAppClass().GetCoreEngine()->m_DtvEngine.m_TvRockGrabber.SetMediaGrabCallback(StreamCallback, this);

	ClearError();

	Trace(TEXT("TvRockDTVTargetの初期化が完了しました。"));
	StartInitialChannelSelection();

	return true;
}


bool CTvRockDTVTarget::Finalize()
{
    LibraryStream().Stop();
	StopInitialChannelSelection();
	Trace(TEXT("TvRockDTVTargetの終了処理を実行しています..."));

	GetAppClass().GetCoreEngine()->m_DtvEngine.m_TvRockGrabber.SetMediaGrabCallback(NULL);

	if (m_hStreamThread) {
		Trace(TEXT("TvRockストリーミングスレッドを終了しています..."));
		m_fStreamEnd = true;
		if (::WaitForSingleObject(m_hStreamThread, 2000) == WAIT_TIMEOUT) {
			::TerminateThread(m_hStreamThread, 1);
			Trace(TEXT("TvRockストリーミングスレッドを強制終了しました。"));
		}
		::CloseHandle(m_hStreamThread);
		m_hStreamThread = NULL;
	}
	if (m_hProcessTSEvent) {
		::CloseHandle(m_hProcessTSEvent);
		m_hProcessTSEvent = NULL;
	}
	SAFE_DELETE_ARRAY(m_pStreamBuffer);

	// TvRockDTVTargetp
	if (m_pTvRock) {
		Trace(TEXT("TvRockDTVTargetを破棄しています..."));
		m_pTvRock->Dispose();
		delete m_pTvRock;
		m_pTvRock = NULL;
	}
	SAFE_DELETE(m_pRequest);
	SAFE_DELETE(m_pChannel);
	m_DeviceID = -1;
	if (m_hPlugin) {
		Trace(TEXT("TvRockDTVライブラリを解放しています..."));
		FreeLibrary(m_hPlugin);
		m_hPlugin = NULL;
	}

	Trace(TEXT("TvRockDTVTargetの終了処理が完了しました。"));

	return true;
}


// 通常の起動時選局・前回チャンネル復元が終わってから未選局を確認する。
// 画面スレッドのタイマーを使い、UIやChannelManagerを別スレッドから操作しない。
void CTvRockDTVTarget::StartInitialChannelSelection()
{
	StopInitialChannelSelection();
	CAppMain& App = GetAppClass();
	if (!App.GetCoreEngine()->m_DtvEngine.IsSrcFilterOpen()
			|| App.GetCoreEngine()->IsUDPDriver() || App.GetMainWindow()->GetStandby()
			|| App.GetRecordManager()->IsRecording())
		return;
	m_InitialChannelIndex = 0;
	m_fInitialChannelStarted = false;
	m_fInitialChannelWaiting = false;
	m_hwndInitialChannel = App.GetMainWindow()->GetHandle();
	m_InitialChannelTimer = ::SetTimer(m_hwndInitialChannel,
		reinterpret_cast<UINT_PTR>(this), 1000, InitialChannelTimer);
	if (m_InitialChannelTimer == 0)
		Trace(TEXT("初期チャンネルの選局タイマーを開始できません。"));
}

void CTvRockDTVTarget::StopInitialChannelSelection()
{
	if (m_InitialChannelTimer != 0) {
		::KillTimer(m_hwndInitialChannel, m_InitialChannelTimer);
		m_InitialChannelTimer = 0;
	}
	m_InitialChannels.clear();
	m_hwndInitialChannel = NULL;
}

void CALLBACK CTvRockDTVTarget::InitialChannelTimer(HWND hwnd,UINT Message,UINT_PTR ID,DWORD Time)
{
	CTvRockDTVTarget* pThis = reinterpret_cast<CTvRockDTVTarget*>(ID);
	if (pThis->m_InitialChannelTimer == ID && pThis->m_hwndInitialChannel == hwnd
			&& !pThis->m_fSelectingInitialChannel)
		pThis->UpdateInitialChannelSelection();
}

void CTvRockDTVTarget::UpdateInitialChannelSelection()
{
	CAppMain& App = GetAppClass();
	CDtvEngine& Engine = App.GetCoreEngine()->m_DtvEngine;
	const CChannelManager* pManager = App.GetChannelManager();
	if (!Engine.IsSrcFilterOpen() || App.GetMainWindow()->GetStandby()
			|| App.GetRecordManager()->IsRecording()) {
		StopInitialChannelSelection();
		return;
	}
	WORD ServiceID = 0;
	if (Engine.GetServiceID(&ServiceID) && ServiceID != 0
			&& ServiceID != CDtvEngine::SID_INVALID) {
		if (m_fInitialChannelStarted)
			Trace(TEXT("初期チャンネルでサービス情報を受信しました。(SID %u)"), ServiceID);
		StopInitialChannelSelection();
		return;
	}
	if (!m_fInitialChannelStarted) {
		// 選局済みなら、サービス情報の到着待ち中であっても変更しない。
		if (pManager->GetCurrentRealChannelInfo() != NULL) {
			StopInitialChannelSelection();
			return;
		}
		Trace(TEXT("起動時のチャンネルが未選局のため、初期チャンネルを探します。"));
		// .ch2がない空間ではGetChannelListがBonDriverの一覧を返す。
		for (int Space = 0; Space < pManager->NumSpaces(); Space++) {
			const CChannelList* pList = pManager->GetChannelList(Space);
			if (pList == NULL)
				continue;
			for (int Channel = 0; Channel < pList->NumChannels(); Channel++) {
				const CChannelInfo* pInfo = pList->GetChannelInfo(Channel);
				if (pInfo == NULL || !pInfo->IsEnabled())
					continue;
				bool Duplicate = false;
				for (SIZE_T i = 0; i < m_InitialChannels.size(); i++) {
					if (m_InitialChannels[i].Space == Space
							&& m_InitialChannels[i].DriverChannel == pInfo->GetChannelIndex()) {
						Duplicate = true;
						break;
					}
				}
				if (!Duplicate) {
					InitialChannelInfo Info = { Space, Channel, pInfo->GetChannelIndex() };
					m_InitialChannels.push_back(Info);
				}
			}
		}
		m_fInitialChannelStarted = true;
	}
	if (m_fInitialChannelWaiting && ::GetTickCount() - m_InitialChannelSetTime < 3000)
		return;
	if (m_InitialChannelIndex >= m_InitialChannels.size()) {
		Trace(TEXT("受信可能な初期チャンネルが見付かりませんでした。"));
		StopInitialChannelSelection();
		return;
	}
	const InitialChannelInfo Info = m_InitialChannels[m_InitialChannelIndex++];
	m_fSelectingInitialChannel = true;
	m_fInitialChannelWaiting = App.SetChannel(Info.Space, Info.Channel);
	m_fSelectingInitialChannel = false;
	m_InitialChannelSetTime = ::GetTickCount();
}


void CTvRockDTVTarget::OnEvent(UINT Event)
{
    if(Event==EVENT_CHANNELCHANGE || Event==EVENT_DRIVERCHANGE)LibraryStream().Reset();

	// 起動時にキューへ積まれた復元選局や、TvRock・手動の選局を優先する。
	if (!m_fSelectingInitialChannel
			&& (Event == EVENT_CHANNELCHANGE || Event == EVENT_SERVICECHANGE
				|| Event == EVENT_DRIVERCHANGE))
		StopInitialChannelSelection();
	switch (Event) {
	case EVENT_CHANNELCHANGE:
	case EVENT_DRIVERCHANGE:
		PurgeStream();
	case EVENT_SERVICECHANGE:
	case EVENT_SERVICEUPDATE:
		if (m_pTvRock) {
			CAppMain& AppClass = GetAppClass();
			CDtvEngine* pDtvEngine = &AppClass.GetCoreEngine()->m_DtvEngine;
			WORD Service, ServiceID;
			DWORD Channel;

			if (m_pChannel == NULL || (Channel = m_pChannel->GetChannel()) == 0)
				return;
			Service = pDtvEngine->GetService();
			if (pDtvEngine->m_ProgManager.GetServiceID(&ServiceID, Service))
				m_pTvRock->OnStreamChange(Channel, ServiceID);
		}
		break;
	}
}


void CTvRockDTVTarget::ProcessTS(BYTE* pData, DWORD Length)
{
	static DWORD dwCount = 0;

	if (m_pTvRock) {
		m_pTvRock->ProcessTS(pData, Length);
		RockStatus* Stat = m_pTvRock->GetStatus();
		if (!Stat)
			return;
		dwCount += Length;
		if (dwCount >= 0x100000) {
			const CCoreEngine* pCoreEngine = GetAppClass().GetCoreEngine();

			Stat->SetStatusSignalLevel(pCoreEngine->GetSignalLevel());
			Stat->SetStatusBitrate(pCoreEngine->GetBitRate() * 8);
			Stat->SetStatusErrorPacket(pCoreEngine->GetErrorPacketCount() +
				pCoreEngine->GetContinuityErrorPacketCount());
			Stat->SetStatusScramblingPacket(pCoreEngine->GetScramblePacketCount());
			dwCount -= 0x100000;
		}
	}
}


bool CTvRockDTVTarget::GetStream(BYTE* pBuffer, SIZE_T* pBufferLength)
{
	m_StreamLock.Lock();
	if (m_StreamBufferUsed == 0) {
		m_StreamLock.Unlock();
		return false;
	}
	SIZE_T Size = min(*pBufferLength, m_StreamBufferUsed);
	if (Size > m_StreamBufferLength - m_StreamBufferPos)
		Size = m_StreamBufferLength - m_StreamBufferPos;
	::CopyMemory(pBuffer, m_pStreamBuffer + m_StreamBufferPos * 188, Size * 188);
	*pBufferLength = Size;
	m_StreamBufferPos += Size;
	if (m_StreamBufferPos == m_StreamBufferLength)
		m_StreamBufferPos = 0;
	m_StreamBufferUsed -= Size;
	m_StreamLock.Unlock();
	return true;
}


void CTvRockDTVTarget::PurgeStream()
{
	m_StreamLock.Lock();
	m_StreamBufferUsed = 0;
	m_StreamBufferPos = 0;
	m_StreamLock.Unlock();
	if (m_hProcessTSEvent == NULL || m_hStreamThread == NULL)
		return;
	::ResetEvent(m_hProcessTSEvent);
	HANDLE Handles[] = { m_hProcessTSEvent, m_hStreamThread };
	DWORD Result = ::WaitForMultipleObjects(2, Handles, FALSE, 2000);
	if (Result == WAIT_TIMEOUT || Result == WAIT_FAILED)
		Trace(TEXT("TvRock stream purge did not complete (result %lu)."), Result);
}


DWORD WINAPI CTvRockDTVTarget::StreamThread(LPVOID lpParameter)
{
	CTvRockDTVTarget* pThis = static_cast<CTvRockDTVTarget*>(lpParameter);
	BYTE* pBuffer = new BYTE[pThis->m_StreamSendBufferLength * 188];
	while (!pThis->m_fStreamEnd) {
		SIZE_T Length = pThis->m_StreamSendBufferLength;
		if (pThis->GetStream(pBuffer, &Length)) {
			pThis->ProcessTS(pBuffer, Length * 188);
			::SetEvent(pThis->m_hProcessTSEvent);
		}
		else {
			::SetEvent(pThis->m_hProcessTSEvent);
			::Sleep(5);
		}
	}
	delete[] pBuffer;
	return 0;
}


bool CALLBACK CTvRockDTVTarget::StreamCallback(const CMediaData* pMediaData, const PVOID pParam)
{
	CTvRockDTVTarget* pThis = static_cast<CTvRockDTVTarget*>(pParam);

    LibraryStream().Feed(pMediaData->GetData(), pMediaData->GetSize());
	pThis->m_StreamLock.Lock();
	if (pThis->m_StreamBufferUsed == pThis->m_StreamBufferLength) {
		pThis->m_DropCount++;
		pThis->m_StreamLock.Unlock();
		return TRUE;
	}
	::CopyMemory(pThis->m_pStreamBuffer + (pThis->m_StreamBufferPos + pThis->m_StreamBufferUsed) % pThis->m_StreamBufferLength * 188, pMediaData->GetData(), 188/*pMediaData->GetSize()*/);
	pThis->m_StreamBufferUsed++;
	pThis->m_StreamLock.Unlock();
	return true;
}


void CTvRockDTVTarget::SetTerrestrialSpace(LPCTSTR pszName)
{
	if (pszName == NULL)
		m_szTerrestrialSpaceName[0] = '\0';
	else
		::lstrcpyn(m_szTerrestrialSpaceName, pszName, lengthof(m_szTerrestrialSpaceName));
}
