#define _WIN32_IE 0x0500

#include <windows.h>
#include <wininet.h>
#include <shellapi.h>
#include <stdio.h>
#include <wininet.h>
#include <io.h>
#include "ras.h"
#include "raserror.h"
#include "psapi.h"
#include "resource.h"

#pragma comment(lib, "rasapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "wininet.lib")

#ifndef INTERNET_OPTION_PER_CONNECTION_OPTION

#define INTERNET_OPTION_PER_CONNECTION_OPTION           75
// Options used in INTERNET_PER_CONN_OPTON struct
#define INTERNET_PER_CONN_FLAGS                         1
#define INTERNET_PER_CONN_PROXY_SERVER                  2
#define INTERNET_PER_CONN_PROXY_BYPASS                  3
#define INTERNET_PER_CONN_AUTOCONFIG_URL                4
#define INTERNET_PER_CONN_AUTODISCOVERY_FLAGS           5
// PER_CONN_FLAGS
#define PROXY_TYPE_DIRECT                               0x00000001
#define PROXY_TYPE_PROXY                                0x00000002
#define PROXY_TYPE_AUTO_PROXY_URL                       0x00000004
#define PROXY_TYPE_AUTO_DETECT                          0x00000008

typedef struct {
  DWORD dwOption;
  union {
	DWORD    dwValue;
	LPTSTR   pszValue;
	FILETIME ftValue;
  } Value;
} INTERNET_PER_CONN_OPTION, *LPINTERNET_PER_CONN_OPTION;

typedef struct {
  DWORD                      dwSize;
  LPTSTR                     pszConnection;
  DWORD                      dwOptionCount;
  DWORD                      dwOptionError;
  LPINTERNET_PER_CONN_OPTION pOptions;
} INTERNET_PER_CONN_OPTION_LIST, *LPINTERNET_PER_CONN_OPTION_LIST;

#endif

extern "C" WINBASEAPI HWND WINAPI GetConsoleWindow();

#define NID_UID 123
#define WM_TASKBARNOTIFY WM_USER+20
#define WM_TASKBARNOTIFY_MENUITEM_SHOW (WM_USER + 21)
#define WM_TASKBARNOTIFY_MENUITEM_HIDE (WM_USER + 22)
#define WM_TASKBARNOTIFY_MENUITEM_RELOAD (WM_USER + 23)
#define WM_TASKBARNOTIFY_MENUITEM_ABOUT (WM_USER + 24)
#define WM_TASKBARNOTIFY_MENUITEM_EXIT (WM_USER + 25)
#define WM_TASKBARNOTIFY_MENUITEM_PROXYLIST_BASE (WM_USER + 26)

HINSTANCE hInst;
HWND hWnd;
HWND hConsole;
WCHAR szTitle[64] = L"";
WCHAR szWindowClass[16] = L"taskbar";
WCHAR szCommandLine[1024] = L"";
WCHAR szTooltip[512] = L"";
WCHAR szBalloon[512] = L"";
WCHAR szEnvironment[1024] = L"";
WCHAR szProxyString[2048] = L"";
WCHAR *lpProxyList[8] = {0};
volatile DWORD dwChildrenPid;

static DWORD MyGetProcessId(HANDLE hProcess)
{
	// https://gist.github.com/kusma/268888
	typedef DWORD (WINAPI *pfnGPI)(HANDLE);
	typedef ULONG (WINAPI *pfnNTQIP)(HANDLE, ULONG, PVOID, ULONG, PULONG);

	static int first = 1;
	static pfnGPI pfnGetProcessId;
	static pfnNTQIP ZwQueryInformationProcess;
	if (first)
	{
		first = 0;
		pfnGetProcessId = (pfnGPI)GetProcAddress(
			GetModuleHandleW(L"KERNEL32.DLL"), "GetProcessId");
		if (!pfnGetProcessId)
			ZwQueryInformationProcess = (pfnNTQIP)GetProcAddress(
				GetModuleHandleW(L"NTDLL.DLL"),
				"ZwQueryInformationProcess");
	}
	if (pfnGetProcessId)
		return pfnGetProcessId(hProcess);
	if (ZwQueryInformationProcess)
	{
		struct
		{
			PVOID Reserved1;
			PVOID PebBaseAddress;
			PVOID Reserved2[2];
			ULONG UniqueProcessId;
			PVOID Reserved3;
		} pbi;
		ZwQueryInformationProcess(hProcess, 0, &pbi, sizeof(pbi), 0);
		return pbi.UniqueProcessId;
	}
	return 0;
}

