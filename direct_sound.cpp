#include <dsound.h>
#include "utility.h"
#include <stdio.h>
#include <dxerr9.h>
#include <process.h>
#pragma comment(lib, "dsound.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "dxerr8.lib")

#define NUM_REC_NOTIFICATIONS 16
WAVEFORMATEX WaveDataFormat={WAVE_FORMAT_PCM, 1,8000,16000,2,16, 0}; 

IDirectSoundFullDuplex8 *_ds_fp;
LPDIRECTSOUNDCAPTUREBUFFER8 _ds_capture_buffer = 0;
LPDIRECTSOUNDBUFFER8 _ds_playback_buffer = 0;
HWND _hwnd = 0;
HANDLE _hnotify[2]; // 通知事件
#define  BUFS 3
#define FRAMESIZE (160)
DataChunk _capture_buf;
DataChunk _playback_buf;

SOCKET _sock_sender;
SOCKET _sock_listen;
char forge_addr[256];
struct sockaddr_in _addr;

CRITICAL_SECTION _buf_lock;

// 数据使用 40ms，非常希望使用 20ms，但是看起来windows无法达到这个精度
#define BUFSIZE (FRAMESIZE*2*2*2)
int open_speaker_mic();
static unsigned __stdcall thread_work (void *param);
static unsigned __stdcall thread_listen (void *param);

int send_pcm(char *data, int len)
{
	int r = sendto(_sock_sender, data, len , 0, (sockaddr *)&_addr, sizeof(_addr));
	if (r <= 0)
	{
		printf ("sendto failed (%d)\n", WSAGetLastError());
		exit (1);
	}
	return r;
}

int init_aec(HWND hwnd, char *faddr, unsigned short fport, unsigned short lport)
{
	WSADATA data;
	WSAStartup(0x202, &data);
	CoInitialize(0);

	HRESULT hr;
	_hwnd = hwnd;
	_hnotify[0] = CreateEvent(0, 0, 0, 0);
	_hnotify[1] = CreateEvent(0, 0, 0, 0);
	_playback_buf.allocate(BUFSIZE * 20);

	struct sockaddr_in lo_addr;
	memset(&lo_addr, 0, sizeof(lo_addr));
	lo_addr.sin_addr.s_addr = INADDR_ANY;
	lo_addr.sin_family = AF_INET;
	lo_addr.sin_port = htons(lport);

	_sock_listen = socket(AF_INET, SOCK_DGRAM, 0);
	if (bind(_sock_listen, (sockaddr*)&lo_addr, sizeof(lo_addr)) < 0) {
		fprintf(stderr, "%s: bind %d err\n", "init_aec", lport);
		closesocket(_sock_listen);
		_sock_listen = INVALID_SOCKET;
		return -1;
	}
	printf("listen local port %u\n", lport);

	_sock_sender = socket(AF_INET, SOCK_DGRAM, 0);
	memset(&_addr, 0, sizeof(_addr));
	_addr.sin_addr.s_addr = inet_addr(faddr);
	_addr.sin_family = AF_INET;
	_addr.sin_port = htons(fport);
	sprintf(forge_addr, "%s:%u", faddr, fport);
	printf("sender voice to %s\n", forge_addr);

	InitializeCriticalSection(&_buf_lock);

	if (open_speaker_mic() != 0)
	{
		printf("open speaker failed\n");
		closesocket(_sock_sender);
		closesocket(_sock_listen);
		return -2;
	}
	hr = _ds_capture_buffer->Start(DSCBSTART_LOOPING);
	hr = _ds_playback_buffer->Play(0, 0, DSBPLAY_LOOPING);

	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
	HANDLE th = (HANDLE)_beginthreadex(0, 0, thread_work, 0, 0, 0);
 	SetThreadPriority(th, THREAD_PRIORITY_TIME_CRITICAL);
	_beginthreadex(0, 0, thread_listen, 0, 0, 0);
	
	return 0;
}

