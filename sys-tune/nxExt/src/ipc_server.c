/*
 * --------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * <p-sam@d3vs.net>, <natinusala@gmail.com>, <m4x@m4xw.net>
 * wrote this file. As long as you retain this notice you can do whatever you
 * want with this stuff. If you meet any of us some day, and you think this
 * stuff is worth it, you can buy us a beer in return.  - The sys-clk authors
 * --------------------------------------------------------------------------
 */

#include "nxExt/ipc_server.h"
#include <string.h>

enum { commandBufferSize = 0x100, cmifPadding = 0x10 };
#define RESPONSE_MAX_DATA_SIZE (commandBufferSize - sizeof(HipcHeader) - cmifPadding - sizeof(CmifOutHeader))

Result ipcServerInit(IpcServer* server, const char* name, u32 max_sessions, bool publishSm)
{
    const u32 maxPorts = publishSm ? 2 : 1;
    if (max_sessions < 1 || max_sessions > MAX_WAIT_OBJECTS / maxPorts - 1)
    {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    server->srvName = smEncodeName(name);
    server->max = max_sessions + 1;
    server->ports = 0;
    server->count = 0;

    Result rc = svcManageNamedPort(&server->handles[0], server->srvName.name, max_sessions);
    if (R_FAILED(rc))
        return rc;

    server->ports = 1;
    // Keep the kernel port usable if registration with SM fails.
    if (publishSm && R_SUCCEEDED(smRegisterService(&server->handles[1], server->srvName, false, max_sessions))) {
        server->ports = 2;
        server->max *= 2;
    }
    server->count = server->ports;
    return 0;
}

Result ipcServerExit(IpcServer* server)
{
    if (!server->count)
        return 0;

    Handle unused;
    const Result smResult = server->ports == 2 ? smUnregisterService(server->srvName) : 0;
    const Result portResult = svcManageNamedPort(&unused, server->srvName.name, 0);
    for(u32 i = 0; i < server->count; i++)
    {
        svcCloseHandle(server->handles[i]);
    }
    server->count = 0;
    server->ports = 0;
    return R_FAILED(smResult) ? smResult : portResult;
}

static Result _ipcServerAddSession(IpcServer* server, Handle session)
{
    if(server->count >= server->max)
    {
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }

    server->handles[server->count] = session;
    server->count++;
    return 0;
}

static Result _ipcServerDeleteSession(IpcServer* server, u32 index)
{
    if(index < server->ports || index >= server->count)
    {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    svcCloseHandle(server->handles[index]);

    for(u32 j = index; j < (server->count - 1); j++)
    {
        server->handles[j] = server->handles[j + 1];
    }
    server->count--;
    return 0;
}

static Result _ipcServerParseRequest(IpcServerRequest* r, u32* token)
{
    u8* base = armGetTls();

    r->hipc = hipcParseRequest(base);
    r->data.cmdId = 0;
    r->data.size = 0;
    r->data.ptr =  NULL;

    // This service accepts raw data and map-alias buffers only.
    for (u32 i = 0; i < r->hipc.meta.num_copy_handles; ++i)
        svcCloseHandle(r->hipc.data.copy_handles[i]);
    for (u32 i = 0; i < r->hipc.meta.num_move_handles; ++i)
        svcCloseHandle(r->hipc.data.move_handles[i]);
    if (r->hipc.meta.num_copy_handles || r->hipc.meta.num_move_handles || r->hipc.meta.num_send_statics)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    if (r->hipc.meta.type == CmifCommandType_Request || r->hipc.meta.type == CmifCommandType_Control ||
        r->hipc.meta.type == CmifCommandType_RequestWithContext || r->hipc.meta.type == CmifCommandType_ControlWithContext)
    {
        const size_t rawOffset = (uintptr_t)r->hipc.data.data_words - (uintptr_t)base;
        const size_t rawSize = r->hipc.meta.num_data_words * sizeof(u32);
        if (rawOffset > commandBufferSize || rawSize > commandBufferSize - rawOffset ||
            rawSize < sizeof(CmifInHeader) + cmifPadding)
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);

        const CmifInHeader* header = cmifGetAlignedDataStart(r->hipc.data.data_words, base);
        if (header->magic != CMIF_IN_HEADER_MAGIC)
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);

        r->data.cmdId = header->command_id;
        r->data.size = rawSize - sizeof(CmifInHeader) - cmifPadding;
        r->data.ptr = (u8*)header + sizeof(CmifInHeader);
        *token = header->token;
    }

    return 0;
}

