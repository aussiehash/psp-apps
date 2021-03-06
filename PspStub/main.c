#include <types.h>
#include <cdefs.h>
#include <svc.h>
#include <string.h>
#include <err.h>
#include <log.h>
#include <x86mem.h>
#include <binloader.h>

#define IN_PSP
#include <psp-stub/psp-stub.h>
#include <sev/sev.h>

typedef struct PSPSTUBSTATE
{
    /** The CPU ID of the PSP. */
    uint32_t                    uCpuId;
    /** Flag whether a binary is loaded. */
    uint8_t                     fBinLoaded;
    /** Flag whether the binary we loaded is run for the first time. */
    uint8_t                     fFirstRun;
    /** Padding. */
    uint8_t                     abPadding0[2];
    /** State of the binary we loaded. */
    uint8_t                     abBinState[2048];
    /** The binary data itself we loaded. */
    uint8_t                     abBinary[2048];
    /** The logger instance to use. */
    LOGGER                      Logger;
    /** Helper structure. */
    BINLDRHLP                   Hlp;
} PSPSTUBSTATE;
/** Pointer to the binary loader state. */
typedef PSPSTUBSTATE *PPSPSTUBSTATE;

typedef int32_t FNPSPREQHANDLER(PPSPSTUBSTATE pBinLdrState, PPSPSTUBREQHDR pReqHdr);
typedef FNPSPREQHANDLER *PFNPSPREQHANDLER;

typedef struct PSPSTUBREQHANDLER
{
    /** Size of the request structure in bytes. */
    uint32_t                    cbReq;
    /** The handler to call. */
    PFNPSPREQHANDLER            pfnReq;
} PSPSTUBREQHANDLER;
typedef const PSPSTUBREQHANDLER *PCPSPSTUBREQHANDLER;

/** The global binary loader state. */
static PSPSTUBSTATE g_StubState;
/** Our scratch buffer for temporary memory. */
static uint8_t g_abScratch[16 * 1024];

static void pspstubLoggerFlush(void *pvUser, uint8_t *pbBuf, size_t cbBuf)
{
    (void)pvUser;
    svc_log_char_buf(pbBuf, cbBuf);
}

static void psp_stub_log(const char *pszFmt, ...)
{
    va_list hArgs;
    va_start(hArgs, pszFmt);

    LOGLoggerV(&g_StubState.Logger, pszFmt, hArgs);

    va_end(hArgs);
}

static int bin_ldr_ctx_load_or_init(PPSPSTUBSTATE pCtx, uint32_t idCcd, uint32_t cCcds, uint8_t fFirstRun)
{
    if (!fFirstRun)
    {
        void *pvLoad = svc_get_state_buffer(sizeof(*pCtx));
        if (pvLoad != NULL)
        {
            memcpy(pCtx, pvLoad, sizeof(*pCtx));
            LOGLoggerSetDefaultInstance(&pCtx->Logger);
            LogRel("Stub called, loading state\n");
        }
    }
    else
    {
        pCtx->uCpuId = idCcd;
        pCtx->fBinLoaded = 0;
        pCtx->fFirstRun  = 1;
        pCtx->Hlp.idCcd  = idCcd;
        pCtx->Hlp.cCcds  = cCcds;
        pCtx->Hlp.pfnLog = psp_stub_log;
        LOGLoggerInit(&pCtx->Logger, pspstubLoggerFlush, pCtx,
                      "PspStub", NULL, 0);
        LOGLoggerSetDefaultInstance(&pCtx->Logger);
        LogRel("Stub called for the first time\n");
    }

    return PSPSTATUS_SUCCESS;
}

static void bin_ldr_ctx_save(PPSPSTUBSTATE pCtx)
{
    void *pvSave = svc_get_state_buffer(sizeof(*pCtx));
    if (pvSave != NULL)
        memcpy(pvSave, pCtx, sizeof(*pCtx));
}

static int bin_ldr_read_from_x86(X86PADDR PhysX86Addr, void *pvDst, size_t cbDst)
{
    int rc = PSPSTATUS_SUCCESS;

    void *pvX86Map = svc_x86_host_memory_map(PhysX86Addr, 4);
    if (pvX86Map != NULL)
    {
        memcpy(pvDst, pvX86Map, cbDst);
        int rcPsp = svc_x86_host_memory_unmap(pvX86Map);
        if (rcPsp != PSPSTATUS_SUCCESS)
            LogRel("Unmapping x86 memory failed with %d\n", rc);
    }
    else
    {
        LogRel("Mapping x86 memory failed\n");
        rc = ERR_INVALID_STATE;
    }

    return rc;
}

