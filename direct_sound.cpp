#include "direct_sound.h"
#include <dsound.h>
#include <stdio.h>
#include <dxerr9.h>
#include <process.h>
#pragma comment(lib, "dsound.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "dxerr8.lib")

static IDirectSoundFullDuplex8 *_ds_fp;
static LPDIRECTSOUNDCAPTUREBUFFER8 _ds_capture_buffer = 0;
static LPDIRECTSOUNDBUFFER8 _ds_playback_buffer = 0;
static HWND _hwnd = 0;
static HANDLE _hnotify[2];
static HANDLE _work_notify;
static CRITICAL_SECTION _buf_lock;
//play control
static int _status_play = 0;
static int _status_capture = 0;
static int _status_playback = 0;

// 开启ACE 经测试最小时间间隔为128
#define BUFS 3
#define FRAMESIZE (160)
#define BUFSIZE (FRAMESIZE*2*2*2)
#define SAMPLE_RATE 8000

//处理音频数据的用户回调函数
typedef int (*sound_data_handler_t)(void *userp, void *data, size_t data_len);
static void *_capture_user = 0;
static sound_data_handler_t _capture_handler = 0;
static void *_playback_user = 0;
static sound_data_handler_t _playback_handler = 0;
static void set_handler(void *userp, sound_data_handler_t handler, int is_capture);

void set_handler_capture(void *userp, sound_data_handler_t handler)
{
	set_handler(userp, handler, 1);
}

void set_handler_playback(void *userp, sound_data_handler_t handler)
{
	set_handler(userp, handler, 0);
}


// data buf
BufferChunk _capture_buf;
BufferChunk _playback_buf;

// for demo
SOCKET _sock_sender;
SOCKET _sock_listen;
char forge_addr[256];
struct sockaddr_in _addr;

static int open_speaker_mic();
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

static int demo_cap_handler(void *userp, void *data, size_t data_len)
{
	return send_pcm((char *)data, data_len);
}

static int demo_pb_handler(void *userp, void *data, size_t data_len)
{
	return (int)_playback_buf.pop_front((unsigned char *)data, data_len);
}

// demo trans audio data
static int init_udp_sock(char *faddr, unsigned short fport, unsigned short lport)
{
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
	return 0;
}

int init_direct_sound(HWND hwnd)
{
	CoInitialize(0);
	_hwnd = hwnd;
	_hnotify[0] = CreateEvent(0, 0, 0, 0);
	_hnotify[1] = CreateEvent(0, 0, 0, 0);
	_work_notify = CreateEvent(0, 0, 0, 0);
	InitializeCriticalSection(&_buf_lock);
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

	if (open_speaker_mic() != 0)
	{
		printf("open speaker failed\n");
		return -2;
	}
	return 0;
}

int direct_sound_ctrl(unsigned int flags)
{
	if (flags == _DS_STOP)
	{
		_status_play = 0;
		WaitForSingleObject(_work_notify, -1);
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
	}
	else
	{
        _status_play = 1;
		HRESULT hr;
		ResetEvent(_work_notify);
		HANDLE th = (HANDLE)_beginthreadex(0, 0, thread_work, 0, 0, 0);
		SetThreadPriority(th, THREAD_PRIORITY_TIME_CRITICAL);

		if (flags & _DS_CAPTURE)
			hr = _ds_capture_buffer->Start(DSCBSTART_LOOPING);
		if (flags & _DS_PLAYBACK)
			hr = _ds_playback_buffer->Play(0, 0, DSBPLAY_LOOPING);
		
	}

	return 0;
}

static void set_handler(void *userp, sound_data_handler_t handler, int is_capture)
{
	if (is_capture)
	{
		_capture_handler = handler;
		_capture_user = userp;
	}
	else
	{
		_playback_handler = handler;
		_playback_user = userp;
	}
}

int init_aec(HWND hwnd, char *faddr, unsigned short fport, unsigned short lport)
{
	WSADATA data;
	WSAStartup(0x202, &data);

	if ( 0 != init_udp_sock(faddr, fport, lport) )
		return -1;
	_playback_buf.allocate(BUFSIZE * 20);

	if (init_direct_sound(hwnd) != 0)
	{
		closesocket(_sock_sender);
		closesocket(_sock_listen);
		return -2;
	}

	set_handler(0, demo_cap_handler, 1);
	set_handler(0, demo_pb_handler, 0);
	direct_sound_ctrl(_DS_CAPTURE | _DS_PLAYBACK);
	_beginthreadex(0, 0, thread_listen, 0, 0, 0);
	return 0;
}

void lock_buffer(void)
{
	EnterCriticalSection(&_buf_lock);
}

void unlock_buffer(void)
{
	LeaveCriticalSection(&_buf_lock);
}

static int open_speaker_mic()
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

	hr = DirectSoundFullDuplexCreate8(0, 0,
		&cdesc, &desc, _hwnd, 
		DSSCL_PRIORITY,
		&_ds_fp, 
		&_ds_capture_buffer,
		&_ds_playback_buffer, 
		0);
	if ((hr != DS_OK))
	{
        printf("DirectSoundFullDuplexCreate8 failed %ld\n", hr);
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

	while (_status_play) 
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
			
		if (l1 >= BUFSIZE) {
			memcpy(buf_in, p1, l1);
		}
		else {
			memcpy(buf_in, p1, l1);
			memcpy(buf_in + l1, p2, l2);
		}

		_ds_capture_buffer->Unlock(p1, l1, p2, l2);

		lock_buffer();
		if (_capture_handler)
			(*_capture_handler) (_capture_user, buf_in, BUFSIZE);
		unlock_buffer();
	}
	else
	{
		memset(buf_out, 0, BUFSIZE);
		EnterCriticalSection(&_buf_lock);
		if (_playback_handler)
			(*_playback_handler)(_playback_user, buf_out, BUFSIZE);	
		LeaveCriticalSection(&_buf_lock);

		DWORD wp, pp;
		hr = _ds_playback_buffer->GetCurrentPosition(&pp, &wp);
		
		void *p1, *p2;
		DWORD l1, l2;
		hr = _ds_playback_buffer->Lock(wp, BUFSIZE, &p1, &l1, &p2, &l2, 0);
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
    SetEvent(_work_notify);
	return 0;
}

static unsigned __stdcall thread_listen (void *param)
{
	while (1) {
		char buf[BUFSIZE];
		sockaddr_in from;
		int len = sizeof(from);
		int rc = recvfrom(_sock_listen, buf, sizeof(buf), 0, (sockaddr*)&from, &len);
		lock_buffer();
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
		unlock_buffer();
	}
	
	return 0;
}

