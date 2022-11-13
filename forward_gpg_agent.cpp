/* Copyright (c) 2022 Denis Byzov. All rights reserved.
 * Use of this file is governed by the BSD 3-clause license that
 * can be found in the LICENSE.txt file in the project root.
 */

// Rework of MSDN example from https://github.com/microsoft/Windows-classic-samples/tree/main/Samples/Win7Samples/netds/winsock/iocp/serverex

#include "forward_gpg_agent.h"

// #define CreateAcceptSocket_AcceptEx_dwReceiveDataLength 0

BOOL g_bEndServer = FALSE; // set to TRUE on CTRL-C
BOOL g_bRestart = TRUE;	   // set to TRUE to CTRL-BRK
BOOL g_bVerbose = FALSE;
SECURITY_DESCRIPTOR *g_pSD = NULL;
PTOKEN_DEFAULT_DACL g_pDACL = NULL;
unsigned long long g_counterFileMap = 0;
HANDLE g_hIOCP = INVALID_HANDLE_VALUE;
PORT2GPGSOCKET g_port2gpgsocket[MAX_LISTENING_PORTS];
NAMEDPIPE2GPGSOCKET g_npipe2gpgsocket[MAX_LISTENING_NPIPES];
HANDLE g_ThreadHandles[MAX_WORKER_THREAD];
WSAEVENT g_hCleanupEvent[1];
PCONTEXT_SOCKET g_pCtxtList = NULL; // linked list of context info structures
										// maintained to allow the the cleanup
										// handler to cleanly close all sockets and
										// free resources.

CRITICAL_SECTION g_CriticalSection; // guard access to the global context list

int msprintf(const TCHAR *lpFormat, ...);
int msprintfA(const char *lpFormat, ...);

int __cdecl _tmain(int argc, TCHAR *argv[])
{
	// SYSTEM_INFO systemInfo;
	WSADATA wsaData;
	DWORD dwThreadCount = 1;
	int nRet = 0;
	int numListeningPorts = 0;
	int numListeningNPipes = 0;
	SOCKET tmpsocket = INVALID_SOCKET;

	g_hCleanupEvent[0] = WSA_INVALID_EVENT;

	for (int i = 0; i < MAX_WORKER_THREAD; ++i)
	{
		g_ThreadHandles[i] = INVALID_HANDLE_VALUE;
	}
	for (int i = 0; i < MAX_LISTENING_PORTS; ++i)
	{
		g_port2gpgsocket[i].port = NULL;
		g_port2gpgsocket[i].gpgsocket = NULL;
		g_port2gpgsocket[i].ctConnectionTypeLinked = ConnectionTypeInvalid;
		g_port2gpgsocket[i].sdListen = INVALID_SOCKET;
		g_port2gpgsocket[i].pCtxtListenSocket = NULL;
	}
	for (int i = 0; i < MAX_LISTENING_NPIPES; ++i)
	{
		g_npipe2gpgsocket[i].gpgsocket = NULL;
		g_npipe2gpgsocket[i].ctConnectionTypeLinked = ConnectionTypeInvalid;
		g_npipe2gpgsocket[i].pCtxtNPipe = NULL;
		g_npipe2gpgsocket[i].pipenameshort = NULL;
	}

	if (!ValidOptions(argc, argv, g_port2gpgsocket, numListeningPorts, g_npipe2gpgsocket, numListeningNPipes, dwThreadCount))
		return 1;

	if (!SetConsoleCtrlHandler(CtrlHandler, TRUE))
	{
		msprintf(_T("[ERROR] SetConsoleCtrlHandler() failed to install console handler: %d\n"),
				 GetLastError());
		return 2;
	}

	// GetSystemInfo(&systemInfo);
	// dwThreadCount = systemInfo.dwNumberOfProcessors * 2;
	dwThreadCount = max(dwThreadCount, 1);
	dwThreadCount = min(dwThreadCount, MAX_WORKER_THREAD);

	if (WSA_INVALID_EVENT == (g_hCleanupEvent[0] = WSACreateEvent()))
	{
		msprintf(_T("[ERROR] WSACreateEvent() failed: %d\n"), WSAGetLastError());
		return 3;
	}

	if ((nRet = WSAStartup(0x202, &wsaData)) != 0)
	{
		msprintf(_T("[ERROR] WSAStartup() failed: %d\n"), nRet);
		SetConsoleCtrlHandler(CtrlHandler, FALSE);
		if (g_hCleanupEvent[0] != WSA_INVALID_EVENT)
		{
			WSACloseEvent(g_hCleanupEvent[0]);
			g_hCleanupEvent[0] = WSA_INVALID_EVENT;
		}
		return 4;
	}

	__try
	{
		InitializeCriticalSection(&g_CriticalSection);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		msprintf(_T("[ERROR] InitializeCriticalSection raised an exception.\n"));
		SetConsoleCtrlHandler(CtrlHandler, FALSE);
		if (g_hCleanupEvent[0] != WSA_INVALID_EVENT)
		{
			WSACloseEvent(g_hCleanupEvent[0]);
			g_hCleanupEvent[0] = WSA_INVALID_EVENT;
		}
		return 5;
	}

	while (g_bRestart)
	{
		g_bRestart = FALSE;
		g_bEndServer = FALSE;
		WSAResetEvent(g_hCleanupEvent[0]);

		__try
		{
			//
			// notice that we will create more worker threads (dwThreadCount) than
			// the thread concurrency limit on the IOCP.
			//
			g_hIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
			if (g_hIOCP == NULL)
			{
				msprintf(_T("[ERROR] CreateIoCompletionPort() failed to create I/O completion port: %d\n"),
						 GetLastError());
				__leave;
			}

			for (DWORD dwCPU = 0; dwCPU < dwThreadCount; dwCPU++)
			{
				//
				// Create worker threads to service the overlapped I/O requests.  The decision
				// to create 2 worker threads per CPU in the system is a heuristic.  Also,
				// note that thread handles are closed right away, because we will not need them
				// and the worker threads will continue to execute.
				//
				HANDLE hThread = INVALID_HANDLE_VALUE;
				DWORD dwThreadId = 0;

				hThread = CreateThread(NULL, 0, WorkerThread, g_hIOCP, 0, &dwThreadId);
				if (hThread == NULL)
				{
					msprintf(_T("[ERROR] CreateThread() failed to create worker thread: %d\n"),
							 GetLastError());
					__leave;
				}
				g_ThreadHandles[dwCPU] = hThread;
				hThread = INVALID_HANDLE_VALUE;
			}

			for (int i = 0; i < numListeningPorts; ++i)
			{
				if (!CreateListenSocket(g_port2gpgsocket[i]))
					__leave;
				if (!CreateAcceptSocket(g_port2gpgsocket[i].sdListen, NULL, &g_port2gpgsocket[i]))
					__leave;
			}
			for (int i = 0; i < numListeningNPipes; ++i)
			{
				if (!CreateNPipeAndUpdateIOCP(g_npipe2gpgsocket[i]))
					__leave;
			}

			WSAWaitForMultipleEvents(1, g_hCleanupEvent, TRUE, WSA_INFINITE, FALSE);
		}

		__finally
		{
			g_bEndServer = TRUE;

			//
			// Cause worker threads to exit
			//
			if (g_hIOCP)
			{
				for (DWORD i = 0; i < dwThreadCount; i++)
					PostQueuedCompletionStatus(g_hIOCP, 0, 0, NULL);
			}

			//
			// Make sure worker threads exits.
			//
			if (WAIT_OBJECT_0 != WaitForMultipleObjects(dwThreadCount, g_ThreadHandles, TRUE, 1000))
				msprintf(_T("[ERROR] WaitForMultipleObjects() failed: %d\n"), GetLastError());
			else
				for (DWORD i = 0; i < dwThreadCount; i++)
				{
					if (g_ThreadHandles[i] != INVALID_HANDLE_VALUE)
						CloseHandle(g_ThreadHandles[i]);
					g_ThreadHandles[i] = INVALID_HANDLE_VALUE;
				}

			for (PPORT2GPGSOCKET ptmp = g_port2gpgsocket; ptmp < g_port2gpgsocket + numListeningPorts; ++ptmp)
			{
				if (ptmp->pCtxtListenSocket)
				{
					PIOCONTEXT_SOCKET pIOCtxtTemp = ptmp->pCtxtListenSocket->pIOContext;
					if (pIOCtxtTemp)
					{
						if (CancelIoEx((HANDLE)ptmp->sdListen, &pIOCtxtTemp->Overlapped) == 0)
						{
							// msprintf(_T("main_finally: CancelIoEx(Socket(%d)) failed: %d\n"), ptmp->sdListen, GetLastError());
							while (!HasOverlappedIoCompleted((LPOVERLAPPED)&pIOCtxtTemp->Overlapped))
								Sleep(0);
						}
						else if (g_bVerbose)
							msprintf(_T("main_finally: CancelIoEx(Socket(%d)) succeeds\n"), ptmp->sdListen);
						// while (!HasOverlappedIoCompleted((LPOVERLAPPED)&pIOCtxtTemp->Overlapped))
						// {
						// 	// Без CancelIoEx цикл никогда не завершится,
						// 	// т.к. мы отправили AcceptEx на ptmp->sdListen и закрыли все WorkingThread-ы.
						//  // Однако, он не завершается даже с CancelIoEx :-). WTF?
						// 	Sleep(0);
						// }

						tmpsocket = pIOCtxtTemp->SocketAccept;
						if (tmpsocket != INVALID_SOCKET)
						{
							if (g_bVerbose)
								msprintf(_T("main_finally: closesocket(Socket(%d))\n"), tmpsocket);
							closesocket(tmpsocket);
							pIOCtxtTemp->SocketAccept = INVALID_SOCKET;
						}
					}

					//
					// We know there is only one overlapped I/O on the listening socket
					//
					if (pIOCtxtTemp)
						xfree(pIOCtxtTemp);
						ptmp->pCtxtListenSocket->pIOContext = NULL;

					if (ptmp->pCtxtListenSocket)
						xfree(ptmp->pCtxtListenSocket);
					ptmp->pCtxtListenSocket = NULL;
				}

				if (ptmp->sdListen != INVALID_SOCKET)
				{
					if (g_bVerbose)	msprintf(_T("main_finally: closesocket(Socket(%d))\n"), ptmp->sdListen);
					closesocket(ptmp->sdListen);
					ptmp->sdListen = INVALID_SOCKET;
				}
			}

			for (PNAMEDPIPE2GPGSOCKET ptmp = g_npipe2gpgsocket; ptmp < g_npipe2gpgsocket + numListeningNPipes; ++ptmp)
			{
				CloseNPipeCtxt(ptmp->pCtxtNPipe);
			}

			CtxtListFree();

			if (g_hIOCP)
			{
				CloseHandle(g_hIOCP);
				g_hIOCP = NULL;
			}
			if (g_pSD)
			{
				xfree(g_pSD);
				g_pSD = NULL;
			}
			if (g_pDACL)
			{
				xfree(g_pDACL);
				g_pDACL = NULL;
			}
		} // finally

		if (g_bRestart)
			msprintf(_T("\nforward_gpg_agent is restarting...\n"));
		else
			msprintf(_T("\nforward_gpg_agent is exiting...\n"));

	} // while (g_bRestart)

	DeleteCriticalSection(&g_CriticalSection);
	if (g_hCleanupEvent[0] != WSA_INVALID_EVENT)
	{
		WSACloseEvent(g_hCleanupEvent[0]);
		g_hCleanupEvent[0] = WSA_INVALID_EVENT;
	}
	WSACleanup();
	SetConsoleCtrlHandler(CtrlHandler, FALSE);
	return 0;
} // main

//
//  Just validate the command line options.
//
BOOL ValidOptions(
	int argc,
	TCHAR *argv[],
	PORT2GPGSOCKET (&port2gpgsocket)[MAX_LISTENING_PORTS],
	int &numListeningPorts,
	NAMEDPIPE2GPGSOCKET (&npipe2gpgsocket)[MAX_LISTENING_NPIPES],
	int &numListeningNPipes,
	DWORD &numWorkerThreads)
{
	BOOL bRet = TRUE;
	int numLPT = numListeningPorts, numGPGS = numListeningPorts, numLNP = numListeningNPipes;
	size_t size = 0;
	TCHAR *pageantport = NULL;

	if (argc < 2)
	{
		PrintUsageHint();
		return (FALSE);
	}
	for (int i = 1; i < argc; i++)
	{
		if ((argv[i][0] == '-') || (argv[i][0] == '/'))
		{
			switch (_totlower(argv[i][1]))
			{
			case _T('p'):
				if (numLPT < MAX_LISTENING_PORTS && _tcslen(argv[i]) > 3 && argv[i][2] == _T(':'))
					port2gpgsocket[numLPT++].port = argv[i] + 3;
				break;

			case _T('a'):
				if (numGPGS < MAX_LISTENING_PORTS && _tcslen(argv[i]) > 2)
				{
					port2gpgsocket[numGPGS].ctConnectionTypeLinked = ConnectionTypeSocketGPG;
					port2gpgsocket[numGPGS++].gpgsocket = argv[i] + 2;
				}
				break;

			case _T('s'):
				size = _tcslen(argv[i]);
				if (size > 3 && argv[i][2] == _T(':') && numGPGS < MAX_LISTENING_PORTS && numLPT < MAX_LISTENING_PORTS)
				{
					pageantport = argv[i] + 3;
				}
				else if (size > 2 && argv[i][2] != _T(':') && numLNP < MAX_LISTENING_NPIPES)
				{
					npipe2gpgsocket[numLNP].ctConnectionTypeLinked = ConnectionTypePAgeant;
					npipe2gpgsocket[numLNP++].pipenameshort = argv[i] + 2;
				}
				break;

			case _T('t'):
				if (_tcslen(argv[i]) > 3 && argv[i][2] == _T(':'))
					numWorkerThreads = _ttoi(argv[i] + 3);
				break;

			case _T('v'):
				g_bVerbose = TRUE;
				break;

			case _T('?'):
				PrintUsageHint();
				return (FALSE);

			default:
				msprintf(_T("[ERROR] Unknown options flag %s\n"), argv[i]);
				bRet = FALSE;
				break;
			}
		}
	}

	numLPT = min(numGPGS, numLPT);
	if (pageantport != NULL && numLPT < MAX_LISTENING_PORTS)
	{
		port2gpgsocket[numLPT].ctConnectionTypeLinked = ConnectionTypePAgeant;
		port2gpgsocket[numLPT++].port = pageantport;
	}
	if (numLPT == numListeningPorts && numLNP == numListeningNPipes)
	{
		msprintf(_T("[ERROR] You must use at least one ('-p' and '-a') options or one '-s' option.\n"));
		bRet = FALSE;
	}
	else
	{
		numListeningPorts = numLPT;
		numListeningNPipes = numLNP;
	}
	return (bRet);
}

VOID PrintUsageHint()
{
	msprintf(_T("Usage:\n  forward_gpg_agent [-p:<port1>] [-p:<port2>...] [-a<gpg_agent_socket1>] [-a<gpg_agent_socket2>...] [-s:<PAgeant2port>] [-s<PAgeant2NamedPipe>] [-t:<threads number>] [-v] [-?]\n"));
	msprintf(_T("  -p:<port>\tSpecify the listening port number for \"-a\" option\n"));
	msprintf(_T("  -a<gpg_agent_socket>\tSpecify gpg agent \"socket\" file path (only \"gpg-agent\" and \"gpg-agent.extra\" works on Windows)\n"));
	msprintf(_T("  -s:<PAgeant2port>\tSpecify the listening port that will be connected with PAgeant (up to 1)\n"));
	msprintf(_T("  -s<PAgeant2NamedPipe>\tSpecify the NamedPipe short name (without \"\\\\.\\pipe\\\") that will be connected with PAgeant (up to %d)\n"), MAX_LISTENING_NPIPES);
	msprintf(_T("  -t:<threads number>\tSpecify number of worker threads (up to %d)\n"), MAX_WORKER_THREAD);
	msprintf(_T("  -v\t\tVerbose\n"));
	msprintf(_T("  -?\t\tDisplay this help\n"));
	msprintf(_T("  The total number of \"-p:\" and \"-s:\" options cannot be more than %d\n"), MAX_LISTENING_PORTS);
	msprintf(_T("  Press CTRL+C to stop server.\n"));
}