static int bin_ldr_write_to_x86(X86PADDR PhysX86Addr, void *pvSrc, size_t cbCopy)
{
    int rc = PSPSTATUS_SUCCESS;

    void *pvX86Map = svc_x86_host_memory_map(PhysX86Addr, 4);
    if (pvX86Map != NULL)
    {
        memcpy(pvX86Map, pvSrc, cbCopy);
        int rcPsp = svc_x86_host_memory_unmap(pvX86Map);
        if (rcPsp != PSPSTATUS_SUCCESS)
            LogRel("Unmapping x86 memory failed with %d\n", rc);
    }
    else
    {
        LogRel("Mapping x86 memory failed\n");
        rc = ERR_INVALID_STATE;
    }

    return rc;
}


static int32_t pspstubReqLoadBin(PPSPSTUBSTATE pBinLdrState, PPSPSTUBREQHDR pReqHdr)
{
    PPSPSTUBREQLOADBIN pReq = (PPSPSTUBREQLOADBIN)pReqHdr;
    int32_t rc = INF_SUCCESS;

    if (pReq->cbBinary <= sizeof(pBinLdrState->abBinary))
    {
        LogRel("Loading binary from %#X (%u bytes)\n", pReq->PhysX86AddrLoad, pReq->cbBinary);
        rc = bin_ldr_read_from_x86(pReq->PhysX86AddrLoad, (uint8_t *)&pBinLdrState->abBinary[0],
                                   pReq->cbBinary);
        if (rc == PSPSTATUS_SUCCESS)
        {
            pBinLdrState->fFirstRun = 1;
            pBinLdrState->fBinLoaded = 1;
            LogRel("Loaded binary successfully\n");
            return INF_SUCCESS;
        }
        else
            LogRel("Loading binary failed\n");
    }
    else
    {
        LogRel("Binary exceeds internal buffer size\n");
        rc = ERR_BUFFER_OVERFLOW;
    }

    return rc;
}


static int32_t pspstubReqExecBin(PPSPSTUBSTATE pBinLdrState, PPSPSTUBREQHDR pReqHdr)
{
    PPSPSTUBREQEXECBIN pReq = (PPSPSTUBREQEXECBIN)pReqHdr;

    int32_t rc = INF_SUCCESS;

    LogRel("Calling loaded binary\n");

    memcpy((void *)(uintptr_t)BIN_LOADER_LOAD_ADDR, &g_StubState.abBinary[0], sizeof(g_StubState.abBinary));
    ((PFNBINLOADENTRY)BIN_LOADER_LOAD_ADDR)(&g_StubState.abBinState[0], &pBinLdrState->Hlp, pReq->PhysX8AddrExec, 0, pBinLdrState->fFirstRun);
    pBinLdrState->fFirstRun = 0;

    return rc;
}


static int32_t pspstubReqSmnRwWorker(PPSPSTUBSTATE pBinLdrState, PPSPSTUBREQHDR pReqHdr, uint8_t fWrite)
{
    PPSPSTUBREQSMNRW pReq = (PPSPSTUBREQSMNRW)pReqHdr;
    int32_t rc = INF_SUCCESS;

    void *pvSmn = svc_smn_map_ex(pReq->u32Addr, pReq->idCcdTgt);
    if (pvSmn != NULL)
    {
        if (fWrite)
        {
            switch (pReq->cbVal)
            {
                case 1:
                    *(volatile uint8_t *)pvSmn = (uint8_t)pReq->u64Val;
                    break;
                case 2:
                    *(volatile uint16_t *)pvSmn = (uint16_t)pReq->u64Val;
                    break;
                case 4:
                    *(volatile uint32_t *)pvSmn = (uint32_t)pReq->u64Val;
                    break;
                case 8:
                    *(volatile uint64_t *)pvSmn = (uint64_t)pReq->u64Val;
                    break;
                default:
                    LogRel("Invalid write size %u\n", pReq->cbVal);
                    rc = ERR_INVALID_PARAMETER;
            }
        }
        else
        {
            switch (pReq->cbVal)
            {
                case 1:
                    pReq->u64Val = (uint64_t)*(volatile uint8_t *)pvSmn;
                    break;
                case 2:
                    pReq->u64Val = (uint64_t)*(volatile uint16_t *)pvSmn;
                    break;
                case 4:
                    pReq->u64Val = (uint64_t)*(volatile uint32_t *)pvSmn;
                    break;
                case 8:
                    pReq->u64Val = *(volatile uint64_t *)pvSmn;
                    break;
                default:
                    LogRel("Invalid read size %u\n", pReq->cbVal);
                    rc = ERR_INVALID_PARAMETER;
            }
        }

        svc_smn_unmap(pvSmn);
    }
    else
    {
        LogRel("Mapping SMN address %#x on CCD %u failed\n", pReq->u32Addr, pReq->idCcdTgt);
        rc = ERR_INVALID_STATE;
    }

    return rc;
}

