#include <stdio.h>
#include <iostream>
#include "SocketServer.h"   
#include "SocketClient.h"
#include <vector>

#ifndef USING_LOG
#define MY_LOG(pStr)	std::cout << pStr << std::endl
#else 
#include "../log.h"
#endif
#include <stdlib.h>
#include <process.h>

#include "..\AppListCtrl.h"
#include <io.h>

#ifdef W2A
#undef W2A
#endif

#ifdef USES_CONVERSION
#undef USES_CONVERSION
#endif

#define W2A W_2_A
#define USES_CONVERSION

extern CAppListCtrl *g_pList;

// CheckIO线程[该线程最多只会启动一次]
UINT WINAPI CSocketServer::CheckIOThread(LPVOID param)
{
	CSocketServer *pThis = (CSocketServer*)param;
	if (pThis->m_bIsListen)
		return -1;
	pThis->m_bIsListen = true;
	OutputDebugStringA("CSocketServer CheckIOThread Start.\n");
	while (!pThis->m_bExit)
	{
		Sleep(20);
		pThis->CheckIO();
	}
	pThis->m_bIsListen = false;
	OutputDebugStringA("CSocketServer CheckIOThread Stop.\n");
	return 0xDead666;
}

/**
* @brief 默认的构造函数
*/
CSocketServer::CSocketServer()
{
	m_bExit = false;
	m_bIsListen = false;

	memset(g_fd_ArrayC, 0, sizeof(g_fd_ArrayC));

	InitializeCriticalSection(&m_cs);
}

/**
* @brief ~CSocketServer
*/
CSocketServer::~CSocketServer()
{
	DeleteCriticalSection(&m_cs);
}

// 在调用完基类的初始化函数之后，Server开启监听Client连接请求的线程
int CSocketServer::init(const char *pIp, int nPort, int nType)
{
	m_bExit = false;
	GetAllSoftwareVersion();
	int ret = CSocketBase::init(pIp, nPort, nType);
	if(0 == ret){
		// 开始监听
		HANDLE h = (HANDLE)_beginthreadex(NULL, 0, &CheckIOThread, this, 0, NULL);
		CloseHandle(h);
	}
	return ret;
}

#pragma comment(lib, "version.lib")

// 获取exe文件的版本信息
std::string GetExeVersion(const CString &exePath)
{
	char version[_MAX_PATH] = {0};
	UINT sz = GetFileVersionInfoSize(exePath, NULL);
	if (sz)
	{
		char *pBuf = new char[sz + 1]();
		if (GetFileVersionInfo(exePath, NULL, sz, pBuf))
		{
			VS_FIXEDFILEINFO *pVsInfo = NULL;
			if (VerQueryValueA(pBuf, "\\", (void**)&pVsInfo, &sz))
			{
				sprintf(version, "%d.%d.%d.%d", 
					HIWORD(pVsInfo->dwFileVersionMS), 
					LOWORD(pVsInfo->dwFileVersionMS), 
					HIWORD(pVsInfo->dwFileVersionLS), 
					LOWORD(pVsInfo->dwFileVersionLS));
			}
		}
		delete [] pBuf;
	}
	return version;
}

void CSocketServer::GetAllSoftwareVersion()
{
	char folder[_MAX_PATH], *p = folder;
	GetModuleFileNameA(NULL, folder, _MAX_PATH);
	while(*p) ++p;
	while('\\' != *p) --p;
	strcpy(p+1, "*.*");
	CFileFind ff;
	BOOL bFind = ff.FindFile(CString(folder));
	// 获取全部文件，并获取每个可执行文件版本
	USES_CONVERSION;
	while (bFind)
	{
		bFind = ff.FindNextFile();
		if (ff.IsDots())continue;
		if (ff.IsDirectory())
		{
			CString name = ff.GetFileName();
			CString path = ff.GetFilePath() + "\\" + name + ".exe";
			if (PathFileExists(path))
			{
				String app = W2A(name);
				Lock();
				m_mapVersion.insert(make_pair(app.tolower(), GetExeVersion(path)));
				Unlock();
				CString filelist = ff.GetFilePath() + _T("\\filelist.txt");
				DeleteFile(filelist);
				CheckFilelist(ff.GetFilePath(), filelist);
			}
		}
	}
	ff.Close();
}

