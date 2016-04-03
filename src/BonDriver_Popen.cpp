#include "BonDriver_Popen.h"

namespace BonDriver_Popen {

static char g_strSpace[32];
static stChannel g_stChannels[2][MAX_CH];
static int g_Type;			// 0 : ISDB_S / 1 : ISDB_T  <-  設定ファイルで指定(デフォルト=0)
static float g_SignalLevel = 0.0f;		// GetSignalLevelで返す値(一定値)
static int g_PollTimeout = 5000;		// Poll用タイムアウト(msec)
static int g_WaitTimeout = 2000;		// SIGKILL用タイムアウト(msec)
static int g_TsBufferSize = TS_BUFSIZE;	// TSバッファのサイズ
static BOOL g_UseServiceID;
static DWORD g_Crc32Table[256];
static BOOL g_ModPMT;
static BOOL g_TsSync;
static DWORD g_dwDelFlag;

static int Convert(char *src, char *dst, size_t dstsize)
{
	iconv_t d = ::iconv_open("UTF-16LE", "UTF-8");
	if (d == (iconv_t)-1)
		return -1;
	size_t srclen = ::strlen(src) + 1;
	size_t dstlen = dstsize - 2;
	size_t ret = ::iconv(d, &src, &srclen, &dst, &dstlen);
	*dst = *(dst + 1) = '\0';
	::iconv_close(d);
	if (ret == (size_t)-1)
		return -2;
	return 0;
}

static void InitCrc32Table()
{
	DWORD i, j, crc;
	for (i = 0; i < 256; i++)
	{
		crc = i << 24;
		for (j = 0; j < 8; j++)
			crc = (crc << 1) ^ ((crc & 0x80000000) ? 0x04c11db7 : 0);
		g_Crc32Table[i] = crc;
	}
}

static DWORD CalcCRC32(BYTE *p, DWORD len)
{
	DWORD i, crc = 0xffffffff;
	for (i = 0; i < len; i++)
		crc = (crc << 8) ^ g_Crc32Table[(crc >> 24) ^ p[i]];
	return crc;
}

static BOOL IsTagMatch(const char *line, const char *tag, char **value)
{
	const int taglen = ::strlen(tag);
	const char *p;

	if (::strncmp(line, tag, taglen) != 0)
		return FALSE;
	p = line + taglen;
	while (*p == ' ' || *p == '\t')
		p++;
	if (value == NULL && *p == '\0')
		return TRUE;
	if (*p++ != '=')
		return FALSE;
	while (*p == ' ' || *p == '\t')
		p++;
	*value = const_cast<char *>(p);
	return TRUE;
}

static int Init()
{
	FILE *fp;
	char *p, buf[512];
	std::string defaultcommand[2];

	Dl_info info;
	if (::dladdr((void *)Init, &info) == 0)
		return -1;
	::strncpy(buf, info.dli_fname, sizeof(buf) - 8);
	buf[sizeof(buf) - 8] = '\0';
	::strcat(buf, ".conf");

	fp = ::fopen(buf, "r");
	if (fp == NULL)
		return -2;
	for (int i = 0; i < MAX_CH; i++)
	{
		g_stChannels[0][i].bUnused = TRUE;
		g_stChannels[1][i].bUnused = TRUE;
	}

	int idx = 0;
	BOOL bitFlag = FALSE;
	BOOL bscFlag = FALSE;
	BOOL btcFlag = FALSE;
	BOOL bslFlag = FALSE;
	BOOL bptFlag = FALSE;
	BOOL bwtFlag = FALSE;
	BOOL bbsFlag = FALSE;
	BOOL bsFlag = FALSE;
	BOOL bmFlag = FALSE;
	BOOL btFlag = FALSE;
	BOOL bdFlag = FALSE;
	while (::fgets(buf, sizeof(buf), fp))
	{
		if (buf[0] == ';')
			continue;
		p = buf + ::strlen(buf) - 1;
		if (*p != '\n')
		{
			fprintf(stderr, "stop too long line = %s\n", buf);
			::fclose(fp);
			return -3;
		}
		while ((p >= buf) && (*p == '\r' || *p == '\n'))
			*p-- = '\0';
		if (p < buf)
			continue;
		if ((idx != 0) && IsTagMatch(buf, "#ISDB_S", NULL))
			idx = 0;
		else if ((idx != 1) && IsTagMatch(buf, "#ISDB_T", NULL))
			idx = 1;
		else if (!bitFlag && IsTagMatch(buf, "#ISDBTYPE", &p))
		{
			g_Type = ::atoi(p);
			if (g_Type < 0 or g_Type > 1)
				g_Type = 0;
			if (g_Type == 0)
				p = (char *)"BS/CS110";
			else
				p = (char *)"UHF";
			if (Convert(p, g_strSpace, sizeof(g_strSpace)) < 0)
			{
				::fclose(fp);
				return -3;
			}
			bitFlag = TRUE;
		}
		else if (!bscFlag && IsTagMatch(buf, "#ISDBSCOMMAND", &p))
		{
			defaultcommand[0] = p;
			bscFlag = TRUE;
		}
		else if (!btcFlag && IsTagMatch(buf, "#ISDBTCOMMAND", &p))
		{
			defaultcommand[1] = p;
			btcFlag = TRUE;
		}
		else if (!bslFlag && IsTagMatch(buf, "#SIGNALLEVEL", &p))
		{
			g_SignalLevel = (float)::atof(p);
			bslFlag = TRUE;
		}
		else if (!bptFlag && IsTagMatch(buf, "#POLLTIMEOUT", &p))
		{
			g_PollTimeout = ::atoi(p);
			if (g_PollTimeout < 0)
				g_PollTimeout = 0;
			bptFlag = TRUE;
		}
		else if (!bwtFlag && IsTagMatch(buf, "#WAITTIMEOUT", &p))
		{
			g_WaitTimeout = ::atoi(p);
			if (g_WaitTimeout < 100)	// 100msec
				g_WaitTimeout = 100;
			bwtFlag = TRUE;
		}
		else if (!bbsFlag && IsTagMatch(buf, "#TSBUFFERSIZE", &p))
		{
			// デフォルトは 188*256=48128
			int size = ::atoi(p);
			if (size > 0)
			{
				// 188の倍数でない場合は倍数へ変更
				if (size % TS_PKTSIZE != 0)
					size = (size / TS_PKTSIZE + 1) * TS_PKTSIZE;
				g_TsBufferSize = size;
				bbsFlag = TRUE;
			}
		}
		else if (!bsFlag && IsTagMatch(buf, "#USESERVICEID", &p))
		{
			g_UseServiceID = ::atoi(p);
			bsFlag = TRUE;
		}
		else if (!bmFlag && IsTagMatch(buf, "#MODPMT", &p))
		{
			g_ModPMT = ::atoi(p);
			bmFlag = TRUE;
		}
		else if (!btFlag && IsTagMatch(buf, "#TSSYNC", &p))
		{
			g_TsSync = ::atoi(p);
			btFlag = TRUE;
		}
		else if (!bdFlag && IsTagMatch(buf, "#DEL", &p))
		{
			char *pb = p;
			const char *name[] = { "EIT", "H-EIT", "M-EIT", "L-EIT", "CAT", "NIT", "SDT", "TOT", "SDTT", "BIT", "CDT", "ECM", "EMM", "TYPED", NULL };
			int n, cnt = 1;
			while (*p != '\0')
			{
				if (*p == ',')
					cnt++;
				p++;
			}
			char **pp = new char *[cnt];
			p = pb;
			n = 0;
			do
			{
				while (*p == '\t' || *p == ' ')
					p++;
				pp[n++] = p;
				while (*p != '\t' && *p != ' ' && *p != ',' && *p != '\0')
					p++;
				if (*p != ',' && *p != '\0')
				{
					*p++ = '\0';
					while (*p != ',' && *p != '\0')
						p++;
				}
				*p++ = '\0';
			} while (n < cnt);
			for (int i = 0; i < cnt; i++)
			{
				for (int j = 0; name[j] != NULL; j++)
				{
					if (strcmp(pp[i], name[j]) == 0)
					{
						if (j == 0)
							g_dwDelFlag |= 0x7;		// EIT = H-EIT | M-EIT | L-EIT
						else
							g_dwDelFlag |= (1 << (j - 1));
						break;
					}
				}
			}
			delete[] pp;
			bdFlag = TRUE;
		}
		else
		{
			int n = 0;
			char *cp[5];
			BOOL bOk = FALSE;
			p = cp[n++] = buf;
			while (1)
			{
				p = ::strchr(p, '\t');
				if (p)
				{
					*p++ = '\0';
					cp[n++] = p;
				}
				else if (n >= 4)
				{
					bOk = TRUE;
					break;
				}
				else
					break;
			}
			if (bOk)
			{
				// cp[1] : BonDriverとしてのチャンネル番号
				DWORD dw = ::atoi(cp[1]);
				if (dw < MAX_CH)
				{
					// cp[0] : チャンネル名称
					if (Convert(cp[0], g_stChannels[idx][dw].strChName, MAX_CN_LEN) < 0)
					{
						::fclose(fp);
						return -3;
					}
					// cp[2] : コマンド用チャンネル番号
					// cp[3] : サービスID
					// cp[4] : 個別実行コマンド
					g_stChannels[idx][dw].Channel = cp[2];
					g_stChannels[idx][dw].ServiceID = ::strtoul(cp[3], NULL, 10);
					if (n <= 4)
						g_stChannels[idx][dw].Command = defaultcommand[idx];
					else
						g_stChannels[idx][dw].Command = cp[4];
					std::string &strsrc = g_stChannels[idx][dw].Command;
					const std::string strold = "{channel}";
					std::string &strnew =  g_stChannels[idx][dw].Channel;
					if (not strsrc.empty())
					{
						std::string::size_type pos = strsrc.find(strold);
						while(pos != std::string::npos)
						{
							strsrc.replace(pos, strold.size(), strnew);
							pos = strsrc.find(strold, pos + strnew.size());
						}
					}
					else
					{
						fprintf(stderr, "CH %d (%s) undefined command\n", idx, cp[0]);
						::fclose(fp);
						return -3;
					}
					g_stChannels[idx][dw].bUnused = FALSE;
				}
			}
		}
	}
	::fclose(fp);
	if (g_UseServiceID)
		InitCrc32Table();
	return 0;
}

cBonDriverPopen *cBonDriverPopen::m_spThis = NULL;
cCriticalSection cBonDriverPopen::m_sInstanceLock;
BOOL cBonDriverPopen::m_sbInit = TRUE;

extern "C" IBonDriver *CreateBonDriver()
{
	LOCK(cBonDriverPopen::m_sInstanceLock);
	if (cBonDriverPopen::m_sbInit)
	{
		if (Init() < 0)
			return NULL;
		cBonDriverPopen::m_sbInit = FALSE;
	}

	// 複数読み込み禁止
	cBonDriverPopen *pPopen = NULL;
	if (cBonDriverPopen::m_spThis == NULL)
		pPopen = new cBonDriverPopen();
	return pPopen;
}

cBonDriverPopen::cBonDriverPopen() : m_fifoTS(m_c, m_m), m_fifoRawTS(m_c, m_m), m_StopTsSplit(m_c, m_m)
{
	m_spThis = this;
	Convert((char *)TUNER_NAME, m_TunerName, sizeof(m_TunerName));
	m_LastBuf = NULL;
	m_bTuner = FALSE;
	m_fCNR = 0;
	m_dwSpace = m_dwChannel = 0x7fffffff;	// INT_MAX
	m_dwServiceID = 0xffffffff;
	m_pid = m_fd = -1;
	m_hTsRead = m_hTsSplit = 0;
	m_bStopTsRead = m_bChannelChanged = m_bUpdateCNR = FALSE;
	m_dwUnitSize = m_dwSyncBufPos = 0;

	pthread_mutexattr_t attr;
	::pthread_mutexattr_init(&attr);
	::pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	::pthread_mutex_init(&m_m, &attr);
	::pthread_cond_init(&m_c, NULL);
}

cBonDriverPopen::~cBonDriverPopen()
{
	CloseTuner();
	{
		LOCK(m_writeLock);
		TsFlush(g_UseServiceID);
		delete m_LastBuf;
	}
	::pthread_cond_destroy(&m_c);
	::pthread_mutex_destroy(&m_m);
	m_spThis = NULL;
}

const BOOL cBonDriverPopen::OpenTuner(void)
{
	if (m_bTuner)
		return TRUE;

	// 何もせずにTRUEを返す。
	// プロセスの起動はSetChannelで行う。

	m_bTuner = TRUE;
	return TRUE;
}

void cBonDriverPopen::CloseTuner(void)
{
	if (m_bTuner)
	{
		// プロセスの停止
		StopProcess(&m_pid, &m_fd);
		if (m_hTsRead)
		{
			m_bStopTsRead = TRUE;
			::pthread_join(m_hTsRead, NULL);
			m_hTsRead = 0;
		}
		m_bTuner = FALSE;
	}
}

const BOOL cBonDriverPopen::SetChannel(const BYTE bCh)
{
	return TRUE;
}

const float cBonDriverPopen::GetSignalLevel(void)
{
	timespec ts;
	ts.tv_sec = 0;
	ts.tv_nsec = 10 * 1000 * 1000;
	for (int i = 0; m_bUpdateCNR && (i < 50); i++)
		::nanosleep(&ts, NULL);
	return m_fCNR;
}

const DWORD cBonDriverPopen::WaitTsStream(const DWORD dwTimeOut)
{
	if (!m_bTuner)
		return WAIT_ABANDONED;
	if (m_fifoTS.Size() != 0)
		return WAIT_OBJECT_0;
	else
		return WAIT_TIMEOUT;	// 手抜き
}

const DWORD cBonDriverPopen::GetReadyCount(void)
{
	if (!m_bTuner)
		return 0;
	return (DWORD)m_fifoTS.Size();
}

const BOOL cBonDriverPopen::GetTsStream(BYTE *pDst, DWORD *pdwSize, DWORD *pdwRemain)
{
	if (!m_bTuner)
		return FALSE;
	BYTE *pSrc;
	if (GetTsStream(&pSrc, pdwSize, pdwRemain))
	{
		if (*pdwSize)
			::memcpy(pDst, pSrc, *pdwSize);
		return TRUE;
	}
	return FALSE;
}

const BOOL cBonDriverPopen::GetTsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain)
{
	if (!m_bTuner)
		return FALSE;
	BOOL b;
	{
		LOCK(m_writeLock);
		if (m_fifoTS.Size() != 0)
		{
			delete m_LastBuf;
			m_fifoTS.Pop(&m_LastBuf);
			*ppDst = m_LastBuf->pbBuf;
			*pdwSize = m_LastBuf->dwSize;
			*pdwRemain = (DWORD)m_fifoTS.Size();
			b = TRUE;
		}
		else
		{
			*pdwSize = 0;
			*pdwRemain = 0;
			b = FALSE;
		}
	}
	return b;
}