static LPCTSTR MyGetActiveRasConnectionName()
{
#ifndef RASDT_PPPoE
	#define RASDT_PPPoE TEXT("PPPoE")
#endif

	static WCHAR szConnectionName[512] = L"";

	DWORD dwCb = 0;
	DWORD dwRet = ERROR_SUCCESS;
	DWORD dwConnections = 0;
	LPRASCONN lpRasConn = NULL;

	memset(szConnectionName, 0, sizeof(szConnectionName[0])*sizeof(szConnectionName));

	dwRet = RasEnumConnections(lpRasConn, &dwCb, &dwConnections);
	if (dwRet == ERROR_BUFFER_TOO_SMALL)
	{
		lpRasConn = (LPRASCONN) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, dwCb);
		if (lpRasConn == NULL)
		{
			wprintf(L"RasEnumConnections: HeapAlloc failed!\n");
			return NULL;
		}
		lpRasConn[0].dwSize = sizeof(RASCONN);
		dwRet = RasEnumConnections(lpRasConn, &dwCb, &dwConnections);
		if (ERROR_SUCCESS == dwRet)
		{
			for (size_t i = 0; i < dwConnections; i++)
			{
				if((lstrcmp(lpRasConn[i].szDeviceType, RASDT_Modem) == 0) ||
					(lstrcmp(lpRasConn[i].szDeviceType, RASDT_Isdn) == 0) ||
					(lstrcmp(lpRasConn[i].szDeviceType, RASDT_PPPoE)==0))
				{
					lstrcpy(szConnectionName, lpRasConn[i].szEntryName);
					wprintf(L"RasEnumConnections: The RAS connections are currently active:\"%s\"\n", szConnectionName);
				}
			}
		}
		HeapFree(GetProcessHeap(), 0, lpRasConn);
		lpRasConn = NULL;
	}

	return szConnectionName[0] ? szConnectionName : NULL;
}

BOOL ShowTrayIcon(LPCTSTR lpszProxy, DWORD dwMessage=NIM_ADD)
{
	NOTIFYICONDATA nid;
	ZeroMemory(&nid, sizeof(NOTIFYICONDATA));
	nid.cbSize = (DWORD)sizeof(NOTIFYICONDATA);
	nid.hWnd   = hWnd;
	nid.uID	   = NID_UID;
	nid.uFlags = NIF_ICON|NIF_MESSAGE;
	nid.dwInfoFlags=NIIF_INFO;
	nid.uCallbackMessage = WM_TASKBARNOTIFY;
	nid.hIcon = LoadIcon(hInst, (LPCTSTR)IDI_SMALL);
	nid.uTimeoutAndVersion = 3 * 1000 | NOTIFYICON_VERSION;
	lstrcpy(nid.szInfoTitle, szTitle);
	if (lpszProxy)
	{
		nid.uFlags |= NIF_INFO|NIF_TIP;
		if (lstrlen(lpszProxy) > 0)
		{
			lstrcpy(nid.szTip, lpszProxy);
			lstrcpy(nid.szInfo, lpszProxy);
		}
		else
		{
			lstrcpy(nid.szInfo, szBalloon);
			lstrcpy(nid.szTip, szTooltip);
		}
	}
	Shell_NotifyIcon(dwMessage, &nid);
	return TRUE;
}