static int32_t pspstubReqSmnRead(PPSPSTUBSTATE pBinLdrState, PPSPSTUBREQHDR pReqHdr)
{
    return pspstubReqSmnRwWorker(pBinLdrState, pReqHdr, 0 /*fWrite*/);
}


static int32_t pspstubReqSmnWrite(PPSPSTUBSTATE pBinLdrState, PPSPSTUBREQHDR pReqHdr)
{
    return pspstubReqSmnRwWorker(pBinLdrState, pReqHdr, 1 /*fWrite*/);
}


static int32_t pspstubReqPspRwWorker(PPSPSTUBSTATE pBinLdrState, PPSPSTUBREQHDR pReqHdr, uint8_t fWrite)
{
    PCPSPSTUBREQPSPRW pReq = (PCPSPSTUBREQPSPRW)pReqHdr;
    void *pvAddrPsp = (void *)pReq->u32Addr;
    int32_t rc = PSPSTATUS_SUCCESS;

    void *pvX86Map = svc_x86_host_memory_map(pReq->PhysX86Addr, 4);
    if (pvX86Map != NULL)
    {
        if (fWrite)
            memcpy(pvAddrPsp, pvX86Map, pReq->cbCopy);
        else
            memcpy(pvX86Map, pvAddrPsp, pReq->cbCopy);
        int rcPsp = svc_x86_host_memory_unmap(pvX86Map);
        if (rcPsp != PSPSTATUS_SUCCESS)
        {
            LogRel("Unmapping x86 memory failed with %d\n", rc);
            rc = ERR_INVALID_STATE;
        }
    }
    else
    {
        LogRel("Mapping x86 memory failed\n");
        rc = ERR_INVALID_STATE;
    }

    return rc;
}

static int32_t pspstubReqPspRead(PPSPSTUBSTATE pBinLdrState, PPSPSTUBREQHDR pReqHdr)
{
    return pspstubReqPspRwWorker(pBinLdrState, pReqHdr, 0 /*fWrite*/);
}


static int32_t pspstubReqPspWrite(PPSPSTUBSTATE pBinLdrState, PPSPSTUBREQHDR pReqHdr)
{
    return pspstubReqPspRwWorker(pBinLdrState, pReqHdr, 1 /*fWrite*/);
}


static int32_t pspstubReqSvcCall(PPSPSTUBSTATE pBinLdrState, PPSPSTUBREQHDR pReqHdr)
{
    PPSPSTUBREQCALLSVC pReq = (PPSPSTUBREQCALLSVC)pReqHdr;

    /* Modify the svc template to call the requested syscall. */
    volatile uint8_t *pbSyscall = (volatile uint8_t *)svc_template;
    *pbSyscall++ = (uint8_t)(pReq->idxSyscall & 0xff);
    *pbSyscall++ = (uint8_t)(pReq->idxSyscall >> 8)& 0xff;
    *pbSyscall++ = (uint8_t)(pReq->idxSyscall >> 16)& 0xff;
    *pbSyscall   = 0xef; /* svc */

#if 0
    uint32_t *pu32Syscall = (uint32_t *)svc_template;

    for (uint32_t i = 0; i < 4; i++)
        LogRel("BinLoader#%u: SVC[%u]: %#x\n", pBinLdrState->uCpuId, i, pu32Syscall[i]);
#endif

    /* Clean instruction cache. */
    svc_invalidate_mem(SVC_INV_MEM_OP_CLEAN_AND_INVALIDATE, 1 /*fInsnMem*/, (void *)svc_template, sizeof(uint64_t));
    pReq->u32R0Return = svc_template(pReq->u32R0, pReq->u32R1, pReq->u32R2, pReq->u32R3);

    return INF_SUCCESS;
}


static int32_t pspstubReqQueryInfo(PPSPSTUBSTATE pBinLdrState, PPSPSTUBREQHDR pReqHdr)
{
    PPSPSTUBREQQUERYINFO pReq = (PPSPSTUBREQQUERYINFO)pReqHdr;

    pReq->u32PspScratchAddr = (uint32_t)(uintptr_t)&g_abScratch[0];
    pReq->cbScratch         = sizeof(g_abScratch);

    return INF_SUCCESS;
}


/** Request handlers. */
static const PSPSTUBREQHANDLER g_aReqHandlers[] =
{
    { sizeof(PSPSTUBREQLOADBIN),   pspstubReqLoadBin   },
    { sizeof(PSPSTUBREQEXECBIN),   pspstubReqExecBin   },
    { sizeof(PSPSTUBREQSMNRW),     pspstubReqSmnRead   },
    { sizeof(PSPSTUBREQSMNRW),     pspstubReqSmnWrite  },
    { sizeof(PSPSTUBREQPSPRW),     pspstubReqPspRead   },
    { sizeof(PSPSTUBREQPSPRW),     pspstubReqPspWrite  },
    { sizeof(PSPSTUBREQCALLSVC),   pspstubReqSvcCall   },
    { sizeof(PSPSTUBREQQUERYINFO), pspstubReqQueryInfo }
};


