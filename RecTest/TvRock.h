// Modified from RecTest (GPL; version unspecified in original notice).
// See LICENSE.md for the original GPL and warranty notices.
#ifndef TV_ROCK_H
#define TV_ROCK_H


#include "TvRockDTV.h"
#include "BonBaseClass.h"
#include <vector>


class CTvRockDTVTarget : public CBonBaseClass
{
	enum {
		DEFAULT_BUFFER_LENGTH=20000UL,
		DEFAULT_SEND_BUFFER_LENGTH=256UL
	};
	HINSTANCE m_hPlugin;
	TvRock::TvRockDTV *m_pTvRock;
	class CTvRockRequest *m_pRequest;
	class CChannelSearch *m_pChannel;
	int m_DeviceID;
	CCriticalLock m_StreamLock;
	HANDLE m_hProcessTSEvent;
	SIZE_T m_StreamBufferLength;
	BYTE *m_pStreamBuffer;
	SIZE_T m_StreamSendBufferLength;
	HANDLE m_hStreamThread;
	volatile bool m_fStreamEnd;
	SIZE_T m_StreamBufferUsed;
	SIZE_T m_StreamBufferPos;
	DWORD m_DropCount;
	TCHAR m_szTerrestrialSpaceName[32];
	struct InitialChannelInfo {
		int Space;
		int Channel;
		int DriverChannel;
	};
	std::vector<InitialChannelInfo> m_InitialChannels;
	HWND m_hwndInitialChannel;
	UINT_PTR m_InitialChannelTimer;
	SIZE_T m_InitialChannelIndex;
	DWORD m_InitialChannelSetTime;
	bool m_fInitialChannelStarted;
	bool m_fInitialChannelWaiting;
	bool m_fSelectingInitialChannel;
	void StartInitialChannelSelection();
	void StopInitialChannelSelection();
	void UpdateInitialChannelSelection();
	static void CALLBACK InitialChannelTimer(HWND hwnd,UINT Message,UINT_PTR ID,DWORD Time);
	void ProcessTS(BYTE *pData,DWORD Length);
	bool GetStream(BYTE *pBuffer,SIZE_T *pBufferLength);
	void PurgeStream();
	static DWORD WINAPI StreamThread(LPVOID lpParameter);
	static bool CALLBACK StreamCallback(const CMediaData *pMediaData,const PVOID pParam);
public:
	CTvRockDTVTarget();
	~CTvRockDTVTarget();
	bool Initialize();
	bool Finalize();
	enum {
		EVENT_CHANNELCHANGE,
		EVENT_SERVICECHANGE,
		EVENT_DRIVERCHANGE,
		EVENT_SERVICEUPDATE
	};
	void OnEvent(UINT Event);
	bool IsOpen() const { return m_pTvRock!=NULL; }
	void SetTerrestrialSpace(LPCTSTR pszName);
	int GetDeviceID() const { return m_DeviceID; }
};


#endif