static void _ipcServerPrepareResponse(Result rc, u32 token, const void* data, size_t dataSize)
{
    if (dataSize > RESPONSE_MAX_DATA_SIZE)
        rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (R_FAILED(rc))
        dataSize = 0;

    u8* base = armGetTls();
    const size_t words = (sizeof(CmifOutHeader) + dataSize + cmifPadding + 3) / sizeof(u32);
    HipcRequest hipc = hipcMakeRequestInline(base,
        .type = CmifCommandType_Invalid,
        .num_data_words = words,
    );

    memset(hipc.data_words, 0, words * sizeof(u32));
    CmifOutHeader* header = cmifGetAlignedDataStart(hipc.data_words, base);
    *header = (CmifOutHeader){.magic = CMIF_OUT_HEADER_MAGIC, .result = rc, .token = token};

    if (dataSize)
        memcpy(header + 1, data, dataSize);
}

static Result _ipcServerProcessNewSession(IpcServer* server, u32 portIndex)
{
    Handle session;
    Result rc = svcAcceptSession(&session, server->handles[portIndex]);
    if(R_SUCCEEDED(rc) && R_FAILED(rc = _ipcServerAddSession(server, session)))
    {
        svcCloseHandle(session);
    }
    return rc;
}

static Result _ipcServerProcessSession(IpcServer* server, IpcServerRequestHandler handler, void* userdata, u32 handleIndex)
{
    s32 unusedIndex;
    IpcServerRequest r;
    size_t dataSize = 0;
    _Alignas(8) u8 data[RESPONSE_MAX_DATA_SIZE];
    bool close = false;
    u32 token = 0;

    // The kernel reads the receive configuration from TLS before copying the
    // incoming request. The previous command on this thread may be a client reply.
    memset(armGetTls(), 0, sizeof(HipcHeader));
    Result rc = svcReplyAndReceive(&unusedIndex, &server->handles[handleIndex], 1, 0, UINT64_MAX);
    if(R_SUCCEEDED(rc))
    {
        rc = _ipcServerParseRequest(&r, &token);
    }

    if(R_SUCCEEDED(rc))
    {
        switch(r.hipc.meta.type)
        {
            case CmifCommandType_Request:
            case CmifCommandType_RequestWithContext: {
                Result result = handler(userdata, &r, data, &dataSize);
                _ipcServerPrepareResponse(result, token, data, dataSize);
                break;
            }
            case CmifCommandType_Control:
            case CmifCommandType_ControlWithContext: {
                const u16 pointerBufferSize = 0;
                if (r.data.cmdId == 3)
                    _ipcServerPrepareResponse(0, token, &pointerBufferSize, sizeof(pointerBufferSize));
                else
                    _ipcServerPrepareResponse(MAKERESULT(11, 403), token, NULL, 0);
                break;
            }
            case CmifCommandType_Close:
                close = true;
                break;
            default:
                _ipcServerPrepareResponse(MAKERESULT(11, 403), token, NULL, 0);
                break;
        }

        if (!close) {
            rc = svcReplyAndReceive(&unusedIndex, &server->handles[handleIndex], 0, server->handles[handleIndex], 0);
            if (rc == KERNELRESULT(TimedOut))
                rc = 0;
        }
    }

    if(R_FAILED(rc) || close)
    {
        _ipcServerDeleteSession(server, handleIndex);
    }

    return rc;
}

Result ipcServerProcess(IpcServer* server, IpcServerRequestHandler handler, void* userdata)
{
    s32 handleIndex = -1;
    Result rc = svcWaitSynchronization(&handleIndex, server->handles, server->count, UINT64_MAX);
    if (rc == KERNELRESULT(InvalidHandle))
        return KERNELRESULT(Cancelled);

    if(R_SUCCEEDED(rc) && (handleIndex < 0 || handleIndex >= server->count))
    {
        rc = MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }

    if(R_SUCCEEDED(rc))
    {
        if ((u32)handleIndex >= server->ports)
        {
            rc = _ipcServerProcessSession(server, handler, userdata, handleIndex);
        }
        else
        {
            rc = _ipcServerProcessNewSession(server, handleIndex);
        }
    }

    return rc;
}