void cBonDriverPopen::PurgeTsStream(void)
{
	if (!m_bTuner)
		return;
	{
		LOCK(m_writeLock);
		TsFlush(g_UseServiceID);
	}
}

void cBonDriverPopen::Release(void)
{
	LOCK(m_sInstanceLock);
	delete this;
}

LPCTSTR cBonDriverPopen::GetTunerName(void)
{
	return (LPCTSTR)m_TunerName;
}

const BOOL cBonDriverPopen::IsTunerOpening(void)
{
	return FALSE;
}

LPCTSTR cBonDriverPopen::EnumTuningSpace(const DWORD dwSpace)
{
	if (!m_bTuner)
		return NULL;
	if (dwSpace != 0)
		return NULL;
	return (LPCTSTR)g_strSpace;
}

LPCTSTR cBonDriverPopen::EnumChannelName(const DWORD dwSpace, const DWORD dwChannel)
{
	if (!m_bTuner)
		return NULL;
	if (dwSpace != 0)
		return NULL;
	if (dwChannel >= MAX_CH)
		return NULL;
	if (g_stChannels[g_Type][dwChannel].bUnused)
		return NULL;
	return (LPCTSTR)(g_stChannels[g_Type][dwChannel].strChName);
}

const BOOL cBonDriverPopen::SetChannel(const DWORD dwSpace, const DWORD dwChannel)
{
	BOOL bFlag = FALSE;
	if (!m_bTuner)
		goto err;
	if (dwSpace != 0)
		goto err;
	if (dwChannel >= MAX_CH)
		goto err;
	if (g_stChannels[g_Type][dwChannel].bUnused)
		goto err;
	if (dwChannel == m_dwChannel)
		return TRUE;

	if (g_stChannels[g_Type][dwChannel].Command.empty())
	{
		::fprintf(stderr, "SetChannel() undefined command at channel = %u\n", dwChannel);
		goto err;
	}

	::fprintf(stderr, "type = %d channel = %u service id = %u command = %s\n",
		g_Type, dwChannel,
		g_stChannels[g_Type][dwChannel].ServiceID,
		g_stChannels[g_Type][dwChannel].Command.c_str()
	);

	bFlag = TRUE;
	if (g_UseServiceID)
	{
		if (m_dwChannel != 0x7fffffff)
		{
			if (g_stChannels[g_Type][dwChannel].Channel == g_stChannels[g_Type][m_dwChannel].Channel)
			{
				bFlag = FALSE;
			}
		}
		m_bChannelChanged = TRUE;
		m_dwServiceID = g_stChannels[g_Type][dwChannel].ServiceID;
	}

	if (bFlag)
	{
		StopProcess(&m_pid, &m_fd);
		if (StartProcess(g_stChannels[g_Type][dwChannel].Command.c_str(), &m_pid, &m_fd) < 0)
		{
			::perror("SetChannel() failed to start process");
			goto err;
		}

		// データが読み出せる状態になったかを確認する
		pollfd fds = {
			m_fd,
			POLLIN | POLLPRI | POLLERR | POLLHUP,
			0
		};
		int result = poll(&fds, 1, g_PollTimeout);
		if (result < 0)
		{
			::perror("SetChannel() poll");
			goto err;
		}
		else if (result == 0)
		{
			// Timeout
			::fprintf(stderr, "SetChannel() poll timeout\n");
			goto err;
		}
		else if ((fds.revents & POLLIN) or (fds.revents & POLLPRI))
		{
		}
		else if((fds.revents & POLLERR) or (fds.revents & POLLHUP))
		{
			::fprintf(stderr, "SetChannel() poll fd error\n");
			goto err;
		}
		else
		{
			goto err;
		}
	}

	{
		LOCK(m_writeLock);
		TsFlush(g_UseServiceID);
		m_bUpdateCNR = TRUE;
	}

	if (!m_hTsRead)
	{
		m_bStopTsRead = FALSE;
		if (::pthread_create(&m_hTsRead, NULL, cBonDriverPopen::TsReader, this))
		{
			::perror("pthread_create1");
			goto err;
		}
	}

	m_dwSpace = dwSpace;
	m_dwChannel = dwChannel;
	return TRUE;
err:
	StopProcess(&m_pid, &m_fd);
	m_bChannelChanged = FALSE;
	m_fCNR = 0;
	return FALSE;
}