BOOL DeleteTrayIcon()
{
	NOTIFYICONDATA nid;
	nid.cbSize = (DWORD)sizeof(NOTIFYICONDATA);
	nid.hWnd   = hWnd;
	nid.uID	   = NID_UID;
	Shell_NotifyIcon(NIM_DELETE, &nid);
	return TRUE;
}


LPCTSTR GetWindowsProxy()
{
	static WCHAR szProxy[1024] = {0};
	HKEY hKey;
	DWORD dwData = 0;
	DWORD dwSize = sizeof(DWORD);

	if (ERROR_SUCCESS == RegOpenKeyEx(HKEY_CURRENT_USER,
									  L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
									  0,
									  KEY_READ | 0x0200,
									  &hKey))
	{
		szProxy[0] = 0;
		dwSize = sizeof(szProxy)/sizeof(szProxy[0]);
		RegQueryValueExW(hKey, L"AutoConfigURL", NULL, 0, (LPBYTE)&szProxy, &dwSize);
		if (wcslen(szProxy))
		{
			RegCloseKey(hKey);
			return szProxy;
		}
		dwData = 0;
		RegQueryValueExW(hKey, L"ProxyEnable", NULL, 0, (LPBYTE)&dwData, &dwSize);
		if (dwData == 0)
		{
			RegCloseKey(hKey);
			return L"";
		}
		szProxy[0] = 0;
		dwSize = sizeof(szProxy)/sizeof(szProxy[0]);
		RegQueryValueExW(hKey, L"ProxyServer", NULL, 0, (LPBYTE)&szProxy, &dwSize);
		if (wcslen(szProxy))
		{
			RegCloseKey(hKey);
			return szProxy;
		}
	}
	return szProxy;
}


BOOL SetWindowsProxy(WCHAR* szProxy, const WCHAR* szProxyInterface=NULL)
{
	INTERNET_PER_CONN_OPTION_LIST conn_options;
	BOOL    bReturn;
	DWORD   dwBufferSize = sizeof(conn_options);

	if (wcslen(szProxy) == 0)
	{
		conn_options.dwSize = dwBufferSize;
		conn_options.pszConnection = (WCHAR*)szProxyInterface;
		conn_options.dwOptionCount = 1;
		conn_options.pOptions = (INTERNET_PER_CONN_OPTION*)malloc(sizeof(INTERNET_PER_CONN_OPTION)*conn_options.dwOptionCount);
		conn_options.pOptions[0].dwOption = INTERNET_PER_CONN_FLAGS;
		conn_options.pOptions[0].Value.dwValue = PROXY_TYPE_DIRECT;
	}
	else if (wcsstr(szProxy, L"://") != NULL)
	{
		conn_options.dwSize = dwBufferSize;
		conn_options.pszConnection = (WCHAR*)szProxyInterface;
		conn_options.dwOptionCount = 3;
		conn_options.pOptions = (INTERNET_PER_CONN_OPTION*)malloc(sizeof(INTERNET_PER_CONN_OPTION)*conn_options.dwOptionCount);
		conn_options.pOptions[0].dwOption = INTERNET_PER_CONN_FLAGS;
		conn_options.pOptions[0].Value.dwValue = PROXY_TYPE_DIRECT | PROXY_TYPE_AUTO_PROXY_URL;
		conn_options.pOptions[1].dwOption = INTERNET_PER_CONN_AUTOCONFIG_URL;
		conn_options.pOptions[1].Value.pszValue = szProxy;
		conn_options.pOptions[2].dwOption = INTERNET_PER_CONN_PROXY_BYPASS;
		conn_options.pOptions[2].Value.pszValue = L"<local>";
	}
	else
	{
		conn_options.dwSize = dwBufferSize;
		conn_options.pszConnection = (WCHAR*)szProxyInterface;
		conn_options.dwOptionCount = 3;
		conn_options.pOptions = (INTERNET_PER_CONN_OPTION*)malloc(sizeof(INTERNET_PER_CONN_OPTION)*conn_options.dwOptionCount);
		conn_options.pOptions[0].dwOption = INTERNET_PER_CONN_FLAGS;
		conn_options.pOptions[0].Value.dwValue = PROXY_TYPE_DIRECT | PROXY_TYPE_PROXY;
		conn_options.pOptions[1].dwOption = INTERNET_PER_CONN_PROXY_SERVER;
		conn_options.pOptions[1].Value.pszValue = szProxy;
		conn_options.pOptions[2].dwOption = INTERNET_PER_CONN_PROXY_BYPASS;
		conn_options.pOptions[2].Value.pszValue = L"<local>";
	}

	bReturn = InternetSetOption(NULL, INTERNET_OPTION_PER_CONNECTION_OPTION, &conn_options, dwBufferSize);
	free(conn_options.pOptions);
	InternetSetOption(NULL, INTERNET_OPTION_SETTINGS_CHANGED, NULL, 0);
	InternetSetOption(NULL, INTERNET_OPTION_REFRESH , NULL, 0);
	return bReturn;
}