typedef struct BINLDRSLVREQ
{
    uint32_t idCmd;
    X86PADDR PhysX86AddrReqBuf;
} BINLDRSLVREQ;
typedef BINLDRSLVREQ *PBINLDRSLVREQ;

uint32_t main(uint32_t idCcd, uint32_t cCcds, PSEVCMDBUF pCmdBuf, uint8_t fFirstRun)
{
    if (bin_ldr_ctx_load_or_init(&g_StubState, idCcd, cCcds, fFirstRun) != PSPSTATUS_SUCCESS)
        return 1;

    g_StubState.Hlp.pvCmdBuf = (void *)pCmdBuf;

    if (idCcd == 0)
    {
        uint32_t idCmd = (pCmdBuf->idCmd >> 16) & 0xff;
        X86PADDR PhysX86AddrReqBuf = (X86PADDR)pCmdBuf->PhysX86CmdBufHigh << 32 | pCmdBuf->PhysX86CmdBufLow;

        if (idCmd >= PSP_STUB_REQ_FIRST && idCmd <= PSP_STUB_REQ_LAST)
        {
            PCPSPSTUBREQHANDLER pHandler = &g_aReqHandlers[idCmd - PSP_STUB_REQ_FIRST];
            PSPSTUBREQ BinLoaderReq;

            LogRel("Loading request buffer from %#X (%u bytes)\n", PhysX86AddrReqBuf, pHandler->cbReq);
            int rc = bin_ldr_read_from_x86(PhysX86AddrReqBuf, &BinLoaderReq, pHandler->cbReq);
            if (rc == PSPSTATUS_SUCCESS)
            {
                /* Check whether the request is designated for us or one of the slaves. */
                if (BinLoaderReq.Hdr.idCcd == 0)
                {
                    /* Execute handler and pass return value back. */
                    int32_t rcReq = pHandler->pfnReq(&g_StubState, &BinLoaderReq.Hdr);
                    BinLoaderReq.Hdr.i32Sts = rcReq;
                    rc = bin_ldr_write_to_x86(PhysX86AddrReqBuf, &BinLoaderReq, pHandler->cbReq);
                    if (rc != PSPSTATUS_SUCCESS)
                        LogRel("writing request buffer back failed with %d\n", rc);
                }
                else
                {
                    BINLDRSLVREQ SlvReq;

                    LogRel("Poking slave CCD %u\n", BinLoaderReq.Hdr.idCcd);
                    SlvReq.idCmd             = idCmd;
                    SlvReq.PhysX86AddrReqBuf = PhysX86AddrReqBuf;
                    uint32_t rcPsp = svc_call_other_psp(BinLoaderReq.Hdr.idCcd, &SlvReq, sizeof(SlvReq));
                    LogRel("Poking slave returned %u\n", rcPsp);
                }
            }
            else
                LogRel("Reading request buffer failed with %d\n", rc);
        }
        else
            LogRel("Ignoring command %#x probably designated for original SEV APP\n", idCmd);
    }
    else
    {
        PBINLDRSLVREQ pSlvReq = (PBINLDRSLVREQ)pCmdBuf;

        LogRel("Got kicked by master\n");
        PCPSPSTUBREQHANDLER pHandler = &g_aReqHandlers[pSlvReq->idCmd - PSP_STUB_REQ_FIRST];
        PSPSTUBREQ BinLoaderReq;

        LogRel("Loading request buffer from %#X (%u bytes)\n", pSlvReq->PhysX86AddrReqBuf, pHandler->cbReq);
        int rc = bin_ldr_read_from_x86(pSlvReq->PhysX86AddrReqBuf, &BinLoaderReq, pHandler->cbReq);
        if (rc == PSPSTATUS_SUCCESS)
        {
            /* Execute handler and pass return value back. */
            int32_t rcReq = pHandler->pfnReq(&g_StubState, &BinLoaderReq.Hdr);
            BinLoaderReq.Hdr.i32Sts = rcReq;
            rc = bin_ldr_write_to_x86(pSlvReq->PhysX86AddrReqBuf, &BinLoaderReq, pHandler->cbReq);
            if (rc != PSPSTATUS_SUCCESS)
                LogRel("writing request buffer back failed with %d\n", rc);
        }
        else
            LogRel("Reading request buffer failed with %d\n", rc);
    }

    bin_ldr_ctx_save(&g_StubState);
    return 0;
}
