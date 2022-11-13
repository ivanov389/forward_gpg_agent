/* Copyright (c) 2022 Denis Byzov. All rights reserved.
 * Use of this file is governed by the BSD 3-clause license that
 * can be found in the LICENSE.txt file in the project root.
 */

// Rework of MSDN example from https://github.com/microsoft/Windows-classic-samples/tree/main/Samples/Win7Samples/netds/winsock/iocp/serverex

#ifndef FORWARDGPGAGENT_H
#define FORWARDGPGAGENT_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <Ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <strsafe.h>
#include <mswsock.h>
#include <tchar.h>

#ifdef UNICODE
#define AddrInfo addrinfoW
#else
#define AddrInfo addrinfo
#endif

#define MAX_BUFF_SIZE 4096 // inet sockets and GPGSocketAgent buffer size
#define MAX_NPIPE_BUFF_SIZE 4096 // NPipes buffers size
#define MAX_WORKER_THREAD 16
#define MAX_LISTENING_PORTS 5
#define MAX_LISTENING_NPIPES 5
#define MAX_PATH_LEN 256

#define PAGEANT_SHAREDMEM_SIZE 8192 // up to 256*1024
#define PAGEANT_MAX_MSG_LEN PAGEANT_SHAREDMEM_SIZE
#define PAGEANT_MSG_ID 0x804E50BA // DO NOT EDIT. This is from PAgeant spec.

#define xmalloc(s) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (s))
#define xfree(p) HeapFree(GetProcessHeap(), 0, (p))

typedef enum _IO_OPERATION
{
    ClientIo_Invalid,
    ClientIo_Socket_Accept,         // src: ConnectionTypeSocket; dst: ConnectionTypeSocketGPG, ConnectionTypePAgeant
    ClientIo_Socket2Socket_Read,    // src: ConnectionTypeSocket; dst: ConnectionTypeSocketGPG
    ClientIo_Socket2Socket_Write,   // src: ConnectionTypeSocket; dst: ConnectionTypeSocketGPG
    ClientIo_Socket2SocketGPG_GPGAuthorization, // src: ConnectionTypeSocket; dst: ConnectionTypeSocketGPG
    ClientIo_Socket2PAgeant_Read,   // src: ConnectionTypeSocket; dst: ConnectionTypePAgeant
    ClientIo_Socket2PAgeant_Write,  // src: ConnectionTypeSocket; dst: ConnectionTypePAgeant
    ClientIo_NPipe_Connect,         // src: ConnectionTypeNPipe; dst: ConnectionTypePAgeant
    ClientIo_NPipe2PAgeant_Read,    // src: ConnectionTypeNPipe; dst: ConnectionTypePAgeant
    ClientIo_NPipe2PAgeant_Write    // src: ConnectionTypeNPipe; dst: ConnectionTypePAgeant
} IO_OPERATION,
    *PIO_OPERATION;

typedef enum _CONNECTIONTYPE
{
    ConnectionTypeInvalid,
    ConnectionTypeSocket,
    ConnectionTypeNPipe,
    ConnectionTypePAgeant,
    ConnectionTypeSocketGPG
} CONNECTIONTYPE,
    *PCONNECTIONTYPE;

//
// data to be associated for every I/O operation on a socket
//
typedef struct _IOCONTEXT_SOCKET
{
    WSAOVERLAPPED Overlapped;
    IO_OPERATION IOOperation;
    int nTotalBytes;
    int nSentBytes;
    char Buffer[MAX_BUFF_SIZE];

    WSABUF wsabuf;
    SOCKET SocketAccept;
    struct _IOCONTEXT_SOCKET *pIOContextForward;
} IOCONTEXT_SOCKET, *PIOCONTEXT_SOCKET;
//
// For AcceptEx, the IOCP key is the CONTEXT_SOCKET for the listening socket,
// so we need to another field SocketAccept in IOCONTEXT_SOCKET. When the outstanding
// AcceptEx completes, this field is our connection socket handle.
//

typedef struct _IOCONTEXT_NPIPE
{
    WSAOVERLAPPED Overlapped;
    IO_OPERATION IOOperation;
    int nTotalBytes;
    int nSentBytes;
    char Buffer[MAX_NPIPE_BUFF_SIZE];

    DWORD dwClientPID;
} IOCONTEXT_NPIPE, *PIOCONTEXT_NPIPE;

typedef struct _CONTEXT_PAGEANT
{
    HWND hwndPAgeant;
    HANDLE hFileMap;
    LPVOID lpSharedMem;
    DWORD dwMsgTotal;
    DWORD dwMsgSent;
    char sFileMapName[MAX_PATH_LEN];
    // TCHAR sFileMapName[MAX_PATH_LEN];
    COPYDATASTRUCT cds;
} CONTEXT_PAGEANT,
    *PCONTEXT_PAGEANT;

//
// data to be associated with every socket added to the IOCP
//
typedef struct _CONTEXT_SOCKET
{
    CONNECTIONTYPE ctConnectionType;
    CONNECTIONTYPE ctConnectionTypeLinked;

    // struct _CONTEXT_SOCKET *pCtxtLinked; // ctConnectionTypeLinked==(ConnectionTypeSocketGPG||ConnectionTypeSocket)
    // PCONTEXT_PAGEANT pCtxtLinked;        // ctConnectionTypeLinked==ConnectionTypePAgeant
    void *pCtxtLinked;

    PIOCONTEXT_SOCKET pIOContext;

    SOCKET Socket;
    LPFN_ACCEPTEX fnAcceptEx;

    //
    // linked list for all outstanding i/o on the socket
    //
    struct _CONTEXT_SOCKET *pCtxtBack;
    struct _CONTEXT_SOCKET *pCtxtForward;
} CONTEXT_SOCKET, *PCONTEXT_SOCKET;