BOOL ShowPopupMenu()
{
	POINT pt;
	HMENU hSubMenu = NULL;
	LPCTSTR lpCurrentProxy = GetWindowsProxy();
	if (lpProxyList[1] != NULL)
	{
		hSubMenu = CreatePopupMenu();
		for (int i = 0; lpProxyList[i]; i++)
		{
			UINT uFlags = wcscmp(lpProxyList[i], lpCurrentProxy) == 0 ? MF_STRING | MF_CHECKED : MF_STRING;
			LPCTSTR lpText = wcslen(lpProxyList[i]) ? lpProxyList[i] : L"\x7981\x7528\x4ee3\x7406";
			AppendMenu(hSubMenu, uFlags, WM_TASKBARNOTIFY_MENUITEM_PROXYLIST_BASE+i, lpText);
		}
	}

	HMENU hMenu = CreatePopupMenu();
	AppendMenu(hMenu, MF_STRING, WM_TASKBARNOTIFY_MENUITEM_SHOW, L"\x663e\x793a");
	AppendMenu(hMenu, MF_STRING, WM_TASKBARNOTIFY_MENUITEM_HIDE, L"\x9690\x85cf");
	if (hSubMenu != NULL)
	{
		AppendMenu(hMenu, MF_STRING | MF_POPUP, (UINT_PTR)hSubMenu, L"\x8bbe\x7f6e IE \x4ee3\x7406");
	}
	AppendMenu(hMenu, MF_STRING, WM_TASKBARNOTIFY_MENUITEM_RELOAD, L"\x91cd\x65b0\x8f7d\x5165");
	AppendMenu(hMenu, MF_STRING, WM_TASKBARNOTIFY_MENUITEM_EXIT,   L"\x9000\x51fa");
	GetCursorPos(&pt);
	TrackPopupMenu(hMenu, TPM_LEFTALIGN, pt.x, pt.y, 0, hWnd, NULL);
	PostMessage(hWnd, WM_NULL, 0, 0);
	if (hSubMenu != NULL)
		DestroyMenu(hSubMenu);
	DestroyMenu(hMenu);
	return TRUE;
}

BOOL ParseProxyList()
{
	WCHAR * tmpProxyString = _wcsdup(szProxyString);
	ExpandEnvironmentStrings(tmpProxyString, szProxyString, sizeof(szProxyString)/sizeof(szProxyString[0]));
	free(tmpProxyString);
	WCHAR *sep = L"\n";
	WCHAR *pos = wcstok(szProxyString, sep);
	INT i = 0;
	lpProxyList[i++] = L"";
	while (pos && i < sizeof(lpProxyList)/sizeof(lpProxyList[0]))
	{
		lpProxyList[i++] = pos;
		pos = wcstok(NULL, sep);
	}
	lpProxyList[i] = 0;
	return TRUE;
}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
   hWnd = CreateWindow(szWindowClass, szTitle, WS_OVERLAPPED|WS_SYSMENU,
	  NULL, NULL, CW_USEDEFAULT, CW_USEDEFAULT, NULL, NULL, hInstance, NULL);

   if (!hWnd)
   {
	  return FALSE;
   }

   ShowWindow(hWnd, nCmdShow);
   UpdateWindow(hWnd);

   return TRUE;
}