//
//  Intercept CTRL-C or CTRL-BRK events and cause the server to initiate shutdown.
//  CTRL-BRK resets the restart flag, and after cleanup the server restarts.
//
BOOL WINAPI CtrlHandler(DWORD dwEvent)
{
	switch (dwEvent)
	{
	case CTRL_BREAK_EVENT:
		g_bRestart = TRUE;
	case CTRL_C_EVENT:
	case CTRL_LOGOFF_EVENT:
	case CTRL_SHUTDOWN_EVENT:
	case CTRL_CLOSE_EVENT:
		if (g_bVerbose)
			msprintf(_T("CtrlHandler: closing listening sockets\n"));
		g_bEndServer = TRUE;
		WSASetEvent(g_hCleanupEvent[0]);
		break;

	default:
		//
		// unknown type--better pass it on.
		//
		return (FALSE);
	}
	return (TRUE);
}

//
// Create a socket with all the socket options we need, namely disable buffering
// and set linger.
//
SOCKET CreateSocket(void)
{
	int nRet = 0;
	int nZero = 0;
	SOCKET sdSocket = INVALID_SOCKET;

	sdSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_IP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (sdSocket == INVALID_SOCKET)
	{
		msprintf(_T("[ERROR] WSASocket(sdSocket) failed: %d\n"), WSAGetLastError());
		return (sdSocket);
	}

	//
	// Disable send buffering on the socket.  Setting SO_SNDBUF
	// to 0 causes winsock to stop buffering sends and perform
	// sends directly from our buffers, thereby save one memory copy.
	//
	// However, this does prevent the socket from ever filling the
	// send pipeline. This can lead to packets being sent that are
	// not full (i.e. the overhead of the IP and TCP headers is
	// great compared to the amount of data being carried).
	//
	// Disabling the send buffer has less serious repercussions
	// than disabling the receive buffer.
	//
	nZero = 0;
	nRet = setsockopt(sdSocket, SOL_SOCKET, SO_SNDBUF, (char *)&nZero, sizeof(nZero));
	if (nRet == SOCKET_ERROR)
	{
		msprintf(_T("[ERROR] setsockopt(SNDBUF) failed: %d\n"), WSAGetLastError());
		return (sdSocket);
	}

	//
	// Don't disable receive buffering. This will cause poor network
	// performance since if no receive is posted and no receive buffers,
	// the TCP stack will set the window size to zero and the peer will
	// no longer be allowed to send data.
	//

	//
	// Do not set a linger value...especially don't set it to an abortive
	// close. If you set abortive close and there happens to be a bit of
	// data remaining to be transfered (or data that has not been
	// acknowledged by the peer), the connection will be forcefully reset
	// and will lead to a loss of data (i.e. the peer won't get the last
	// bit of data). This is BAD. If you are worried about malicious
	// clients connecting and then not sending or receiving, the server
	// should maintain a timer on each connection. If after some point,
	// the server deems a connection is "stale" it can then set linger
	// to be abortive and close the connection.
	//

	/*
	LINGER lingerStruct;

	lingerStruct.l_onoff = 1;
	lingerStruct.l_linger = 0;
	nRet = setsockopt(sdSocket, SOL_SOCKET, SO_LINGER,
					  (char *)&lingerStruct, sizeof(lingerStruct));
	if( nRet == SOCKET_ERROR ) {
		msprintf(_T("[ERROR] setsockopt(SO_LINGER) failed: %d\n"), WSAGetLastError());
		return(sdSocket);
	}
	*/

	return (sdSocket);
}

//
//  Create a listening socket, bind, and set up its listening backlog.
//
BOOL CreateListenSocket(PORT2GPGSOCKET &port2gpgsocket)
{
	int nRet = 0;
	struct AddrInfo hints;
	struct AddrInfo *addrlocal = NULL;
	SOCKET sdListen = INVALID_SOCKET;

	SecureZeroMemory((PVOID)&hints, sizeof(struct AddrInfo));
	//
	// Resolve the interface
	//
	hints.ai_flags = AI_PASSIVE;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_IP;

	if (GetAddrInfo(NULL, port2gpgsocket.port, &hints, &addrlocal) != 0)
	{
		msprintf(_T("[ERROR] getaddrinfo() failed with error %d\n"), WSAGetLastError());
		return (FALSE);
	}

	if (addrlocal == NULL)
	{
		msprintf(_T("[ERROR] getaddrinfo() failed to resolve/convert the interface\n"));
		return (FALSE);
	}

	sdListen = CreateSocket();
	if (sdListen == INVALID_SOCKET)
	{
		FreeAddrInfo(addrlocal);
		return (FALSE);
	}

	nRet = bind(sdListen, addrlocal->ai_addr, (int)addrlocal->ai_addrlen);
	if (nRet == SOCKET_ERROR)
	{
		msprintf(_T("[ERROR] bind() failed: %d\n"), WSAGetLastError());
		FreeAddrInfo(addrlocal);
		return (FALSE);
	}

	nRet = listen(sdListen, 5);
	if (nRet == SOCKET_ERROR)
	{
		msprintf(_T("[ERROR] listen() failed: %d\n"), WSAGetLastError());
		FreeAddrInfo(addrlocal);
		return (FALSE);
	}

	if (g_bVerbose)
	{
		TCHAR tmpaddrbuf[64];
		if (addrlocal->ai_addr->sa_family == AF_INET)
		{
			sockaddr_in *tmpaddr = (sockaddr_in*)addrlocal->ai_addr;
			InetNtop(AF_INET, &tmpaddr->sin_addr, tmpaddrbuf, 64);
			msprintf(_T("CreateListenSocket: Socket(%d) listens to %s:%d\n"), sdListen, tmpaddrbuf, ntohs(tmpaddr->sin_port));
		}
		else if (addrlocal->ai_addr->sa_family == AF_INET6)
		{
			sockaddr_in6 *tmpaddr = (sockaddr_in6*)addrlocal->ai_addr;
			InetNtop(AF_INET6, &tmpaddr->sin6_addr, tmpaddrbuf, 64);
			msprintf(_T("CreateListenSocket: Socket(%d) listens to %s:%d\n"), sdListen, tmpaddrbuf, ntohs(tmpaddr->sin6_port));
		}
	}

	FreeAddrInfo(addrlocal);
	port2gpgsocket.sdListen = sdListen;

	return (TRUE);
}

BOOL CreateNPipeAndUpdateIOCP(
	NAMEDPIPE2GPGSOCKET &npipe2gpgsocket)
{
	PCONTEXT_NPIPE pCtxtNPipe;

	pCtxtNPipe = CtxtAllocateNPipe(ClientIo_NPipe_Connect);
	if (pCtxtNPipe == NULL)
	{
		msprintf(_T("[ERROR] CreateNPipeAndUpdateIOCP: CtxtAllocateNPipe\n"));
		return (FALSE);
	}
	pCtxtNPipe->ctConnectionTypeLinked = npipe2gpgsocket.ctConnectionTypeLinked;

	if (_stprintf_s(pCtxtNPipe->sPipeName, _T("\\\\.\\pipe\\%s"), npipe2gpgsocket.pipenameshort) <= 0)
	{
		msprintf(_T("[ERROR] CreateNPipeAndUpdateIOCP: sprintf_s: %d\n"), GetLastError());
		// In CreateNPipeAndUpdateIOCP we just create pCtxtNPipe
		// so pCtxtNPipe->pIOContext is used by nowhere,
		// and here we can use xfree instead of CtxtFreeNPipe.
		if (pCtxtNPipe->pIOContext)
			xfree(pCtxtNPipe->pIOContext);
		xfree(pCtxtNPipe);
		return (FALSE);
	}
	pCtxtNPipe->pPipeNameShort = pCtxtNPipe->sPipeName + 9;

	pCtxtNPipe->hNPipe = CreateNamedPipe(
		pCtxtNPipe->sPipeName,
		PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED,
		PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT,
		PIPE_UNLIMITED_INSTANCES,
		MAX_NPIPE_BUFF_SIZE,
		MAX_NPIPE_BUFF_SIZE,
		0,
		NULL);
	if (pCtxtNPipe->hNPipe == INVALID_HANDLE_VALUE)
	{
		msprintf(_T("[ERROR] CreateNPipeAndUpdateIOCP: CreateNamedPipe(\"%s\"): %d\n"),
			pCtxtNPipe->pPipeNameShort, GetLastError());
		if (pCtxtNPipe->pIOContext)
			xfree(pCtxtNPipe->pIOContext);
		xfree(pCtxtNPipe);
		return (FALSE);
	}

	g_hIOCP = CreateIoCompletionPort(pCtxtNPipe->hNPipe, g_hIOCP, (ULONG_PTR)pCtxtNPipe, 0);
	if (g_hIOCP == NULL)
	{
		msprintf(_T("[ERROR] CreateNPipeAndUpdateIOCP: CreateIoCompletionPort() failed: %d\n"), GetLastError());
		CloseHandle(pCtxtNPipe->hNPipe);
		if (pCtxtNPipe->pIOContext)
			xfree(pCtxtNPipe->pIOContext);
		xfree(pCtxtNPipe);
		return (FALSE);
	}
	npipe2gpgsocket.pCtxtNPipe = pCtxtNPipe;

	if (!ConnectNPipe(pCtxtNPipe))
	{
		msprintf(_T("[ERROR] CreateNPipeAndUpdateIOCP: ConnectNPipe\n"));
		return (FALSE);
	}
	if (g_bVerbose)
	{
		msprintf(_T("CreateNPipeAndUpdateIOCP: NPipe(\"%s\") created and added to IOCP, connect posted\n"),
			pCtxtNPipe->pPipeNameShort);
	}

	return (TRUE);
}

//
// Create a socket and invoke AcceptEx.  Only the original call to to this
// function needs to be added to the IOCP.
//
// If the expected behaviour of connecting client applications is to NOT
// send data right away, then only posting one AcceptEx can cause connection
// attempts to be refused if a client connects without sending some initial
// data (notice that the associated iocpclient does not operate this way
// but instead makes a connection and starts sending data write away).
// This is because the IOCP packet does not get delivered without the initial
// data (as implemented in this sample) thus preventing the worker thread
// from posting another AcceptEx and eventually the backlog value set in
// listen() will be exceeded if clients continue to try to connect.
//
// One technique to address this situation is to simply cause AcceptEx
// to return right away upon accepting a connection without returning any
// data.  This can be done by setting dwReceiveDataLength=0 when calling AcceptEx.
//
// Another technique to address this situation is to post multiple calls
// to AcceptEx.  Posting multiple calls to AcceptEx is similar in concept to
// increasing the backlog value in listen(), though posting AcceptEx is
// dynamic (i.e. during the course of running your application you can adjust
// the number of AcceptEx calls you post).  It is important however to keep
// your backlog value in listen() high in your server to ensure that the
// stack can accept connections even if your application does not get enough
// CPU cycles to repost another AcceptEx under stress conditions.
//
// This sample implements neither of these techniques and is therefore
// susceptible to the behaviour described above.
//
BOOL CreateAcceptSocket(const SOCKET sdListen, PCONTEXT_SOCKET pCtxtListenSocket, PPORT2GPGSOCKET pport2gpgsocket_out)
{
	int nRet = 0;
	DWORD dwRecvNumBytes = 0;
	DWORD bytes = 0;

	//
	// GUID to Microsoft specific extensions
	//
	GUID acceptex_guid = WSAID_ACCEPTEX;

	//
	// The context for listening socket uses the SockAccept member to store the
	// socket for client connection.
	//
	if (pCtxtListenSocket == NULL)
	{
		pCtxtListenSocket = UpdateCompletionPort(sdListen, ClientIo_Socket_Accept, FALSE);
		if (pCtxtListenSocket == NULL)
		{
			msprintf(_T("[ERROR] failed to update listen socket to IOCP\n"));
			return (FALSE);
		}
		if (pport2gpgsocket_out != NULL)
		{
			pCtxtListenSocket->ctConnectionTypeLinked = pport2gpgsocket_out->ctConnectionTypeLinked;
			pport2gpgsocket_out->pCtxtListenSocket = pCtxtListenSocket;
		}
		
		// msprintf(_T("addressof pCtxtListenSocket=0x%p\n"), pCtxtListenSocket);

		// Load the AcceptEx extension function from the provider for this socket
		nRet = WSAIoctl(
			sdListen,
			SIO_GET_EXTENSION_FUNCTION_POINTER,
			&acceptex_guid,
			sizeof(acceptex_guid),
			&pCtxtListenSocket->fnAcceptEx,
			sizeof(pCtxtListenSocket->fnAcceptEx),
			&bytes,
			NULL,
			NULL);
		if (nRet == SOCKET_ERROR)
		{
			msprintf(_T("[ERROR] failed to load AcceptEx: %d\n"), WSAGetLastError());
			return (FALSE);
		}
	}

	pCtxtListenSocket->pIOContext->SocketAccept = CreateSocket();
	if (pCtxtListenSocket->pIOContext->SocketAccept == INVALID_SOCKET)
	{
		msprintf(_T("[ERROR] failed to create new accept socket\n"));
		return (FALSE);
	}

	//
	// pay close attention to these parameters and buffer lengths
	//
	// #if CreateAcceptSocket_AcceptEx_dwReceiveDataLength != 0
	// nRet = pCtxtListenSocket->fnAcceptEx(sdListen, pCtxtListenSocket->pIOContext->SocketAccept,
	// 									   (LPVOID)(pCtxtListenSocket->pIOContext->Buffer),
	// 									   MAX_BUFF_SIZE - (2 * (sizeof(SOCKADDR_STORAGE) + 16)),
	// 									   sizeof(SOCKADDR_STORAGE) + 16, sizeof(SOCKADDR_STORAGE) + 16,
	// 									   &dwRecvNumBytes,
	// 									   (LPOVERLAPPED) & (pCtxtListenSocket->pIOContext->Overlapped));
	// #else // CreateAcceptSocket_AcceptEx_dwReceiveDataLength
	nRet = pCtxtListenSocket->fnAcceptEx(sdListen, pCtxtListenSocket->pIOContext->SocketAccept,
										   (LPVOID)(pCtxtListenSocket->pIOContext->Buffer),
										   0,
										   sizeof(SOCKADDR_STORAGE) + 16, sizeof(SOCKADDR_STORAGE) + 16,
										   &dwRecvNumBytes,
										   (LPOVERLAPPED) & (pCtxtListenSocket->pIOContext->Overlapped));
	// #endif // CreateAcceptSocket_AcceptEx_dwReceiveDataLength
	if (nRet == SOCKET_ERROR && (ERROR_IO_PENDING != WSAGetLastError()))
	{
		msprintf(_T("[ERROR] AcceptEx() failed: %d\n"), WSAGetLastError());
		return (FALSE);
	}
	if (g_bVerbose)
	{
		msprintf(_T("CreateAcceptSocket: AcceptEx(Socket(%d), Socket(%d)) posted\n"), sdListen, pCtxtListenSocket->pIOContext->SocketAccept);
	}

	return (TRUE);
}