/************************************************************************
* @brief 扫描指定目录，将该目录下的文件写入指定文件
* @param[in] folder		指定目录
* @param[in] file_list	指定文件
* @param[in] file		file_list根目录
************************************************************************/
void CSocketServer::CheckFilelist(const CString &folder, 
								  const CString &file_list, 
								  const CString &root)
{
	CFileFind ff;
	CString filter = folder + _T("\\*.*");
	BOOL bFind = ff.FindFile(filter);
	std::vector<CString> v_AllFiles;
	USES_CONVERSION;
	while (bFind)
	{
		bFind = ff.FindNextFile();
		if (ff.IsDots())continue;
		else if(ff.IsDirectory())
			CheckFilelist(folder + _T("\\") + ff.GetFileName(), file_list, ff.GetFileName());
		else
		{
			CString name = root.IsEmpty() ? ff.GetFileName() : root + _T("\\") + ff.GetFileName();
			int n = name.Find(L".conf", 0); // 排除配置文件
			if (ff.GetFileName() != L"filelist.txt" && n == -1)
			{
				v_AllFiles.push_back(name);
			}
		}
	}
	ff.Close();

	FILE *filelist = NULL;
	if (filelist = fopen(W2A(file_list), "a+"))
	{
		for (std::vector<CString>::const_iterator itor = v_AllFiles.begin(); 
			itor != v_AllFiles.end(); ++itor)
		{
			String cur = W2A(*itor);
			fwrite(cur, 1, strlen(cur), filelist);
			fwrite("\n", 1, 1, filelist);
		}
		fclose(filelist);
	}
}

std::string CSocketServer::CheckUpdate(const char *app)
{
	Lock();
	std::map<std::string, std::string>::const_iterator fd = m_mapVersion.find(app);
	std::string ver = m_mapVersion.end() == fd ? "" : fd->second;
	Unlock();
	return ver;
}

// 反初始化过程：先退出本类线程，清理本类内存，然后调用基类unInit函数
void CSocketServer::unInit()
{
	// 退出CheckIO
	Lock();
	m_bExit = true;
	Unlock();
	while (m_bIsListen)
		Sleep(10);

	CSocketClient *buf[MAX_LISTEN] = { 0 };

	Lock();
	for (int i = 0; i < MAX_LISTEN; ++i)
	{
		if (g_fd_ArrayC[i])
		{
			g_fd_ArrayC[i]->SetExit();
			buf[i] = g_fd_ArrayC[i];
			g_fd_ArrayC[i] = NULL;
		}
	}
	Unlock();
	for (int i = 0; i < MAX_LISTEN; ++i)
	{
		if (buf[i])
		{
			buf[i]->unInit();
			buf[i]->Destroy();
		}
	}

	CSocketBase::unInit();
}


void CSocketServer::SendCommand(const char *msg, const char *id)
{
	TRACE("======> SendMessage[%s]: %s\n", id ? id : "ALL", msg);
	bool reFresh = (0 == strcmp(msg, REFRESH));
	Lock();
	for (int i = 0; i < MAX_LISTEN; ++i)
	{
		if (g_fd_ArrayC[i] && g_fd_ArrayC[i]->IsAlive())
		{
			if (id)
			{
				if(0 == strcmp(id, g_fd_ArrayC[i]->GetNo()))
				{
					g_fd_ArrayC[i]->sendData(msg, strlen(msg));
					if (reFresh) g_fd_ArrayC[i]->StartClock();
					TRACE("======> SendCommand: %s[%s] OK\n", g_fd_ArrayC[i]->GetIp(), g_fd_ArrayC[i]->GetNo());
					break;
				}
			}else
			{
				g_fd_ArrayC[i]->sendData(msg, strlen(msg));
				if (reFresh) g_fd_ArrayC[i]->StartClock();
				TRACE("======> SendCommand: %s[%s] OK\n", g_fd_ArrayC[i]->GetIp(), g_fd_ArrayC[i]->GetNo());
			}
		}
	}
	Unlock();
}