const DWORD cBonDriverPopen::GetCurSpace(void)
{
	return m_dwSpace;
}

const DWORD cBonDriverPopen::GetCurChannel(void)
{
	return m_dwChannel;
}

void *cBonDriverPopen::TsReader(LPVOID pv)
{
	cBonDriverPopen *pPopen = static_cast<cBonDriverPopen *>(pv);
	DWORD now, before = 0;
	DWORD &ret = pPopen->m_tRet;
	ret = 300;
	BYTE *pBuf, *pTsBuf;
	timeval tv;
	timespec ts;
	int len, pos;

	if (g_UseServiceID)
	{
		if (::pthread_create(&(pPopen->m_hTsSplit), NULL, cBonDriverPopen::TsSplitter, pPopen))
		{
			::perror("pthread_create2");
			ret = 301;
			return &ret;
		}
	}

	ts.tv_sec = 0;
	ts.tv_nsec = WAIT_TIME * 1000 * 1000;

	// TS読み込みループ
	pTsBuf = new BYTE[g_TsBufferSize];
	pos = 0;
	while (!pPopen->m_bStopTsRead)
	{
		pPopen->m_writeLock.Enter();

		::gettimeofday(&tv, NULL);
		now = (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
		if (((now - before) >= 1000) || pPopen->m_bUpdateCNR)
		{
			pPopen->m_fCNR = g_SignalLevel;
			before = now;
			if (pPopen->m_bUpdateCNR)
			{
				pos = 0;
				pPopen->m_bUpdateCNR = FALSE;
			}
		}

		if (pos == g_TsBufferSize)
		{
			TS_DATA *pData = new TS_DATA();
			pData->dwSize = g_TsBufferSize;
			pData->pbBuf = pTsBuf;
			if (g_UseServiceID)
				pPopen->m_fifoRawTS.Push(pData);
			else
				pPopen->m_fifoTS.Push(pData);
			pTsBuf = new BYTE[g_TsBufferSize];
			pos = 0;
		}

		pPopen->m_writeLock.Leave();

		// この不自然な位置で読み込みを行うのは、ロック/アンロックの回数を減らしつつ
		// スレッド起動時に初回の読み込みを捨てないようにする為…
		pBuf = pTsBuf + pos;
		if ((len = ::read(pPopen->m_fd, pBuf, g_TsBufferSize - pos)) <= 0)
		{
			::nanosleep(&ts, NULL);
			continue;
		}
		pos += len;
	}
	delete[] pTsBuf;

	if (g_UseServiceID)
	{
		pPopen->m_StopTsSplit.Set();
		::pthread_join(pPopen->m_hTsSplit, NULL);
		pPopen->m_hTsSplit = 0;
		pPopen->m_StopTsSplit.Reset();
	}
	return &ret;
}

#define MAX_PID	0x2000		// (8 * sizeof(int))で割り切れる
#define PID_SET(pid, map)	((map)->bits[(pid) / (8 * sizeof(int))] |= (1 << ((pid) % (8 * sizeof(int)))))
#define PID_CLR(pid, map)	((map)->bits[(pid) / (8 * sizeof(int))] &= ~(1 << ((pid) % (8 * sizeof(int)))))
#define PID_ISSET(pid, map)	((map)->bits[(pid) / (8 * sizeof(int))] & (1 << ((pid) % (8 * sizeof(int)))))
#define PID_MERGE(dst, src)	{for(int i=0;i<(int)(MAX_PID / (8 * sizeof(int)));i++){(dst)->bits[i] |= (src)->bits[i];}}
#define PID_ZERO(map)		(::memset((map), 0 , sizeof(*(map))))
struct pid_set {
	int bits[MAX_PID / (8 * sizeof(int))];
};
#define FLAG_HEIT	0x0001
#define FLAG_MEIT	0x0002
#define FLAG_LEIT	0x0004
#define FLAG_CAT	0x0008
#define FLAG_NIT	0x0010
#define FLAG_SDT	0x0020
#define FLAG_TOT	0x0040
#define FLAG_SDTT	0x0080
#define FLAG_BIT	0x0100
#define FLAG_CDT	0x0200
#define FLAG_ECM	0x0400
#define FLAG_EMM	0x0800
#define FLAG_TYPED	0x1000

void *cBonDriverPopen::TsSplitter(LPVOID pv)
{
	cBonDriverPopen *pPopen = static_cast<cBonDriverPopen *>(pv);
	BYTE *pTsBuf, pPAT[TS_PKTSIZE];
	BYTE pPMT[4104+TS_PKTSIZE];	// 4104 = 8(TSヘッダ + pointer_field + table_idからsection_length) + 4096(セクション長最大値)
	BYTE pPMTPackets[TS_PKTSIZE*32];
	int pos, iNumSplit;
	unsigned char pat_ci, rpmt_ci, wpmt_ci, lpmt_version, lcat_version, ver;
	unsigned short ltsid, pidPMT, pidEMM, pmt_tail;
	BOOL bChangePMT, bSplitPMT, bPMTComplete;
	pid_set pids, save_pids[2], *p_new_pids, *p_old_pids;

	pTsBuf = new BYTE[g_TsBufferSize];
	pos = 0;
	pat_ci = 0x10;						// 0x1(payloadのみ) << 4 | 0x0(ci初期値)
	lpmt_version = lcat_version = wpmt_ci = 0xff;
	ltsid = pidPMT = pidEMM = 0xffff;	// 現在のTSID及びPMT,EMMのPID
	bChangePMT = bSplitPMT = bPMTComplete = FALSE;
	PID_ZERO(&pids);
	p_new_pids = &save_pids[0];
	p_old_pids = &save_pids[1];
	PID_ZERO(p_new_pids);
	PID_ZERO(p_old_pids);

	cEvent *h[2] = { &(pPopen->m_StopTsSplit), pPopen->m_fifoRawTS.GetEventHandle() };
	while (1)
	{
		DWORD dwRet = cEvent::MultipleWait(2, h);
		switch (dwRet)
		{
		case WAIT_OBJECT_0:
			goto end;

		case WAIT_OBJECT_0 + 1:
		{
			TS_DATA *pRawBuf = NULL;
			pPopen->m_fifoRawTS.Pop(&pRawBuf);
			if (pRawBuf == NULL)	// イベントのトリガからPop()までの間に別スレッドにFlush()される可能性はゼロではない
				break;
			BYTE *pSrc, *pSrcHead;
			DWORD dwLeft;
			if (g_TsSync)
			{
				pPopen->TsSync(pRawBuf->pbBuf, pRawBuf->dwSize, &pSrcHead, &dwLeft);
				pSrc = pSrcHead;
			}
			else
			{
				pSrc = pRawBuf->pbBuf;
				dwLeft = pRawBuf->dwSize;	// 必ずTS_PKTSIZEの倍数で来る
			}
			while (dwLeft > 0)
			{
				unsigned short pid = GetPID(&pSrc[1]);
				if (pid == 0x0000)	// PAT
				{
					// ビットエラー無しかつpayload先頭かつadaptation_field無し、PSIのpointer_fieldは0x00の前提
					if (!(pSrc[1] & 0x80) && (pSrc[1] & 0x40) && !(pSrc[3] & 0x20) && (pSrc[4] == 0x00))
					{
						// section_length
						// 9 = transport_stream_idからlast_section_numberまでの5バイト + CRC_32の4バイト
						int len = (((int)(pSrc[6] & 0x0f) << 8) | pSrc[7]) - 9;
						// 13 = TSパケットの頭から最初のprogram_numberまでのオフセット
						int off = 13;
						// PATは1TSパケットに収まってる前提
						while ((len >= 4) && ((off + 4) < TS_PKTSIZE))
						{
							unsigned short sid = GetSID(&pSrc[off]);
							if (pPopen->m_dwServiceID == sid)
							{
								pid = GetPID(&pSrc[off+2]);
								break;
							}
							off += 4;
							len -= 4;
						}
						if (pid != 0x0000)	// 対象ServiceIDのPMTのPIDが取得できた
						{
							// transport_stream_id
							unsigned short tsid = ((unsigned short )pSrc[8] << 8) | pSrc[9];
							if (pidPMT != pid || ltsid != tsid)	// PMTのPIDが更新された or チャンネルが変更された
							{
								// TSヘッダ
								pPAT[0] = 0x47;
								pPAT[1] = 0x60;
								pPAT[2] = 0x00;
								pPAT[3] = pat_ci;
								// pointer_field
								pPAT[4] = 0x00;
								// PAT
								pPAT[5] = 0x00;		// table_id
								pPAT[6] = 0xb0;		// section_syntax_indicator(1) + '0'(1) + reserved(2) + section_length(4/12)
								pPAT[7] = 0x11;		// section_length(8/12)
								pPAT[8] = tsid >> 8;
								pPAT[9] = tsid & 0xff;
								pPAT[10] = 0xc1;	// reserved(2) + version_number(5) + current_next_indicator(1)
								pPAT[11] = 0x00;	// section_number
								pPAT[12] = 0x00;	// last_section_number

								pPAT[13] = 0x00;	// program_number(8/16)
								pPAT[14] = 0x00;	// program_number(8/16)
								pPAT[15] = 0xe0;	// reserved(3) + network_PID(5/13)
								pPAT[16] = 0x10;	// network_PID(8/13)

								// 対象ServiceIDのテーブルコピー
								pPAT[17] = pSrc[off];
								pPAT[18] = pSrc[off+1];
								pPAT[19] = pSrc[off+2];
								pPAT[20] = pSrc[off+3];

								// CRC_32
								DWORD crc = CalcCRC32(&pPAT[5], 16);
								pPAT[21] = (BYTE)(crc >> 24);
								pPAT[22] = (BYTE)((crc >> 16) & 0xff);
								pPAT[23] = (BYTE)((crc >> 8) & 0xff);
								pPAT[24] = (BYTE)(crc & 0xff);

								::memset(&pPAT[25], 0xff, TS_PKTSIZE - 25);

								ltsid = tsid;
								pidPMT = pid;
								// PAT更新時には必ずPMT及びCATの更新処理を行う
								lpmt_version = lcat_version = 0xff;
								pidEMM = 0xffff;
								// PATより先に分割PMTの先頭が来ていた場合、そのPMTは破棄
								bSplitPMT = FALSE;
								// なんとなく
								wpmt_ci = 0xff;
							}
							else
							{
								if (pat_ci == 0x1f)
									pat_ci = 0x10;
								else
									pat_ci++;
								pPAT[3] = pat_ci;
							}
							::memcpy(&pTsBuf[pos], pPAT, TS_PKTSIZE);
							pos += TS_PKTSIZE;
						}
					}
				}
				else if (pid == 0x0001)	// CAT
				{
					if (!(g_dwDelFlag & FLAG_CAT))
					{
						// ビットエラー無しかつpayload先頭かつadaptation_field無し、PSIのpointer_fieldは0x00の前提
						if (!(pSrc[1] & 0x80) && (pSrc[1] & 0x40) && !(pSrc[3] & 0x20) && (pSrc[4] == 0x00))
						{
							// version_number
							ver = (pSrc[10] >> 1) & 0x1f;
							if (ver != lcat_version)
							{
								// section_length
								// 9 = 2つ目のreservedからlast_section_numberまでの5バイト + CRC_32の4バイト
								int len = (((int)(pSrc[6] & 0x0f) << 8) | pSrc[7]) - 9;
								// 13 = TSパケットの頭から最初のdescriptorまでのオフセット
								int off = 13;
								// CATも1TSパケットに収まってる前提
								while (len >= 2)
								{
									if ((off + 2) > TS_PKTSIZE)
										break;
									int cdesc_len = 2 + pSrc[off+1];
									if (cdesc_len > len || (off + cdesc_len) > TS_PKTSIZE)	// descriptor長さ異常
										break;
									if (pSrc[off] == 0x09)	// Conditional Access Descriptor
									{
										if (pSrc[off+1] >= 4 && (pSrc[off+4] & 0xe0) == 0xe0)	// 内容が妥当なら
										{
											// EMM PIDセット
											pid = GetPID(&pSrc[off+4]);
											if (pid != pidEMM)
											{
												if (pidEMM != 0xffff)
													PID_CLR(pidEMM, &pids);
												if (!(g_dwDelFlag & FLAG_EMM))
												{
													PID_SET(pid, &pids);
													pidEMM = pid;
												}
											}
											break;	// EMMが複数のPIDで送られてくる事は無い前提
										}
									}
									off += cdesc_len;
									len -= cdesc_len;
								}
								lcat_version = ver;
							}
							::memcpy(&pTsBuf[pos], pSrc, TS_PKTSIZE);
							pos += TS_PKTSIZE;
						}
					}
				}
				else if(pid == pidPMT)	// PMT
				{
					// ビットエラーがあったら無視
					if (pSrc[1] & 0x80)
						goto next;

					// 分割PMTをまとめる必要が無ければ
					if (!g_ModPMT)
					{
						// とりあえずコピーしてしまう
						::memcpy(&pTsBuf[pos], pSrc, TS_PKTSIZE);
						pos += TS_PKTSIZE;
					}

					int len;
					BYTE *p;
					// payload先頭か？(adaptation_field無し、PSIのpointer_fieldは0x00の前提)
					if ((pSrc[1] & 0x40) && !(pSrc[3] & 0x20) && (pSrc[4] == 0x00))
					{
						// version_number
						ver = (pSrc[10] >> 1) & 0x1f;
						if (ver != lpmt_version)	// バージョンが更新された
						{
							bChangePMT = TRUE;	// PMT更新処理開始
							bSplitPMT = FALSE;
							lpmt_version = ver;
							// 分割PMTをまとめる場合は
							if (g_ModPMT)
							{
								// 送信用PMTも更新を行う
								bPMTComplete = FALSE;
								// 送信用PMT用CI初期値保存
								if (wpmt_ci == 0xff)
									wpmt_ci = (pSrc[3] & 0x0f) | 0x10;
							}
						}
						// PMT更新処理中でなければ何もしない
						// (バージョンチェックのelseにしないのは、分割PMTの処理中にドロップがあった場合などの為)
						if (!bChangePMT)
						{
							// 分割PMTをまとめる場合かつ、送信用PMTができているなら
							if (g_ModPMT && bPMTComplete)
							{
							complete:
								for (int i = 0; i < iNumSplit; i++)
								{
									pPMTPackets[(TS_PKTSIZE * i) + 3] = wpmt_ci;
									if (wpmt_ci == 0x1f)
										wpmt_ci = 0x10;
									else
										wpmt_ci++;
								}
								int sent, left;
								sent = 0;
								left = TS_PKTSIZE * iNumSplit;
								while (1)
								{
									if ((pos + left) <= g_TsBufferSize)
									{
										::memcpy(&pTsBuf[pos], &pPMTPackets[sent], left);
										pos += left;
										break;
									}
									// バッファサイズが足りない場合
									int diff = (pos + left) - g_TsBufferSize;
									// 入るだけ入れて
									::memcpy(&pTsBuf[pos], &pPMTPackets[sent], (left - diff));
									// キューに投げ込んでから新たにバッファ確保
									TS_DATA *pData = new TS_DATA();
									pData->dwSize = g_TsBufferSize;
									pData->pbBuf = pTsBuf;
									pPopen->m_fifoTS.Push(pData);
									pTsBuf = new BYTE[g_TsBufferSize];
									pos = 0;
									// 送信済みサイズ及び残りサイズ更新
									sent += (left - diff);
									left = diff;
								}
							}
							goto next;
						}
						// section_length
						len = (((int)(pSrc[6] & 0x0f) << 8) | pSrc[7]);
						if (len > (TS_PKTSIZE - 8))	// TSパケットを跨ってる
						{
							::memcpy(pPMT, pSrc, TS_PKTSIZE);
							// コピーしたデータの終端位置
							pmt_tail = TS_PKTSIZE;
							bSplitPMT = TRUE;
							rpmt_ci = pSrc[3] & 0x0f;
							if (rpmt_ci == 0x0f)
								rpmt_ci = 0;
							else
								rpmt_ci++;
							goto next;
						}
						// 揃った
						p = pSrc;
					}
					else
					{
						if (!bChangePMT)	// PMT更新処理中でなければ
							goto next;
						if (!bSplitPMT)		// 分割PMTの続き待ち中でなければ
							goto next;
						// CIが期待している値ではない、もしくはpayloadが無い場合
						if (((pSrc[3] & 0x0f) != rpmt_ci) || !(pSrc[3] & 0x10))
						{
							// 最初からやり直し
							bSplitPMT = FALSE;
							goto next;
						}
						int adplen;
						if (pSrc[3] & 0x20)	// adaptation_field有り(まあ無いとは思うけど一応)
						{
							adplen = pSrc[4] + 1;
							if (adplen >= (TS_PKTSIZE - 4))
							{
								// adaptation_fieldの長さが異常なので最初からやり直し
								bSplitPMT = FALSE;
								goto next;
							}
						}
						else
							adplen = 0;
						// 分割PMTの続きコピー
						// pPMTのサイズはTS_PKTSIZEバイト余分に確保しているのでこれでも大丈夫
						::memcpy(&pPMT[pmt_tail], &pSrc[4 + adplen], TS_PKTSIZE - 4 - adplen);
						// section_length
						len = (((int)(pPMT[6] & 0x0f) << 8) | pPMT[7]);
						if (len > (pmt_tail - 8 + (TS_PKTSIZE - 4 - adplen)))	// まだ全部揃ってない
						{
							pmt_tail += (TS_PKTSIZE - 4 - adplen);
							if (rpmt_ci == 0x0f)
								rpmt_ci = 0;
							else
								rpmt_ci++;
							goto next;
						}
						// 揃った
						p = pPMT;
					}
					// この時点でセクションは必ず揃っている
					int limit = 8 + len;
					// 新PIDマップ初期化
					PID_ZERO(p_new_pids);
					// PMT PIDセット(マップにセットしても意味無いけど一応)
					PID_SET(pidPMT, p_new_pids);
					if (!(g_dwDelFlag & FLAG_NIT))
						PID_SET(0x0010, p_new_pids);	// NIT PIDセット
					if (!(g_dwDelFlag & FLAG_SDT))
						PID_SET(0x0011, p_new_pids);	// SDT PIDセット
					if (!(g_dwDelFlag & FLAG_HEIT))
						PID_SET(0x0012, p_new_pids);	// H-EIT PIDセット
					if (!(g_dwDelFlag & FLAG_TOT))
						PID_SET(0x0014, p_new_pids);	// TOT PIDセット
					if (!(g_dwDelFlag & FLAG_SDTT))
						PID_SET(0x0023, p_new_pids);	// SDTT PIDセット
					if (!(g_dwDelFlag & FLAG_BIT))
						PID_SET(0x0024, p_new_pids);	// BIT PIDセット
					if (!(g_dwDelFlag & FLAG_MEIT))
						PID_SET(0x0026, p_new_pids);	// M-EIT PIDセット
					if (!(g_dwDelFlag & FLAG_LEIT))
						PID_SET(0x0027, p_new_pids);	// L-EIT PIDセット
					if (!(g_dwDelFlag & FLAG_CDT))
						PID_SET(0x0029, p_new_pids);	// CDT PIDセット
					if (pidEMM != 0xffff)				// FLAG_EMMが立っている時はpidEMMは必ず0xffff
						PID_SET(pidEMM, p_new_pids);	// EMM PIDセット
					// PCR PIDセット
					pid = GetPID(&p[13]);
					if (pid != 0x1fff)
						PID_SET(pid, p_new_pids);
					// program_info_length
					int desc_len = (((int)(p[15] & 0x0f) << 8) | p[16]);
					// 17 = 最初のdescriptorのオフセット
					int off = 17;
					int left = desc_len;
					while (left >= 2)
					{
						if ((off + 2) > limit)	// program_info_length異常
						{
							bSplitPMT = FALSE;
							goto next;
						}
						int cdesc_len = 2 + p[off+1];
						if (cdesc_len > left || (off + cdesc_len) > limit)	// descriptor長さ異常
						{
							bSplitPMT = FALSE;
							goto next;
						}
						if (p[off] == 0x09)	// Conditional Access Descriptor
						{
							if (p[off+1] >= 4 && (p[off+4] & 0xe0) == 0xe0)	// 内容が妥当なら
							{
								// ECM PIDセット(第1ループに無効ECMは来ない / ARIB TR-B14/B15)
								pid = GetPID(&p[off+4]);
								if (!(g_dwDelFlag & FLAG_ECM))
									PID_SET(pid, p_new_pids);
							}
						}
						off += cdesc_len;
						left -= cdesc_len;
					}
					// データ異常が無ければ必要無いが一応
					off = 17 + desc_len;
					// 13 = program_numberからprogram_info_lengthまでの9バイト + CRC_32の4バイト
					len -= (13 + desc_len);
					while (len >= 5)
					{
						if ((off + 5) > limit)	// program_info_length異常
						{
							bSplitPMT = FALSE;
							goto next;
						}
						if ((p[off] != 0x0d) || !(g_dwDelFlag & FLAG_TYPED))	// stream_type "ISO/IEC 13818-6 type D"以外は無条件で残す
						{
							pid = GetPID(&p[off+1]);
							PID_SET(pid, p_new_pids);
						}
						// ES_info_length
						desc_len = (((int)(p[off+3] & 0x0f) << 8) | p[off+4]);
						// 5 = 最初のdescriptorのオフセット
						int coff = off + 5;
						left = desc_len;
						while (left >= 2)
						{
							if ((coff + 2) > limit)	// ES_info_length異常
							{
								bSplitPMT = FALSE;
								goto next;
							}
							int cdesc_len = 2 + p[coff+1];
							if (cdesc_len > left || (coff + cdesc_len) > limit)	// descriptor長さ異常
							{
								bSplitPMT = FALSE;
								goto next;
							}
							if (p[coff] == 0x09)	// Conditional Access Descriptor
							{
								if (p[coff+1] >= 4 && (p[coff+4] & 0xe0) == 0xe0)	// 内容が妥当なら
								{
									// ECM PIDセット
									pid = GetPID(&p[coff+4]);
									if (pid != 0x1fff)
									{
										if (!(g_dwDelFlag & FLAG_ECM))
											PID_SET(pid, p_new_pids);
									}
								}
							}
							coff += cdesc_len;
							left -= cdesc_len;
						}
						// 5 = stream_typeからES_info_lengthまでの5バイト
						off += (5 + desc_len);
						len -= (5 + desc_len);
					}
					// section_length
					len = (((int)(p[6] & 0x0f) << 8) | p[7]);
					// CRC_32チェック
					// 3 = table_idからsection_lengthまでの3バイト
					if (CalcCRC32(&p[5], len + 3) == 0)
					{
						// 新PIDマップを適用
						::memcpy(&pids, p_new_pids, sizeof(pids));
						// チャンネル変更でなければ
						if (!pPopen->m_bChannelChanged)
						{
							// 旧PIDマップをマージ
							PID_MERGE(&pids, p_old_pids);
						}
						else
							pPopen->m_bChannelChanged = FALSE;
						// 次回は今回のPMTで示されたPIDを旧PIDマップとする
						pid_set *p_tmp_pids;
						p_tmp_pids = p_old_pids;
						p_old_pids = p_new_pids;
						p_new_pids = p_tmp_pids;
						// PMT更新処理完了
						bChangePMT = bSplitPMT = FALSE;
						// 分割PMTをまとめる場合は、送信用PMTパケット作成
						if (g_ModPMT)
						{
							// TSヘッダを除いた残りデータサイズ
							// 4 = pointer_fieldの1バイト + 上のと同じ3バイト
							int left = 4 + len;
							// このPMTをいくつのTSパケットに分割する必要があるか
							iNumSplit = ((left - 1) / (TS_PKTSIZE - 4)) + 1;
							::memset(pPMTPackets, 0xff, (TS_PKTSIZE * iNumSplit));
							for (int i = 0; i < iNumSplit; i++)
							{
								// TSヘッダの4バイト分をコピー
								::memcpy(&pPMTPackets[TS_PKTSIZE * i], p, 4);
								// 先頭パケット以外はunit_start_indicatorを外す
								if (i != 0)
									pPMTPackets[(TS_PKTSIZE * i) + 1] &= ~0x40;
								int n;
								if (left > (TS_PKTSIZE - 4))
									n = TS_PKTSIZE - 4;
								else
									n = left;
								::memcpy(&pPMTPackets[(TS_PKTSIZE * i) + 4], &p[4 + ((TS_PKTSIZE - 4) * i)], n);
								left -= n;
							}
							bPMTComplete = TRUE;
							// まずこのパケットを送信
							goto complete;
						}
					}
					else
					{
						// CRC_32チェックエラーなので最初からやり直し
						bSplitPMT = FALSE;
					}
				}
				else
				{
					if (PID_ISSET(pid, &pids))
					{
						::memcpy(&pTsBuf[pos], pSrc, TS_PKTSIZE);
						pos += TS_PKTSIZE;
					}
				}

			next:
				pSrc += TS_PKTSIZE;
				dwLeft -= TS_PKTSIZE;

				// 1ループでのposの増加は0もしくはTS_PKTSIZEなので、
				// バウンダリチェックはこれで大丈夫なハズ
				if (pos == g_TsBufferSize)
				{
					TS_DATA *pData = new TS_DATA();
					pData->dwSize = g_TsBufferSize;
					pData->pbBuf = pTsBuf;
					pPopen->m_fifoTS.Push(pData);
					pTsBuf = new BYTE[g_TsBufferSize];
					pos = 0;
				}
			}
			if (g_TsSync)
				delete[] pSrcHead;
			delete pRawBuf;
		}
		}
	}
end:
	delete[] pTsBuf;
	return NULL;
}

BOOL cBonDriverPopen::TsSync(BYTE *pSrc, DWORD dwSrc, BYTE **ppDst, DWORD *pdwDst)
{
	// 同期チェックの開始位置
	DWORD dwCheckStartPos = 0;
	// 既に同期済みか？
	if (m_dwUnitSize != 0)
	{
		for (DWORD pos = m_dwUnitSize - m_dwSyncBufPos; pos < dwSrc; pos += m_dwUnitSize)
		{
			if (pSrc[pos] != TS_SYNC_BYTE)
			{
				// 今回の入力バッファで同期が崩れてしまうので要再同期
				m_dwUnitSize = 0;
				// 今回の入力バッファの先頭から同期の崩れた場所までは破棄する事になる
				dwCheckStartPos = pos;
				goto resync;
			}
		}
		DWORD dwDst = TS_PKTSIZE * (((m_dwSyncBufPos + dwSrc) - 1) / m_dwUnitSize);
		if (dwDst == 0)
		{
			// 同期用繰り越しバッファと今回の入力バッファを合わせてもユニットサイズ+1に
			// 届かなかった(==次の同期バイトのチェックが行えなかった)ので、
			// 今回の入力バッファを同期用繰り越しバッファに追加するだけで終了
			::memcpy(&m_SyncBuf[m_dwSyncBufPos], pSrc, dwSrc);
			m_dwSyncBufPos += dwSrc;
			*ppDst = NULL;	// 呼び出し側でのdelete[]を保証する
			*pdwDst = 0;
			return FALSE;
		}
		BYTE *pDst = new BYTE[dwDst];
		if (m_dwSyncBufPos >= TS_PKTSIZE)
			::memcpy(pDst, m_SyncBuf, TS_PKTSIZE);
		else
		{
			if (m_dwSyncBufPos == 0)
				::memcpy(pDst, pSrc, TS_PKTSIZE);
			else
			{
				::memcpy(pDst, m_SyncBuf, m_dwSyncBufPos);
				::memcpy(&pDst[m_dwSyncBufPos], pSrc, TS_PKTSIZE - m_dwSyncBufPos);
			}
		}
		DWORD dwSrcPos = m_dwUnitSize - m_dwSyncBufPos;
		if (m_dwUnitSize == TS_PKTSIZE)
		{
			// 普通のTSパケットの場合はそのままコピーできる
			if ((dwDst - TS_PKTSIZE) != 0)
			{
				::memcpy(&pDst[TS_PKTSIZE], &pSrc[dwSrcPos], (dwDst - TS_PKTSIZE));
				dwSrcPos += (dwDst - TS_PKTSIZE);
			}
		}
		else
		{
			// それ以外のパケットの場合は普通のTSパケットに変換
			for (DWORD pos = TS_PKTSIZE; (dwSrcPos + m_dwUnitSize) < dwSrc; dwSrcPos += m_dwUnitSize, pos += TS_PKTSIZE)
				::memcpy(&pDst[pos], &pSrc[dwSrcPos], TS_PKTSIZE);
		}
		if ((dwSrc - dwSrcPos) != 0)
		{
			// 入力バッファに余りがあるので同期用繰り越しバッファに保存
			::memcpy(m_SyncBuf, &pSrc[dwSrcPos], (dwSrc - dwSrcPos));
			m_dwSyncBufPos = dwSrc - dwSrcPos;
		}
		else
			m_dwSyncBufPos = 0;
		*ppDst = pDst;
		*pdwDst = dwDst;
		return TRUE;
	}

resync:
	// 同期処理開始
	DWORD dwSyncBufPos = m_dwSyncBufPos;
	for (DWORD off = dwCheckStartPos; (off + TS_PKTSIZE) < (dwSyncBufPos + dwSrc); off++)
	{
		if (((off >= dwSyncBufPos) && (pSrc[off - dwSyncBufPos] == TS_SYNC_BYTE)) || ((off < dwSyncBufPos) && (m_SyncBuf[off] == TS_SYNC_BYTE)))
		{
			for (int type = 0; type < 4; type++)
			{
				DWORD dwUnitSize;
				switch (type)
				{
				case 0:
					dwUnitSize = TS_PKTSIZE;
					break;
				case 1:
					dwUnitSize = TTS_PKTSIZE;
					break;
				case 2:
					dwUnitSize = TS_FEC_PKTSIZE;
					break;
				default:
					dwUnitSize = TTS_FEC_PKTSIZE;
					break;
				}
				BOOL bSync = TRUE;
				// 次の同期バイトが同期用繰り越しバッファ内に含まれている可能性があるか？
				if (dwUnitSize >= dwSyncBufPos)
				{
					// なかった場合は同期用繰り越しバッファのチェックは不要
					DWORD pos = off + (dwUnitSize - dwSyncBufPos);
					if (pos >= dwSrc)
					{
						// bSync = FALSE;
						// これ以降のユニットサイズではこの場所で同期成功する事は無いのでbreak
						break;
					}
					else
					{
						// 同一ユニットサイズのバッファが8個もしくは今回の入力バッファの
						// 最後まで並んでいるなら同期成功とみなす
						int n = 0;
						do
						{
							if (pSrc[pos] != TS_SYNC_BYTE)
							{
								bSync = FALSE;
								break;
							}
							pos += dwUnitSize;
							n++;
						} while ((n < 8) && (pos < dwSrc));
					}
				}
				else
				{
					DWORD pos = off + dwUnitSize;
					if (pos >= (dwSyncBufPos + dwSrc))
					{
						// bSync = FALSE;
						// これ以降のユニットサイズではこの場所で同期成功する事は無いのでbreak
						break;
					}
					else
					{
						// 同一ユニットサイズのバッファが8個もしくは今回の入力バッファの
						// 最後まで並んでいるなら同期成功とみなす
						int n = 0;
						do
						{
							if (((pos >= dwSyncBufPos) && (pSrc[pos - dwSyncBufPos] != TS_SYNC_BYTE)) || ((pos < dwSyncBufPos) && (m_SyncBuf[pos] != TS_SYNC_BYTE)))
							{
								bSync = FALSE;
								break;
							}
							pos += dwUnitSize;
							n++;
						} while ((n < 8) && (pos < (dwSyncBufPos + dwSrc)));
					}
				}
				if (bSync)
				{
					m_dwUnitSize = dwUnitSize;
					if (off < dwSyncBufPos)
					{
						if (off != 0)
						{
							dwSyncBufPos -= off;
							::memmove(m_SyncBuf, &m_SyncBuf[off], dwSyncBufPos);
						}
						// この同期検出ロジックでは↓の状態は起こり得ないハズ
#if 0
						// 同期済み時の同期用繰り越しバッファサイズはユニットサイズ以下である必要がある
						if (dwSyncBufPos > dwUnitSize)
						{
							dwSyncBufPos -= dwUnitSize;
							::memmove(m_SyncBuf, &m_SyncBuf[dwUnitSize], dwSyncBufPos);
						}
#endif
						m_dwSyncBufPos = dwSyncBufPos;
						return TsSync(pSrc, dwSrc, ppDst, pdwDst);
					}
					else
					{
						m_dwSyncBufPos = 0;
						return TsSync(&pSrc[off - dwSyncBufPos], (dwSrc - (off - dwSyncBufPos)), ppDst, pdwDst);
					}
				}
			}
		}
	}

	// 今回の入力では同期できなかったので、同期用繰り越しバッファに保存だけして終了
	if (dwSrc >= sizeof(m_SyncBuf))
	{
		::memcpy(m_SyncBuf, &pSrc[dwSrc - sizeof(m_SyncBuf)], sizeof(m_SyncBuf));
		m_dwSyncBufPos = sizeof(m_SyncBuf);
	}
	else if ((dwSyncBufPos + dwSrc) > sizeof(m_SyncBuf))
	{
		::memmove(m_SyncBuf, &m_SyncBuf[(dwSyncBufPos + dwSrc) - sizeof(m_SyncBuf)], (sizeof(m_SyncBuf) - dwSrc));
		::memcpy(&m_SyncBuf[sizeof(m_SyncBuf) - dwSrc], pSrc, dwSrc);
		m_dwSyncBufPos = sizeof(m_SyncBuf);
	}
	else
	{
		::memcpy(&m_SyncBuf[dwSyncBufPos], pSrc, dwSrc);
		m_dwSyncBufPos += dwSrc;
	}
	*ppDst = NULL;	// 呼び出し側でのdelete[]を保証する
	*pdwDst = 0;
	return FALSE;
}

const int cBonDriverPopen::StartProcess(const char *program, pid_t *pid, int *fd)
{
	// パイプを作成する 0 : 読み込み側, 1 : 書込み側
	int pipedes[2];
	if (::pipe(pipedes) < 0)
	{
		::perror("pipe");
		return -1;
	}
	// 子プロセスの作成
	pid_t childpid = ::fork();
	switch (childpid)
	{
	case -1:// Error
		::close(pipedes[0]);
		::close(pipedes[1]);
		::perror("fork");
		return -1;
	case 0:	// 子プロセス側
		{
			// 親プロセスから子プロセスの出力を読むpopenでの"r"
			// 子プロセスでは読み込み側は必要ないので閉じる
			::close(pipedes[0]);
			if (pipedes[1] != STDOUT_FILENO)
			{
				// パイプの書込み側に子プロセスのSTDOUTを設定する
				::dup2(pipedes[1], STDOUT_FILENO);
				::close(pipedes[1]);
			}
			// SIGTERMを同一のPGIDに送信するためにPGIDをPIDと同じ値に設定する
			if (::setpgid(0, 0) < 0)
			{
				::perror("setpgid");
				return -1;
			}
			// SIGTERMのBLOCKを解除
			// recbondのHTTPサーバーは2回目の接続以降SIGTERMをBLOCKする形に
			// なってSIGTERMで終了しないことに対する対処を行う
			sigset_t sigmask;
			::sigemptyset(&sigmask);
			::sigaddset(&sigmask, SIGTERM);
			::pthread_sigmask(SIG_UNBLOCK, &sigmask, NULL);
			// 録画コマンドの実行
			char *const argv[] = {"sh", "-c", const_cast<char*>(program), NULL};
			::execve(_PATH_BSHELL, argv, environ);
			// 子プロセスが作成出来なかった
			::_exit(127);
		}
	}

	// 親プロセス側
	// 親プロセスでは書込み側が必要ないので閉じる
	::close(pipedes[1]);

	// 読み込み側のファイル記述子と子プロセスのPIDを返す
	*fd = pipedes[0];
	*pid = childpid;

	return 0;
}

const int cBonDriverPopen::StopProcess(pid_t *pid, int *fd)
{
	if (*pid < 0)
		return 0;

	// 子プロセスのPGIDを取得してSIGTERMを送信する
	pid_t pgid = ::getpgid(*pid);
	::fprintf(stderr, "kill SIGTERM pid = %d, pgid = %d\n", *pid, pgid);
	if (pgid > 0)
		::killpg(pgid, SIGTERM);
	else
		::kill(*pid, SIGTERM);

	// SIGTERM送信後はプロセスの終了を0.1秒間隔でg_WaitTimeout程度確認する
	// それでも終了しない場合はSIGKILLを送信して再度1秒程度終了を確認する
	// それでもダメならあきらめる
	int waitmsec = 100;
	int waitcount = g_WaitTimeout / waitmsec;
	int aftercount = 10;
	int result = -1;
	int status = 0;
	timespec ts = {0, waitmsec * 1000 * 1000};
	for (int i = 0; i < waitcount + aftercount; i++)
	{
		pid_t pid2 = ::waitpid(*pid, &status, WNOHANG);
		if (pid2 < 0)
		{
			::perror("waitpid");
			result = -1;
			break;
		}
		else if (pid2 == *pid)
		{
			::fprintf(stderr, "stop process pid = %d, pgid = %d\n", *pid, pgid);
			result = 0;
			break;
		}
		else if (i == waitcount)
		{
			::fprintf(stderr, "kill SIGKILL pid = %d, pgid = %d\n", *pid, pgid);
			if (pgid > 0)
				::killpg(pgid, SIGKILL);
			else
				::kill(*pid, SIGKILL);
		}
		::nanosleep(&ts, NULL);
	}
	::close(*fd);
	*pid = *fd = -1;

	return result;
}

}