//
// Worker thread that handles all I/O requests on any socket handle added to the IOCP.
//
DWORD WINAPI WorkerThread(LPVOID WorkThreadContext)
{
	HANDLE hIOCP = (HANDLE)WorkThreadContext;
	BOOL bSuccess = FALSE;
	int nRet = 0;
	BOOL bRet = FALSE;
	ULONG_PTR lpCompletionKey;
	LPWSAOVERLAPPED lpOverlapped = NULL;
	PCONTEXT_SOCKET lpPerSocketContext = NULL;
	PCONTEXT_SOCKET lpAcceptSocketContext = NULL;
	PCONTEXT_SOCKET lpGPGSocketContext = NULL;
	PCONTEXT_PAGEANT lpPAgeantConnectionContext = NULL;
	PCONTEXT_NPIPE lpNPipeContext = NULL;
	PIOCONTEXT_SOCKET lpIOContext = NULL;
	PIOCONTEXT_NPIPE lpNPipeIOContext;
	WSABUF buffRecv;
	WSABUF buffSend;
	DWORD dwRecvNumBytes = 0;
	DWORD dwSendNumBytes = 0;
	DWORD dwFlags = 0;
	DWORD dwIoSize = 0;
	DWORD dwIoSizeResp = 0;
	DWORD dwIOCPError = 0;
	DWORD dwMsgRemain = 0;
	DWORD dwSrcRemain = 0;
	const DWORD dwCurrentThreadId = GetCurrentThreadId();
	// HRESULT hRet;

	while (TRUE)
	{
		bSuccess = GetQueuedCompletionStatus(
			hIOCP,
			&dwIoSize,
			&lpCompletionKey,
			(LPOVERLAPPED*)&lpOverlapped,
			INFINITE);

		if (lpCompletionKey == 0)
			return 0;

		switch (*(PCONNECTIONTYPE)lpCompletionKey)
		{
		case ConnectionTypeSocket:
		{
			lpPerSocketContext = (PCONTEXT_SOCKET)lpCompletionKey;
			if (!bSuccess)
			{
				dwIOCPError = GetLastError();
				if (dwIOCPError == ERROR_OPERATION_ABORTED)
				{
					if (g_bVerbose)
						msprintf(_T("GetQueuedCompletionStatus: Socket(%d) operation %d aborted\n"), lpPerSocketContext->Socket, ((PIOCONTEXT_SOCKET)lpOverlapped)->IOOperation);
				}
				else
					msprintf(_T("[ERROR] GetQueuedCompletionStatus() failed: %d\n"), dwIOCPError);
			}

			// if (lpPerSocketContext == NULL)
			// {
			// 	//
			// 	// CTRL-C handler used PostQueuedCompletionStatus to post an I/O packet with
			// 	// a NULL CompletionKey (or if we get one for any reason).  It is time to exit.
			// 	//
			// 	return (0);
			// }

			if (g_bEndServer)
			{
				//
				// main thread will do all cleanup needed - see finally block
				//
				return (0);
			}

			lpIOContext = (PIOCONTEXT_SOCKET)lpOverlapped;

			//
			// We should never skip the loop and not post another AcceptEx if the current
			// completion packet is for previous AcceptEx
			//
			if (lpIOContext->IOOperation == ClientIo_Socket_Accept)
			{
				if (!bSuccess)
				{
					if (g_bVerbose)
						msprintf(_T("WorkerThread: closesocket(Socket(%d))\n"), lpPerSocketContext->pIOContext->SocketAccept);
					closesocket(lpPerSocketContext->pIOContext->SocketAccept);
					lpPerSocketContext->pIOContext->SocketAccept = INVALID_SOCKET;
					if (!CreateAcceptSocket(lpPerSocketContext->Socket, lpPerSocketContext, NULL))
					{
						msprintf(_T("Please shut down and reboot the server.\n"));
						WSASetEvent(g_hCleanupEvent[0]);
						return (0);
					}
					continue;
				}
			}
			else
			{
				if (!bSuccess || (bSuccess && (0 == dwIoSize)))
				{
					//
					// client connection dropped, continue to service remaining (and possibly
					// new) client connections
					//
					if (g_bVerbose)
					{
						msprintf(_T("WorkerThread %d: Socket(%d) connection closed\n"),
								 dwCurrentThreadId, lpPerSocketContext->Socket);
					}
					CancelIOLinkedAndCloseClient(lpPerSocketContext, FALSE);
					continue;
				}
			}

			//
			// determine what type of IO packet has completed by checking the IOCONTEXT_SOCKET
			// associated with this socket.  This will determine what action to take.
			//
			switch (lpIOContext->IOOperation)
			{
			case ClientIo_Socket_Accept:
				//
				// When the AcceptEx function returns, the socket sAcceptSocket is
				// in the default state for a connected socket. The socket sAcceptSocket
				// does not inherit the properties of the socket associated with
				// sListenSocket parameter until SO_UPDATE_ACCEPT_CONTEXT is set on
				// the socket. Use the setsockopt function to set the SO_UPDATE_ACCEPT_CONTEXT
				// option, specifying sAcceptSocket as the socket handle and sListenSocket
				// as the option value.
				//
				nRet = setsockopt(
					lpPerSocketContext->pIOContext->SocketAccept,
					SOL_SOCKET,
					SO_UPDATE_ACCEPT_CONTEXT,
					(char *)&lpPerSocketContext->Socket,
					sizeof(lpPerSocketContext->Socket));
				if (nRet == SOCKET_ERROR)
				{
					// just warn user here.
					msprintf(_T("[ERROR] setsockopt(SO_UPDATE_ACCEPT_CONTEXT) failed to update accept socket\n"));
					WSASetEvent(g_hCleanupEvent[0]);
					return (0);
				}
				if (g_bVerbose)
				{
					msprintf(_T("Socket(%d) accept connection, Socket(%d) accepted\n"), lpPerSocketContext->Socket, lpPerSocketContext->pIOContext->SocketAccept);
				}

				lpAcceptSocketContext = UpdateCompletionPort(
					lpPerSocketContext->pIOContext->SocketAccept,
					ClientIo_Socket_Accept, TRUE);
				if (lpAcceptSocketContext == NULL)
				{
					// just warn user here.
					msprintf(_T("[ERROR] failed to update accept socket to IOCP\n"));
					WSASetEvent(g_hCleanupEvent[0]);
					return (0);
				}
				lpAcceptSocketContext->ctConnectionTypeLinked = lpPerSocketContext->ctConnectionTypeLinked;

				switch (lpAcceptSocketContext->ctConnectionTypeLinked)
				{
				case ConnectionTypeSocketGPG:
					lpGPGSocketContext = CreateGPGSocket(
						GetPort2gpgsocket(lpPerSocketContext->Socket, g_port2gpgsocket)->gpgsocket,
						lpAcceptSocketContext);
					if (lpGPGSocketContext == NULL)
					{
						msprintf(_T("[ERROR] CreateGPGSocket failed\n"));
						WSASetEvent(g_hCleanupEvent[0]);
						return (0);
					}
					lpAcceptSocketContext->pCtxtLinked = lpGPGSocketContext;
					lpAcceptSocketContext->pIOContext->IOOperation = ClientIo_Socket2Socket_Read;
					break;

				case ConnectionTypePAgeant:
					lpPAgeantConnectionContext = CreatePAgeantCtxt(lpAcceptSocketContext);
					if (lpPAgeantConnectionContext == NULL)
					{
						msprintf(_T("[ERROR] CreatePAgeantCtxt failed\n"));
						WSASetEvent(g_hCleanupEvent[0]);
						return (0);
					}
					lpAcceptSocketContext->pIOContext->IOOperation = ClientIo_Socket2PAgeant_Read;
					// lpAcceptSocketContext->pIOContext->nTotalBytes = 0;
					// lpAcceptSocketContext->pIOContext->nSentBytes = 0;
					break;

				default: // сюда не должны попадать
					msprintf(_T("[?WTF?] Unknown AcceptSocketContext->ctConnectionTypeLinked: %d\n"), lpAcceptSocketContext->ctConnectionTypeLinked);
					CloseClient(lpAcceptSocketContext, FALSE);
					if (!CreateAcceptSocket(lpPerSocketContext->Socket, lpPerSocketContext, NULL))
					{
						msprintf(_T("Please shut down and reboot the server.\n"));
						WSASetEvent(g_hCleanupEvent[0]);
						return (0);
					}
					continue;
				} // switch(lpAcceptSocketContext->ctConnectionTypeLinked)

				// was AcceptEx(lpPerSocketContext, lpIOContext)
				// lpIOContext == lpPerSocketContext->pIOContext
				// now Recv(lpAcceptSocketContext, lpAcceptSocketContext->pIOContext)
				dwRecvNumBytes = 0;
				dwFlags = 0;
				buffRecv.buf = lpAcceptSocketContext->pIOContext->Buffer;
				buffRecv.len = MAX_BUFF_SIZE;
				nRet = WSARecv(
					lpAcceptSocketContext->Socket,
					&buffRecv, 1,
					&dwRecvNumBytes,
					&dwFlags,
					&lpAcceptSocketContext->pIOContext->Overlapped, NULL);
				if (nRet == SOCKET_ERROR && (ERROR_IO_PENDING != WSAGetLastError()))
				{
					msprintf(_T("[ERROR] WSARecv() failed: %d\n"), WSAGetLastError());
					// CloseClientAndLinked(lpAcceptSocketContext, FALSE);
					CancelIOLinkedAndCloseClient(lpAcceptSocketContext, FALSE);
				}
				else if (g_bVerbose)
				{
					msprintf(_T("WorkerThread %d: Socket(%d) AcceptEx completed. Socket(%d) Recv posted\n"),
							 dwCurrentThreadId, lpPerSocketContext->Socket, lpAcceptSocketContext->Socket);
				}

				//
				// Time to post another outstanding AcceptEx
				//
				if (!CreateAcceptSocket(lpPerSocketContext->Socket, lpPerSocketContext, NULL))
				{
					msprintf(_T("Please shut down and reboot the server.\n"));
					WSASetEvent(g_hCleanupEvent[0]);
					return (0);
				}
				break;

			case ClientIo_Socket2PAgeant_Read:
				// was Recv(lpPerSocketContext, lpIOContext)
				// lpIOContext == lpPerSocketContext->pIOContext
				// buf == lpIOContext->Buffer
				//
				lpPAgeantConnectionContext = (PCONTEXT_PAGEANT)lpPerSocketContext->pCtxtLinked;
				lpIOContext->nTotalBytes = dwIoSize;
				lpIOContext->nSentBytes = 0;
				if (lpPAgeantConnectionContext->dwMsgTotal == 0)
				{
					// This is first link in message chain
					lpPAgeantConnectionContext->dwMsgTotal = ntohl(*(u_long *)lpIOContext->Buffer) + sizeof(DWORD);
					if (lpPAgeantConnectionContext->dwMsgTotal == 0 || lpPAgeantConnectionContext->dwMsgTotal > PAGEANT_MAX_MSG_LEN)
					{
						msprintf(_T("[ERROR] WorkerThread %d: ClientIo_Socket2PAgeant_Read: PAgeant query for Socket(%d) too large or have zero size: %d bytes\n"),
								 dwCurrentThreadId, lpPerSocketContext->Socket, lpPAgeantConnectionContext->dwMsgTotal);
						CancelIOLinkedAndCloseClient(lpPerSocketContext, FALSE);
						break;
					}
					lpPAgeantConnectionContext->dwMsgSent = 0;
				}

				dwMsgRemain = lpPAgeantConnectionContext->dwMsgTotal - lpPAgeantConnectionContext->dwMsgSent;
				if (dwMsgRemain > dwIoSize)
				{
					memcpy_s(
						(char *)lpPAgeantConnectionContext->lpSharedMem + lpPAgeantConnectionContext->dwMsgSent,
						PAGEANT_MAX_MSG_LEN - lpPAgeantConnectionContext->dwMsgSent,
						lpIOContext->Buffer,
						dwIoSize);
					lpPAgeantConnectionContext->dwMsgSent += dwIoSize;

					// lpIOContext->Buffer ended
					// Recv(lpPerSocketContext, lpIOContext, buf=lpIOContext->Buffer)
					lpIOContext->IOOperation = ClientIo_Socket2PAgeant_Read;
					dwRecvNumBytes = 0;
					dwFlags = 0;
					buffRecv.buf = lpIOContext->Buffer;
					buffRecv.len = MAX_BUFF_SIZE;
					nRet = WSARecv(
						lpPerSocketContext->Socket,
						&buffRecv, 1, &dwRecvNumBytes,
						&dwFlags,
						&lpIOContext->Overlapped, NULL);
					if (nRet == SOCKET_ERROR && (ERROR_IO_PENDING != WSAGetLastError()))
					{
						msprintf(_T("[ERROR] ClientIo_Socket2PAgeant_Read: WSARecv() failed: %d\n"), WSAGetLastError());
						CancelIOLinkedAndCloseClient(lpPerSocketContext, FALSE);
						break;
					}
					else if (g_bVerbose)
					{
						msprintf(_T("WorkerThread %d: ClientIo_Socket2PAgeant_Read: Socket(%d) Recv completed (%d bytes), %d more bytes are needed to fill the PAgeantMSG, Socket(%d) Recv posted\n"),
								 dwCurrentThreadId, lpPerSocketContext->Socket,
								 dwIoSize, dwMsgRemain - dwIoSize, lpPerSocketContext->Socket);
					}
				}	 // dwMsgRemain > dwIoSize
				else // dwMsgRemain <= dwIoSize
				{
					memcpy_s(
						(char *)lpPAgeantConnectionContext->lpSharedMem + lpPAgeantConnectionContext->dwMsgSent,
						PAGEANT_MAX_MSG_LEN - lpPAgeantConnectionContext->dwMsgSent,
						lpIOContext->Buffer, dwMsgRemain);
					// lpPAgeantConnectionContext->dwMsgSent = lpPAgeantConnectionContext->dwMsgTotal;
					lpIOContext->nSentBytes = dwMsgRemain;

					// lpPAgeantConnectionContext->lpSharedMem is filled
					// Send2PAgeant
					// if (!SendMessage(lpPAgeantConnectionContext->hwndPAgeant, WM_COPYDATA, 0, (LPARAM)(&lpPAgeantConnectionContext->cds)))
					if (!SendMessageTimeout(lpPAgeantConnectionContext->hwndPAgeant, WM_COPYDATA, 0, (LPARAM)(&lpPAgeantConnectionContext->cds), SMTO_BLOCK, 0, NULL))
					{
						msprintf(_T("[ERROR] ClientIo_Socket2PAgeant_Read: SendMessage(0x%p,WM_COPYDATA) failed: %d\n"), lpPAgeantConnectionContext->hwndPAgeant, GetLastError());
						CancelIOLinkedAndCloseClient(lpPerSocketContext, FALSE);
						break;
					}
					dwIoSizeResp = ntohl(*(u_long *)lpPAgeantConnectionContext->lpSharedMem) + sizeof(DWORD);
					if (dwIoSizeResp == sizeof(DWORD))
					{
						if (g_bVerbose)
						{
							msprintf(_T("WorkerThread %d: ClientIo_Socket2PAgeant_Read: Socket(%d) Recv completed (%d bytes), PAgeant request completed (4 bytes)\n"),
									 dwCurrentThreadId, lpPerSocketContext->Socket,
									 dwIoSize);
						}
						CancelIOLinkedAndCloseClient(lpPerSocketContext, FALSE);
						break;
					}
					else if (dwIoSizeResp > PAGEANT_MAX_MSG_LEN)
					{
						msprintf(_T("[ERROR] WorkerThread %d: ClientIo_Socket2PAgeant_Read: PAgeant reply for Socket(%d) is too large (%d bytes)\n"),
								 dwCurrentThreadId, lpPerSocketContext->Socket, dwIoSizeResp);
						CancelIOLinkedAndCloseClient(lpPerSocketContext, FALSE);
						break;
					}

					// Send(lpPerSocketContext, lpIOContext, buf=lpPAgeantConnectionContext->lpSharedMem)
					lpIOContext->IOOperation = ClientIo_Socket2PAgeant_Write;
					lpPAgeantConnectionContext->dwMsgTotal = dwIoSizeResp;
					lpPAgeantConnectionContext->dwMsgSent = 0;
					lpIOContext->wsabuf.buf = (char *)lpPAgeantConnectionContext->lpSharedMem;
					lpIOContext->wsabuf.len = dwIoSizeResp;
					dwFlags = 0;
					nRet = WSASend(
						lpPerSocketContext->Socket,
						&lpIOContext->wsabuf, 1, &dwSendNumBytes,
						dwFlags,
						&(lpIOContext->Overlapped), NULL);
					if (nRet == SOCKET_ERROR && (ERROR_IO_PENDING != WSAGetLastError()))
					{
						msprintf(_T("[ERROR] WSASend() failed: %d\n"), WSAGetLastError());
						CancelIOLinkedAndCloseClient(lpPerSocketContext, FALSE);
						break;
					}
					else if (g_bVerbose)
					{
						msprintf(_T("WorkerThread %d: ClientIo_Socket2PAgeant_Read: Socket(%d) Recv completed (%d bytes), PAgeant request completed (%d bytes), Socket(%d) Send posted\n"),
								 dwCurrentThreadId, lpPerSocketContext->Socket,
								 dwIoSize, dwIoSizeResp, lpPerSocketContext->Socket);
					}
				} // dwMsgRemain <= dwIoSize
				break;

			case ClientIo_Socket2PAgeant_Write:
				lpPAgeantConnectionContext = (PCONTEXT_PAGEANT)lpPerSocketContext->pCtxtLinked;
				// a write operation has completed, determine if all the data intended to be
				// sent actually was sent.
				lpPAgeantConnectionContext->dwMsgSent += dwIoSize;
				dwFlags = 0;
				if (lpPAgeantConnectionContext->dwMsgSent < lpPAgeantConnectionContext->dwMsgTotal)
				{
					// the previous write operation didn't send all the data,
					// post another send to complete the operation
					// was Send(lpPerSocketContext, lpIOContext, buf=lpPAgeantConnectionContext->lpSharedMem)
					// lpIOContext == lpPerSocketContext->pIOContext
					// now SendMore(lpPerSocketContext, lpIOContext, buf=lpPAgeantConnectionContext->lpSharedMem)
					//
					lpIOContext->IOOperation = ClientIo_Socket2PAgeant_Write;
					buffSend.buf = (char *)lpPAgeantConnectionContext->lpSharedMem + lpPAgeantConnectionContext->dwMsgSent;
					buffSend.len = lpPAgeantConnectionContext->dwMsgTotal - lpPAgeantConnectionContext->dwMsgSent;
					nRet = WSASend(
						lpPerSocketContext->Socket,
						&buffSend, 1, &dwSendNumBytes,
						dwFlags,
						&(lpIOContext->Overlapped), NULL);
					if (nRet == SOCKET_ERROR && (ERROR_IO_PENDING != WSAGetLastError()))
					{
						msprintf(_T("[ERROR] ClientIo_Socket2PAgeant_Write: WSASend() failed: %d\n"), WSAGetLastError());
						CancelIOLinkedAndCloseClient(lpPerSocketContext, FALSE);
						break;
					}
					else if (g_bVerbose)
					{
						msprintf(_T("WorkerThread %d: ClientIo_Socket2PAgeant_Write: Socket(%d) Send partially completed (%d bytes), Socket(%d) Send posted\n"),
								 dwCurrentThreadId, lpPerSocketContext->Socket, dwIoSize, lpPerSocketContext->Socket);
					}
					break;
				}
				// Previous write operation completed for this socket.
				// was Send(lpPerSocketContext, lpIOContext, buf=lpPAgeantConnectionContext->lpSharedMem)
				// lpIOContext == lpPerSocketContext->pIOContext
				lpPAgeantConnectionContext->dwMsgTotal = 0;
				lpPAgeantConnectionContext->dwMsgSent = 0;

				if (lpIOContext->nSentBytes == lpIOContext->nTotalBytes)
				{
					// lpIOContext->Buffer ended
					// Recv(lpPerSocketContext, lpIOContext, buf=lpIOContext->Buffer)
					lpIOContext->IOOperation = ClientIo_Socket2PAgeant_Read;
					dwRecvNumBytes = 0;
					dwFlags = 0;
					buffRecv.buf = lpIOContext->Buffer;
					buffRecv.len = MAX_BUFF_SIZE;
					nRet = WSARecv(
						lpPerSocketContext->Socket,
						&buffRecv, 1, &dwRecvNumBytes,
						&dwFlags,
						&lpIOContext->Overlapped, NULL);
					if (nRet == SOCKET_ERROR && (ERROR_IO_PENDING != WSAGetLastError()))
					{
						msprintf(_T("[ERROR] ClientIo_Socket2PAgeant_Write: WSARecv() failed: %d\n"), WSAGetLastError());
						CancelIOLinkedAndCloseClient(lpPerSocketContext, FALSE);
						break;
					}
					else if (g_bVerbose)
					{
						msprintf(_T("WorkerThread %d: ClientIo_Socket2PAgeant_Write: Socket(%d) Send completed (%d bytes), start new PAgeantMSG, Socket(%d) Recv posted\n"),
								 dwCurrentThreadId, lpPerSocketContext->Socket,
								 dwIoSize, lpPerSocketContext->Socket);
					}
					break;
				}

				// lpIOContext->nSentBytes < lpIOContext->nTotalBytes
				dwSrcRemain = lpIOContext->nTotalBytes - lpIOContext->nSentBytes;
				if (lpIOContext->nTotalBytes - lpIOContext->nSentBytes < 4)
				{
					msprintf(_T("[ERROR] ClientIo_Socket2PAgeant_Write: we have to start new PAgeantMSG but src buffer length < 4 bytes\n"));
					CancelIOLinkedAndCloseClient(lpPerSocketContext, FALSE);
					break;
				}
				// This is first link in message chain
				lpPAgeantConnectionContext->dwMsgTotal = ntohl(*(u_long *)(lpIOContext->Buffer + lpIOContext->nSentBytes)) + sizeof(DWORD);
				if (lpPAgeantConnectionContext->dwMsgTotal == 0 || lpPAgeantConnectionContext->dwMsgTotal > PAGEANT_MAX_MSG_LEN)
				{
					msprintf(_T("[ERROR] WorkerThread %d: ClientIo_Socket2PAgeant_Write: PAgeant query for Socket(%d) too large or have zero size: %d bytes\n"),
							 dwCurrentThreadId, lpPerSocketContext->Socket, lpPAgeantConnectionContext->dwMsgTotal);
					CancelIOLinkedAndCloseClient(lpPerSocketContext, FALSE);
					break;
				}
				lpPAgeantConnectionContext->dwMsgSent = 0;

				dwMsgRemain = lpPAgeantConnectionContext->dwMsgTotal;
				if (dwMsgRemain > dwSrcRemain)
				{
					memcpy_s(
						lpPAgeantConnectionContext->lpSharedMem,
						PAGEANT_MAX_MSG_LEN,
						lpIOContext->Buffer + lpIOContext->nSentBytes,
						dwSrcRemain);
					lpPAgeantConnectionContext->dwMsgSent += dwSrcRemain;

					// lpIOContext->Buffer ended
					// Recv(lpPerSocketContext, lpIOContext, buf=lpIOContext->Buffer)
					lpIOContext->IOOperation = ClientIo_Socket2PAgeant_Read;
					dwRecvNumBytes = 0;
					dwFlags = 0;
					buffRecv.buf = lpIOContext->Buffer;
					buffRecv.len = MAX_BUFF_SIZE;
					nRet = WSARecv(
						lpPerSocketContext->Socket,
						&buffRecv, 1, &dwRecvNumBytes,
						&dwFlags,
						&lpIOContext->Overlapped, NULL);
					if (nRet == SOCKET_ERROR && (ERROR_IO_PENDING != WSAGetLastError()))
					{
						msprintf(_T("[ERROR] ClientIo_Socket2PAgeant_Write: WSARecv() failed: %d\n"), WSAGetLastError());
						CancelIOLinkedAndCloseClient(lpPerSocketContext, FALSE);
						break;
					}
					else if (g_bVerbose)
					{
						msprintf(_T("WorkerThread %d: ClientIo_Socket2PAgeant_Write: Socket(%d) Send completed (%d bytes), %d more bytes are needed to fill the PAgeantMSG, Socket(%d) Recv posted\n"),
								 dwCurrentThreadId, lpPerSocketContext->Socket,
								 dwIoSize, dwMsgRemain - dwSrcRemain, lpPerSocketContext->Socket);
					}
				}	 // dwMsgRemain > dwSrcRemain
				else // dwMsgRemain <= dwSrcRemain
				{
					memcpy_s(
						lpPAgeantConnectionContext->lpSharedMem,
						PAGEANT_MAX_MSG_LEN,
						lpIOContext->Buffer + lpIOContext->nSentBytes,
						dwMsgRemain);
					// lpPAgeantConnectionContext->dwMsgSent = lpPAgeantConnectionContext->dwMsgTotal;
					lpIOContext->nSentBytes += dwMsgRemain;

					// lpPAgeantConnectionContext->lpSharedMem is filled
					// Send2PAgeant
					// if (!SendMessage(lpPAgeantConnectionContext->hwndPAgeant, WM_COPYDATA, 0, (LPARAM)(&lpPAgeantConnectionContext->cds)))
					if (!SendMessageTimeout(lpPAgeantConnectionContext->hwndPAgeant, WM_COPYDATA, 0, (LPARAM)(&lpPAgeantConnectionContext->cds), SMTO_BLOCK, 0, NULL))
					{
						msprintf(_T("[ERROR] ClientIo_Socket2PAgeant_Write: SendMessage(0x%p,WM_COPYDATA) failed: %d\n"), lpPAgeantConnectionContext->hwndPAgeant, GetLastError());
						CancelIOLinkedAndCloseClient(lpPerSocketContext, FALSE);
						break;
					}
					dwIoSizeResp = ntohl(*(u_long *)lpPAgeantConnectionContext->lpSharedMem) + sizeof(DWORD);
					if (dwIoSizeResp == sizeof(DWORD))
					{
						if (g_bVerbose)
						{
							msprintf(_T("WorkerThread %d: ClientIo_Socket2PAgeant_Write: Socket(%d) Send completed (%d bytes), PAgeant request completed (4 bytes)\n"),
									 dwCurrentThreadId, lpPerSocketContext->Socket,
									 dwIoSize);
						}
						CancelIOLinkedAndCloseClient(lpPerSocketContext, FALSE);
						break;
					}
					else if (dwIoSizeResp > PAGEANT_MAX_MSG_LEN)
					{
						msprintf(_T("[ERROR] WorkerThread %d: ClientIo_Socket2PAgeant_Write: PAgeant reply for Socket(%d) is too large (%d bytes)\n"),
								 dwCurrentThreadId, lpPerSocketContext->Socket, dwIoSizeResp);
						CancelIOLinkedAndCloseClient(lpPerSocketContext, FALSE);
						break;
					}

					// Send(lpPerSocketContext, lpIOContext, buf=lpPAgeantConnectionContext->lpSharedMem)
					lpIOContext->IOOperation = ClientIo_Socket2PAgeant_Write;
					lpPAgeantConnectionContext->dwMsgTotal = dwIoSizeResp;
					lpPAgeantConnectionContext->dwMsgSent = 0;
					lpIOContext->wsabuf.buf = (char *)lpPAgeantConnectionContext->lpSharedMem;
					lpIOContext->wsabuf.len = dwIoSizeResp;
					dwFlags = 0;
					nRet = WSASend(
						lpPerSocketContext->Socket,
						&lpIOContext->wsabuf, 1, &dwSendNumBytes,
						dwFlags,
						&(lpIOContext->Overlapped), NULL);
					if (nRet == SOCKET_ERROR && (ERROR_IO_PENDING != WSAGetLastError()))
					{
						msprintf(_T("[ERROR] ClientIo_Socket2PAgeant_Write: WSASend() failed: %d\n"), WSAGetLastError());
						CancelIOLinkedAndCloseClient(lpPerSocketContext, FALSE);
						break;
					}
					else if (g_bVerbose)
					{
						msprintf(_T("WorkerThread %d: ClientIo_Socket2PAgeant_Write: Socket(%d) Send completed (%d bytes), PAgeant request completed (%d bytes), Socket(%d) Send posted\n"),
								 dwCurrentThreadId, lpPerSocketContext->Socket,
								 dwIoSize, dwIoSizeResp, lpPerSocketContext->Socket);
					}
				} // dwMsgRemain <= dwSrcRemain
				break;

			case ClientIo_Socket2SocketGPG_GPGAuthorization:
				lpIOContext->nSentBytes += dwIoSize;
				dwFlags = 0;
				if (lpIOContext->nSentBytes < lpIOContext->nTotalBytes)
				{
					//
					// the previous write operation didn't send all the data,
					// post another send to complete the operation
					// was Send(lpPerSocketContext, lpIOContext)
					// lpIOContext == lpPerSocketContext->pIOContext // this is for ClientIo_Socket2SocketGPG_GPGAuthorization
					// now SendMore(lpPerSocketContext, lpIOContext)
					//
					lpIOContext->IOOperation = ClientIo_Socket2SocketGPG_GPGAuthorization;
					buffSend.buf = lpIOContext->Buffer + lpIOContext->nSentBytes;
					buffSend.len = lpIOContext->nTotalBytes - lpIOContext->nSentBytes;
					nRet = WSASend(
						lpPerSocketContext->Socket, // send to the same socket
						&buffSend, 1, &dwSendNumBytes,
						dwFlags,
						&(lpIOContext->Overlapped), NULL);
					if (nRet == SOCKET_ERROR && (ERROR_IO_PENDING != WSAGetLastError()))
					{
						msprintf(_T("[ERROR] WSASend() failed: %d\n"), WSAGetLastError());
						// CloseClientAndLinked(lpPerSocketContext, FALSE);
						CancelIOLinkedAndCloseClient(lpPerSocketContext, FALSE);
					}
					else if (g_bVerbose)
					{
						msprintf(_T("WorkerThread %d: GPGAuthorization: Socket(%d) Send partially completed (%d bytes), Socket(%d) Send posted\n"),
								 dwCurrentThreadId, lpPerSocketContext->Socket, dwIoSize, lpPerSocketContext->Socket);
					}
				}
				else
				{
					//
					// previous write operation completed for this socket, post recv
					// from the same socket with his buffer (it is the same as lpIOContext)
					// was Send(lpPerSocketContext, lpIOContext)
					// lpIOContext == lpPerSocketContext->pIOContext // this is for ClientIo_Socket2SocketGPG_GPGAuthorization
					// now Recv(lpPerSocketContext, lpIOContext)
					//
					lpIOContext->IOOperation = ClientIo_Socket2Socket_Read;
					dwRecvNumBytes = 0;
					dwFlags = 0;
					buffRecv.buf = lpIOContext->Buffer;
					buffRecv.len = MAX_BUFF_SIZE;
					nRet = WSARecv(
						lpPerSocketContext->Socket,
						&buffRecv, 1, &dwRecvNumBytes,
						&dwFlags,
						&lpIOContext->Overlapped, NULL);
					if (nRet == SOCKET_ERROR && (ERROR_IO_PENDING != WSAGetLastError()))
					{
						msprintf(_T("[ERROR] WSARecv() failed: %d\n"), WSAGetLastError());
						// CloseClientAndLinked(lpPerSocketContext, FALSE);
						CancelIOLinkedAndCloseClient(lpPerSocketContext, FALSE);
					}
					else if (g_bVerbose)
					{
						msprintf(_T("WorkerThread %d: GPGAuthorization: Socket(%d) Send completed (%d bytes), Socket(%d) Recv posted\n"),
								 dwCurrentThreadId, lpPerSocketContext->Socket, dwIoSize,
								 lpPerSocketContext->Socket);
					}
				}
				break;

			case ClientIo_Socket2Socket_Read:
				//
				// a read operation has completed, post a write operation
				// to linked socket using the same data buffer.
				// was Recv(lpPerSocketContext, lpIOContext)
				// lpIOContext == lpPerSocketContext->pIOContext
				// now Send(lpPerSocketContext->pCtxtLinked, lpIOContext)
				//
				lpIOContext->IOOperation = ClientIo_Socket2Socket_Write;
				lpIOContext->nTotalBytes = dwIoSize;
				lpIOContext->nSentBytes = 0;
				lpIOContext->wsabuf.len = dwIoSize;
				dwFlags = 0;
				nRet = WSASend(
					((PCONTEXT_SOCKET)lpPerSocketContext->pCtxtLinked)->Socket,
					&lpIOContext->wsabuf, 1, &dwSendNumBytes,
					dwFlags,
					&(lpIOContext->Overlapped), NULL);
				if (nRet == SOCKET_ERROR && (ERROR_IO_PENDING != WSAGetLastError()))
				{
					msprintf(_T("[ERROR] WSASend() failed: %d\n"), WSAGetLastError());
					// CloseClientAndLinked(lpPerSocketContext, FALSE);
					CancelIOLinkedAndCloseClient(lpPerSocketContext, FALSE);
				}
				else if (g_bVerbose)
				{
					msprintf(_T("WorkerThread %d: ClientIo_Socket2Socket_Read: Socket(%d) Recv completed (%d bytes), Socket(%d) Send posted\n"),
							 dwCurrentThreadId, lpPerSocketContext->Socket, dwIoSize,
							 ((PCONTEXT_SOCKET)lpPerSocketContext->pCtxtLinked)->Socket);
				}
				break;

			case ClientIo_Socket2Socket_Write:
				//
				// a write operation has completed, determine if all the data intended to be
				// sent actually was sent.
				//
				lpIOContext->nSentBytes += dwIoSize;
				dwFlags = 0;
				if (lpIOContext->nSentBytes < lpIOContext->nTotalBytes)
				{
					//
					// the previous write operation didn't send all the data,
					// post another send to complete the operation
					// was Send(lpPerSocketContext, lpIOContext)
					// lpIOContext == lpPerSocketContext->pCtxtLinked->pIOContext
					// now SendMore(lpPerSocketContext, lpIOContext)
					//
					lpIOContext->IOOperation = ClientIo_Socket2Socket_Write;
					buffSend.buf = lpIOContext->Buffer + lpIOContext->nSentBytes;
					buffSend.len = lpIOContext->nTotalBytes - lpIOContext->nSentBytes;
					nRet = WSASend(
						lpPerSocketContext->Socket, // send to the same socket
						&buffSend, 1, &dwSendNumBytes,
						dwFlags,
						&(lpIOContext->Overlapped), NULL);
					if (nRet == SOCKET_ERROR && (ERROR_IO_PENDING != WSAGetLastError()))
					{
						msprintf(_T("[ERROR] WSASend() failed: %d\n"), WSAGetLastError());
						// CloseClientAndLinked(lpPerSocketContext, FALSE);
						CancelIOLinkedAndCloseClient(lpPerSocketContext, FALSE);
					}
					else if (g_bVerbose)
					{
						msprintf(_T("WorkerThread %d: ClientIo_Socket2Socket_Write: Socket(%d) Send partially completed (%d bytes), Socket(%d) Send posted\n"),
								 dwCurrentThreadId, lpPerSocketContext->Socket, dwIoSize, lpPerSocketContext->Socket);
					}
				}
				else
				{
					//
					// previous write operation completed for this socket, post another recv
					// from the linked socket with his buffer (it is the same as lpIOContext)
					// was Send(lpPerSocketContext, lpIOContext)
					// lpIOContext == lpPerSocketContext->pCtxtLinked->pIOContext
					// now Recv(lpPerSocketContext->pCtxtLinked, lpIOContext)
					//
					lpIOContext->IOOperation = ClientIo_Socket2Socket_Read;
					dwRecvNumBytes = 0;
					dwFlags = 0;
					buffRecv.buf = lpIOContext->Buffer;
					buffRecv.len = MAX_BUFF_SIZE;
					nRet = WSARecv(
						((PCONTEXT_SOCKET)lpPerSocketContext->pCtxtLinked)->Socket,
						&buffRecv, 1, &dwRecvNumBytes,
						&dwFlags,
						&lpIOContext->Overlapped, NULL);
					if (nRet == SOCKET_ERROR && (ERROR_IO_PENDING != WSAGetLastError()))
					{
						msprintf(_T("[ERROR] WSARecv() failed: %d\n"), WSAGetLastError());
						// CloseClientAndLinked(lpPerSocketContext, FALSE);
						CancelIOLinkedAndCloseClient(lpPerSocketContext, FALSE);
					}
					else if (g_bVerbose)
					{
						msprintf(_T("WorkerThread %d: ClientIo_Socket2Socket_Write: Socket(%d) Send completed (%d bytes), Socket(%d) Recv posted\n"),
								 dwCurrentThreadId, lpPerSocketContext->Socket, dwIoSize,
								 ((PCONTEXT_SOCKET)lpPerSocketContext->pCtxtLinked)->Socket);
					}
				}
				break;

			default:
				break;
			} // switch (lpIOContext->IOOperation)
			break;
		} // case ConnectionTypeSocket

		case ConnectionTypeNPipe:
		{
			lpNPipeContext = (PCONTEXT_NPIPE)lpCompletionKey;
			if (!bSuccess)
			{
				dwIOCPError = GetLastError();
				switch (dwIOCPError)
				{
				case ERROR_BROKEN_PIPE:
					if (g_bVerbose)
					{
						msprintf(_T("GetQueuedCompletionStatus: NPipe(\"%s\") has been ended (during operation %d)\n"),
							lpNPipeContext->pPipeNameShort, ((PIOCONTEXT_NPIPE)lpOverlapped)->IOOperation);
					}
					break;

				case ERROR_OPERATION_ABORTED:
					if (g_bVerbose)
					{
						msprintf(_T("GetQueuedCompletionStatus: NPipe(\"%s\") operation %d aborted\n"),
							lpNPipeContext->pPipeNameShort, ((PIOCONTEXT_NPIPE)lpOverlapped)->IOOperation);
					}
					break;

				default:
					msprintf(_T("[ERROR] GetQueuedCompletionStatus: ConnectionTypeNPipe: failed: %d\n"), dwIOCPError);
					break;
				}
			}

			// if (lpNPipeContext == NULL)
			// {
			// 	// CTRL-C handler used PostQueuedCompletionStatus to post an I/O packet with
			// 	// a NULL CompletionKey (or if we get one for any reason).  It is time to exit.
			// 	return (0);
			// }

			if (g_bEndServer)
			{
				// main thread will do all cleanup needed - see finally block
				return (0);
			}

			lpNPipeIOContext = (PIOCONTEXT_NPIPE)lpOverlapped;

			// We should never skip the loop and not post another ConnectNamedPipe if the current
			// completion packet is for previous ConnectNamedPipe
			if (lpNPipeIOContext->IOOperation == ClientIo_NPipe_Connect)
			{
				if (!bSuccess)
				{
					if (!ConnectNPipe(lpNPipeContext))
					{
						msprintf(_T("Please shut down and reboot the server.\n"));
						WSASetEvent(g_hCleanupEvent[0]);
						return (0);
					}
					continue;
				}
			}
			else
			{
				if (!bSuccess || (bSuccess && (0 == dwIoSize)))
				{
					// client connection dropped, continue to service remaining (and possibly
					// new) client connections
					if (!ReconnectNPipeAndCancelIOLinked(lpNPipeContext))
					{
						msprintf(_T("Please shut down and reboot the server.\n"));
						WSASetEvent(g_hCleanupEvent[0]);
						return (0);
					}
					continue;
				}
			}

			switch (lpNPipeIOContext->IOOperation)
			{
			case ClientIo_NPipe_Connect:
				if (!GetNamedPipeClientProcessId(lpNPipeContext->hNPipe, &lpNPipeIOContext->dwClientPID))
				{
					msprintf(_T("[WARNING] ClientIo_NPipe_Connect: GetNamedPipeClientProcessId(\"%s\"): %d\n"),
						lpNPipeContext->pPipeNameShort, GetLastError());
				}
				lpPAgeantConnectionContext = CreatePAgeantCtxt(lpNPipeContext);
				if (lpPAgeantConnectionContext == NULL)
				{
					msprintf(_T("[ERROR] CreatePAgeantCtxt failed\n"));
					WSASetEvent(g_hCleanupEvent[0]);
					return (0);
				}

				lpNPipeIOContext->IOOperation = ClientIo_NPipe2PAgeant_Read;
				lpNPipeIOContext->nSentBytes = 0;
				lpNPipeIOContext->nTotalBytes = 0;
				// was ConnectNamedPipe(lpNPipeContext, lpNPipeIOContext)
				// lpNPipeIOContext == lpNPipeContext->pIOContext
				// now Read(lpNPipeContext, lpNPipeIOContext)
				dwRecvNumBytes = 0;
				bRet = ReadFile(
					lpNPipeContext->hNPipe,
					lpNPipeIOContext->Buffer,
					MAX_NPIPE_BUFF_SIZE,
					&dwRecvNumBytes,
					&lpNPipeIOContext->Overlapped);
				if (!bRet && (ERROR_IO_PENDING != GetLastError()))
				{
					msprintf(_T("[ERROR] ClientIo_NPipe_Connect: ReadFile(\"%s\") failed: %d\n"),
						lpNPipeContext->pPipeNameShort, GetLastError());
					if (!ReconnectNPipeAndCancelIOLinked(lpNPipeContext))
					{
						WSASetEvent(g_hCleanupEvent[0]);
						return (0);
					}
				}
				else if (g_bVerbose)
				{
					msprintf(_T("WorkerThread %d: ConnectNamedPipe(\"%s\") completed from PID(%d). ReadFile(\"%s\") posted\n"),
							 dwCurrentThreadId, lpNPipeContext->pPipeNameShort, lpNPipeIOContext->dwClientPID, lpNPipeContext->pPipeNameShort);
				}

				// Newer post another ConnectNamedPipe before there is opened one
				break;

			case ClientIo_NPipe2PAgeant_Read:
				// was ReadFile(lpNPipeContext, lpNPipeIOContext)
				// lpNPipeIOContext == lpNPipeContext->pIOContext
				// buf == lpNPipeIOContext->Buffer
				//
				lpPAgeantConnectionContext = (PCONTEXT_PAGEANT)lpNPipeContext->pCtxtLinked;
				lpNPipeIOContext->nTotalBytes = dwIoSize;
				lpNPipeIOContext->nSentBytes = 0;
				if (lpPAgeantConnectionContext->dwMsgTotal == 0)
				{
					// This is first link in message chain
					lpPAgeantConnectionContext->dwMsgTotal = ntohl(*(u_long *)lpNPipeIOContext->Buffer) + sizeof(DWORD);
					if (lpPAgeantConnectionContext->dwMsgTotal == 0 || lpPAgeantConnectionContext->dwMsgTotal > PAGEANT_MAX_MSG_LEN)
					{
						msprintf(_T("[ERROR] WorkerThread %d: ClientIo_NPipe2PAgeant_Read: PAgeant query for NPipe(\"%s\") too large or have zero size: %d bytes\n"),
								 dwCurrentThreadId, lpNPipeContext->pPipeNameShort, lpPAgeantConnectionContext->dwMsgTotal);
						if (!ReconnectNPipeAndCancelIOLinked(lpNPipeContext))
						{
							WSASetEvent(g_hCleanupEvent[0]);
							return (0);
						}
						break;
					}
					lpPAgeantConnectionContext->dwMsgSent = 0;
				}

				dwMsgRemain = lpPAgeantConnectionContext->dwMsgTotal - lpPAgeantConnectionContext->dwMsgSent;
				if (dwMsgRemain > dwIoSize)
				{
					memcpy_s(
						(char *)lpPAgeantConnectionContext->lpSharedMem + lpPAgeantConnectionContext->dwMsgSent,
						PAGEANT_MAX_MSG_LEN - lpPAgeantConnectionContext->dwMsgSent,
						lpNPipeIOContext->Buffer,
						dwIoSize);
					lpPAgeantConnectionContext->dwMsgSent += dwIoSize;

					// lpNPipeIOContext->Buffer ended
					// ReadFile(lpNPipeContext, lpNPipeIOContext, buf=lpNPipeIOContext->Buffer)
					lpNPipeIOContext->IOOperation = ClientIo_NPipe2PAgeant_Read;
					dwRecvNumBytes = 0;
					bRet = ReadFile(
						lpNPipeContext->hNPipe,
						lpNPipeIOContext->Buffer,
						MAX_NPIPE_BUFF_SIZE,
						&dwRecvNumBytes,
						&lpNPipeIOContext->Overlapped);
					if (!bRet && (ERROR_IO_PENDING != GetLastError()))
					{
						msprintf(_T("[ERROR] ClientIo_NPipe2PAgeant_Read: ReadFile(\"%s\") failed: %d\n"),
								 lpNPipeContext->pPipeNameShort, GetLastError());
						if (!ReconnectNPipeAndCancelIOLinked(lpNPipeContext))
						{
							WSASetEvent(g_hCleanupEvent[0]);
							return (0);
						}
					}
					else if (g_bVerbose)
					{
						msprintf(_T("WorkerThread %d: ClientIo_NPipe2PAgeant_Read: ReadFile(\"%s\") completed (%d bytes), %d more bytes are needed to fill the PAgeantMSG, ReadFile(\"%s\") posted\n"),
								 dwCurrentThreadId, lpNPipeContext->pPipeNameShort, dwIoSize,
								 dwMsgRemain - dwIoSize, lpNPipeContext->pPipeNameShort);
					}
				} // dwMsgRemain > dwIoSize
				else // dwMsgRemain <= dwIoSize
				{
					memcpy_s(
						(char *)lpPAgeantConnectionContext->lpSharedMem + lpPAgeantConnectionContext->dwMsgSent,
						PAGEANT_MAX_MSG_LEN - lpPAgeantConnectionContext->dwMsgSent,
						lpNPipeIOContext->Buffer, dwMsgRemain);
					// lpPAgeantConnectionContext->dwMsgSent = lpPAgeantConnectionContext->dwMsgTotal;
					lpNPipeIOContext->nSentBytes = dwMsgRemain;

					// lpPAgeantConnectionContext->lpSharedMem is filled
					// Send2PAgeant
					// if (!SendMessage(lpPAgeantConnectionContext->hwndPAgeant, WM_COPYDATA, 0, (LPARAM)(&lpPAgeantConnectionContext->cds)))
					if (!SendMessageTimeout(lpPAgeantConnectionContext->hwndPAgeant, WM_COPYDATA, 0, (LPARAM)(&lpPAgeantConnectionContext->cds), SMTO_BLOCK, 0, NULL))
					{
						msprintf(_T("[ERROR] ClientIo_NPipe2PAgeant_Read: SendMessage(0x%p,WM_COPYDATA) failed: %d\n"), lpPAgeantConnectionContext->hwndPAgeant, GetLastError());
						if (!ReconnectNPipeAndCancelIOLinked(lpNPipeContext))
						{
							WSASetEvent(g_hCleanupEvent[0]);
							return (0);
						}
						break;
					}
					dwIoSizeResp = ntohl(*(u_long *)lpPAgeantConnectionContext->lpSharedMem) + sizeof(DWORD);
					if (dwIoSizeResp == sizeof(DWORD))
					{
						if (g_bVerbose)
						{
							msprintf(_T("WorkerThread %d: ClientIo_NPipe2PAgeant_Read: ReadFile(\"%s\") completed (%d bytes), PAgeant request completed (4 bytes)\n"),
									 dwCurrentThreadId, lpNPipeContext->pPipeNameShort, dwIoSize);
						}
						if (!ReconnectNPipeAndCancelIOLinked(lpNPipeContext))
						{
							WSASetEvent(g_hCleanupEvent[0]);
							return (0);
						}
						break;
					}
					else if (dwIoSizeResp > PAGEANT_MAX_MSG_LEN)
					{
						msprintf(_T("[ERROR] WorkerThread %d: ClientIo_NPipe2PAgeant_Read: PAgeant reply for NPipe(\"%s\") is too large (%d bytes)\n"),
								 dwCurrentThreadId, lpNPipeContext->pPipeNameShort, dwIoSizeResp);
						if (!ReconnectNPipeAndCancelIOLinked(lpNPipeContext))
						{
							WSASetEvent(g_hCleanupEvent[0]);
							return (0);
						}
						break;
					}

					// WriteFile(lpNPipeContext, lpNPipeIOContext, buf=lpPAgeantConnectionContext->lpSharedMem)
					lpNPipeIOContext->IOOperation = ClientIo_NPipe2PAgeant_Write;
					lpPAgeantConnectionContext->dwMsgTotal = dwIoSizeResp;
					lpPAgeantConnectionContext->dwMsgSent = 0;
					bRet = WriteFile(
						lpNPipeContext->hNPipe,
						lpPAgeantConnectionContext->lpSharedMem,
						dwIoSizeResp,
						&dwSendNumBytes,
						&lpNPipeIOContext->Overlapped);
					if (!bRet && (ERROR_IO_PENDING != GetLastError()))
					{
						msprintf(_T("[ERROR] ClientIo_NPipe2PAgeant_Read: WriteFile(\"%s\") failed: %d\n"),
							lpNPipeContext->pPipeNameShort, GetLastError());
						if (!ReconnectNPipeAndCancelIOLinked(lpNPipeContext))
						{
							WSASetEvent(g_hCleanupEvent[0]);
							return (0);
						}
						break;
					}
					else if (g_bVerbose)
					{
						msprintf(_T("WorkerThread %d: ClientIo_NPipe2PAgeant_Read: ReadFile(\"%s\") completed (%d bytes), PAgeant request completed (%d bytes), WriteFile(\"%s\") posted\n"),
								 dwCurrentThreadId, lpNPipeContext->pPipeNameShort,
								 dwIoSize, dwIoSizeResp, lpNPipeContext->pPipeNameShort);
					}
				} // dwMsgRemain <= dwIoSize
				break;

			case ClientIo_NPipe2PAgeant_Write:
				lpPAgeantConnectionContext = (PCONTEXT_PAGEANT)lpNPipeContext->pCtxtLinked;
				// a write operation has completed, determine if all the data intended to be
				// sent actually was sent.
				lpPAgeantConnectionContext->dwMsgSent += dwIoSize;
				if (lpPAgeantConnectionContext->dwMsgSent < lpPAgeantConnectionContext->dwMsgTotal)
				{
					// the previous write operation didn't send all the data,
					// post another write to complete the operation
					// was WriteFile(lpNPipeContext, lpNPipeIOContext, buf=lpPAgeantConnectionContext->lpSharedMem)
					// lpNPipeIOContext == lpNPipeContext->pIOContext
					// now WriteMore(lpNPipeContext, lpNPipeIOContext, buf=lpPAgeantConnectionContext->lpSharedMem)
					//
					lpNPipeIOContext->IOOperation = ClientIo_NPipe2PAgeant_Write;
					bRet = WriteFile(
						lpNPipeContext->hNPipe,
						(char *)lpPAgeantConnectionContext->lpSharedMem + lpPAgeantConnectionContext->dwMsgSent,
						lpPAgeantConnectionContext->dwMsgTotal - lpPAgeantConnectionContext->dwMsgSent,
						&dwSendNumBytes,
						&lpNPipeIOContext->Overlapped);
					if (!bRet && (ERROR_IO_PENDING != GetLastError()))
					{
						msprintf(_T("[ERROR] ClientIo_NPipe2PAgeant_Write: WriteFile(\"%s\") failed: %d\n"),
							lpNPipeContext->pPipeNameShort, GetLastError());
						if (!ReconnectNPipeAndCancelIOLinked(lpNPipeContext))
						{
							WSASetEvent(g_hCleanupEvent[0]);
							return (0);
						}
						break;
					}
					else if (g_bVerbose)
					{
						msprintf(_T("WorkerThread %d: ClientIo_NPipe2PAgeant_Write: WriteFile(\"%s\") partially completed (%d bytes), WriteFile(\"%s\") posted\n"),
								 dwCurrentThreadId, lpNPipeContext->pPipeNameShort, dwIoSize, lpNPipeContext->pPipeNameShort);
					}
					break;
				}
				// Previous write operation completed.
				// was WriteFile(lpNPipeContext, lpNPipeIOContext, buf=lpPAgeantConnectionContext->lpSharedMem)
				// lpNPipeIOContext == lpNPipeContext->pIOContext
				lpPAgeantConnectionContext->dwMsgTotal = 0;
				lpPAgeantConnectionContext->dwMsgSent = 0;

				if (lpNPipeIOContext->nSentBytes == lpNPipeIOContext->nTotalBytes)
				{
					// lpNPipeIOContext->Buffer ended
					// ReadFile(lpNPipeContext, lpNPipeIOContext, buf=lpNPipeIOContext->Buffer)
					lpNPipeIOContext->IOOperation = ClientIo_NPipe2PAgeant_Read;
					dwRecvNumBytes = 0;
					bRet = ReadFile(
						lpNPipeContext->hNPipe,
						lpNPipeIOContext->Buffer,
						MAX_NPIPE_BUFF_SIZE,
						&dwRecvNumBytes,
						&lpNPipeIOContext->Overlapped);
					if (!bRet && (ERROR_IO_PENDING != GetLastError()))
					{
						msprintf(_T("[ERROR] ClientIo_NPipe2PAgeant_Write: ReadFile(\"%s\") failed: %d\n"),
								 lpNPipeContext->pPipeNameShort, GetLastError());
						if (!ReconnectNPipeAndCancelIOLinked(lpNPipeContext))
						{
							WSASetEvent(g_hCleanupEvent[0]);
							return (0);
						}
					}
					else if (g_bVerbose)
					{
						msprintf(_T("WorkerThread %d: ClientIo_NPipe2PAgeant_Write: WriteFile(\"%s\") completed (%d bytes), start new PAgeantMSG, ReadFile(\"%s\") posted\n"),
								 dwCurrentThreadId, lpNPipeContext->pPipeNameShort,
								 dwIoSize, lpNPipeContext->pPipeNameShort);
					}
					break;
				}

				// lpNPipeIOContext->nSentBytes < lpNPipeIOContext->nTotalBytes
				dwSrcRemain = lpNPipeIOContext->nTotalBytes - lpNPipeIOContext->nSentBytes;
				if (lpNPipeIOContext->nTotalBytes - lpNPipeIOContext->nSentBytes < 4)
				{
					msprintf(_T("[ERROR] ClientIo_NPipe2PAgeant_Write: we have to start new PAgeantMSG but src buffer length < 4 bytes\n"));
					if (!ReconnectNPipeAndCancelIOLinked(lpNPipeContext))
					{
						WSASetEvent(g_hCleanupEvent[0]);
						return (0);
					}
					break;
				}
				// This is first link in message chain
				lpPAgeantConnectionContext->dwMsgTotal = ntohl(*(u_long *)(lpNPipeIOContext->Buffer + lpNPipeIOContext->nSentBytes)) + sizeof(DWORD);
				if (lpPAgeantConnectionContext->dwMsgTotal == 0 || lpPAgeantConnectionContext->dwMsgTotal > PAGEANT_MAX_MSG_LEN)
				{
					msprintf(_T("[ERROR] WorkerThread %d: ClientIo_NPipe2PAgeant_Write: PAgeant query for NPipe(\"%s\") too large or have zero size: %d bytes\n"),
							 dwCurrentThreadId, lpNPipeContext->pPipeNameShort,
							 lpPAgeantConnectionContext->dwMsgTotal);
					if (!ReconnectNPipeAndCancelIOLinked(lpNPipeContext))
					{
						WSASetEvent(g_hCleanupEvent[0]);
						return (0);
					}
					break;
				}
				lpPAgeantConnectionContext->dwMsgSent = 0;

				dwMsgRemain = lpPAgeantConnectionContext->dwMsgTotal;
				if (dwMsgRemain > dwSrcRemain)
				{
					memcpy_s(
						lpPAgeantConnectionContext->lpSharedMem,
						PAGEANT_MAX_MSG_LEN,
						lpNPipeIOContext->Buffer + lpNPipeIOContext->nSentBytes,
						dwSrcRemain);
					lpPAgeantConnectionContext->dwMsgSent += dwSrcRemain;

					// lpNPipeIOContext->Buffer ended
					// ReadFile(lpNPipeContext, lpNPipeIOContext, buf=lpNPipeIOContext->Buffer)
					lpNPipeIOContext->IOOperation = ClientIo_NPipe2PAgeant_Read;
					dwRecvNumBytes = 0;
					bRet = ReadFile(
						lpNPipeContext->hNPipe,
						lpNPipeIOContext->Buffer,
						MAX_NPIPE_BUFF_SIZE,
						&dwRecvNumBytes,
						&lpNPipeIOContext->Overlapped);
					if (!bRet && (ERROR_IO_PENDING != GetLastError()))
					{
						msprintf(_T("[ERROR] ClientIo_NPipe2PAgeant_Write: ReadFile(\"%s\") failed: %d\n"),
								 lpNPipeContext->pPipeNameShort, GetLastError());
						if (!ReconnectNPipeAndCancelIOLinked(lpNPipeContext))
						{
							WSASetEvent(g_hCleanupEvent[0]);
							return (0);
						}
					}
					else if (g_bVerbose)
					{
						msprintf(_T("WorkerThread %d: ClientIo_NPipe2PAgeant_Write: WriteFile(\"%s\") completed (%d bytes), %d more bytes are needed to fill the PAgeantMSG, ReadFile(\"%s\") posted\n"),
								 dwCurrentThreadId, lpNPipeContext->pPipeNameShort, dwIoSize,
								 dwMsgRemain - dwSrcRemain, lpNPipeContext->pPipeNameShort);
					}
				} // dwMsgRemain > dwSrcRemain
				else // dwMsgRemain <= dwSrcRemain
				{
					memcpy_s(
						lpPAgeantConnectionContext->lpSharedMem,
						PAGEANT_MAX_MSG_LEN,
						lpNPipeIOContext->Buffer + lpNPipeIOContext->nSentBytes,
						dwMsgRemain);
					// lpPAgeantConnectionContext->dwMsgSent = lpPAgeantConnectionContext->dwMsgTotal;
					lpNPipeIOContext->nSentBytes += dwMsgRemain;

					// lpPAgeantConnectionContext->lpSharedMem is filled
					// Send2PAgeant
					// if (!SendMessage(lpPAgeantConnectionContext->hwndPAgeant, WM_COPYDATA, 0, (LPARAM)(&lpPAgeantConnectionContext->cds)))
					if (!SendMessageTimeout(lpPAgeantConnectionContext->hwndPAgeant, WM_COPYDATA, 0, (LPARAM)(&lpPAgeantConnectionContext->cds), SMTO_BLOCK, 0, NULL))
					{
						msprintf(_T("[ERROR] ClientIo_NPipe2PAgeant_Write: SendMessage(0x%p,WM_COPYDATA) failed: %d\n"),
							lpPAgeantConnectionContext->hwndPAgeant, GetLastError());
						if (!ReconnectNPipeAndCancelIOLinked(lpNPipeContext))
						{
							WSASetEvent(g_hCleanupEvent[0]);
							return (0);
						}
						break;
					}
					dwIoSizeResp = ntohl(*(u_long *)lpPAgeantConnectionContext->lpSharedMem) + sizeof(DWORD);
					if (dwIoSizeResp == sizeof(DWORD))
					{
						if (g_bVerbose)
						{
							msprintf(_T("WorkerThread %d: ClientIo_NPipe2PAgeant_Write: WriteFile(\"%s\") completed (%d bytes), PAgeant request completed (4 bytes)\n"),
									 dwCurrentThreadId, lpNPipeContext->pPipeNameShort, dwIoSize);
						}
						if (!ReconnectNPipeAndCancelIOLinked(lpNPipeContext))
						{
							WSASetEvent(g_hCleanupEvent[0]);
							return (0);
						}
						break;
					}
					else if (dwIoSizeResp > PAGEANT_MAX_MSG_LEN)
					{
						msprintf(_T("[ERROR] WorkerThread %d: ClientIo_NPipe2PAgeant_Write: PAgeant reply for NPipe(\"%s\") is too large (%d bytes)\n"),
								 dwCurrentThreadId, lpNPipeContext->pPipeNameShort, dwIoSizeResp);
						if (!ReconnectNPipeAndCancelIOLinked(lpNPipeContext))
						{
							WSASetEvent(g_hCleanupEvent[0]);
							return (0);
						}
						break;
					}

					// WriteFile(lpNPipeContext, lpNPipeIOContext, buf=lpPAgeantConnectionContext->lpSharedMem)
					lpNPipeIOContext->IOOperation = ClientIo_NPipe2PAgeant_Write;
					lpPAgeantConnectionContext->dwMsgTotal = dwIoSizeResp;
					lpPAgeantConnectionContext->dwMsgSent = 0;
					bRet = WriteFile(
						lpNPipeContext->hNPipe,
						lpPAgeantConnectionContext->lpSharedMem,
						dwIoSizeResp,
						&dwSendNumBytes,
						&lpNPipeIOContext->Overlapped);
					if (!bRet && (ERROR_IO_PENDING != GetLastError()))
					{
						msprintf(_T("[ERROR] ClientIo_NPipe2PAgeant_Write: WriteFile(\"%s\") failed: %d\n"),
							lpNPipeContext->pPipeNameShort, GetLastError());
						if (!ReconnectNPipeAndCancelIOLinked(lpNPipeContext))
						{
							WSASetEvent(g_hCleanupEvent[0]);
							return (0);
						}
						break;
					}
					else if (g_bVerbose)
					{
						msprintf(_T("WorkerThread %d: ClientIo_NPipe2PAgeant_Write: WriteFile(\"%s\") completed (%d bytes), PAgeant request completed (%d bytes), WriteFile(\"%s\") posted\n"),
								 dwCurrentThreadId, lpNPipeContext->pPipeNameShort,
								 dwIoSize, dwIoSizeResp, lpNPipeContext->pPipeNameShort);
					}
				} // dwMsgRemain <= dwSrcRemain
				break;

			default:
				break;
			} // switch (lpNPipeIOContext->IOOperation)
			break;
		} // case ConnectionTypeNPipe

		default:
			break;
		} // switch (*(PCONNECTIONTYPE)lpCompletionKey)
	} // while
	return (0);
}