void error_print(HRESULT hr)
{
	switch (hr)
	{
	case DSERR_ALLOCATED:
		printf("DSERR_ALLOCATED \n");
		break;
	case DSERR_INVALIDCALL: 
		printf( "DSERR_INVALIDCALL \n" );	
		break; 
	case DSERR_INVALIDPARAM:
		printf("DSERR_INVALIDPARAM \n");
		break;
	case DSERR_NOAGGREGATION:
		printf("DSERR_NOAGGREGATION \n");
		break;
	case DSERR_NODRIVER:
		printf("DSERR_NODRIVER \n");
		break;
	case DSERR_OUTOFMEMORY:
		printf("DSERR_OUTOFMEMORY \n");
		break;
	default:
		printf("unknow error %ld\n", hr);
		break;
	}
}
#define  SAMPLE_RATE 8000
int open_speaker_mic()
{
	HRESULT hr;
	int i;

	WAVEFORMATEX wfx;
	wfx.wFormatTag = WAVE_FORMAT_PCM;
	wfx.nChannels = 1;
	wfx.nSamplesPerSec = SAMPLE_RATE;
	wfx.wBitsPerSample = 16;
	wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
	wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
	wfx.cbSize = 0;

#if 1
	DSBUFFERDESC desc;
	memset(&desc, 0, sizeof(desc)); 
	desc.dwSize = sizeof(desc);
	desc.dwFlags =	DSBCAPS_CTRLFX
					| DSBCAPS_GLOBALFOCUS 
					| DSBCAPS_CTRLPOSITIONNOTIFY 
					| DSBCAPS_GETCURRENTPOSITION2
					| DSBCAPS_CTRLFREQUENCY;
	desc.dwBufferBytes = BUFSIZE * BUFS;//待定
	desc.dwReserved = 0;
	desc.lpwfxFormat = &wfx;

	//捕捉缓冲区AEC和NS效果。
	DSCEFFECTDESC efft[2];
	memset(efft, 0, sizeof(efft));
	//AEC效果
	efft[0].dwSize = sizeof(efft[0]);
	efft[0].dwFlags = DSCFX_LOCSOFTWARE;
	efft[0].guidDSCFXClass = GUID_DSCFX_CLASS_AEC;
	efft[0].guidDSCFXInstance = GUID_DSCFX_MS_AEC;
// 	efft[0].guidDSCFXInstance = GUID_DSCFX_SYSTEM_AEC;
	//NS效果
	efft[1].dwSize = sizeof(efft[1]);
	efft[1].dwFlags = DSCFX_LOCSOFTWARE;
	efft[1].guidDSCFXClass = GUID_DSCFX_CLASS_NS;
	efft[1].guidDSCFXInstance = GUID_DSCFX_MS_NS;
// 	efft[1].guidDSCFXInstance = GUID_DSCFX_SYSTEM_NS;

	//捕捉缓冲区。capture buffer
	DSCBUFFERDESC cdesc;
	memset(&cdesc, 0, sizeof(cdesc));
	cdesc.dwSize = sizeof(cdesc);
	cdesc.dwFlags = DSCBCAPS_CTRLFX; 
	cdesc.dwBufferBytes =  BUFSIZE * BUFS;//待定
	cdesc.lpwfxFormat = &wfx;
	cdesc.dwFXCount = 2;
	cdesc.lpDSCFXDesc = efft;
#else
	DSBUFFERDESC desc;
	memset(&desc, 0, sizeof(desc)); 
	desc.dwSize = sizeof(desc);
	desc.dwFlags = DSBCAPS_CTRLPOSITIONNOTIFY | DSBCAPS_GETCURRENTPOSITION2 |
		DSBCAPS_GLOBALFOCUS | DSBCAPS_LOCSOFTWARE;
	desc.dwBufferBytes = BUFSIZE * BUFS;//待定
	desc.dwReserved = 0;
	desc.lpwfxFormat = &wfx;

	//捕捉缓冲区。capture buffer
	DSCBUFFERDESC cdesc;
	memset(&cdesc, 0, sizeof(cdesc));
	cdesc.dwSize = sizeof(cdesc);
	cdesc.dwFlags = 0; 
	cdesc.dwBufferBytes =  BUFSIZE * BUFS;//待定
	cdesc.lpwfxFormat = &wfx;
#endif

	hr = DirectSoundFullDuplexCreate(0, 0,
		&cdesc, &desc, _hwnd, 
		DSSCL_PRIORITY,
		&_ds_fp, 
		&_ds_capture_buffer,
		&_ds_playback_buffer, 
		0);
	if ((hr != DS_OK))
	{
		int error = WSAGetLastError();
		error_print(hr);
		return -1;
	}

	LPDIRECTSOUNDNOTIFY8 notify;
	_ds_capture_buffer->QueryInterface(IID_IDirectSoundNotify8, (void**)&notify);
	DSBPOSITIONNOTIFY pts[BUFS];
	for (i = 0; i < BUFS; i++) {
		pts[i].dwOffset = (i + 1) * BUFSIZE - 1;
		pts[i].hEventNotify = _hnotify[0];
	}
	notify->SetNotificationPositions(BUFS, pts);
	notify->Release();
	
	hr = _ds_playback_buffer->QueryInterface(IID_IDirectSoundNotify8, (void**)&notify);
	if (FAILED(hr))
	{
		printf("pb buffer QueryInterface failed %ld\n", hr);
		printf("device on support\n");
		int error = WSAGetLastError();
		return -1;
	}
	for (i = 0; i < BUFS; i++) {
		pts[i].dwOffset = (i + 1) * BUFSIZE - 1;
		pts[i].hEventNotify = _hnotify[1];
	}
	notify->SetNotificationPositions(BUFS, pts);
	notify->Release();

	return 0;
}