void CSocketServer::SetAliveTime(int msg, const char *id)
{
	TRACE("======> SetAliveTime[%s]: %d\n", id ? id : "ALL", msg);
	Lock();
	for (int i = 0; i < MAX_LISTEN; ++i)
	{
		if (g_fd_ArrayC[i] && g_fd_ArrayC[i]->IsAlive())
		{
			if (id)
			{
				if(0 == strcmp(id, g_fd_ArrayC[i]->GetNo()))
				{
					char arg[64] = { 0 };
					sprintf_s(arg, "%d", msg);
					std::string cmd = MAKE_CMD(KEEPALIVE, arg);
					g_fd_ArrayC[i]->SetAliveTime(msg);
					g_fd_ArrayC[i]->sendData(cmd.c_str(), cmd.length());
					TRACE("======> SetAliveTime: %s[%s] OK\n", g_fd_ArrayC[i]->GetIp(), g_fd_ArrayC[i]->GetNo());
					break;
				}
			}else
			{
				char arg[64] = { 0 };
				sprintf_s(arg, "%d", msg);
				std::string cmd = MAKE_CMD(KEEPALIVE, arg);
				g_fd_ArrayC[i]->SetAliveTime(msg);
				g_fd_ArrayC[i]->sendData(cmd.c_str(), cmd.length());
				TRACE("======> SetAliveTime: %s[%s] OK\n", g_fd_ArrayC[i]->GetIp(), g_fd_ArrayC[i]->GetNo());
			}
		}
	}
	Unlock();
}


std::string CSocketServer::getVersion(const std::string &name)
{
	char path[_MAX_PATH], *p = path;
	GetModuleFileNameA(NULL, path, _MAX_PATH);
	while(*p) ++p;
	while('\\' != *p) --p;
	sprintf(p+1, "%s", name.c_str());
	CString filelist = CString(path)+_T("\\filelist.txt");
	DeleteFile(filelist);
	CheckFilelist(CString(path), filelist);
	sprintf(p+1, "%s\\%s.exe", name.c_str(), name.c_str());
	std::string ver = GetExeVersion(CString(path));
	Lock();
	m_mapVersion[name] = ver;
	Unlock();
	return ver;
}

// 是否在容器中已存在该IP
bool IsExist(const std::string &ip, const std::vector<std::string> &v)
{
	for (std::vector<std::string>::const_iterator p = v.begin(); p != v.end(); ++p)
		if (*p == ip)
			return true;
	return false;
}

// 确保一台机器只收到一次信令
void CSocketServer::ControlDevice( const char *msg )
{
	TRACE("======> ControlDevice: %s\n", msg);
	// 如果是关机、重启，则先停止所有程序
	if (0 == strcmp(msg, SHUTDOWN) || 0 == strcmp(msg, REBOOT))
	{
		SendCommand(STOP);
		Sleep(3000);//发送太快了，处理不过来，先等待停止操作执行完毕
	}
	std::vector<std::string> v_ip;
	Lock();
	for (int i = 0; i < MAX_LISTEN; ++i)
	{
		if (g_fd_ArrayC[i] && g_fd_ArrayC[i]->IsAlive())
		{
			std::string ip = g_fd_ArrayC[i]->GetIp();
			if (false == IsExist(ip, v_ip))
			{
				v_ip.push_back(ip);
				g_fd_ArrayC[i]->sendData(msg, strlen(msg));
				TRACE("======> ControlDevice: %s OK\n", ip.c_str());
			}
		}
	}
	Unlock();
}