//
//  Allocate a context structures for the socket and add the socket to the IOCP.
//  Additionally, add the context structure to the global list of context structures.
//
PCONTEXT_SOCKET UpdateCompletionPort(SOCKET sd, IO_OPERATION ClientIo,
										 BOOL bAddToList)
{
	PCONTEXT_SOCKET lpPerSocketContext;

	lpPerSocketContext = CtxtAllocate(sd, ClientIo);
	if (lpPerSocketContext == NULL)
		return (NULL);

	// msprintf(_T("UpdateCompletionPort: lpPerSocketContext=0x%p, Socket(%d)\n"), lpPerSocketContext, sd);

	g_hIOCP = CreateIoCompletionPort((HANDLE)sd, g_hIOCP, (DWORD_PTR)lpPerSocketContext, 0);
	if (g_hIOCP == NULL)
	{
		msprintf(_T("[ERROR] CreateIoCompletionPort() failed: %d\n"), GetLastError());
		if (lpPerSocketContext->pIOContext)
			xfree(lpPerSocketContext->pIOContext);
		xfree(lpPerSocketContext);
		return (NULL);
	}

	//
	// The listening socket context (bAddToList is FALSE) is not added to the list.
	// All other socket contexts are added to the list.
	//
	if (bAddToList)
		CtxtListAddTo(lpPerSocketContext);

	if (g_bVerbose)
		msprintf(_T("UpdateCompletionPort: Socket(%d) added to IOCP\n"), lpPerSocketContext->Socket);

	return (lpPerSocketContext);
}