BOOL CDCurrentDirectory()
{
	WCHAR szPath[4096] = L"";
	GetModuleFileName(NULL, szPath, sizeof(szPath)/sizeof(szPath[0])-1);
	*wcsrchr(szPath, L'\\') = 0;
	SetCurrentDirectory(szPath);
	SetEnvironmentVariableW(L"CWD", szPath);
	return TRUE;
}

BOOL SetEenvironment()
{
	LoadString(hInst, IDS_CMDLINE, szCommandLine, sizeof(szCommandLine)/sizeof(szCommandLine[0])-1);
	LoadString(hInst, IDS_ENVIRONMENT, szEnvironment, sizeof(szEnvironment)/sizeof(szEnvironment[0])-1);
	LoadString(hInst, IDS_PROXYLIST, szProxyString, sizeof(szProxyString)/sizeof(szEnvironment[0])-1);

	WCHAR *sep = L"\n";
	WCHAR *pos = NULL;
	WCHAR *token = wcstok(szEnvironment, sep);
	while(token != NULL)
	{
		if (pos = wcschr(token, L'='))
		{
			*pos = 0;
			SetEnvironmentVariableW(token, pos+1);
			//wprintf(L"[%s] = [%s]\n", token, pos+1);
		}
		token = wcstok(NULL, sep);
	}

	GetEnvironmentVariableW(L"TASKBAR_TITLE", szTitle, sizeof(szTitle)/sizeof(szTitle[0])-1);
	GetEnvironmentVariableW(L"TASKBAR_TOOLTIP", szTooltip, sizeof(szTooltip)/sizeof(szTooltip[0])-1);
	GetEnvironmentVariableW(L"TASKBAR_BALLOON", szBalloon, sizeof(szBalloon)/sizeof(szBalloon[0])-1);

	return TRUE;
}

BOOL CreateConsole()
{
	WCHAR szVisible[BUFSIZ] = L"";

	AllocConsole();
	_wfreopen(L"CONIN$",  L"r+t", stdin);
	_wfreopen(L"CONOUT$", L"w+t", stdout);

	hConsole = GetConsoleWindow();

	if (GetEnvironmentVariableW(L"TASKBAR_VISIBLE", szVisible, BUFSIZ-1) && szVisible[0] == L'0')
	{
		ShowWindow(hConsole, SW_HIDE);
	}
	else
	{
		SetForegroundWindow(hConsole);
	}

	return TRUE;
}

BOOL ExecCmdline()
{
	SetWindowText(hConsole, szTitle);
	STARTUPINFO si = { sizeof(si) };
	PROCESS_INFORMATION pi;
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = TRUE;
	BOOL bRet = CreateProcess(NULL, szCommandLine, NULL, NULL, FALSE, NULL, NULL, NULL, &si, &pi);
	if(bRet)
	{
		dwChildrenPid = MyGetProcessId(pi.hProcess);
	}
	else
	{
		wprintf(L"ExecCmdline \"%s\" failed!\n", szCommandLine);
		MessageBox(NULL, szCommandLine, L"Error: \x6267\x884c\x547d\x4ee4\x5931\x8d25!", MB_OK);
		ExitProcess(0);
	}
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return TRUE;
}