typedef struct _CONTEXT_NPIPE
{
    CONNECTIONTYPE ctConnectionType;
    CONNECTIONTYPE ctConnectionTypeLinked;

    // PCONTEXT_PAGEANT pCtxtLinked; // ctConnectionTypeLinked==ConnectionTypePAgeant
    PCONTEXT_PAGEANT pCtxtLinked;

    PIOCONTEXT_NPIPE pIOContext;

    HANDLE hNPipe;
    TCHAR sPipeName[MAX_PATH_LEN];
    TCHAR *pPipeNameShort;
} CONTEXT_NPIPE, *PCONTEXT_NPIPE;

typedef struct _PORT2GPGSOCKET
{
    TCHAR *port;
    TCHAR *gpgsocket;
    SOCKET sdListen;
    CONNECTIONTYPE ctConnectionTypeLinked;
    PCONTEXT_SOCKET pCtxtListenSocket;
} PORT2GPGSOCKET, *PPORT2GPGSOCKET;

typedef struct _NAMEDPIPE2GPGSOCKET
{
    TCHAR *pipenameshort;
    TCHAR *gpgsocket;
    CONNECTIONTYPE ctConnectionTypeLinked;
    PCONTEXT_NPIPE pCtxtNPipe;
} NAMEDPIPE2GPGSOCKET, *PNAMEDPIPE2GPGSOCKET;

BOOL ValidOptions(
    int argc,
    TCHAR *argv[],
    PORT2GPGSOCKET (&port2gpgsocket)[MAX_LISTENING_PORTS],
    int &numListeningPorts,
    NAMEDPIPE2GPGSOCKET (&npipe2gpgsocket)[MAX_LISTENING_NPIPES],
    int &numListeningNPipes,
    DWORD &numWorkerThreads);

BOOL WINAPI CtrlHandler(
    DWORD dwEvent);

BOOL CreateListenSocket(
    PORT2GPGSOCKET &port2gpgsocket);

BOOL CreateAcceptSocket(
    const SOCKET sdListen,
    PCONTEXT_SOCKET pCtxtListenSocket,
    PPORT2GPGSOCKET pport2gpgsocket_out);

DWORD WINAPI WorkerThread(
    LPVOID WorkContext);

PCONTEXT_SOCKET UpdateCompletionPort(
    SOCKET s,
    IO_OPERATION ClientIo,
    BOOL bAddToList);
//
// bAddToList is FALSE for listening socket, and TRUE for connection sockets.
// As we maintain the context for listening socket in a global structure, we
// don't need to add it to the list.
//

VOID CloseClient(
    PCONTEXT_SOCKET lpPerSocketContext,
    BOOL bGraceful);

VOID CloseClientAndLinked(
    PCONTEXT_SOCKET lpPerSocketContext,
    BOOL bGraceful);

VOID CancelIOLinkedAndCloseClient(
    PCONTEXT_SOCKET lpPerSocketContext,
    BOOL bGraceful);

PCONTEXT_SOCKET CtxtAllocate(
    SOCKET s,
    IO_OPERATION ClientIO);

VOID CtxtListFree();

VOID CtxtListAddTo(
    PCONTEXT_SOCKET lpPerSocketContext);

VOID CtxtListDeleteFrom(
    PCONTEXT_SOCKET lpPerSocketContext);

PPORT2GPGSOCKET GetPort2gpgsocket(
    const SOCKET sdListen,
    PORT2GPGSOCKET (&port2gpgsocket)[MAX_LISTENING_PORTS]);

PCONTEXT_SOCKET CreateGPGSocket(
    const TCHAR* gpgsocket,
    const PCONTEXT_SOCKET lpCtxtLinkedSocket);

PCONTEXT_PAGEANT CreatePAgeantCtxt();

PCONTEXT_PAGEANT CreatePAgeantCtxt(
    PCONTEXT_SOCKET lpPerSocketContext);

PCONTEXT_PAGEANT CreatePAgeantCtxt(
    PCONTEXT_NPIPE lpNPipeContext);

BOOL ClosePAgeantCtxt(
    PCONTEXT_PAGEANT lpPAgeantConnectionContext);

PCONTEXT_NPIPE CtxtAllocateNPipe(
    IO_OPERATION ClientIO);

VOID CtxtFreeNPipe(
    PCONTEXT_NPIPE pCtxtNPipe);

BOOL CreateNPipeAndUpdateIOCP(
    NAMEDPIPE2GPGSOCKET &npipe2gpgsocket);

VOID CloseNPipeCtxt(
    PCONTEXT_NPIPE pCtxtNPipe);

BOOL ConnectNPipe(
    PCONTEXT_NPIPE pCtxtNPipe);

BOOL DisconnectNPipe(
    PCONTEXT_NPIPE pCtxtNPipe);

BOOL DisconnectNPipeAndCancelIOLinked(
    PCONTEXT_NPIPE pCtxtNPipe);

BOOL ReconnectNPipeAndCancelIOLinked(
    PCONTEXT_NPIPE pCtxtNPipe);

VOID PrintUsageHint();

#endif // FORWARDGPGAGENT_H