VOID CloseClientAndLinked(PCONTEXT_SOCKET lpPerSocketContext, BOOL bGraceful)
{
	if (CancelIoEx((HANDLE)lpPerSocketContext->Socket, NULL) != 0 && g_bVerbose)
		msprintf(_T("CloseClientAndLinked: CancelIoEx(Socket(%d)) succeeds\n"), lpPerSocketContext->Socket);

	if (lpPerSocketContext->pCtxtLinked && lpPerSocketContext->ctConnectionTypeLinked == ConnectionTypeSocketGPG)
	{
		if (CancelIoEx((HANDLE)((PCONTEXT_SOCKET)lpPerSocketContext->pCtxtLinked)->Socket, NULL) != 0 && g_bVerbose)
			msprintf(_T("CloseClientAndLinked: CancelIoEx(Socket(%d)) succeeds\n"), ((PCONTEXT_SOCKET)lpPerSocketContext->pCtxtLinked)->Socket);

		CloseClient((PCONTEXT_SOCKET)lpPerSocketContext->pCtxtLinked, bGraceful);
	}
	CloseClient(lpPerSocketContext, bGraceful);
}

VOID CancelIOLinkedAndCloseClient(PCONTEXT_SOCKET lpPerSocketContext, BOOL bGraceful)
{
	if (CancelIoEx((HANDLE)lpPerSocketContext->Socket, NULL) != 0 && g_bVerbose)
		msprintf(_T("CancelIOLinkedAndCloseClient: CancelIoEx(Socket(%d)) succeeds\n"), lpPerSocketContext->Socket);
	if (lpPerSocketContext->pCtxtLinked && lpPerSocketContext->ctConnectionTypeLinked == ConnectionTypeSocketGPG)
	{
		if (CancelIoEx((HANDLE)((PCONTEXT_SOCKET)lpPerSocketContext->pCtxtLinked)->Socket, NULL) != 0 && g_bVerbose)
			msprintf(_T("CancelIOLinkedAndCloseClient: CancelIoEx(Socket(%d)) succeeds\n"), ((PCONTEXT_SOCKET)lpPerSocketContext->pCtxtLinked)->Socket);
	}
	CloseClient(lpPerSocketContext, bGraceful);
}