static unsigned __stdcall thread_work (void *param)
{
	HRESULT hr;
	unsigned char buf_out[BUFSIZE];
	unsigned char buf_in[BUFSIZE];

	while (1) 
	{

	DWORD rc = WaitForMultipleObjects(2, _hnotify, FALSE, -1);
	if (rc == WAIT_OBJECT_0)
	{
		DWORD wp, rp;
		hr = _ds_capture_buffer->GetCurrentPosition(&wp, &rp);
		
		// capture notify
		void *p1, *p2;
		DWORD l1, l2;
		hr = _ds_capture_buffer->Lock(rp, BUFSIZE, &p1, &l1, &p2, &l2, 0);
		if (hr != S_OK) {
			fprintf(stderr, "%s: capture Lock err\n", "thread_work");
			exit(-1);
		}
		
#if 0		// save
		if (_capture_buf.freespace() < BUFSIZE) {
			fprintf(stderr, "\n%s: capture buf overflow!!!!\n", "thread_work");
			// exit(-1);
			//util_cbuf_consume(_cbuf_input, CBUFSIZE/2);
			_capture_buf.data_clear(); // 清空得了
		}

		_capture_buf.push_back((unsigned char *)p1, l1);
		_capture_buf.push_back((unsigned char *)p2, l2);
#endif
		
		if (l1 >= BUFSIZE) {
			memcpy(buf_in, p1, l1);
		}
		else {
			memcpy(buf_in, p1, l1);
			memcpy(buf_in + l1, p2, l2);
		}

		_ds_capture_buffer->Unlock(p1, l1, p2, l2);
// 		printf("send voice to %s\n", forge_addr);
		send_pcm((char *)buf_in, BUFSIZE);
	}
	else
	{
		memset(buf_out, 0, BUFSIZE);
		EnterCriticalSection(&_buf_lock);
		_playback_buf.pop_front(buf_out, BUFSIZE);
		LeaveCriticalSection(&_buf_lock);

		DWORD wp, pp;
		hr = _ds_playback_buffer->GetCurrentPosition(&pp, &wp);
		
		void *p1, *p2;
		DWORD l1, l2;
		hr = _ds_playback_buffer->Lock(wp, BUFSIZE, &p1, &l1, &p2, &l2, 0);
		//hr = _ds_playback_buffer->Lock(pos_write, BUFSIZE, &p1, &l1, &p2, &l2, 0);
		if (hr != S_OK)
		{
			fprintf(stderr, "\n%s: playback Lock err\n", "thread_work");
			::exit(-1);
		}
		if (l1 >= BUFSIZE) {
			memcpy(p1, buf_out, l1);
		}
		else {
			memcpy(p1, buf_out, l1);
			memcpy(p2, buf_out+l1, l2);
		}
		
		hr = _ds_playback_buffer->Unlock(p1, l1, p2, l2);
	}
	}
	return 0;
}

static unsigned __stdcall thread_listen (void *param)
{
	while (1) {
		char buf[BUFSIZE];
		sockaddr_in from;
		int len = sizeof(from);
		int rc = recvfrom(_sock_listen, buf, sizeof(buf), 0, (sockaddr*)&from, &len);
		EnterCriticalSection(&_buf_lock);
		if (rc > 0) {
// 			printf("recv data %d bytes\n", rc);
			if (_playback_buf.freespace() < (size_t)rc) {
				fprintf(stderr, "%s: _cbuf_recv OVERFLOW!!!!!\n", "thread_listen");
				_playback_buf.data_clear();
			}
			_playback_buf.push_back((unsigned char *)buf, rc);
		}
		else {
			fprintf(stderr, "%s: recvfrom err\n", "thread_listen");
			exit(-1);
		}
		LeaveCriticalSection(&_buf_lock);
	}
	
	return 0;
}