BOOL ReloadCmdline()
{
	HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, dwChildrenPid);
	if (hProcess)
	{
		TerminateProcess(hProcess, 0);
	}
	ShowWindow(hConsole, SW_SHOW);
	SetForegroundWindow(hConsole);
	wprintf(L"\n\n");
	Sleep(200);
	ExecCmdline();
	return TRUE;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	static const UINT WM_TASKBARCREATED = ::RegisterWindowMessage(L"TaskbarCreated");
	int nID;
	switch (message)
	{
		case WM_TASKBARNOTIFY:
			if (lParam == WM_LBUTTONUP)
			{
				ShowWindow(hConsole, !IsWindowVisible(hConsole));
				SetForegroundWindow(hConsole);
			}
			else if (lParam == WM_RBUTTONUP)
			{
				SetForegroundWindow(hWnd);
				ShowPopupMenu();
			}
			break;
		case WM_COMMAND:
			nID = LOWORD(wParam);
			if (nID == WM_TASKBARNOTIFY_MENUITEM_SHOW)
			{
				ShowWindow(hConsole, SW_SHOW);
				SetForegroundWindow(hConsole);
			}
			else if (nID == WM_TASKBARNOTIFY_MENUITEM_HIDE)
			{
				ShowWindow(hConsole, SW_HIDE);
			}
			if (nID == WM_TASKBARNOTIFY_MENUITEM_RELOAD)
			{
				ReloadCmdline();
			}
			else if (nID == WM_TASKBARNOTIFY_MENUITEM_ABOUT)
			{
				MessageBoxW(hWnd, szTooltip, szWindowClass, 0);
			}
			else if (nID == WM_TASKBARNOTIFY_MENUITEM_EXIT)
			{
				DeleteTrayIcon();
				PostMessage(hConsole, WM_CLOSE, 0, 0);
			}
			else if (WM_TASKBARNOTIFY_MENUITEM_PROXYLIST_BASE <= nID && nID <= WM_TASKBARNOTIFY_MENUITEM_PROXYLIST_BASE+sizeof(lpProxyList)/sizeof(lpProxyList[0]))
			{
				WCHAR *szProxy = lpProxyList[nID-WM_TASKBARNOTIFY_MENUITEM_PROXYLIST_BASE];
				SetWindowsProxy(szProxy);
				LPCTSTR szRasConnectionName = MyGetActiveRasConnectionName();
				if (szRasConnectionName)
				{
					SetWindowsProxy(szProxy, szRasConnectionName);
				}
				ShowTrayIcon(szProxy, NIM_MODIFY);
			}
			break;
		case WM_DESTROY:
			PostQuitMessage(0);
			break;
		default:
			if (message == WM_TASKBARCREATED)
			{
				ShowTrayIcon(NULL, NIM_ADD);
				break;
			}
			return DefWindowProc(hWnd, message, wParam, lParam);
   }
   return 0;
}

ATOM MyRegisterClass(HINSTANCE hInstance)
{
	WNDCLASSEX wcex;

	wcex.cbSize = sizeof(WNDCLASSEX);

	wcex.style			= CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc	= (WNDPROC)WndProc;
	wcex.cbClsExtra		= 0;
	wcex.cbWndExtra		= 0;
	wcex.hInstance		= hInstance;
	wcex.hIcon			= LoadIcon(hInstance, (LPCTSTR)IDI_TASKBAR);
	wcex.hCursor		= LoadCursor(NULL, IDC_ARROW);
	wcex.hbrBackground	= (HBRUSH)(COLOR_WINDOW+1);
	wcex.lpszMenuName	= (LPCTSTR)NULL;
	wcex.lpszClassName	= szWindowClass;
	wcex.hIconSm		= LoadIcon(wcex.hInstance, (LPCTSTR)IDI_SMALL);

	return RegisterClassEx(&wcex);
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPTSTR lpCmdLine, int nCmdShow)
{
	MSG msg;
	hInst = hInstance;
	CDCurrentDirectory();
	SetEenvironment();
	ParseProxyList();
	MyRegisterClass(hInstance);
	if (!InitInstance (hInstance, SW_HIDE))
	{
		return FALSE;
	}
	CreateConsole();
	ExecCmdline();
	ShowTrayIcon(GetWindowsProxy());
	while (GetMessage(&msg, NULL, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	return 0;
}