//
//  Close down a connection with a client.  This involves closing the socket (when
//  initiated as a result of a CTRL-C the socket closure is not graceful).  Additionally,
//  any context data associated with that socket is free'd.
//
VOID CloseClient(PCONTEXT_SOCKET lpPerSocketContext, BOOL bGraceful)
{
	__try
	{
		EnterCriticalSection(&g_CriticalSection);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		msprintf(_T("[ERROR] EnterCriticalSection raised an exception.\n"));
		return;
	}

	if (lpPerSocketContext)
	{
		// if (g_bVerbose)
		// 	msprintf(_T("CloseClient: Socket(%d) connection closing (graceful=%s)\n"),
		// 			 lpPerSocketContext->Socket, (bGraceful ? _T("TRUE") : _T("FALSE")));
		if (!bGraceful)
		{
			//
			// force the subsequent closesocket to be abortative.
			//
			LINGER lingerStruct;

			lingerStruct.l_onoff = 1;
			lingerStruct.l_linger = 0;
			setsockopt(lpPerSocketContext->Socket, SOL_SOCKET, SO_LINGER,
					   (char *)&lingerStruct, sizeof(lingerStruct));
		}
		if (lpPerSocketContext->pIOContext->SocketAccept != INVALID_SOCKET)
		{
			if (g_bVerbose)
				msprintf(_T("CloseClient: closesocket(Socket(%d))\n"), lpPerSocketContext->pIOContext->SocketAccept);
			closesocket(lpPerSocketContext->pIOContext->SocketAccept);
			lpPerSocketContext->pIOContext->SocketAccept = INVALID_SOCKET;
		};

		SOCKET sdTemp = lpPerSocketContext->Socket;
		// lpPerSocketContext->Socket = INVALID_SOCKET;
		if (lpPerSocketContext->pCtxtLinked)
		{
			switch (lpPerSocketContext->ctConnectionTypeLinked)
			{
			case ConnectionTypeSocketGPG:
				((PCONTEXT_SOCKET)lpPerSocketContext->pCtxtLinked)->pCtxtLinked = NULL;
				break;

			case ConnectionTypePAgeant:
				ClosePAgeantCtxt((PCONTEXT_PAGEANT)lpPerSocketContext->pCtxtLinked);
				if (g_bVerbose)
					msprintf(_T("CloseClient: PAgeant context for Socket(%d) was deleted\n"), lpPerSocketContext->Socket);
				break;

			default:
				break;
			}
			lpPerSocketContext->pCtxtLinked = NULL;
		}
		CtxtListDeleteFrom(lpPerSocketContext);
		lpPerSocketContext = NULL;
		if (g_bVerbose)
			msprintf(_T("CloseClient: closesocket(Socket(%d))\n"), sdTemp);
		closesocket(sdTemp);
	}
	else
	{
		msprintf(_T("[ERROR] CloseClient: lpPerSocketContext is NULL\n"));
	}

	LeaveCriticalSection(&g_CriticalSection);

	return;
}