/** 
* @brief 只针对server端，监听数据
* @return 返回0为成功，其他为失败
*/
int CSocketServer::CheckIO()
{
	assert(0 == m_nType);
	const timeval tv = {1, 0};
	FD_ZERO(&fdRead);
	FD_SET(m_SocketListen, &fdRead);
	/// 调用select模式进行监听
	int nRes = select(0, &fdRead, NULL, NULL, &tv);
	if (0 == nRes)
	{
		/// 超时，继续
		return _NO__ERROR;
	}
	else if (nRes < 0)
	{
		char str[DEFAULT_BUFFER];
		sprintf_s(str, "CSocketServer select failed, code = %d.\n", WSAGetLastError());
		MY_LOG(str);
		return ERROR_SELECT;
	}
	sockaddr_in addrClient;
	int addrClientLen = sizeof(addrClient);
	/// 检查是否有新的连接进入
	if (FD_ISSET(m_SocketListen, &fdRead))
	{
		m_Socket = accept(m_SocketListen, (sockaddr*)&addrClient, &addrClientLen);
		if (m_Socket == WSAEWOULDBLOCK)
		{
			MY_LOG("非阻塞模式设定accept调用不正确.\n");
			return ERROR_ACCEPT;
		}
		else if (m_Socket == INVALID_SOCKET)
		{
			char str[DEFAULT_BUFFER];
			sprintf_s(str, "CSocketServer accept failed, code = %d.\n", WSAGetLastError());
			MY_LOG(str);
			return ERROR_ACCEPT;
		}
		/// 检测端口是否已占用，端口作为报表的关键字，不得重复
		if(!CanAccept(ntohs(addrClient.sin_port)))
		{
			closesocket(m_Socket);
			m_Socket = INVALID_SOCKET;
			return ERROR_PORTERROR;
		}
		bool bFull = false;
		/// 新的连接可以使用，查看待决处理队列
		int idx = GetAvailabeClient();
		if (idx >= 0)
		{
			g_fd_ArrayC[idx] = new CSocketClient(m_Socket, 
				inet_ntoa(addrClient.sin_addr), ntohs(addrClient.sin_port));
			g_pList->PostMessage(MSG_InsertApp, g_fd_ArrayC[idx]->GetSrcPort());
		}else
		{
			char str[128];
			sprintf(str, "CSocketServer服务器端连接数已满（未接受%d）.\n", m_Socket);
			MY_LOG(str);
			closesocket(m_Socket);
			return ERROR_CLIENTFULL;
		}
	}//if (FD_ISSET(sListen, &fdRead))
	return _NO__ERROR;
}


// 获得处于可用或者处于空闲状态的Client
int CSocketServer::GetAvailabeClient()
{
	int idx = -1;
	std::vector<CSocketClient *> bDel; // 已失联的客户端
	Lock();
	for (int nLoopi = 0; nLoopi < MAX_LISTEN; ++nLoopi)
	{
		if (0 == g_fd_ArrayC[nLoopi] || !g_fd_ArrayC[nLoopi]->IsAlive())
		{
			if (g_fd_ArrayC[nLoopi])
			{
				bDel.push_back(g_fd_ArrayC[nLoopi]);
				g_fd_ArrayC[nLoopi] = NULL;
			}
			idx = nLoopi;
			break;
		}
	}
	Unlock();
	for (std::vector<CSocketClient *>::const_iterator iter = bDel.begin(); iter != bDel.end(); ++iter)
	{
		CSocketClient *cur = *iter;
		g_pList->PostMessage(MSG_DeleteApp, cur->GetSrcPort());
		cur->unInit();
		cur->Destroy();
	}
	return idx;
}

// 指定端口是否已存在，不存在则允许接入，返回true
bool CSocketServer::CanAccept(int port)
{
	bool can = true;
	Lock();
	for (int nLoopi = 0; nLoopi < MAX_LISTEN; ++nLoopi)
	{
		if (g_fd_ArrayC[nLoopi] && port == g_fd_ArrayC[nLoopi]->GetSrcPort())
		{
			can = false;
			break;
		}
	}
	Unlock();
	return can;
}