VOID CloseNPipeCtxt(
	PCONTEXT_NPIPE pCtxtNPipe)
{
	if (pCtxtNPipe == NULL)
	{
		msprintf(_T("[ERROR] CloseNPipeCtxt: pCtxtNPipe is NULL\n"));
		return;
	}

	__try
	{
		EnterCriticalSection(&g_CriticalSection);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		msprintf(_T("[ERROR] CloseNPipeCtxt: EnterCriticalSection raised an exception.\n"));
		return;
	}

	DisconnectNPipeAndCancelIOLinked(pCtxtNPipe);

	HANDLE hTmp = pCtxtNPipe->hNPipe;
	CtxtFreeNPipe(pCtxtNPipe);
	pCtxtNPipe = NULL;
	CloseHandle(hTmp);

	LeaveCriticalSection(&g_CriticalSection);
}

//
// Allocate a socket context for the new connection.
//
PCONTEXT_SOCKET CtxtAllocate(SOCKET sd, IO_OPERATION ClientIO)
{
	PCONTEXT_SOCKET lpPerSocketContext;

	__try
	{
		EnterCriticalSection(&g_CriticalSection);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		msprintf(_T("[ERROR] EnterCriticalSection raised an exception.\n"));
		return NULL;
	}

	lpPerSocketContext = (PCONTEXT_SOCKET)xmalloc(sizeof(CONTEXT_SOCKET));
	if (lpPerSocketContext)
	{
		lpPerSocketContext->pIOContext = (PIOCONTEXT_SOCKET)xmalloc(sizeof(IOCONTEXT_SOCKET));
		if (lpPerSocketContext->pIOContext)
		{
			lpPerSocketContext->ctConnectionType = ConnectionTypeSocket;
			lpPerSocketContext->Socket = sd;
			lpPerSocketContext->ctConnectionTypeLinked = ConnectionTypeInvalid;
			lpPerSocketContext->pCtxtLinked = NULL;
			lpPerSocketContext->pCtxtBack = NULL;
			lpPerSocketContext->pCtxtForward = NULL;

			lpPerSocketContext->pIOContext->Overlapped.Internal = 0;
			lpPerSocketContext->pIOContext->Overlapped.InternalHigh = 0;
			lpPerSocketContext->pIOContext->Overlapped.Offset = 0;
			lpPerSocketContext->pIOContext->Overlapped.OffsetHigh = 0;
			lpPerSocketContext->pIOContext->Overlapped.hEvent = NULL;
			lpPerSocketContext->pIOContext->IOOperation = ClientIO;
			lpPerSocketContext->pIOContext->pIOContextForward = NULL;
			lpPerSocketContext->pIOContext->nTotalBytes = 0;
			lpPerSocketContext->pIOContext->nSentBytes = 0;
			lpPerSocketContext->pIOContext->wsabuf.buf = lpPerSocketContext->pIOContext->Buffer;
			lpPerSocketContext->pIOContext->wsabuf.len = sizeof(lpPerSocketContext->pIOContext->Buffer);
			lpPerSocketContext->pIOContext->SocketAccept = INVALID_SOCKET;

			ZeroMemory(lpPerSocketContext->pIOContext->wsabuf.buf, lpPerSocketContext->pIOContext->wsabuf.len);
		}
		else
		{
			xfree(lpPerSocketContext);
			msprintf(_T("[ERROR] HeapAlloc() IOCONTEXT_SOCKET failed: %d\n"), GetLastError());
			LeaveCriticalSection(&g_CriticalSection);
			return (NULL);
		}
	}
	else
	{
		msprintf(_T("[ERROR] HeapAlloc() CONTEXT_SOCKET failed: %d\n"), GetLastError());
		LeaveCriticalSection(&g_CriticalSection);
		return (NULL);
	}

	LeaveCriticalSection(&g_CriticalSection);

	return (lpPerSocketContext);
}

PCONTEXT_NPIPE CtxtAllocateNPipe(IO_OPERATION ClientIO)
{
	PCONTEXT_NPIPE lpPerNPipeContext;

	__try
	{
		EnterCriticalSection(&g_CriticalSection);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		msprintf(_T("[ERROR] EnterCriticalSection raised an exception.\n"));
		return NULL;
	}

	lpPerNPipeContext = (PCONTEXT_NPIPE)xmalloc(sizeof(CONTEXT_NPIPE));
	if (lpPerNPipeContext)
	{
		lpPerNPipeContext->pIOContext = (PIOCONTEXT_NPIPE)xmalloc(sizeof(IOCONTEXT_NPIPE));
		if (lpPerNPipeContext->pIOContext)
		{
			lpPerNPipeContext->ctConnectionType = ConnectionTypeNPipe;
			lpPerNPipeContext->hNPipe = INVALID_HANDLE_VALUE;
			lpPerNPipeContext->ctConnectionTypeLinked = ConnectionTypeInvalid;
			lpPerNPipeContext->pCtxtLinked = NULL;
			lpPerNPipeContext->sPipeName[0] = _T('\0');
			lpPerNPipeContext->pPipeNameShort = lpPerNPipeContext->sPipeName;

			lpPerNPipeContext->pIOContext->Overlapped.Internal = 0;
			lpPerNPipeContext->pIOContext->Overlapped.InternalHigh = 0;
			lpPerNPipeContext->pIOContext->Overlapped.Offset = 0;
			lpPerNPipeContext->pIOContext->Overlapped.OffsetHigh = 0;
			lpPerNPipeContext->pIOContext->Overlapped.hEvent = NULL;
			lpPerNPipeContext->pIOContext->IOOperation = ClientIO;
			lpPerNPipeContext->pIOContext->nTotalBytes = 0;
			lpPerNPipeContext->pIOContext->nSentBytes = 0;
			lpPerNPipeContext->pIOContext->dwClientPID = 0;

			ZeroMemory(lpPerNPipeContext->pIOContext->Buffer, sizeof(lpPerNPipeContext->pIOContext->Buffer));
		}
		else
		{
			xfree(lpPerNPipeContext);
			msprintf(_T("[ERROR] HeapAlloc() IOCONTEXT_NPIPE failed: %d\n"), GetLastError());
			LeaveCriticalSection(&g_CriticalSection);
			return (NULL);
		}
	}
	else
	{
		msprintf(_T("[ERROR] HeapAlloc() CONTEXT_NPIPE failed: %d\n"), GetLastError());
		LeaveCriticalSection(&g_CriticalSection);
		return (NULL);
	}

	LeaveCriticalSection(&g_CriticalSection);

	return (lpPerNPipeContext);
}

VOID CtxtFreeNPipe(
    PCONTEXT_NPIPE pCtxtNPipe)
{
	__try
	{
		EnterCriticalSection(&g_CriticalSection);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		msprintf(_T("[ERROR] CtxtFreeNPipe: EnterCriticalSection raised an exception.\n"));
		return;
	}

	if (pCtxtNPipe == NULL)
	{
		msprintf(_T("[ERROR] CtxtFreeNPipe: pCtxtNPipe == NULL.\n"));
		return;
	}
	if (pCtxtNPipe->pIOContext != NULL)
	{
		if (CancelIoEx(pCtxtNPipe->hNPipe, NULL) != 0 && g_bVerbose)
		{
			msprintf(_T("CtxtFreeNPipe: CancelIoEx(NPipe(\"%s\")) succeeds\n"),
				pCtxtNPipe->pPipeNameShort);
		}
		xfree(pCtxtNPipe->pIOContext);
	}
	if (g_bVerbose)
	{
		msprintf(_T("CtxtFreeNPipe: NPipe(\"%s\") will be closed\n"),
			pCtxtNPipe->pPipeNameShort);
	}
	xfree(pCtxtNPipe);

	LeaveCriticalSection(&g_CriticalSection);
}

//
//  Add a client connection context structure to the global list of context structures.
//
VOID CtxtListAddTo(PCONTEXT_SOCKET lpPerSocketContext)
{
	PCONTEXT_SOCKET pTemp;

	__try
	{
		EnterCriticalSection(&g_CriticalSection);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		msprintf(_T("[ERROR] EnterCriticalSection raised an exception.\n"));
		return;
	}

	if (g_pCtxtList == NULL)
	{
		//
		// add the first node to the linked list
		//
		lpPerSocketContext->pCtxtBack = NULL;
		lpPerSocketContext->pCtxtForward = NULL;
		g_pCtxtList = lpPerSocketContext;
	}
	else
	{
		//
		// add node to head of list
		//
		pTemp = g_pCtxtList;

		g_pCtxtList = lpPerSocketContext;
		lpPerSocketContext->pCtxtBack = pTemp;
		lpPerSocketContext->pCtxtForward = NULL;

		pTemp->pCtxtForward = lpPerSocketContext;
	}

	LeaveCriticalSection(&g_CriticalSection);

	return;
}

//
//  Remove a client context structure from the global list of context structures.
//
VOID CtxtListDeleteFrom(PCONTEXT_SOCKET lpPerSocketContext)
{
	PCONTEXT_SOCKET pBack;
	PCONTEXT_SOCKET pForward;
	PIOCONTEXT_SOCKET pNextIO = NULL;
	PIOCONTEXT_SOCKET pTempIO = NULL;

	__try
	{
		EnterCriticalSection(&g_CriticalSection);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		msprintf(_T("[ERROR] EnterCriticalSection raised an exception.\n"));
		return;
	}

	if (lpPerSocketContext)
	{
		pBack = lpPerSocketContext->pCtxtBack;
		pForward = lpPerSocketContext->pCtxtForward;

		if (pBack == NULL && pForward == NULL)
		{
			//
			// This is the only node in the list to delete
			//
			g_pCtxtList = NULL;
		}
		else if (pBack == NULL && pForward != NULL)
		{
			//
			// This is the start node in the list to delete
			//
			pForward->pCtxtBack = NULL;
			g_pCtxtList = pForward;
		}
		else if (pBack != NULL && pForward == NULL)
		{
			//
			// This is the end node in the list to delete
			//
			pBack->pCtxtForward = NULL;
		}
		else if (pBack && pForward)
		{
			//
			// Neither start node nor end node in the list
			//
			pBack->pCtxtForward = pForward;
			pForward->pCtxtBack = pBack;
		}

		//
		// Free all i/o context structures per socket
		//
		pTempIO = (PIOCONTEXT_SOCKET)(lpPerSocketContext->pIOContext);
		do
		{
			pNextIO = (PIOCONTEXT_SOCKET)(pTempIO->pIOContextForward);
			if (pTempIO)
			{
				//
				// The overlapped structure is safe to free when only the posted i/o has
				// completed. Here we only need to test those posted but not yet received
				// by PQCS in the shutdown process.
				//
				if (g_bEndServer)
				{
					if (CancelIoEx((HANDLE)lpPerSocketContext->Socket, (LPOVERLAPPED)pTempIO) == 0)
					{
						// msprintf(_T("CtxtListDeleteFrom: CancelIoEx(Socket(%d)) failed: %d\n"), lpPerSocketContext->Socket, GetLastError());
						while (!HasOverlappedIoCompleted((LPOVERLAPPED)pTempIO))
							Sleep(0);
					}
					else if (g_bVerbose)
						msprintf(_T("CtxtListDeleteFrom: CancelIoEx(Socket(%d)) succeeds\n"), lpPerSocketContext->Socket);
				}
				xfree(pTempIO);
				pTempIO = NULL;
			}
			pTempIO = pNextIO;
		} while (pNextIO);

		xfree(lpPerSocketContext);
		lpPerSocketContext = NULL;
	}
	else
	{
		msprintf(_T("[ERROR] CtxtListDeleteFrom: lpPerSocketContext is NULL\n"));
	}

	LeaveCriticalSection(&g_CriticalSection);

	return;
}

//
//  Free all context structure in the global list of context structures.
//
VOID CtxtListFree()
{
	PCONTEXT_SOCKET pTemp1, pTemp2;

	__try
	{
		EnterCriticalSection(&g_CriticalSection);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		msprintf(_T("[ERROR] EnterCriticalSection raised an exception.\n"));
		return;
	}

	pTemp1 = g_pCtxtList;
	while (pTemp1)
	{
		pTemp2 = pTemp1->pCtxtBack;
		CloseClient(pTemp1, FALSE);
		pTemp1 = pTemp2;
	}

	LeaveCriticalSection(&g_CriticalSection);

	return;
}

PPORT2GPGSOCKET GetPort2gpgsocket(
    const SOCKET sdListen,
    PORT2GPGSOCKET (&port2gpgsocket)[MAX_LISTENING_PORTS])
{
	for (PPORT2GPGSOCKET ptmp = port2gpgsocket; ptmp < port2gpgsocket + MAX_LISTENING_PORTS; ++ptmp)
	{
		if (ptmp->sdListen == sdListen)
			return ptmp;
	}
	return NULL;
}

PCONTEXT_SOCKET CreateGPGSocket(
    const TCHAR* gpgsocket,
    const PCONTEXT_SOCKET lpCtxtLinkedSocket)
{
#define CreateGPGSocket_BUF_SIZE 256
	FILE* finp = NULL;
	errno_t err;
	PCONTEXT_SOCKET lpGPGSocketContext = NULL;
	char buf[CreateGPGSocket_BUF_SIZE];
	char *ptr;
	int nRet = 0;
	HRESULT hRet;
	sockaddr_in addrgpg;
	SOCKET sdGPG = INVALID_SOCKET;
	PIOCONTEXT_SOCKET lpIOContext = NULL;
	DWORD dwSendNumBytes = 0;

	err = _tfopen_s(&finp, gpgsocket, _T("rb"));
	if (finp == NULL || err != 0)
	{
		msprintf(_T("[ERROR] fopen(%s) failed: %d\n"), gpgsocket, err);
		return NULL;
	}
	const size_t bytesread = fread_s(buf, CreateGPGSocket_BUF_SIZE, 1, CreateGPGSocket_BUF_SIZE, finp);
	fclose(finp);

	for (ptr = buf; ptr < buf + bytesread; ++ptr)
		if (*ptr == '\n') break;
	if (ptr == buf + bytesread)
	{
		msprintf(_T("[ERROR] bad file \"%s\"\n"), gpgsocket);
		return NULL;
	}
	*ptr = '\0';
	// buf[bytesread] = '\0';
	const char* port = buf;
	const char* token = ptr + 1;
	const size_t token_size = bytesread - sizeof(token[0])*(token - buf);
	if (token_size > MAX_BUFF_SIZE)
	{
		// Лень мне по частям его отправлять. Но вообще token_size не должен быть большим.
		msprintf(_T("[ERROR] GPGAuthorizationToken size (%d) > MAX_BUFF_SIZE (%d)\n"), token_size, MAX_BUFF_SIZE);
		return NULL;
	}

	addrgpg.sin_family = AF_INET;
	// addrgpg.sin_addr.s_addr = inet_addr("127.0.0.1");
	nRet = InetPton(AF_INET, _T("127.0.0.1"), &addrgpg.sin_addr);
	if (nRet == 0)
	{
		msprintf(_T("[ERROR] InetPton failed: bad address string\n"));
		return NULL;
	}
	else if (nRet == -1)
	{
		msprintf(_T("[ERROR] InetPton failed: %d\n"), WSAGetLastError());
		return NULL;
	}
	addrgpg.sin_port = htons(atoi(port));

	sdGPG = CreateSocket();
	if (sdGPG == INVALID_SOCKET)
		return NULL;

	// nRet = WSAConnect(sdGPG, (sockaddr*)&addrgpg, sizeof(addrgpg), NULL, NULL, NULL, NULL);
	nRet = connect(sdGPG, (sockaddr*)&addrgpg, sizeof(addrgpg));
	if (nRet == SOCKET_ERROR)
	{
		int wsale = WSAGetLastError();
		if (wsale != WSAEWOULDBLOCK)
		{
			msprintf(_T("[ERROR] WSAConnect failed: %d\n"), wsale); //WSAECONNREFUSED, WSAENETUNREACH, WSAETIMEDOUT
			closesocket(sdGPG);
			return NULL;
		}
		// With a nonblocking socket, the connection attempt cannot be completed immediately.
		// TODO: do someting with this!
		msprintf(_T("[PANIC] WSAConnect cannot be completed immediately\n"));
		closesocket(sdGPG);
		return NULL;
	}
	if (g_bVerbose)
	{
		TCHAR tmpaddrbuf[64];
		InetNtop(addrgpg.sin_family, &addrgpg.sin_addr, tmpaddrbuf, 64);
		msprintf(_T("CreateGPGSocket: Socket(%d) connected to %s:%d\n"), sdGPG, tmpaddrbuf, ntohs(addrgpg.sin_port));
	}

	lpGPGSocketContext = UpdateCompletionPort(sdGPG, ClientIo_Socket2SocketGPG_GPGAuthorization, TRUE);
	if (lpGPGSocketContext == NULL)
	{
		msprintf(_T("[ERROR] failed to update GPG socket to IOCP\n"));
		closesocket(sdGPG);
		return (NULL);
	}
	lpGPGSocketContext->pCtxtLinked = lpCtxtLinkedSocket;
	if (lpCtxtLinkedSocket != NULL)
		lpGPGSocketContext->ctConnectionTypeLinked = lpCtxtLinkedSocket->ctConnectionTypeLinked;

	lpIOContext = lpGPGSocketContext->pIOContext;
	lpIOContext->IOOperation = ClientIo_Socket2SocketGPG_GPGAuthorization;
	lpIOContext->nTotalBytes = token_size;
	lpIOContext->nSentBytes = 0;
	lpIOContext->wsabuf.len = token_size;
	hRet = StringCbCopyNA(lpIOContext->Buffer, MAX_BUFF_SIZE,
						  token, token_size);
	if (FAILED(hRet))
	{
		msprintf(_T("[ERROR] StringCbCopyNA() failed\n"));
		CloseClient(lpGPGSocketContext, FALSE);
		return NULL;
	}
	lpIOContext->wsabuf.buf = lpIOContext->Buffer;

	nRet = WSASend(
		lpGPGSocketContext->Socket,
		&lpIOContext->wsabuf, 1, &dwSendNumBytes,
		0, &(lpIOContext->Overlapped), NULL);
	if (nRet == SOCKET_ERROR && (ERROR_IO_PENDING != WSAGetLastError()))
	{
		msprintf(_T("[ERROR] WSASend() failed: %d\n"), WSAGetLastError());
		CloseClient(lpGPGSocketContext, FALSE);
		return NULL;
	}
	if (g_bVerbose)
	{
		msprintf(_T("CreateGPGSocket: Socket(%d) Send posted with GPGAuthorizationToken\n"),
				 lpGPGSocketContext->Socket);
	}

	return lpGPGSocketContext;
#undef CreateGPGSocket_BUF_SIZE
}

PCONTEXT_PAGEANT CreatePAgeantCtxt()
{
	PCONTEXT_PAGEANT lpPAgeantConnectionContext;
	DWORD dwFileMapNameLen = 0;

	__try
	{
		EnterCriticalSection(&g_CriticalSection);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		msprintf(_T("[ERROR] EnterCriticalSection raised an exception.\n"));
		return (NULL);
	}
	lpPAgeantConnectionContext = (PCONTEXT_PAGEANT)xmalloc(sizeof(CONTEXT_PAGEANT));
	if (lpPAgeantConnectionContext)
	{
		lpPAgeantConnectionContext->hwndPAgeant = NULL;
		lpPAgeantConnectionContext->hFileMap = INVALID_HANDLE_VALUE;
		lpPAgeantConnectionContext->lpSharedMem = NULL;
		// dwFileMapNameLen = _stprintf_s(lpPAgeantConnectionContext->sFileMapName, _T("forward_gpg_agent_%08X_%016llX"), GetCurrentProcessId(), g_counterFileMap);
		dwFileMapNameLen = sprintf_s(lpPAgeantConnectionContext->sFileMapName, "forward_gpg_agent_%08X_%016llX", GetCurrentProcessId(), g_counterFileMap);
		if (dwFileMapNameLen <= 0)
		{
			msprintf(_T("[ERROR] CreatePAgeantCtxt: sprintf_s: %d\n"), GetLastError());
			LeaveCriticalSection(&g_CriticalSection);
			xfree(lpPAgeantConnectionContext);
			return (NULL);
		}
		g_counterFileMap++;
	}
	else
	{
		msprintf(_T("[ERROR] HeapAlloc() CONTEXT_PAGEANT failed: %d\n"), GetLastError());
		LeaveCriticalSection(&g_CriticalSection);
		return (NULL);
	}
	LeaveCriticalSection(&g_CriticalSection);

	lpPAgeantConnectionContext->hwndPAgeant = FindWindow(_T("Pageant"), _T("Pageant"));
	if (lpPAgeantConnectionContext->hwndPAgeant == NULL)
	{
		msprintf(_T("[ERROR] CreatePAgeantCtxt: FindWindow(\"Pageant\"): %d\n"), GetLastError());
		xfree(lpPAgeantConnectionContext);
		return (NULL);
	}

	lpPAgeantConnectionContext->hFileMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, PAGEANT_SHAREDMEM_SIZE, lpPAgeantConnectionContext->sFileMapName);
	if (lpPAgeantConnectionContext->hFileMap == NULL)
	{
		// msprintf(_T("[ERROR] CreatePAgeantCtxt: CreateFileMapping(\"%s\"): %d\n"), lpPAgeantConnectionContext->sFileMapName, GetLastError());
		msprintfA("[ERROR] CreatePAgeantCtxt: CreateFileMapping(\"%s\"): %d\n", lpPAgeantConnectionContext->sFileMapName, GetLastError());
		xfree(lpPAgeantConnectionContext);
		return (NULL);
	}

	lpPAgeantConnectionContext->lpSharedMem = MapViewOfFile(lpPAgeantConnectionContext->hFileMap, FILE_MAP_ALL_ACCESS, 0, 0, 0);
	if (lpPAgeantConnectionContext->lpSharedMem == NULL)
	{
		msprintf(_T("[ERROR] CreatePAgeantCtxt: MapViewOfFile: %d\n"), GetLastError());
		CloseHandle(lpPAgeantConnectionContext->hFileMap);
		xfree(lpPAgeantConnectionContext);
		return (NULL);
	}
	lpPAgeantConnectionContext->dwMsgSent = 0;
	lpPAgeantConnectionContext->dwMsgTotal = 0;
	lpPAgeantConnectionContext->cds.dwData = PAGEANT_MSG_ID;
	lpPAgeantConnectionContext->cds.cbData = (dwFileMapNameLen + 1)*sizeof(lpPAgeantConnectionContext->sFileMapName[0]); // including null terminate symbol
	lpPAgeantConnectionContext->cds.lpData = lpPAgeantConnectionContext->sFileMapName;

	return lpPAgeantConnectionContext;
}

PCONTEXT_PAGEANT CreatePAgeantCtxt(
    PCONTEXT_SOCKET lpPerSocketContext)
{
	PCONTEXT_PAGEANT lpPAgeantConnectionContext;

	if (lpPerSocketContext->ctConnectionTypeLinked != ConnectionTypePAgeant)
	{
		msprintf(_T("[ERROR] CreatePAgeantCtxt: lpPerSocketContext->ctConnectionTypeLinked != ConnectionTypePAgeant\n"));
		return (NULL);
	}

	lpPAgeantConnectionContext = CreatePAgeantCtxt();
	if (lpPAgeantConnectionContext == NULL)
		return (NULL);

	if (g_bVerbose)
		msprintf(_T("CreatePAgeantCtxt: PAgeant context created for Socket(%d)\n"), lpPerSocketContext->Socket);

	lpPerSocketContext->pCtxtLinked = lpPAgeantConnectionContext;

	return lpPAgeantConnectionContext;
}

PCONTEXT_PAGEANT CreatePAgeantCtxt(
    PCONTEXT_NPIPE lpNPipeContext)
{
	PCONTEXT_PAGEANT lpPAgeantConnectionContext;

	if (lpNPipeContext->ctConnectionTypeLinked != ConnectionTypePAgeant)
	{
		msprintf(_T("[ERROR] CreatePAgeantCtxt: lpNPipeContext->ctConnectionType != ConnectionTypePAgeant\n"));
		return (NULL);
	}

	lpPAgeantConnectionContext = CreatePAgeantCtxt();
	if (lpPAgeantConnectionContext == NULL)
		return (NULL);

	if (g_bVerbose)
		msprintf(_T("CreatePAgeantCtxt: PAgeant context created for NPipe(\"%s\")\n"), lpNPipeContext->pPipeNameShort);

	lpNPipeContext->pCtxtLinked = lpPAgeantConnectionContext;

	return lpPAgeantConnectionContext;
}

BOOL ClosePAgeantCtxt(
    PCONTEXT_PAGEANT lpPAgeantConnectionContext)
{
	if (lpPAgeantConnectionContext != NULL)
	{
		if (lpPAgeantConnectionContext->lpSharedMem != NULL)
		{
			if (!UnmapViewOfFile(lpPAgeantConnectionContext->lpSharedMem))
				msprintf(_T("[ERROR] ClosePAgeantCtxt: UnmapViewOfFile: %d\n"), GetLastError());
		}
		if (lpPAgeantConnectionContext->hFileMap != INVALID_HANDLE_VALUE)
		{
			if (!CloseHandle(lpPAgeantConnectionContext->hFileMap))
				msprintf(_T("[ERROR] ClosePAgeantCtxt: CloseHandle: %d\n"), GetLastError());
		}
	}
	xfree(lpPAgeantConnectionContext);
	return (TRUE);
}

BOOL ConnectNPipe(
	PCONTEXT_NPIPE pCtxtNPipe)
{
	// At this moment there is no connection with pCtxtNPipe->hNPipe, but just in case
	if (CancelIoEx(pCtxtNPipe->hNPipe, &pCtxtNPipe->pIOContext->Overlapped) != 0)
	{
		msprintf(_T("[WARNING] ConnectNPipe: CancelIoEx(NPipe(\"%s\")) succeeds\n"),
				 pCtxtNPipe->pPipeNameShort);
	}
	pCtxtNPipe->pIOContext->IOOperation = ClientIo_NPipe_Connect;
	if (ConnectNamedPipe(pCtxtNPipe->hNPipe, &pCtxtNPipe->pIOContext->Overlapped))
	{
		msprintf(_T("[ERROR] ConnectNPipe: overlapped ConnectNamedPipe should return zero: %d\n"), GetLastError());
		return (FALSE);
	}
	switch (GetLastError())
	{
	case ERROR_IO_PENDING:
		return (TRUE);

	case ERROR_PIPE_CONNECTED:
		// Если к pipe уже приконнектились, то pIOContext должен быть свободен
		if (!PostQueuedCompletionStatus(g_hIOCP, 0, (ULONG_PTR)pCtxtNPipe, &pCtxtNPipe->pIOContext->Overlapped))
		{
			msprintf(_T("[ERROR] ConnectNPipe: PostQueuedCompletionStatus: %d\n"), GetLastError());
			return (FALSE);
		}
		return (TRUE);

	default:
		msprintf(_T("[ERROR] ConnectNPipe: ConnectNamedPipe: %d\n"), GetLastError());
		return (FALSE);
	}
	return (TRUE);
}

BOOL DisconnectNPipe(
	PCONTEXT_NPIPE pCtxtNPipe)
{
	BOOL bRet = TRUE;
	if (pCtxtNPipe)
	{
		if (CancelIoEx(pCtxtNPipe->hNPipe, NULL) != 0 && g_bVerbose)
		{
			msprintf(_T("DisconnectNPipe: CancelIoEx(NPipe(\"%s\")) succeeds\n"),
				pCtxtNPipe->pPipeNameShort);
		}
		if (!DisconnectNamedPipe(pCtxtNPipe->hNPipe))
		{
			msprintf(_T("[ERROR] DisconnectNPipe: DisconnectNamedPipe(\"%s\"): %d\n"),
					 pCtxtNPipe->pPipeNameShort, GetLastError());
			bRet = FALSE;
		}
		else if (g_bVerbose)
		{
			if (pCtxtNPipe->pIOContext)
			{
				msprintf(_T("DisconnectNPipe: DisconnectNamedPipe(\"%s\") from PID(%d)\n"),
					 pCtxtNPipe->pPipeNameShort, pCtxtNPipe->pIOContext->dwClientPID);
			}
			else
			{
				msprintf(_T("DisconnectNPipe: DisconnectNamedPipe(\"%s\") from PID(\?\?)\n"),
					 pCtxtNPipe->pPipeNameShort);
			}
		}
		if (pCtxtNPipe->pIOContext)
			pCtxtNPipe->pIOContext->dwClientPID = 0;
	}
	else
	{
		msprintf(_T("[ERROR] DisconnectNPipe: pCtxtNPipe is NULL\n"));
		return (FALSE);
	}
	return bRet;
}

BOOL DisconnectNPipeAndCancelIOLinked(
	PCONTEXT_NPIPE pCtxtNPipe)
{
	BOOL bRet = TRUE;
	if (!DisconnectNPipe(pCtxtNPipe))
	{
		msprintf(_T("[ERROR] DisconnectNPipeAndCancelIOLinked: DisconnectNPipe(\"%s\")\n"),
				pCtxtNPipe->pPipeNameShort);
		bRet = FALSE;
	}
	if (pCtxtNPipe && pCtxtNPipe->pCtxtLinked)
	{
		ClosePAgeantCtxt(pCtxtNPipe->pCtxtLinked);
		if (g_bVerbose)
		{
			msprintf(_T("DisconnectNPipeAndCancelIOLinked: PAgeant context for NPipe(\"%s\") was deleted\n"),
					 pCtxtNPipe->pPipeNameShort);
		}
		pCtxtNPipe->pCtxtLinked = NULL;
	}
	return bRet;
}

BOOL ReconnectNPipeAndCancelIOLinked(
	PCONTEXT_NPIPE pCtxtNPipe)
{
	if (!DisconnectNPipeAndCancelIOLinked(pCtxtNPipe))
	{
		msprintf(_T("[ERROR] ReconnectNPipeAndCancelIOLinked: DisconnectNPipeAndCancelIOLinked(\"%s\")\n"),
				pCtxtNPipe->pPipeNameShort);
		return (FALSE);
	}
	if (!ConnectNPipe(pCtxtNPipe))
	{
		msprintf(_T("[ERROR] ReconnectNPipeAndCancelIOLinked: ConnectNPipe(\"%s\")\n"),
				pCtxtNPipe->pPipeNameShort);
		return (FALSE);
	}
	return (TRUE);
}

//
// Our own printf. This is done because calling printf from multiple
// threads can AV. The standard out for WriteConsole is buffered...
//
int msprintf(const TCHAR *lpFormat, ...)
{
#define myprintf_buf_size 512
	int nLen = 0;
	TCHAR cBuffer[myprintf_buf_size];
	va_list arglist;
	HANDLE hOut = NULL;
	HRESULT hRet;

	ZeroMemory(cBuffer, sizeof(cBuffer));

	va_start(arglist, lpFormat);

	nLen = lstrlen(lpFormat);
	if (nLen > myprintf_buf_size * (int)sizeof(cBuffer[0]))
		return 0;
	hRet = StringCchVPrintf(cBuffer, myprintf_buf_size, lpFormat, arglist);

	if (SUCCEEDED(hRet))
	{
		hOut = GetStdHandle(STD_OUTPUT_HANDLE);
		if (hOut != INVALID_HANDLE_VALUE)
		{
			WriteConsole(hOut, cBuffer, lstrlen(cBuffer), (LPDWORD)&nLen, NULL);
			return nLen;
		}
	}

	return 0;
}

int msprintfA(const char *lpFormat, ...)
{
#define msprintfA_buf_size 512
	int nLen = 0;
	char cBuffer[msprintfA_buf_size];
	va_list arglist;
	HANDLE hOut = NULL;
	HRESULT hRet;

	ZeroMemory(cBuffer, sizeof(cBuffer));

	va_start(arglist, lpFormat);

	nLen = lstrlenA(lpFormat);
	if (nLen > msprintfA_buf_size * (int)sizeof(cBuffer[0]))
		return 0;
	hRet = StringCchVPrintfA(cBuffer, msprintfA_buf_size, lpFormat, arglist);

	if (SUCCEEDED(hRet))
	{
		hOut = GetStdHandle(STD_OUTPUT_HANDLE);
		if (hOut != INVALID_HANDLE_VALUE)
		{
			WriteConsoleA(hOut, cBuffer, lstrlenA(cBuffer), (LPDWORD)&nLen, NULL);
			return nLen;
		}
	}

	return 0;
}
