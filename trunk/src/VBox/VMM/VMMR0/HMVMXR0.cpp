/* $Id$ */
/** @file
 * HM VMX (Intel VT-x) - Host Context Ring-0.
 */

/*
 * Copyright (C) 2012 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#define LOG_GROUP LOG_GROUP_HM
#include <iprt/asm-amd64-x86.h>
#include <iprt/thread.h>
#include <iprt/string.h>

#include "HMInternal.h"
#include <VBox/vmm/vm.h>
#include "HWVMXR0.h"
#include <VBox/vmm/pdmapi.h>
#include <VBox/vmm/dbgf.h>
#include <VBox/vmm/iom.h>
#include <VBox/vmm/selm.h>
#include <VBox/vmm/tm.h>
#ifdef VBOX_WITH_REM
# include <VBox/vmm/rem.h>
#endif
#ifdef DEBUG_ramshankar
#define VBOX_ALWAYS_TRAP_ALL_EXCEPTIONS
#endif

/*******************************************************************************
*   Defined Constants And Macros                                               *
*******************************************************************************/
#if defined(RT_ARCH_AMD64)
# define VMX_IS_64BIT_HOST_MODE()   (true)
#elif defined(VBOX_WITH_HYBRID_32BIT_KERNEL)
# define VMX_IS_64BIT_HOST_MODE()   (g_fVMXIs64bitHost != 0)
#else
# define VMX_IS_64BIT_HOST_MODE()   (false)
#endif

#define VMX_SEL_UNUSABLE            RT_BIT(16)

/**
 * Updated-guest-state flags.
 */
#define VMX_UPDATED_GUEST_FPU                   RT_BIT(0)
#define VMX_UPDATED_GUEST_RIP                   RT_BIT(1)
#define VMX_UPDATED_GUEST_RSP                   RT_BIT(2)
#define VMX_UPDATED_GUEST_RFLAGS                RT_BIT(3)
#define VMX_UPDATED_GUEST_CR0                   RT_BIT(4)
#define VMX_UPDATED_GUEST_CR3                   RT_BIT(5)
#define VMX_UPDATED_GUEST_CR4                   RT_BIT(6)
#define VMX_UPDATED_GUEST_GDTR                  RT_BIT(7)
#define VMX_UPDATED_GUEST_IDTR                  RT_BIT(8)
#define VMX_UPDATED_GUEST_LDTR                  RT_BIT(9)
#define VMX_UPDATED_GUEST_TR                    RT_BIT(10)
#define VMX_UPDATED_GUEST_SEGMENT_REGS          RT_BIT(11)
#define VMX_UPDATED_GUEST_DEBUG                 RT_BIT(12)
#define VMX_UPDATED_GUEST_FS_BASE_MSR           RT_BIT(13)
#define VMX_UPDATED_GUEST_GS_BASE_MSR           RT_BIT(14)
#define VMX_UPDATED_GUEST_SYSENTER_CS_MSR       RT_BIT(15)
#define VMX_UPDATED_GUEST_SYSENTER_EIP_MSR      RT_BIT(16)
#define VMX_UPDATED_GUEST_SYSENTER_ESP_MSR      RT_BIT(17)
#define VMX_UPDATED_GUEST_AUTO_LOAD_STORE_MSRS  RT_BIT(18)
#define VMX_UPDATED_GUEST_ACTIVITY_STATE        RT_BIT(19)
#define VMX_UPDATED_GUEST_APIC_STATE            RT_BIT(20)
#define VMX_UPDATED_GUEST_ALL                   (  VMX_UPDATED_GUEST_FPU                   \
                                                 | VMX_UPDATED_GUEST_RIP                   \
                                                 | VMX_UPDATED_GUEST_RSP                   \
                                                 | VMX_UPDATED_GUEST_RFLAGS                \
                                                 | VMX_UPDATED_GUEST_CR0                   \
                                                 | VMX_UPDATED_GUEST_CR3                   \
                                                 | VMX_UPDATED_GUEST_CR4                   \
                                                 | VMX_UPDATED_GUEST_GDTR                  \
                                                 | VMX_UPDATED_GUEST_IDTR                  \
                                                 | VMX_UPDATED_GUEST_LDTR                  \
                                                 | VMX_UPDATED_GUEST_TR                    \
                                                 | VMX_UPDATED_GUEST_SEGMENT_REGS          \
                                                 | VMX_UPDATED_GUEST_DEBUG                 \
                                                 | VMX_UPDATED_GUEST_FS_BASE_MSR           \
                                                 | VMX_UPDATED_GUEST_GS_BASE_MSR           \
                                                 | VMX_UPDATED_GUEST_SYSENTER_CS_MSR       \
                                                 | VMX_UPDATED_GUEST_SYSENTER_EIP_MSR      \
                                                 | VMX_UPDATED_GUEST_SYSENTER_ESP_MSR      \
                                                 | VMX_UPDATED_GUEST_AUTO_LOAD_STORE_MSRS  \
                                                 | VMX_UPDATED_GUEST_ACTIVITY_STATE        \
                                                 | VMX_UPDATED_GUEST_APIC_STATE)

/**
 * Flags to skip redundant reads of some common VMCS fields.
 */
#define VMX_TRANSIENT_IDT_VECTORING_INFO            RT_BIT(0)
#define VMX_TRANSIENT_IDT_VECTORING_ERROR_CODE      RT_BIT(1)
#define VMX_TRANSIENT_EXIT_QUALIFICATION            RT_BIT(2)
#define VMX_TRANSIENT_EXIT_INSTR_LEN                RT_BIT(3)
#define VMX_TRANSIENT_EXIT_INTERRUPTION_INFO        RT_BIT(4)
#define VMX_TRANSIENT_EXIT_INTERRUPTION_ERROR_CODE  RT_BIT(5)

/*
 * Exception bitmap mask for real-mode guests (real-on-v86). We need to intercept all exceptions manually (except #PF).
 * #NM is also handled spearetely, see hmR0VmxLoadGuestControlRegs(). #PF need not be intercepted even in real-mode if
 * we have Nested Paging support.
 */
#define VMX_REAL_MODE_XCPT_BITMAP  ( RT_BIT(X86_XCPT_DE)             | RT_BIT(X86_XCPT_DB)    | RT_BIT(X86_XCPT_NMI)   \
                                   | RT_BIT(X86_XCPT_BP)             | RT_BIT(X86_XCPT_OF)    | RT_BIT(X86_XCPT_BR)    \
                                   | RT_BIT(X86_XCPT_UD)            /* RT_BIT(X86_XCPT_NM) */ | RT_BIT(X86_XCPT_DF)    \
                                   | RT_BIT(X86_XCPT_CO_SEG_OVERRUN) | RT_BIT(X86_XCPT_TS)    | RT_BIT(X86_XCPT_NP)    \
                                   | RT_BIT(X86_XCPT_SS)             | RT_BIT(X86_XCPT_GP)   /* RT_BIT(X86_XCPT_PF) */ \
                                   | RT_BIT(X86_XCPT_MF)             | RT_BIT(X86_XCPT_AC)    | RT_BIT(X86_XCPT_MC)    \
                                   | RT_BIT(X86_XCPT_XF))

/* Maximum VM-instruction error number. */
#define VMX_INSTR_ERROR_MAX     28

/*******************************************************************************
*   Structures and Typedefs                                                    *
*******************************************************************************/
/**
 * A state structure for holding miscellaneous information across
 * VMX non-root operation and restored after the transition.
 */
typedef struct VMXTRANSIENT
{
    /** The host's rflags/eflags. */
    RTCCUINTREG     uEFlags;
    /** The guest's LSTAR MSR value used for TPR patching for 32-bit guests. */
    uint64_t        u64LStarMsr;
    /** The guest's TPR value used for TPR shadowing. */
    uint8_t         u8GuestTpr;

    /** The basic VM-exit reason. */
    uint16_t        uExitReason;
    /** The VM-exit exit qualification. */
    RTGCUINTPTR     uExitQualification;
    /** The VM-exit interruption error code. */
    uint32_t        uExitIntrErrorCode;

    /** The VM-exit interruption-information field. */
    uint32_t        uExitIntrInfo;
    /** Whether the VM-entry failed or not. */
    bool            fVMEntryFailed;
    /** The VM-exit instruction-length field. */
    uint32_t        cbInstr;

    /** The VM-entry interruption-information field. */
    uint32_t        uEntryIntrInfo;
    /** The VM-entry exception error code field. */
    uint32_t        uEntryXcptErrorCode;
    /** The VM-entry instruction length field. */
    uint32_t        cbEntryInstr;

    /** IDT-vectoring information field. */
    uint32_t        uIdtVectoringInfo;
    /** IDT-vectoring error code. */
    uint32_t        uIdtVectoringErrorCode;

    /** Mask of currently read VMCS fields; VMX_TRANSIENT_*. */
    uint32_t        fVmcsFieldsRead;
    /** Whether TSC-offsetting should be setup before VM-entry. */
    bool            fUpdateTscOffsettingAndPreemptTimer;
    /** Whether the VM-exit was caused by a page-fault during delivery of a
     *  contributary exception or a page-fault. */
    bool            fVectoringPF;
} VMXTRANSIENT, *PVMXTRANSIENT;

/**
 * MSR-bitmap read permissions.
 */
typedef enum VMXMSREXITREAD
{
    /** Reading this MSR causes a VM-exit. */
    VMXMSREXIT_INTERCEPT_READ = 0xb,
    /** Reading this MSR does not cause a VM-exit. */
    VMXMSREXIT_PASSTHRU_READ
} VMXMSREXITREAD;

/**
 * MSR-bitmap write permissions.
 */
typedef enum VMXMSREXITWRITE
{
    /** Writing to this MSR causes a VM-exit. */
    VMXMSREXIT_INTERCEPT_WRITE = 0xd,
    /** Writing to this MSR does not cause a VM-exit. */
    VMXMSREXIT_PASSTHRU_WRITE
} VMXMSREXITWRITE;

/*******************************************************************************
*   Internal Functions                                                         *
*******************************************************************************/
static void               hmR0VmxFlushVpid(PVM pVM, PVMCPU pVCpu, VMX_FLUSH_VPID enmFlush, RTGCPTR GCPtr);
static int                hmR0VmxInjectEventVmcs(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, uint64_t u64IntrInfo,
                                                 uint32_t cbInstr, uint32_t u32ErrCode);
#if 0
DECLINLINE(int)           hmR0VmxHandleExit(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient,
                                            unsigned rcReason);
#endif

static DECLCALLBACK(int)  hmR0VmxExitXcptNmi(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitExtInt(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitTripleFault(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitInitSignal(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitSipi(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitIoSmi(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitSmi(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitIntWindow(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitNmiWindow(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitTaskSwitch(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitCpuid(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitGetsec(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitHlt(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitInvd(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitInvlpg(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitRdpmc(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitRdtsc(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitRsm(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitInjectXcptUD(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitMovCRx(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitMovDRx(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitIoInstr(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitRdmsr(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitWrmsr(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitErrInvalidGuestState(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitErrMsrLoad(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitErrUndefined(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitMwait(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitMtf(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitMonitor(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitPause(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitErrMachineCheck(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitTprBelowThreshold(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitApicAccess(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitXdtrAccess(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitXdtrAccess(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitEptViolation(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitEptMisconfig(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitRdtscp(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitPreemptionTimer(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitWbinvd(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitXsetbv(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitRdrand(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitInvpcid(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitXcptNM(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitXcptPF(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitXcptMF(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitXcptDB(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitXcptBP(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitXcptGP(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
static DECLCALLBACK(int)  hmR0VmxExitXcptGeneric(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);

/*******************************************************************************
*   Global Variables                                                           *
*******************************************************************************/
/** @todo Move this to hm_vmx.h. */
/**
 * VM-exit handler.
 *
 * @returns VBox status code.
 * @param   pVM             Pointer to the VM.
 * @param   pVCpu           Pointer to the VMCPU.
 * @param   pMixedCtx       Pointer to the guest-CPU context. The data may be
 *                          out-of-sync. Make sure to update the required
 *                          fields before using them.
 * @param   pVmxTransient   Pointer to the VMX-transient structure.
 */
typedef DECLCALLBACK(int) FNVMEXITHANDLER(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient);
/** Pointer to VM-exit handler. */
typedef FNVMEXITHANDLER *PFNVMEXITHANDLER;

static const PFNVMEXITHANDLER s_apfnVMExitHandlers[VMX_EXIT_MAX + 1] =
{
 /* 00  VMX_EXIT_XCPT_NMI                */  hmR0VmxExitXcptNmi,
 /* 01  VMX_EXIT_EXT_INT                 */  hmR0VmxExitExtInt,
 /* 02  VMX_EXIT_TRIPLE_FAULT            */  hmR0VmxExitTripleFault,
 /* 03  VMX_EXIT_INIT_SIGNAL             */  hmR0VmxExitInitSignal,
 /* 04  VMX_EXIT_SIPI                    */  hmR0VmxExitSipi,
 /* 05  VMX_EXIT_IO_SMI                  */  hmR0VmxExitIoSmi,
 /* 06  VMX_EXIT_SMI                     */  hmR0VmxExitSmi,
 /* 07  VMX_EXIT_INT_WINDOW              */  hmR0VmxExitIntWindow,
 /* 08  VMX_EXIT_NMI_WINDOW              */  hmR0VmxExitNmiWindow,
 /* 09  VMX_EXIT_TASK_SWITCH             */  hmR0VmxExitTaskSwitch,
 /* 10  VMX_EXIT_CPUID                   */  hmR0VmxExitCpuid,
 /* 11  VMX_EXIT_GETSEC                  */  hmR0VmxExitGetsec,
 /* 12  VMX_EXIT_HLT                     */  hmR0VmxExitHlt,
 /* 13  VMX_EXIT_INVD                    */  hmR0VmxExitInvd,
 /* 14  VMX_EXIT_INVLPG                  */  hmR0VmxExitInvlpg,
 /* 15  VMX_EXIT_RDPMC                   */  hmR0VmxExitRdpmc,
 /* 16  VMX_EXIT_RDTSC                   */  hmR0VmxExitRdtsc,
 /* 17  VMX_EXIT_RSM                     */  hmR0VmxExitRsm,
 /* 18  VMX_EXIT_VMCALL                  */  hmR0VmxExitInjectXcptUD,
 /* 19  VMX_EXIT_VMCLEAR                 */  hmR0VmxExitInjectXcptUD,
 /* 20  VMX_EXIT_VMLAUNCH                */  hmR0VmxExitInjectXcptUD,
 /* 21  VMX_EXIT_VMPTRLD                 */  hmR0VmxExitInjectXcptUD,
 /* 22  VMX_EXIT_VMPTRST                 */  hmR0VmxExitInjectXcptUD,
 /* 23  VMX_EXIT_VMREAD                  */  hmR0VmxExitInjectXcptUD,
 /* 24  VMX_EXIT_VMRESUME                */  hmR0VmxExitInjectXcptUD,
 /* 25  VMX_EXIT_VMWRITE                 */  hmR0VmxExitInjectXcptUD,
 /* 26  VMX_EXIT_VMXOFF                  */  hmR0VmxExitInjectXcptUD,
 /* 27  VMX_EXIT_VMXON                   */  hmR0VmxExitInjectXcptUD,
 /* 28  VMX_EXIT_MOV_CRX                 */  hmR0VmxExitMovCRx,
 /* 29  VMX_EXIT_MOV_DRX                 */  hmR0VmxExitMovDRx,
 /* 30  VMX_EXIT_IO_INSTR                */  hmR0VmxExitIoInstr,
 /* 31  VMX_EXIT_RDMSR                   */  hmR0VmxExitRdmsr,
 /* 32  VMX_EXIT_WRMSR                   */  hmR0VmxExitWrmsr,
 /* 33  VMX_EXIT_ERR_INVALID_GUEST_STATE */  hmR0VmxExitErrInvalidGuestState,
 /* 34  VMX_EXIT_ERR_MSR_LOAD            */  hmR0VmxExitErrMsrLoad,
 /* 35  UNDEFINED                        */  hmR0VmxExitErrUndefined,
 /* 36  VMX_EXIT_MWAIT                   */  hmR0VmxExitMwait,
 /* 37  VMX_EXIT_MTF                     */  hmR0VmxExitMtf,
 /* 38  UNDEFINED                        */  hmR0VmxExitErrUndefined,
 /* 39  VMX_EXIT_MONITOR                 */  hmR0VmxExitMonitor,
 /* 40  UNDEFINED                        */  hmR0VmxExitPause,
 /* 41  VMX_EXIT_PAUSE                   */  hmR0VmxExitErrMachineCheck,
 /* 42  VMX_EXIT_ERR_MACHINE_CHECK       */  hmR0VmxExitErrUndefined,
 /* 43  VMX_EXIT_TPR_BELOW_THRESHOLD     */  hmR0VmxExitTprBelowThreshold,
 /* 44  VMX_EXIT_APIC_ACCESS             */  hmR0VmxExitApicAccess,
 /* 45  UNDEFINED                        */  hmR0VmxExitErrUndefined,
 /* 46  VMX_EXIT_XDTR_ACCESS             */  hmR0VmxExitXdtrAccess,
 /* 47  VMX_EXIT_TR_ACCESS               */  hmR0VmxExitXdtrAccess,
 /* 48  VMX_EXIT_EPT_VIOLATION           */  hmR0VmxExitEptViolation,
 /* 49  VMX_EXIT_EPT_MISCONFIG           */  hmR0VmxExitEptMisconfig,
 /* 50  VMX_EXIT_INVEPT                  */  hmR0VmxExitInjectXcptUD,
 /* 51  VMX_EXIT_RDTSCP                  */  hmR0VmxExitRdtscp,
 /* 52  VMX_EXIT_PREEMPTION_TIMER        */  hmR0VmxExitPreemptionTimer,
 /* 53  VMX_EXIT_INVVPID                 */  hmR0VmxExitInjectXcptUD,
 /* 54  VMX_EXIT_WBINVD                  */  hmR0VmxExitWbinvd,
 /* 55  VMX_EXIT_XSETBV                  */  hmR0VmxExitXsetbv,
 /* 56  UNDEFINED                        */  hmR0VmxExitErrUndefined,
 /* 57  VMX_EXIT_RDRAND                  */  hmR0VmxExitRdrand,
 /* 58  VMX_EXIT_INVPCID                 */  hmR0VmxExitInvpcid,
 /* 59  VMX_EXIT_VMFUNC                  */  hmR0VmxExitInjectXcptUD
};

static const char* const s_apszVmxInstrErrors[VMX_INSTR_ERROR_MAX + 1] =
{
    /*  0 */ "(Not Used)",
    /*  1 */ "VMCALL executed in VMX root operation.",
    /*  2 */ "VMCLEAR with invalid physical address.",
    /*  3 */ "VMCLEAR with VMXON pointer.",
    /*  4 */ "VMLAUNCH with non-clear VMCS.",
    /*  5 */ "VMRESUME with non-launched VMCS.",
    /*  6 */ "VMRESUME after VMXOFF",
    /*  7 */ "VM entry with invalid control fields.",
    /*  8 */ "VM entry with invalid host state fields.",
    /*  9 */ "VMPTRLD with invalid physical address.",
    /* 10 */ "VMPTRLD with VMXON pointer.",
    /* 11 */ "VMPTRLD with incorrect revision identifier.",
    /* 12 */ "VMREAD/VMWRITE from/to unsupported VMCS component.",
    /* 13 */ "VMWRITE to read-only VMCS component.",
    /* 14 */ "(Not Used)",
    /* 15 */ "VMXON executed in VMX root operation.",
    /* 16 */ "VM entry with invalid executive-VMCS pointer.",
    /* 17 */ "VM entry with non-launched executing VMCS.",
    /* 18 */ "VM entry with executive-VMCS pointer not VMXON pointer.",
    /* 19 */ "VMCALL with non-clear VMCS.",
    /* 20 */ "VMCALL with invalid VM-exit control fields.",
    /* 21 */ "(Not Used)",
    /* 22 */ "VMCALL with incorrect MSEG revision identifier.",
    /* 23 */ "VMXOFF under dual monitor treatment of SMIs and SMM.",
    /* 24 */ "VMCALL with invalid SMM-monitor features.",
    /* 25 */ "VM entry with invalid VM-execution control fields in executive VMCS.",
    /* 26 */ "VM entry with events blocked by MOV SS.",
    /* 27 */ "(Not Used)",
    /* 28 */ "Invalid operand to INVEPT/INVVPID."
};


/**
 * Updates the VM's last error record. If there was a VMX instruction error,
 * reads the error data from the VMCS and updates VCPU's last error record as
 * well.
 *
 * @param    pVM        Pointer to the VM.
 * @param    pVCpu      Pointer to the VMCPU (can be NULL if @a rc is not
 *                      VERR_VMX_GENERIC).
 * @param    rc         The error code.
 */
static void hmR0VmxUpdateErrorRecord(PVM pVM, PVMCPU pVCpu, int rc)
{
    AssertPtr(pVM);
    if (rc == VERR_VMX_GENERIC)
    {
        AssertPtrReturnVoid(pVCpu);
        VMXReadVmcs32(VMX_VMCS32_RO_VM_INSTR_ERROR, &pVCpu->hm.s.vmx.lasterror.u32InstrError);
    }
    pVM->hm.s.lLastError = rc;
}


/**
 * Reads the VM-entry interruption-information field from the VMCS into the VMX
 * transient structure.
 *
 * @returns VBox status code.
 * @param   pVmxTransient   Pointer to the VMX transient structure.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxReadEntryIntrInfoVmcs(PVMXTRANSIENT pVmxTransient)
{
    int rc = VMXReadVmcs32(VMX_VMCS32_CTRL_ENTRY_INTERRUPTION_INFO, &pVmxTransient->uEntryIntrInfo);
    AssertRCReturn(rc, rc);
    return VINF_SUCCESS;
}


/**
 * Reads the VM-entry exception error code field from the VMCS into
 * the VMX transient structure.
 *
 * @returns VBox status code.
 * @param   pVmxTransient   Pointer to the VMX transient structure.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxReadEntryXcptErrorCodeVmcs(PVMXTRANSIENT pVmxTransient)
{
    int rc = VMXReadVmcs32(VMX_VMCS32_CTRL_ENTRY_EXCEPTION_ERRCODE, &pVmxTransient->uEntryXcptErrorCode);
    AssertRCReturn(rc, rc);
    return VINF_SUCCESS;
}


/**
 * Reads the VM-entry exception error code field from the VMCS into
 * the VMX transient structure.
 *
 * @returns VBox status code.
 * @param   pVmxTransient   Pointer to the VMX transient structure.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxReadEntryInstrLenVmcs(PVMXTRANSIENT pVmxTransient)
{
    int rc = VMXReadVmcs32(VMX_VMCS32_CTRL_ENTRY_INSTR_LENGTH, &pVmxTransient->cbEntryInstr);
    AssertRCReturn(rc, rc);
    return VINF_SUCCESS;
}


/**
 * Reads the VM-exit interruption-information field from the VMCS into the VMX
 * transient structure.
 *
 * @returns VBox status code.
 * @param   pVmxTransient   Pointer to the VMX transient structure.
 */
DECLINLINE(int) hmR0VmxReadExitIntrInfoVmcs(PVMXTRANSIENT pVmxTransient)
{
    if (!(pVmxTransient->fVmcsFieldsRead & VMX_TRANSIENT_EXIT_INTERRUPTION_INFO))
    {
        int rc = VMXReadVmcs32(VMX_VMCS32_RO_EXIT_INTERRUPTION_INFO, &pVmxTransient->uExitIntrInfo);
        AssertRCReturn(rc, rc);
        pVmxTransient->fVmcsFieldsRead |= VMX_TRANSIENT_EXIT_INTERRUPTION_INFO;
    }
    return VINF_SUCCESS;
}


/**
 * Reads the VM-exit interruption error code from the VMCS into the VMX
 * transient structure.
 *
 * @returns VBox status code.
 * @param   pVmxTransient   Pointer to the VMX transient structure.
 */
DECLINLINE(int) hmR0VmxReadExitIntrErrorCodeVmcs(PVMXTRANSIENT pVmxTransient)
{
    if (!(pVmxTransient->fVmcsFieldsRead & VMX_TRANSIENT_EXIT_INTERRUPTION_ERROR_CODE))
    {
        int rc = VMXReadVmcs32(VMX_VMCS32_RO_EXIT_INTERRUPTION_ERROR_CODE, &pVmxTransient->uExitIntrErrorCode);
        AssertRCReturn(rc, rc);
        pVmxTransient->fVmcsFieldsRead |= VMX_TRANSIENT_EXIT_INTERRUPTION_ERROR_CODE;
    }
    return VINF_SUCCESS;
}


/**
 * Reads the VM-exit instruction length field from the VMCS into the VMX
 * transient structure.
 *
 * @returns VBox status code.
 * @param   pVmxTransient   Pointer to the VMX transient structure.
 */
DECLINLINE(int) hmR0VmxReadExitInstrLenVmcs(PVMXTRANSIENT pVmxTransient)
{
    if (!(pVmxTransient->fVmcsFieldsRead & VMX_TRANSIENT_EXIT_INSTR_LEN))
    {
        int rc = VMXReadVmcs32(VMX_VMCS32_RO_EXIT_INSTR_LENGTH, &pVmxTransient->cbInstr);
        AssertRCReturn(rc, rc);
        pVmxTransient->fVmcsFieldsRead |= VMX_TRANSIENT_EXIT_INSTR_LEN;
    }
    return VINF_SUCCESS;
}


/**
 * Reads the exit qualification from the VMCS into the VMX transient structure.
 *
 * @returns VBox status code.
 * @param   pVmxTransient   Pointer to the VMX transient structure.
 */
DECLINLINE(int) hmR0VmxReadExitQualificationVmcs(PVMXTRANSIENT pVmxTransient)
{
    if (!(pVmxTransient->fVmcsFieldsRead & VMX_TRANSIENT_EXIT_QUALIFICATION))
    {
        int rc = VMXReadVmcsGstN(VMX_VMCS_RO_EXIT_QUALIFICATION, &pVmxTransient->uExitQualification);
        AssertRCReturn(rc, rc);
        pVmxTransient->fVmcsFieldsRead |= VMX_TRANSIENT_EXIT_QUALIFICATION;
    }
    return VINF_SUCCESS;
}


/**
 * Reads the IDT-vectoring information field from the VMCS into the VMX
 * transient structure.
 *
 * @returns VBox status code.
 * @param   pVmxTransient   Pointer to the VMX transient structure.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxReadIdtVectoringInfoVmcs(PVMXTRANSIENT pVmxTransient)
{
    if (!(pVmxTransient->fVmcsFieldsRead & VMX_TRANSIENT_IDT_VECTORING_INFO))
    {
        int rc = VMXReadVmcs32(VMX_VMCS32_RO_IDT_INFO, &pVmxTransient->uIdtVectoringInfo);
        AssertRCReturn(rc, rc);
        pVmxTransient->fVmcsFieldsRead |= VMX_TRANSIENT_IDT_VECTORING_INFO;
    }
    return VINF_SUCCESS;
}


/**
 * Reads the IDT-vectoring error code from the VMCS into the VMX
 * transient structure.
 *
 * @returns VBox status code.
 * @param   pVmxTransient   Pointer to the VMX transient structure.
 */
DECLINLINE(int) hmR0VmxReadIdtVectoringErrorCodeVmcs(PVMXTRANSIENT pVmxTransient)
{
    if (!(pVmxTransient->fVmcsFieldsRead & VMX_TRANSIENT_IDT_VECTORING_ERROR_CODE))
    {
        int rc = VMXReadVmcs32(VMX_VMCS32_RO_IDT_ERROR_CODE, &pVmxTransient->uIdtVectoringErrorCode);
        AssertRCReturn(rc, rc);
        pVmxTransient->fVmcsFieldsRead |= VMX_TRANSIENT_IDT_VECTORING_ERROR_CODE;
    }
    return VINF_SUCCESS;
}


/**
 * Enters VMX root mode operation on the current CPU.
 *
 * @returns VBox status code.
 * @param   pVM                 Pointer to the VM (optional, can be NULL, after
 *                              a resume).
 * @param   HCPhysCpuPage       Physical address of the VMXON region.
 * @param   pvCpuPage           Pointer to the VMXON region.
 */
DECLINLINE(int) hmR0VmxEnterRootMode(PVM pVM, RTHCPHYS HCPhysCpuPage, void *pvCpuPage)
{
    AssertReturn(HCPhysCpuPage != 0 && HCPhysCpuPage != NIL_RTHCPHYS, VERR_INVALID_PARAMETER);
    AssertReturn(pvCpuPage, VERR_INVALID_PARAMETER);

    if (pVM)
    {
        /* Write the VMCS revision dword to the VMXON region. */
        *(uint32_t *)pvCpuPage = MSR_IA32_VMX_BASIC_INFO_VMCS_ID(pVM->hm.s.vmx.msr.vmx_basic_info);
    }

    /* Disable interrupts. Interrupts handlers might, in theory, change CR4. */
    RTCCUINTREG fFlags = ASMIntDisableFlags();

    /* Enable the VMX bit in CR4. */
    RTCCUINTREG uCr4 = ASMGetCR4();
    if (!(uCr4 & X86_CR4_VMXE))
        ASMSetCR4(uCr4 | X86_CR4_VMXE);

    /* Enter VMXON root mode. */
    int rc = VMXEnable(HCPhysCpuPage);

    /* Restore interrupts. */
    ASMSetFlags(fFlags);
    return rc;
}


/**
 * Exits VMX root mode operation on the current CPU.
 *
 * @returns VBox status code.
 */
static int hmR0VmxLeaveRootMode(void)
{
    /* Disable interrupts. Interrupt handlers might, in theory, change CR4. */
    RTCCUINTREG fFlags = ASMIntDisableFlags();
    int         rc     = VINF_SUCCESS;

    /* If we're for some reason not in VMX root mode, then don't leave it. */
    if (ASMGetCR4() & X86_CR4_VMXE)
    {
        /* Exit root mode using VMXOFF & clear the VMX bit in CR4 */
        VMXDisable();
        ASMSetCR4(ASMGetCR4() & ~X86_CR4_VMXE);
    }
    else
        rc = VERR_VMX_NOT_IN_VMX_ROOT_MODE;

    /* Restore interrupts. */
    ASMSetFlags(fFlags);
    return rc;
}


/**
 * Allocates and maps one physically contiguous page. The allocated page is
 * zero'd out. (Used by various VT-x structures).
 *
 * @returns IPRT status code.
 * @param   pMemObj         Pointer to the ring-0 memory object.
 * @param   ppVirt          Where to store the virtual address of the
 *                          allocation.
 * @param   pPhys           Where to store the physical address of the
 *                          allocation.
 */
DECLINLINE(int) hmR0VmxPageAllocZ(PRTR0MEMOBJ pMemObj, PRTR0PTR ppVirt, PRTHCPHYS pHCPhys)
{
    AssertPtrReturn(pMemObj, VERR_INVALID_PARAMETER);
    AssertPtrReturn(ppVirt, VERR_INVALID_PARAMETER);
    AssertPtrReturn(pHCPhys, VERR_INVALID_PARAMETER);

    int rc = RTR0MemObjAllocCont(pMemObj, PAGE_SIZE, false /* fExecutable */);
    if (RT_FAILURE(rc))
        return rc;
    *ppVirt  = RTR0MemObjAddress(*pMemObj);
    *pHCPhys = RTR0MemObjGetPagePhysAddr(*pMemObj, 0 /* iPage */);
    ASMMemZero32(*ppVirt, PAGE_SIZE);
    return VINF_SUCCESS;
}


/**
 * Frees and unmaps an allocated physical page.
 *
 * @param   pMemObj         Pointer to the ring-0 memory object.
 * @param   ppVirt          Where to re-initialize the virtual address of
 *                          allocation as 0.
 * @param   pHCPhys         Where to re-initialize the physical address of the
 *                          allocation as 0.
 */
DECLINLINE(void) hmR0VmxPageFree(PRTR0MEMOBJ pMemObj, PRTR0PTR ppVirt, PRTHCPHYS pHCPhys)
{
    AssertPtr(pMemObj);
    AssertPtr(ppVirt);
    AssertPtr(pHCPhys);
    if (*pMemObj != NIL_RTR0MEMOBJ)
    {
        int rc = RTR0MemObjFree(*pMemObj, true /* fFreeMappings */);
        AssertRC(rc);
        *pMemObj = NIL_RTR0MEMOBJ;
        *ppVirt  = 0;
        *pHCPhys = 0;
    }
}


/**
 * Worker function to free VT-x related structures.
 *
 * @returns IPRT status code.
 * @param   pVM             Pointer to the VM.
 */
static void hmR0VmxStructsFree(PVM pVM)
{
    for (VMCPUID i = 0; i < pVM->cCpus; i++)
    {
        PVMCPU pVCpu = &pVM->aCpus[i];
        AssertPtr(pVCpu);

        hmR0VmxPageFree(&pVCpu->hm.s.vmx.hMemObjHostMsr, &pVCpu->hm.s.vmx.pvHostMsr, &pVCpu->hm.s.vmx.HCPhysHostMsr);
        hmR0VmxPageFree(&pVCpu->hm.s.vmx.hMemObjGuestMsr, &pVCpu->hm.s.vmx.pvGuestMsr, &pVCpu->hm.s.vmx.HCPhysGuestMsr);

        if (pVM->hm.s.vmx.msr.vmx_proc_ctls.n.allowed1 & VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_USE_MSR_BITMAPS)
            hmR0VmxPageFree(&pVCpu->hm.s.vmx.hMemObjMsrBitmap, &pVCpu->hm.s.vmx.pvMsrBitmap, &pVCpu->hm.s.vmx.HCPhysMsrBitmap);

        hmR0VmxPageFree(&pVCpu->hm.s.vmx.hMemObjVirtApic, (PRTR0PTR)&pVCpu->hm.s.vmx.pbVirtApic, &pVCpu->hm.s.vmx.HCPhysVirtApic);
        hmR0VmxPageFree(&pVCpu->hm.s.vmx.hMemObjVmcs, &pVCpu->hm.s.vmx.pvVmcs, &pVCpu->hm.s.vmx.HCPhysVmcs);
    }

    hmR0VmxPageFree(&pVM->hm.s.vmx.hMemObjApicAccess, (PRTR0PTR)&pVM->hm.s.vmx.pbApicAccess, &pVM->hm.s.vmx.HCPhysApicAccess);
#ifdef VBOX_WITH_CRASHDUMP_MAGIC
    hmR0VmxPageFree(&pVM->hm.s.vmx.hMemObjScratch, &pVM->hm.s.vmx.pbScratch, &pVM->hm.s.vmx.HCPhysScratch);
#endif
}


/**
 * Worker function to allocate VT-x related VM structures.
 *
 * @returns IPRT status code.
 * @param   pVM             Pointer to the VM.
 */
static int hmR0VmxStructsAlloc(PVM pVM)
{
    /*
     * Initialize members up-front so we can cleanup properly on allocation failure.
     */
#define VMXLOCAL_INIT_VM_MEMOBJ(a_Name, a_VirtPrefix)       \
    pVM->hm.s.vmx.hMemObj##a_Name = NIL_RTR0MEMOBJ;         \
    pVM->hm.s.vmx.a_VirtPrefix##a_Name = 0;                 \
    pVM->hm.s.vmx.HCPhys##a_Name = 0;

#define VMXLOCAL_INIT_VMCPU_MEMOBJ(a_Name, a_VirtPrefix)    \
    pVCpu->hm.s.vmx.hMemObj##a_Name = NIL_RTR0MEMOBJ;       \
    pVCpu->hm.s.vmx.a_VirtPrefix##a_Name = 0;               \
    pVCpu->hm.s.vmx.HCPhys##a_Name = 0;

#ifdef VBOX_WITH_CRASHDUMP_MAGIC
    VMXLOCAL_INIT_VM_MEMOBJ(Scratch, pv);
#endif
    VMXLOCAL_INIT_VM_MEMOBJ(ApicAccess, pb);

    AssertCompile(sizeof(VMCPUID) == sizeof(pVM->cCpus));
    for (VMCPUID i = 0; i < pVM->cCpus; i++)
    {
        PVMCPU pVCpu = &pVM->aCpus[i];
        VMXLOCAL_INIT_VMCPU_MEMOBJ(Vmcs, pv);
        VMXLOCAL_INIT_VMCPU_MEMOBJ(VirtApic, pb);
        VMXLOCAL_INIT_VMCPU_MEMOBJ(MsrBitmap, pv);
        VMXLOCAL_INIT_VMCPU_MEMOBJ(GuestMsr, pv);
        VMXLOCAL_INIT_VMCPU_MEMOBJ(HostMsr, pv);
    }
#undef VMXLOCAL_INIT_VMCPU_MEMOBJ
#undef VMXLOCAL_INIT_VM_MEMOBJ

    /*
     * Allocate all the VT-x structures.
     */
    int rc = VINF_SUCCESS;
#ifdef VBOX_WITH_CRASHDUMP_MAGIC
    rc = hmR0VmxPageAllocZ(&pVM->hm.s.vmx.hMemObjScratch, &pVM->hm.s.vmx.pbScratch, &pVM->hm.s.vmx.HCPhysScratch);
    if (RT_FAILURE(rc))
        goto cleanup;
    strcpy((char *)pVM->hm.s.vmx.pbScratch, "SCRATCH Magic");
    *(uint64_t *)(pVM->hm.s.vmx.pbScratch + 16) = UINT64_C(0xDEADBEEFDEADBEEF);
#endif

    /* Allocate the APIC-access page for trapping APIC accesses from the guest. */
    if (pVM->hm.s.vmx.msr.vmx_proc_ctls2.n.allowed1 & VMX_VMCS_CTRL_PROC_EXEC2_VIRT_APIC)
    {
        rc = hmR0VmxPageAllocZ(&pVM->hm.s.vmx.hMemObjApicAccess, (PRTR0PTR)&pVM->hm.s.vmx.pbApicAccess,
                               &pVM->hm.s.vmx.HCPhysApicAccess);
        if (RT_FAILURE(rc))
            goto cleanup;
    }

    /*
     * Initialize per-VCPU VT-x structures.
     */
    for (VMCPUID i =0; i < pVM->cCpus; i++)
    {
        PVMCPU pVCpu = &pVM->aCpus[i];
        AssertPtr(pVCpu);

        /* Allocate the VM control structure (VMCS). */
        AssertReturn(MSR_IA32_VMX_BASIC_INFO_VMCS_SIZE(pVM->hm.s.vmx.msr.vmx_basic_info) <= PAGE_SIZE, VERR_INTERNAL_ERROR);
        rc = hmR0VmxPageAllocZ(&pVCpu->hm.s.vmx.hMemObjVmcs, &pVCpu->hm.s.vmx.pvVmcs, &pVCpu->hm.s.vmx.HCPhysVmcs);
        if (RT_FAILURE(rc))
            goto cleanup;

        /* Allocate the Virtual-APIC page for transparent TPR accesses. */
        if (pVM->hm.s.vmx.msr.vmx_proc_ctls.n.allowed1 & VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_USE_TPR_SHADOW)
        {
            rc = hmR0VmxPageAllocZ(&pVCpu->hm.s.vmx.hMemObjVirtApic, (PRTR0PTR)&pVCpu->hm.s.vmx.pbVirtApic,
                                   &pVCpu->hm.s.vmx.HCPhysVirtApic);
            if (RT_FAILURE(rc))
                goto cleanup;
        }

        /* Allocate the MSR-bitmap if supported by the CPU. The MSR-bitmap is for transparent accesses of specific MSRs. */
        if (pVM->hm.s.vmx.msr.vmx_proc_ctls.n.allowed1 & VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_USE_MSR_BITMAPS)
        {
            rc = hmR0VmxPageAllocZ(&pVCpu->hm.s.vmx.hMemObjMsrBitmap, &pVCpu->hm.s.vmx.pvMsrBitmap,
                                   &pVCpu->hm.s.vmx.HCPhysMsrBitmap);
            if (RT_FAILURE(rc))
                goto cleanup;
            memset(pVCpu->hm.s.vmx.pvMsrBitmap, 0xff, PAGE_SIZE);
        }

        /* Allocate the VM-entry MSR-load and VM-exit MSR-store page for the guest MSRs. */
        rc = hmR0VmxPageAllocZ(&pVCpu->hm.s.vmx.hMemObjGuestMsr, &pVCpu->hm.s.vmx.pvGuestMsr, &pVCpu->hm.s.vmx.HCPhysGuestMsr);
        if (RT_FAILURE(rc))
            goto cleanup;

        /* Allocate the VM-exit MSR-load page for the host MSRs. */
        rc = hmR0VmxPageAllocZ(&pVCpu->hm.s.vmx.hMemObjHostMsr, &pVCpu->hm.s.vmx.pvHostMsr, &pVCpu->hm.s.vmx.HCPhysHostMsr);
        if (RT_FAILURE(rc))
            goto cleanup;
    }

    return VINF_SUCCESS;

cleanup:
    hmR0VmxStructsFree(pVM);
    return rc;
}


/**
 * Does global VT-x initialization (called during module initialization).
 *
 * @returns VBox status code.
 */
VMMR0DECL(int) VMXR0GlobalInit(void)
{
    /* Setup the main VM exit handlers. */
    AssertCompile(VMX_EXIT_MAX + 1 == RT_ELEMENTS(s_apfnVMExitHandlers));
#ifdef DEBUG
    for (unsigned i = 0; i < RT_ELEMENTS(s_apfnVMExitHandlers); i++)
        Assert(s_apfnVMExitHandlers[i]);
#endif
    return VINF_SUCCESS;
}


/**
 * Does global VT-x termination (called during module termination).
 */
VMMR0DECL(void) VMXR0GlobalTerm()
{
    /* Nothing to do currently. */
}


/**
 * Sets up and activates VT-x on the current CPU.
 *
 * @returns VBox status code.
 * @param   pCpu            Pointer to the global CPU info struct.
 * @param   pVM             Pointer to the VM (can be NULL after a host resume
 *                          operation).
 * @param   pvCpuPage       Pointer to the VMXON region (can be NULL if @a
 *                          fEnabledByHost is true).
 * @param   HCPhysCpuPage   Physical address of the VMXON region (can be 0 if
 *                          @a fEnabledByHost is true).
 * @param   fEnabledByHost  Set if SUPR0EnableVTx() or similar was used to
 *                          enable VT-x/AMD-V on the host.
 */
VMMR0DECL(int) VMXR0EnableCpu(PHMGLOBLCPUINFO pCpu, PVM pVM, void *pvCpuPage, RTHCPHYS HCPhysCpuPage, bool fEnabledByHost)
{
    AssertReturn(pCpu, VERR_INVALID_PARAMETER);
    Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));

    if (!fEnabledByHost)
    {
        int rc = hmR0VmxEnterRootMode(pVM, HCPhysCpuPage, pvCpuPage);
        if (RT_FAILURE(rc))
            return rc;
    }

    /*
     * Flush all VPIDs (in case we or any other hypervisor have been using VPIDs) so that
     * we can avoid an explicit flush while using new VPIDs. We would still need to flush
     * each time while reusing a VPID after hitting the MaxASID limit once.
     */
    if (   pVM
        && pVM->hm.s.vmx.fVpid
        && (pVM->hm.s.vmx.msr.vmx_ept_vpid_caps & MSR_IA32_VMX_EPT_VPID_CAP_INVVPID_ALL_CONTEXTS))
    {
        hmR0VmxFlushVpid(pVM, NULL /* pvCpu */, VMX_FLUSH_VPID_ALL_CONTEXTS, 0 /* GCPtr */);
        pCpu->fFlushAsidBeforeUse = false;
    }
    else
        pCpu->fFlushAsidBeforeUse = true;

    /* Ensure each VCPU scheduled on this CPU gets a new VPID on resume. See @bugref{6255}. */
    ++pCpu->cTlbFlushes;

    return VINF_SUCCESS;
}


/**
 * Deactivates VT-x on the current CPU.
 *
 * @returns VBox status code.
 * @param   pCpu            Pointer to the global CPU info struct.
 * @param   pvCpuPage       Pointer to the VMXON region.
 * @param   HCPhysCpuPage   Physical address of the VMXON region.
 */
VMMR0DECL(int) VMXR0DisableCpu(PHMGLOBLCPUINFO pCpu, void *pvCpuPage, RTHCPHYS HCPhysCpuPage)
{
    NOREF(pCpu);
    NOREF(pvCpuPage);
    NOREF(HCPhysCpuPage);

    hmR0VmxLeaveRootMode();
    return VINF_SUCCESS;
}


/**
 * Sets the permission bits for the specified MSR in the MSR bitmap.
 *
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   ulMSR       The MSR value.
 * @param   enmRead     Whether reading this MSR causes a VM-exit.
 * @param   enmWrite    Whether writing this MSR causes a VM-exit.
 */
static void hmR0VmxSetMsrPermission(PVMCPU pVCpu, unsigned ulMsr, VMXMSREXITREAD enmRead, VMXMSREXITWRITE enmWrite)
{
    unsigned ulBit;
    uint8_t *pbMsrBitmap = (uint8_t *)pVCpu->hm.s.vmx.pvMsrBitmap;

    /*
     * Layout:
     * 0x000 - 0x3ff - Low MSR read bits
     * 0x400 - 0x7ff - High MSR read bits
     * 0x800 - 0xbff - Low MSR write bits
     * 0xc00 - 0xfff - High MSR write bits
     */
    if (ulMsr <= 0x00001FFF)
    {
        /* Pentium-compatible MSRs */
        ulBit = ulMsr;
    }
    else if (   ulMsr >= 0xC0000000
             && ulMsr <= 0xC0001FFF)
    {
        /* AMD Sixth Generation x86 Processor MSRs */
        ulBit = (ulMsr - 0xC0000000);
        pbMsrBitmap += 0x400;
    }
    else
    {
        AssertMsgFailed(("Invalid MSR %lx\n", ulMsr));
        return;
    }

    Assert(ulBit <= 0x1fff);
    if (enmRead == VMXMSREXIT_INTERCEPT_READ)
        ASMBitSet(pbMsrBitmap, ulBit);
    else
        ASMBitClear(pbMsrBitmap, ulBit);

    if (enmWrite == VMXMSREXIT_INTERCEPT_WRITE)
        ASMBitSet(pbMsrBitmap + 0x800, ulBit);
    else
        ASMBitClear(pbMsrBitmap + 0x800, ulBit);
}


/**
 * Flushes the TLB using EPT.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   enmFlush    Type of flush.
 */
static void hmR0VmxFlushEpt(PVM pVM, PVMCPU pVCpu, VMX_FLUSH_EPT enmFlush)
{
    AssertPtr(pVM);
    Assert(pVM->hm.s.fNestedPaging);

    LogFlowFunc(("pVM=%p pVCpu=%p enmFlush=%d\n", pVM, pVCpu, enmFlush));

    uint64_t descriptor[2];
    descriptor[0] = pVCpu->hm.s.vmx.GCPhysEPTP;
    descriptor[1] = 0;                           /* MBZ. Intel spec. 33.3 "VMX Instructions" */

    int rc = VMXR0InvEPT(enmFlush, &descriptor[0]);
    AssertMsg(rc == VINF_SUCCESS, ("VMXR0InvEPT %#x %RGv failed with %Rrc\n", enmFlush, pVCpu->hm.s.vmx.GCPhysEPTP, rc));
    STAM_COUNTER_INC(&pVCpu->hm.s.StatFlushNestedPaging);
}


/**
 * Flushes the TLB using VPID.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU (can be NULL depending on @a
 *                      enmFlush).
 * @param   enmFlush    Type of flush.
 * @param   GCPtr       Virtual address of the page to flush (can be 0 depending
 *                      on @a enmFlush).
 */
static void hmR0VmxFlushVpid(PVM pVM, PVMCPU pVCpu, VMX_FLUSH_VPID enmFlush, RTGCPTR GCPtr)
{
    AssertPtr(pVM);
    Assert(pVM->hm.s.vmx.fVpid);

    uint64_t descriptor[2];
    if (enmFlush == VMX_FLUSH_VPID_ALL_CONTEXTS)
    {
        descriptor[0] = 0;
        descriptor[1] = 0;
    }
    else
    {
        AssertPtr(pVCpu);
        AssertMsg(pVCpu->hm.s.uCurrentAsid != 0, ("VMXR0InvVPID: invalid ASID %lu\n", pVCpu->hm.s.uCurrentAsid));
        AssertMsg(pVCpu->hm.s.uCurrentAsid <= UINT16_MAX, ("VMXR0InvVPID: invalid ASID %lu\n", pVCpu->hm.s.uCurrentAsid));
        descriptor[0] = pVCpu->hm.s.uCurrentAsid;
        descriptor[1] = GCPtr;
    }

    int rc = VMXR0InvVPID(enmFlush, &descriptor[0]); NOREF(rc);
    AssertMsg(rc == VINF_SUCCESS,
              ("VMXR0InvVPID %#x %u %RGv failed with %d\n", enmFlush, pVCpu ? pVCpu->hm.s.uCurrentAsid : 0, GCPtr, rc));
    if (   RT_SUCCESS(rc)
        && pVCpu)
    {
        STAM_COUNTER_INC(&pVCpu->hm.s.StatFlushAsid);
    }
}


/**
 * Invalidates a guest page by guest virtual address. Only relevant for
 * EPT/VPID, otherwise there is nothing really to invalidate.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   GCVirt      Guest virtual address of the page to invalidate.
 */
VMMR0DECL(int) VMXR0InvalidatePage(PVM pVM, PVMCPU pVCpu, RTGCPTR GCVirt)
{
    AssertPtr(pVM);
    AssertPtr(pVCpu);
    LogFlowFunc(("pVM=%p pVCpu=%p GCVirt=%RGv\n", pVM, pVCpu, GCVirt));

    bool fFlushPending = VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_TLB_FLUSH);
    if (!fFlushPending)
    {
        /*
         * We must invalidate the guest TLB entry in either case, we cannot ignore it even for the EPT case
         * See @bugref{6043} and @bugref{6177}.
         *
         * Set the VMCPU_FF_TLB_FLUSH force flag and flush before VM-entry in hmR0VmxFlushTLB*() as this
         * function maybe called in a loop with individual addresses.
         */
        if (pVM->hm.s.vmx.fVpid)
        {
            if (pVM->hm.s.vmx.msr.vmx_ept_vpid_caps & MSR_IA32_VMX_EPT_VPID_CAP_INVVPID_INDIV_ADDR)
            {
                hmR0VmxFlushVpid(pVM, pVCpu, VMX_FLUSH_VPID_INDIV_ADDR, GCVirt);
                STAM_COUNTER_INC(&pVCpu->hm.s.StatFlushTlbInvlpgVirt);
            }
            else
                VMCPU_FF_SET(pVCpu, VMCPU_FF_TLB_FLUSH);
        }
        else if (pVM->hm.s.fNestedPaging)
            VMCPU_FF_SET(pVCpu, VMCPU_FF_TLB_FLUSH);
    }

    return VINF_SUCCESS;
}


/**
 * Invalidates a guest page by physical address. Only relevant for EPT/VPID,
 * otherwise there is nothing really to invalidate.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   GCPhys      Guest physical address of the page to invalidate.
 */
VMMR0DECL(int) VMXR0InvalidatePhysPage(PVM pVM, PVMCPU pVCpu, RTGCPHYS GCPhys)
{
    LogFlowFunc(("%RGp\n", GCPhys));

    /*
     * We cannot flush a page by guest-physical address. invvpid takes only a linear address while invept only flushes
     * by EPT not individual addresses. We update the force flag here and flush before the next VM-entry in hmR0VmxFlushTLB*().
     * This function might be called in a loop.
     */
    VMCPU_FF_SET(pVCpu, VMCPU_FF_TLB_FLUSH);
    STAM_COUNTER_INC(&pVCpu->hm.s.StatFlushTlbInvlpgPhys);
    return VINF_SUCCESS;
}


/**
 * Dummy placeholder for tagged-TLB flush handling before VM-entry. Used in the
 * case where neither EPT nor VPID is supported by the CPU.
 *
 * @param   pVM             Pointer to the VM.
 * @param   pVCpu           Pointer to the VMCPU.
 *
 * @remarks Called with interrupts disabled.
 */
static DECLCALLBACK(void) hmR0VmxFlushTaggedTlbNone(PVM pVM, PVMCPU pVCpu)
{
    NOREF(pVM);
    AssertPtr(pVCpu);
    VMCPU_FF_CLEAR(pVCpu, VMCPU_FF_TLB_FLUSH);
    VMCPU_FF_CLEAR(pVCpu, VMCPU_FF_TLB_SHOOTDOWN);

    PHMGLOBLCPUINFO pCpu = HMR0GetCurrentCpu();
    AssertPtr(pCpu);

    pVCpu->hm.s.TlbShootdown.cPages = 0;
    pVCpu->hm.s.idLastCpu           = pCpu->idCpu;
    pVCpu->hm.s.cTlbFlushes         = pCpu->cTlbFlushes;
    pVCpu->hm.s.fForceTLBFlush      = false;
    return;
}


/**
 * Flushes the tagged-TLB entries for EPT+VPID CPUs as necessary.
 *
 * @param    pVM            Pointer to the VM.
 * @param    pVCpu          Pointer to the VMCPU.
 * @remarks All references to "ASID" in this function pertains to "VPID" in
 *          Intel's nomenclature. The reason is, to avoid confusion in compare
 *          statements since the host-CPU copies are named "ASID".
 *
 * @remarks Called with interrupts disabled.
 */
static DECLCALLBACK(void) hmR0VmxFlushTaggedTlbBoth(PVM pVM, PVMCPU pVCpu)
{
    AssertPtr(pVM);
    AssertPtr(pVCpu);
    AssertMsg(pVM->hm.s.fNestedPaging && pVM->hm.s.vmx.fVpid,
              ("hmR0VmxFlushTaggedTlbBoth cannot be invoked unless NestedPaging & VPID are enabled."
               "fNestedPaging=%RTbool fVpid=%RTbool", pVM->hm.s.fNestedPaging, pVM->hm.s.vmx.fVpid));

    PHMGLOBLCPUINFO pCpu = HMR0GetCurrentCpu();
    AssertPtr(pCpu);

    /*
     * Force a TLB flush for the first world-switch if the current CPU differs from the one we ran on last.
     * This can happen both for start & resume due to long jumps back to ring-3.
     * If the TLB flush count changed, another VM (VCPU rather) has hit the ASID limit while flushing the TLB
     * or the host Cpu is online after a suspend/resume, so we cannot reuse the current ASID anymore.
     */
    bool fNewASID = false;
    if (   pVCpu->hm.s.idLastCpu != pCpu->idCpu
        || pVCpu->hm.s.cTlbFlushes != pCpu->cTlbFlushes)
    {
        pVCpu->hm.s.fForceTLBFlush = true;
        fNewASID = true;
        STAM_COUNTER_INC(&pVCpu->hm.s.StatFlushTlbWorldSwitch);
    }

    /*
     * Check for explicit TLB shootdowns.
     */
    if (VMCPU_FF_TESTANDCLEAR(pVCpu, VMCPU_FF_TLB_FLUSH))
    {
        pVCpu->hm.s.fForceTLBFlush = true;
        STAM_COUNTER_INC(&pVCpu->hm.s.StatFlushTlb);
    }

    pVCpu->hm.s.idLastCpu = pCpu->idCpu;
    if (pVCpu->hm.s.fForceTLBFlush)
    {
        if (fNewASID)
        {
            ++pCpu->uCurrentAsid;
            if (pCpu->uCurrentAsid >= pVM->hm.s.uMaxAsid)
            {
                pCpu->uCurrentAsid = 1;       /* start at 1; host uses 0 */
                pCpu->cTlbFlushes++;
                pCpu->fFlushAsidBeforeUse = true;
            }

            pVCpu->hm.s.uCurrentAsid = pCpu->uCurrentAsid;
            if (pCpu->fFlushAsidBeforeUse)
                hmR0VmxFlushVpid(pVM, pVCpu, pVM->hm.s.vmx.enmFlushVpid, 0 /* GCPtr */);
        }
        else
        {
            if (pVM->hm.s.vmx.msr.vmx_ept_vpid_caps & MSR_IA32_VMX_EPT_VPID_CAP_INVVPID_SINGLE_CONTEXT)
                hmR0VmxFlushVpid(pVM, pVCpu, VMX_FLUSH_VPID_SINGLE_CONTEXT, 0 /* GCPtr */);
            else
                hmR0VmxFlushEpt(pVM, pVCpu, pVM->hm.s.vmx.enmFlushEpt);
        }

        pVCpu->hm.s.cTlbFlushes    = pCpu->cTlbFlushes;
        pVCpu->hm.s.fForceTLBFlush = false;
    }
    else
    {
        AssertMsg(pVCpu->hm.s.uCurrentAsid && pCpu->uCurrentAsid,
                  ("hm->uCurrentAsid=%lu hm->cTlbFlushes=%lu cpu->uCurrentAsid=%lu cpu->cTlbFlushes=%lu\n",
                   pVCpu->hm.s.uCurrentAsid, pVCpu->hm.s.cTlbFlushes,
                   pCpu->uCurrentAsid, pCpu->cTlbFlushes));

        /** @todo We never set VMCPU_FF_TLB_SHOOTDOWN anywhere so this path should
         *        not be executed. See hmQueueInvlPage() where it is commented
         *        out. Support individual entry flushing someday. */
        if (VMCPU_FF_IS_PENDING(pVCpu, VMCPU_FF_TLB_SHOOTDOWN))
        {
            STAM_COUNTER_INC(&pVCpu->hm.s.StatTlbShootdown);

            /*
             * Flush individual guest entries using VPID from the TLB or as little as possible with EPT
             * as supported by the CPU.
             */
            if (pVM->hm.s.vmx.msr.vmx_ept_vpid_caps & MSR_IA32_VMX_EPT_VPID_CAP_INVVPID_INDIV_ADDR)
            {
                for (unsigned i = 0; i < pVCpu->hm.s.TlbShootdown.cPages; i++)
                    hmR0VmxFlushVpid(pVM, pVCpu, VMX_FLUSH_VPID_INDIV_ADDR, pVCpu->hm.s.TlbShootdown.aPages[i]);
            }
            else
                hmR0VmxFlushEpt(pVM, pVCpu, pVM->hm.s.vmx.enmFlushEpt);
        }
        else
            STAM_COUNTER_INC(&pVCpu->hm.s.StatNoFlushTlbWorldSwitch);
    }
    pVCpu->hm.s.TlbShootdown.cPages = 0;
    VMCPU_FF_CLEAR(pVCpu, VMCPU_FF_TLB_SHOOTDOWN);

    AssertMsg(pVCpu->hm.s.cTlbFlushes == pCpu->cTlbFlushes,
              ("Flush count mismatch for cpu %d (%u vs %u)\n", pCpu->idCpu, pVCpu->hm.s.cTlbFlushes, pCpu->cTlbFlushes));
    AssertMsg(pCpu->uCurrentAsid >= 1 && pCpu->uCurrentAsid < pVM->hm.s.uMaxAsid,
              ("cpu%d uCurrentAsid = %u\n", pCpu->idCpu, pCpu->uCurrentAsid));
    AssertMsg(pVCpu->hm.s.uCurrentAsid >= 1 && pVCpu->hm.s.uCurrentAsid < pVM->hm.s.uMaxAsid,
              ("cpu%d VM uCurrentAsid = %u\n", pCpu->idCpu, pVCpu->hm.s.uCurrentAsid));

    /* Update VMCS with the VPID. */
    int rc  = VMXWriteVmcs32(VMX_VMCS16_GUEST_FIELD_VPID, pVCpu->hm.s.uCurrentAsid);
    AssertRC(rc);
}


/**
 * Flushes the tagged-TLB entries for EPT CPUs as necessary.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 *
 * @remarks Called with interrupts disabled.
 */
static DECLCALLBACK(void) hmR0VmxFlushTaggedTlbEpt(PVM pVM, PVMCPU pVCpu)
{
    AssertPtr(pVM);
    AssertPtr(pVCpu);
    AssertMsg(pVM->hm.s.fNestedPaging, ("hmR0VmxFlushTaggedTlbEpt cannot be invoked with NestedPaging disabled."));
    AssertMsg(!pVM->hm.s.vmx.fVpid, ("hmR0VmxFlushTaggedTlbEpt cannot be invoked with VPID enabled."));

    PHMGLOBLCPUINFO pCpu = HMR0GetCurrentCpu();
    AssertPtr(pCpu);

    /*
     * Force a TLB flush for the first world-switch if the current CPU differs from the one we ran on last.
     * This can happen both for start & resume due to long jumps back to ring-3.
     * A change in the TLB flush count implies the host CPU is online after a suspend/resume.
     */
    if (   pVCpu->hm.s.idLastCpu != pCpu->idCpu
        || pVCpu->hm.s.cTlbFlushes != pCpu->cTlbFlushes)
    {
        pVCpu->hm.s.fForceTLBFlush = true;
        STAM_COUNTER_INC(&pVCpu->hm.s.StatFlushTlbWorldSwitch);
    }

    /* Check for explicit TLB shootdown flushes. */
    if (VMCPU_FF_TESTANDCLEAR(pVCpu, VMCPU_FF_TLB_FLUSH))
    {
        pVCpu->hm.s.fForceTLBFlush = true;
        STAM_COUNTER_INC(&pVCpu->hm.s.StatFlushTlb);
    }

    pVCpu->hm.s.idLastCpu   = pCpu->idCpu;
    pVCpu->hm.s.cTlbFlushes = pCpu->cTlbFlushes;

    if (pVCpu->hm.s.fForceTLBFlush)
    {
        hmR0VmxFlushEpt(pVM, pVCpu, pVM->hm.s.vmx.enmFlushEpt);
        pVCpu->hm.s.fForceTLBFlush = false;
    }
    else
    {
        /** @todo We never set VMCPU_FF_TLB_SHOOTDOWN anywhere so this path should
         *        not be executed. See hmQueueInvlPage() where it is commented
         *        out. Support individual entry flushing someday. */
        if (VMCPU_FF_IS_PENDING(pVCpu, VMCPU_FF_TLB_SHOOTDOWN))
        {
            /* We cannot flush individual entries without VPID support. Flush using EPT. */
            STAM_COUNTER_INC(&pVCpu->hm.s.StatTlbShootdown);
            hmR0VmxFlushEpt(pVM, pVCpu, pVM->hm.s.vmx.enmFlushEpt);
        }
        else
            STAM_COUNTER_INC(&pVCpu->hm.s.StatNoFlushTlbWorldSwitch);
    }

    pVCpu->hm.s.TlbShootdown.cPages= 0;
    VMCPU_FF_CLEAR(pVCpu, VMCPU_FF_TLB_SHOOTDOWN);
}


/**
 * Flushes the tagged-TLB entries for VPID CPUs as necessary.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 *
 * @remarks Called with interrupts disabled.
 */
static DECLCALLBACK(void) hmR0VmxFlushTaggedTlbVpid(PVM pVM, PVMCPU pVCpu)
{
    AssertPtr(pVM);
    AssertPtr(pVCpu);
    AssertMsg(pVM->hm.s.vmx.fVpid, ("hmR0VmxFlushTlbVpid cannot be invoked with VPID disabled."));
    AssertMsg(!pVM->hm.s.fNestedPaging, ("hmR0VmxFlushTlbVpid cannot be invoked with NestedPaging enabled"));

    PHMGLOBLCPUINFO pCpu = HMR0GetCurrentCpu();

    /*
     * Force a TLB flush for the first world switch if the current CPU differs from the one we ran on last.
     * This can happen both for start & resume due to long jumps back to ring-3.
     * If the TLB flush count changed, another VM (VCPU rather) has hit the ASID limit while flushing the TLB
     * or the host CPU is online after a suspend/resume, so we cannot reuse the current ASID anymore.
     */
    if (   pVCpu->hm.s.idLastCpu != pCpu->idCpu
        || pVCpu->hm.s.cTlbFlushes != pCpu->cTlbFlushes)
    {
        pVCpu->hm.s.fForceTLBFlush = true;
        STAM_COUNTER_INC(&pVCpu->hm.s.StatFlushTlbWorldSwitch);
    }

    /* Check for explicit TLB shootdown flushes. */
    if (VMCPU_FF_TESTANDCLEAR(pVCpu, VMCPU_FF_TLB_FLUSH))
    {
        /*
         * If we ever support VPID flush combinations other than ALL or SINGLE-context (see hmR0VmxSetupTaggedTlb())
         * we would need to explicitly flush in this case (add an fExplicitFlush = true here and change the
         * pCpu->fFlushAsidBeforeUse check below to include fExplicitFlush's too) - an obscure corner case.
         */
        pVCpu->hm.s.fForceTLBFlush = true;
        STAM_COUNTER_INC(&pVCpu->hm.s.StatFlushTlb);
    }

    pVCpu->hm.s.idLastCpu = pCpu->idCpu;
    if (pVCpu->hm.s.fForceTLBFlush)
    {
        ++pCpu->uCurrentAsid;
        if (pCpu->uCurrentAsid >= pVM->hm.s.uMaxAsid)
        {
            pCpu->uCurrentAsid        = 1;       /* start at 1; host uses 0 */
            pCpu->fFlushAsidBeforeUse = true;
            pCpu->cTlbFlushes++;
        }

        pVCpu->hm.s.fForceTLBFlush = false;
        pVCpu->hm.s.cTlbFlushes    = pCpu->cTlbFlushes;
        pVCpu->hm.s.uCurrentAsid   = pCpu->uCurrentAsid;
        if (pCpu->fFlushAsidBeforeUse)
            hmR0VmxFlushVpid(pVM, pVCpu, pVM->hm.s.vmx.enmFlushVpid, 0 /* GCPtr */);
    }
    else
    {
        AssertMsg(pVCpu->hm.s.uCurrentAsid && pCpu->uCurrentAsid,
                  ("hm->uCurrentAsid=%lu hm->cTlbFlushes=%lu cpu->uCurrentAsid=%lu cpu->cTlbFlushes=%lu\n",
                   pVCpu->hm.s.uCurrentAsid, pVCpu->hm.s.cTlbFlushes,
                   pCpu->uCurrentAsid, pCpu->cTlbFlushes));

        /** @todo We never set VMCPU_FF_TLB_SHOOTDOWN anywhere so this path should
         *        not be executed. See hmQueueInvlPage() where it is commented
         *        out. Support individual entry flushing someday. */
        if (VMCPU_FF_IS_PENDING(pVCpu, VMCPU_FF_TLB_SHOOTDOWN))
        {
            /* Flush individual guest entries using VPID or as little as possible with EPT as supported by the CPU. */
            if (pVM->hm.s.vmx.msr.vmx_ept_vpid_caps & MSR_IA32_VMX_EPT_VPID_CAP_INVVPID_INDIV_ADDR)
            {
                for (unsigned i = 0; i < pVCpu->hm.s.TlbShootdown.cPages; i++)
                    hmR0VmxFlushVpid(pVM, pVCpu, VMX_FLUSH_VPID_INDIV_ADDR, pVCpu->hm.s.TlbShootdown.aPages[i]);
            }
            else
                hmR0VmxFlushVpid(pVM, pVCpu, pVM->hm.s.vmx.enmFlushVpid, 0 /* GCPtr */);
        }
        else
            STAM_COUNTER_INC(&pVCpu->hm.s.StatNoFlushTlbWorldSwitch);
    }

    pVCpu->hm.s.TlbShootdown.cPages = 0;
    VMCPU_FF_CLEAR(pVCpu, VMCPU_FF_TLB_SHOOTDOWN);

    AssertMsg(pVCpu->hm.s.cTlbFlushes == pCpu->cTlbFlushes,
              ("Flush count mismatch for cpu %d (%u vs %u)\n", pCpu->idCpu, pVCpu->hm.s.cTlbFlushes, pCpu->cTlbFlushes));
    AssertMsg(pCpu->uCurrentAsid >= 1 && pCpu->uCurrentAsid < pVM->hm.s.uMaxAsid,
              ("cpu%d uCurrentAsid = %u\n", pCpu->idCpu, pCpu->uCurrentAsid));
    AssertMsg(pVCpu->hm.s.uCurrentAsid >= 1 && pVCpu->hm.s.uCurrentAsid < pVM->hm.s.uMaxAsid,
              ("cpu%d VM uCurrentAsid = %u\n", pCpu->idCpu, pVCpu->hm.s.uCurrentAsid));

    int rc  = VMXWriteVmcs32(VMX_VMCS16_GUEST_FIELD_VPID, pVCpu->hm.s.uCurrentAsid);
    AssertRC(rc);
}


/**
 * Sets up the appropriate tagged TLB-flush level and handler for flushing guest
 * TLB entries from the host TLB before VM-entry.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 */
static int hmR0VmxSetupTaggedTlb(PVM pVM)
{
    /*
     * Determine optimal flush type for nested paging.
     * We cannot ignore EPT if no suitable flush-types is supported by the CPU as we've already setup unrestricted
     * guest execution (see hmR3InitFinalizeR0()).
     */
    if (pVM->hm.s.fNestedPaging)
    {
        if (pVM->hm.s.vmx.msr.vmx_ept_vpid_caps & MSR_IA32_VMX_EPT_VPID_CAP_INVEPT)
        {
            if (pVM->hm.s.vmx.msr.vmx_ept_vpid_caps & MSR_IA32_VMX_EPT_VPID_CAP_INVEPT_SINGLE_CONTEXT)
                pVM->hm.s.vmx.enmFlushEpt = VMX_FLUSH_EPT_SINGLE_CONTEXT;
            else if (pVM->hm.s.vmx.msr.vmx_ept_vpid_caps & MSR_IA32_VMX_EPT_VPID_CAP_INVEPT_ALL_CONTEXTS)
                pVM->hm.s.vmx.enmFlushEpt = VMX_FLUSH_EPT_ALL_CONTEXTS;
            else
            {
                /* Shouldn't happen. EPT is supported but no suitable flush-types supported. */
                pVM->hm.s.vmx.enmFlushEpt = VMX_FLUSH_EPT_NOT_SUPPORTED;
                return VERR_HM_UNSUPPORTED_CPU_FEATURE_COMBO;
            }

            /* Make sure the write-back cacheable memory type for EPT is supported. */
            if (!(pVM->hm.s.vmx.msr.vmx_ept_vpid_caps & MSR_IA32_VMX_EPT_VPID_CAP_EMT_WB))
            {
                LogRel(("hmR0VmxSetupTaggedTlb: Unsupported EPTP memory type %#x.\n", pVM->hm.s.vmx.msr.vmx_ept_vpid_caps));
                pVM->hm.s.vmx.enmFlushEpt = VMX_FLUSH_EPT_NOT_SUPPORTED;
                return VERR_HM_UNSUPPORTED_CPU_FEATURE_COMBO;
            }
        }
        else
        {
            /* Shouldn't happen. EPT is supported but INVEPT instruction is not supported. */
            pVM->hm.s.vmx.enmFlushEpt = VMX_FLUSH_EPT_NOT_SUPPORTED;
            return VERR_HM_UNSUPPORTED_CPU_FEATURE_COMBO;
        }
    }

    /*
     * Determine optimal flush type for VPID.
     */
    if (pVM->hm.s.vmx.fVpid)
    {
        if (pVM->hm.s.vmx.msr.vmx_ept_vpid_caps & MSR_IA32_VMX_EPT_VPID_CAP_INVVPID)
        {
            if (pVM->hm.s.vmx.msr.vmx_ept_vpid_caps & MSR_IA32_VMX_EPT_VPID_CAP_INVVPID_SINGLE_CONTEXT)
                pVM->hm.s.vmx.enmFlushVpid = VMX_FLUSH_VPID_SINGLE_CONTEXT;
            else if (pVM->hm.s.vmx.msr.vmx_ept_vpid_caps & MSR_IA32_VMX_EPT_VPID_CAP_INVVPID_ALL_CONTEXTS)
                pVM->hm.s.vmx.enmFlushVpid = VMX_FLUSH_VPID_ALL_CONTEXTS;
            else
            {
                /* Neither SINGLE nor ALL-context flush types for VPID is supported by the CPU. Ignore VPID capability. */
                if (pVM->hm.s.vmx.msr.vmx_ept_vpid_caps & MSR_IA32_VMX_EPT_VPID_CAP_INVVPID_INDIV_ADDR)
                    LogRel(("hmR0VmxSetupTaggedTlb: Only INDIV_ADDR supported. Ignoring VPID.\n"));
                if (pVM->hm.s.vmx.msr.vmx_ept_vpid_caps & MSR_IA32_VMX_EPT_VPID_CAP_INVVPID_SINGLE_CONTEXT_RETAIN_GLOBALS)
                    LogRel(("hmR0VmxSetupTaggedTlb: Only SINGLE_CONTEXT_RETAIN_GLOBALS supported. Ignoring VPID.\n"));
                pVM->hm.s.vmx.enmFlushVpid = VMX_FLUSH_VPID_NOT_SUPPORTED;
                pVM->hm.s.vmx.fVpid = false;
            }
        }
        else
        {
            /*  Shouldn't happen. VPID is supported but INVVPID is not supported by the CPU. Ignore VPID capability. */
            Log(("hmR0VmxSetupTaggedTlb: VPID supported without INVEPT support. Ignoring VPID.\n"));
            pVM->hm.s.vmx.enmFlushVpid = VMX_FLUSH_VPID_NOT_SUPPORTED;
            pVM->hm.s.vmx.fVpid = false;
        }
    }

    /*
     * Setup the handler for flushing tagged-TLBs.
     */
    if (pVM->hm.s.fNestedPaging && pVM->hm.s.vmx.fVpid)
        pVM->hm.s.vmx.pfnFlushTaggedTlb = hmR0VmxFlushTaggedTlbBoth;
    else if (pVM->hm.s.fNestedPaging)
        pVM->hm.s.vmx.pfnFlushTaggedTlb = hmR0VmxFlushTaggedTlbEpt;
    else if (pVM->hm.s.vmx.fVpid)
        pVM->hm.s.vmx.pfnFlushTaggedTlb = hmR0VmxFlushTaggedTlbVpid;
    else
        pVM->hm.s.vmx.pfnFlushTaggedTlb = hmR0VmxFlushTaggedTlbNone;
    return VINF_SUCCESS;
}


/**
 * Sets up pin-based VM-execution controls in the VMCS.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 */
static int hmR0VmxSetupPinCtls(PVM pVM, PVMCPU pVCpu)
{
    AssertPtr(pVM);
    AssertPtr(pVCpu);

    uint32_t val = pVM->hm.s.vmx.msr.vmx_pin_ctls.n.disallowed0;    /* Bits set here must always be set. */
    uint32_t zap = pVM->hm.s.vmx.msr.vmx_pin_ctls.n.allowed1;       /* Bits cleared here must always be cleared. */

    val |=   VMX_VMCS_CTRL_PIN_EXEC_CONTROLS_EXT_INT_EXIT           /* External interrupts causes a VM-exits. */
           | VMX_VMCS_CTRL_PIN_EXEC_CONTROLS_NMI_EXIT;              /* Non-maskable interrupts causes a VM-exit. */
    Assert(!(val & VMX_VMCS_CTRL_PIN_EXEC_CONTROLS_VIRTUAL_NMI));

    /* Enable the VMX preemption timer. */
    if (pVM->hm.s.vmx.fUsePreemptTimer)
        val |= VMX_VMCS_CTRL_PIN_EXEC_CONTROLS_PREEMPT_TIMER;

    if ((val & zap) != val)
    {
        LogRel(("hmR0VmxSetupPinCtls: invalid pin-based VM-execution controls combo! cpu=%#RX64 val=%#RX64 zap=%#RX64\n",
                pVM->hm.s.vmx.msr.vmx_pin_ctls.n.disallowed0, val, zap));
        return VERR_HM_UNSUPPORTED_CPU_FEATURE_COMBO;
    }

    val &= zap;
    int rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_PIN_EXEC_CONTROLS, val);
    AssertRCReturn(rc, rc);

    /* Update VCPU with the currently set pin-based VM-execution controls. */
    pVCpu->hm.s.vmx.u32PinCtls = val;
    return rc;
}


/**
 * Sets up processor-based VM-execution controls in the VMCS.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVMCPU      Pointer to the VMCPU.
 */
static int hmR0VmxSetupProcCtls(PVM pVM, PVMCPU pVCpu)
{
    AssertPtr(pVM);
    AssertPtr(pVCpu);

    int rc = VERR_INTERNAL_ERROR_5;
    uint32_t val = pVM->hm.s.vmx.msr.vmx_proc_ctls.n.disallowed0;       /* Bits set here must be set in the VMCS. */
    uint32_t zap = pVM->hm.s.vmx.msr.vmx_proc_ctls.n.allowed1;          /* Bits cleared here must be cleared in the VMCS. */

    val |=   VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_HLT_EXIT                  /* HLT causes a VM-exit. */
           | VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_USE_TSC_OFFSETTING        /* Use TSC-offsetting. */
           | VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_MOV_DR_EXIT               /* MOV DRx causes a VM-exit. */
           | VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_UNCOND_IO_EXIT            /* All IO instructions cause a VM-exit. */
           | VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_RDPMC_EXIT                /* RDPMC causes a VM-exit. */
           | VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_MONITOR_EXIT              /* MONITOR causes a VM-exit. */
           | VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_MWAIT_EXIT;               /* MWAIT causes a VM-exit. */

    /* We toggle VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_MOV_DR_EXIT later, check if it's not -always- needed to be set or clear. */
    if (   !(pVM->hm.s.vmx.msr.vmx_proc_ctls.n.allowed1 & VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_MOV_DR_EXIT)
        ||  (pVM->hm.s.vmx.msr.vmx_proc_ctls.n.disallowed0 & VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_MOV_DR_EXIT))
    {
        LogRel(("hmR0VmxSetupProcCtls: unsupported VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_MOV_DR_EXIT combo!"));
        return VERR_HM_UNSUPPORTED_CPU_FEATURE_COMBO;
    }

    /* Without nested paging, INVLPG (also affects INVPCID) and MOV CR3 instructions should cause VM-exits. */
    if (!pVM->hm.s.fNestedPaging)
    {
        Assert(!pVM->hm.s.vmx.fUnrestrictedGuest);                      /* Paranoia. */
        val |=   VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_INVLPG_EXIT
               | VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_CR3_LOAD_EXIT
               | VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_CR3_STORE_EXIT;
    }

    /* Use TPR shadowing if supported by the CPU. */
    if (pVM->hm.s.vmx.msr.vmx_proc_ctls.n.allowed1 & VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_USE_TPR_SHADOW)
    {
        Assert(pVCpu->hm.s.vmx.HCPhysVirtApic);
        Assert(!(pVCpu->hm.s.vmx.HCPhysVirtApic & 0xfff));              /* Bits 11:0 MBZ. */
        rc  = VMXWriteVmcs32(VMX_VMCS32_CTRL_TPR_THRESHOLD, 0);
        rc |= VMXWriteVmcs64(VMX_VMCS64_CTRL_VAPIC_PAGEADDR_FULL, pVCpu->hm.s.vmx.HCPhysVirtApic);
        AssertRCReturn(rc, rc);

        val |= VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_USE_TPR_SHADOW;         /* CR8 reads from the Virtual-APIC page. */
                                                                        /* CR8 writes causes a VM-exit based on TPR threshold. */
        Assert(!(val & VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_CR8_STORE_EXIT));
        Assert(!(val & VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_CR8_LOAD_EXIT));
    }
    else
    {
        val |=   VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_CR8_STORE_EXIT        /* CR8 reads causes a VM-exit. */
               | VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_CR8_LOAD_EXIT;        /* CR8 writes causes a VM-exit. */
    }

    /* Use MSR-bitmaps if supported by the CPU. */
    if (pVM->hm.s.vmx.msr.vmx_proc_ctls.n.allowed1 & VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_USE_MSR_BITMAPS)
    {
        val |= VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_USE_MSR_BITMAPS;

        Assert(pVCpu->hm.s.vmx.HCPhysMsrBitmap);
        Assert(!(pVCpu->hm.s.vmx.HCPhysMsrBitmap & 0xfff));             /* Bits 11:0 MBZ. */
        rc = VMXWriteVmcs64(VMX_VMCS64_CTRL_MSR_BITMAP_FULL, pVCpu->hm.s.vmx.HCPhysMsrBitmap);
        AssertRCReturn(rc, rc);

        /*
         * The guest can access the following MSRs (read, write) without causing VM-exits; they are loaded/stored
         * automatically (either as part of the MSR-load/store areas or dedicated fields in the VMCS).
         */
        hmR0VmxSetMsrPermission(pVCpu, MSR_IA32_SYSENTER_CS,  VMXMSREXIT_PASSTHRU_READ, VMXMSREXIT_PASSTHRU_WRITE);
        hmR0VmxSetMsrPermission(pVCpu, MSR_IA32_SYSENTER_ESP, VMXMSREXIT_PASSTHRU_READ, VMXMSREXIT_PASSTHRU_WRITE);
        hmR0VmxSetMsrPermission(pVCpu, MSR_IA32_SYSENTER_EIP, VMXMSREXIT_PASSTHRU_READ, VMXMSREXIT_PASSTHRU_WRITE);
        hmR0VmxSetMsrPermission(pVCpu, MSR_K8_LSTAR,          VMXMSREXIT_PASSTHRU_READ, VMXMSREXIT_PASSTHRU_WRITE);
        hmR0VmxSetMsrPermission(pVCpu, MSR_K6_STAR,           VMXMSREXIT_PASSTHRU_READ, VMXMSREXIT_PASSTHRU_WRITE);
        hmR0VmxSetMsrPermission(pVCpu, MSR_K8_SF_MASK,        VMXMSREXIT_PASSTHRU_READ, VMXMSREXIT_PASSTHRU_WRITE);
        hmR0VmxSetMsrPermission(pVCpu, MSR_K8_KERNEL_GS_BASE, VMXMSREXIT_PASSTHRU_READ, VMXMSREXIT_PASSTHRU_WRITE);
        hmR0VmxSetMsrPermission(pVCpu, MSR_K8_GS_BASE,        VMXMSREXIT_PASSTHRU_READ, VMXMSREXIT_PASSTHRU_WRITE);
        hmR0VmxSetMsrPermission(pVCpu, MSR_K8_FS_BASE,        VMXMSREXIT_PASSTHRU_READ, VMXMSREXIT_PASSTHRU_WRITE);
    }

    /* Use the secondary processor-based VM-execution controls if supported by the CPU. */
    if (pVM->hm.s.vmx.msr.vmx_proc_ctls.n.allowed1 & VMX_VMCS_CTRL_PROC_EXEC_USE_SECONDARY_EXEC_CTRL)
        val |= VMX_VMCS_CTRL_PROC_EXEC_USE_SECONDARY_EXEC_CTRL;

    if ((val & zap) != val)
    {
        LogRel(("hmR0VmxSetupProcCtls: invalid processor-based VM-execution controls combo! cpu=%#RX64 val=%#RX64 zap=%#RX64\n",
                pVM->hm.s.vmx.msr.vmx_proc_ctls.n.disallowed0, val, zap));
        return VERR_HM_UNSUPPORTED_CPU_FEATURE_COMBO;
    }

    val &= zap;
    rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_PROC_EXEC_CONTROLS, val);
    AssertRCReturn(rc, rc);

    /* Update VCPU with the currently set processor-based VM-execution controls. */
    pVCpu->hm.s.vmx.u32ProcCtls = val;

    /*
     * Secondary processor-based VM-execution controls.
     */
    if (pVCpu->hm.s.vmx.u32ProcCtls & VMX_VMCS_CTRL_PROC_EXEC_USE_SECONDARY_EXEC_CTRL)
    {
        val = pVM->hm.s.vmx.msr.vmx_proc_ctls2.n.disallowed0;           /* Bits set here must be set in the VMCS. */
        zap = pVM->hm.s.vmx.msr.vmx_proc_ctls2.n.allowed1;              /* Bits cleared here must be cleared in the VMCS. */

        val |= VMX_VMCS_CTRL_PROC_EXEC2_WBINVD_EXIT;                    /* WBINVD causes a VM-exit. */

        if (pVM->hm.s.fNestedPaging)
            val |= VMX_VMCS_CTRL_PROC_EXEC2_EPT;                        /* Enable EPT. */
        else
        {
            /*
             * Without Nested Paging, INVPCID should cause a VM-exit. Enabling this bit causes the CPU to refer to
             * VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_INVLPG_EXIT when INVPCID is executed by the guest.
             * See Intel spec. 25.4 "Changes to instruction behaviour in VMX non-root operation".
             */
            if (pVM->hm.s.vmx.msr.vmx_proc_ctls2.n.allowed1 & VMX_VMCS_CTRL_PROC_EXEC2_INVPCID)
                val |= VMX_VMCS_CTRL_PROC_EXEC2_INVPCID;
        }

        if (pVM->hm.s.vmx.fVpid)
            val |= VMX_VMCS_CTRL_PROC_EXEC2_VPID;                       /* Enable VPID. */

        if (pVM->hm.s.vmx.fUnrestrictedGuest)
            val |= VMX_VMCS_CTRL_PROC_EXEC2_UNRESTRICTED_GUEST;         /* Enable Unrestricted Execution. */

        /* Enable Virtual-APIC page accesses if supported by the CPU. This is essentially where the TPR shadow resides. */
        /** @todo VIRT_X2APIC support, it's mutually exclusive with this. So must be
         *        done dynamically. */
        if (pVM->hm.s.vmx.msr.vmx_proc_ctls2.n.allowed1 & VMX_VMCS_CTRL_PROC_EXEC2_VIRT_APIC)
        {
            Assert(pVM->hm.s.vmx.HCPhysApicAccess);
            Assert(!(pVM->hm.s.vmx.HCPhysApicAccess & 0xfff));          /* Bits 11:0 MBZ. */
            val |= VMX_VMCS_CTRL_PROC_EXEC2_VIRT_APIC;                  /* Virtualize APIC accesses. */
            rc = VMXWriteVmcs64(VMX_VMCS64_CTRL_APIC_ACCESSADDR_FULL, pVM->hm.s.vmx.HCPhysApicAccess);
            AssertRCReturn(rc, rc);
        }

        if (pVM->hm.s.vmx.msr.vmx_proc_ctls2.n.allowed1 & VMX_VMCS_CTRL_PROC_EXEC2_RDTSCP)
        {
            val |= VMX_VMCS_CTRL_PROC_EXEC2_RDTSCP;                     /* Enable RDTSCP support. */
            if (pVM->hm.s.vmx.msr.vmx_proc_ctls.n.allowed1 & VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_USE_MSR_BITMAPS)
                hmR0VmxSetMsrPermission(pVCpu, MSR_K8_TSC_AUX, VMXMSREXIT_PASSTHRU_READ, VMXMSREXIT_PASSTHRU_WRITE);
        }

        if ((val & zap) != val)
        {
            LogRel(("hmR0VmxSetupProcCtls: invalid secondary processor-based VM-execution controls combo! "
                    "cpu=%#RX64 val=%#RX64 zap=%#RX64\n", pVM->hm.s.vmx.msr.vmx_proc_ctls2.n.disallowed0, val, zap));
            return VERR_HM_UNSUPPORTED_CPU_FEATURE_COMBO;
        }

        val &= zap;
        rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_PROC_EXEC_CONTROLS2, val);
        AssertRCReturn(rc, rc);

        /* Update VCPU with the currently set secondary processor-based VM-execution controls. */
        pVCpu->hm.s.vmx.u32ProcCtls2 = val;
    }

    return VINF_SUCCESS;
}


/**
 * Sets up miscellaneous (everything other than Pin & Processor-based
 * VM-execution) control fields in the VMCS.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 */
static int hmR0VmxSetupMiscCtls(PVM pVM, PVMCPU pVCpu)
{
    AssertPtr(pVM);
    AssertPtr(pVCpu);

    int rc = VERR_GENERAL_FAILURE;

    /* All CR3 accesses cause VM-exits. Later we optimize CR3 accesses (see hmR0VmxLoadGuestControlRegs())*/
    rc  = VMXWriteVmcs32(VMX_VMCS32_CTRL_CR3_TARGET_COUNT, 0);

    rc |= VMXWriteVmcs64(VMX_VMCS64_CTRL_TSC_OFFSET_FULL, 0);

    /*
     * Set MASK & MATCH to 0. VMX checks if GuestPFErrCode & MASK == MATCH. If equal (in our case it always is)
     * and if the X86_XCPT_PF bit in the exception bitmap is set it causes a VM-exit, if clear doesn't cause an exit.
     * We thus use the exception bitmap to control it rather than use both.
     */
    rc |= VMXWriteVmcs32(VMX_VMCS32_CTRL_PAGEFAULT_ERROR_MASK, 0);
    rc |= VMXWriteVmcs32(VMX_VMCS32_CTRL_PAGEFAULT_ERROR_MATCH, 0);

    /** @todo Explore possibility of using IO-bitmaps. */
    /* All IO & IOIO instructions cause VM-exits. */
    rc |= VMXWriteVmcs64(VMX_VMCS64_CTRL_IO_BITMAP_A_FULL, 0);
    rc |= VMXWriteVmcs64(VMX_VMCS64_CTRL_IO_BITMAP_B_FULL, 0);

    /* Setup MSR autoloading/autostoring. */
    Assert(pVCpu->hm.s.vmx.HCPhysGuestMsr);
    Assert(!(pVCpu->hm.s.vmx.HCPhysGuestMsr & 0xf));    /* Lower 4 bits MBZ. */
    rc |= VMXWriteVmcs64(VMX_VMCS64_CTRL_ENTRY_MSR_LOAD_FULL, pVCpu->hm.s.vmx.HCPhysGuestMsr);
    rc |= VMXWriteVmcs64(VMX_VMCS64_CTRL_EXIT_MSR_STORE_FULL, pVCpu->hm.s.vmx.HCPhysGuestMsr);
    rc |= VMXWriteVmcs32(VMX_VMCS32_CTRL_ENTRY_MSR_LOAD_COUNT, 0);
    rc |= VMXWriteVmcs32(VMX_VMCS32_CTRL_EXIT_MSR_STORE_COUNT, 0);

    Assert(pVCpu->hm.s.vmx.HCPhysHostMsr);
    Assert(!(pVCpu->hm.s.vmx.HCPhysHostMsr & 0xf));     /* Lower 4 bits MBZ. */
    rc |= VMXWriteVmcs64(VMX_VMCS64_CTRL_EXIT_MSR_LOAD_FULL,  pVCpu->hm.s.vmx.HCPhysHostMsr);
    rc |= VMXWriteVmcs32(VMX_VMCS32_CTRL_EXIT_MSR_LOAD_COUNT, 0);

    /* Set VMCS link pointer. Reserved for future use, must be -1. Intel spec. 24.4 "Guest-State Area". */
    rc |= VMXWriteVmcs64(VMX_VMCS64_GUEST_VMCS_LINK_PTR_FULL, 0xffffffffffffffffULL);

    /* Setup debug controls */
    rc |= VMXWriteVmcs64(VMX_VMCS64_GUEST_DEBUGCTL_FULL, 0);                /** @todo think about this. */
    rc |= VMXWriteVmcsGstN(VMX_VMCS_GUEST_PENDING_DEBUG_EXCEPTIONS,  0);    /** @todo Intel spec. 26.6.3 think about this */
    AssertRCReturn(rc, rc);
    return rc;
}


/**
 * Sets up the initial exception bitmap in the VMCS based on static conditions
 * (i.e. conditions that cannot ever change at runtime).
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 */
static int hmR0VmxInitXcptBitmap(PVM pVM, PVMCPU pVCpu)
{
    AssertPtr(pVM);
    AssertPtr(pVCpu);

    LogFlowFunc(("pVM=%p pVCpu=%p\n", pVM, pVCpu));

    uint32_t u32XcptBitmap = 0;

    /* Without nested paging, #PF must cause a VM-exit so we can sync our shadow page tables. */
    if (!pVM->hm.s.fNestedPaging)
        u32XcptBitmap |= RT_BIT(X86_XCPT_PF);

    pVCpu->hm.s.vmx.u32XcptBitmap = u32XcptBitmap;
    int rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_EXCEPTION_BITMAP, u32XcptBitmap);
    AssertRCReturn(rc, rc);
    return rc;
}


/**
 * Sets up the initial guest-state mask. The guest-state mask is consulted
 * before reading guest-state fields from the VMCS as VMREADs can be expensive
 * for the nested virtualization case (as it would cause a VM-exit).
 *
 * @param   pVCpu       Pointer to the VMCPU.
 */
static int hmR0VmxInitUpdatedGuestStateMask(PVMCPU pVCpu)
{
    /* Initially the guest-state is up-to-date as there is nothing in the VMCS. */
    pVCpu->hm.s.vmx.fUpdatedGuestState = VMX_UPDATED_GUEST_ALL;
    return VINF_SUCCESS;
}


/**
 * Does per-VM VT-x initialization.
 *
 * @returns VBox status code.
 * @param   pVM             Pointer to the VM.
 */
VMMR0DECL(int) VMXR0InitVM(PVM pVM)
{
    LogFlowFunc(("pVM=%p\n", pVM));

    int rc = hmR0VmxStructsAlloc(pVM);
    if (RT_FAILURE(rc))
    {
        LogRel(("VMXR0InitVM: hmR0VmxStructsAlloc failed! rc=%Rrc\n", rc));
        return rc;
    }

    for (VMCPUID i = 0; i < pVM->cCpus; i++)
    {
        PVMCPU pVCpu = &pVM->aCpus[i];

        /* Current guest paging mode. */
        pVCpu->hm.s.vmx.enmLastSeenGuestMode = PGMMODE_REAL;
    }

    return VINF_SUCCESS;
}


/**
 * Does per-VM VT-x termination.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 */
VMMR0DECL(int) VMXR0TermVM(PVM pVM)
{
    LogFlowFunc(("pVM=%p\n", pVM));

#ifdef VBOX_WITH_CRASHDUMP_MAGIC
    if (pVM->hm.s.vmx.hMemObjScratch != NIL_RTR0MEMOBJ)
        ASMMemZero32(pVM->hm.s.vmx.pvScratch, PAGE_SIZE);
#endif
    hmR0VmxStructsFree(pVM);
    return VINF_SUCCESS;
}


/**
 * Sets up the VM for execution under VT-x.
 * This function is only called once per-VM during initalization.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 */
VMMR0DECL(int) VMXR0SetupVM(PVM pVM)
{
    AssertPtrReturn(pVM, VERR_INVALID_PARAMETER);
    Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));

    LogFlowFunc(("pVM=%p\n", pVM));

    /*
     * Without UnrestrictedGuest, pRealModeTSS and pNonPagingModeEPTPageTable *must* always be allocated.
     * We no longer support the highly unlikely case of UnrestrictedGuest without pRealModeTSS. See hmR3InitFinalizeR0().
     */
    /* -XXX- change hmR3InitFinalizeR0() to fail if pRealModeTSS alloc fails. */
    if (   !pVM->hm.s.vmx.fUnrestrictedGuest
        &&  (   !pVM->hm.s.vmx.pNonPagingModeEPTPageTable
             || !pVM->hm.s.vmx.pRealModeTSS))
    {
        LogRel(("VMXR0SetupVM: invalid real-on-v86 state.\n"));
        return VERR_INTERNAL_ERROR;
    }

    /* Initialize these always, see hmR3InitFinalizeR0().*/
    pVM->hm.s.vmx.enmFlushEpt  = VMX_FLUSH_EPT_NONE;
    pVM->hm.s.vmx.enmFlushVpid = VMX_FLUSH_VPID_NONE;

    /* Setup the tagged-TLB flush handlers. */
    int rc = hmR0VmxSetupTaggedTlb(pVM);
    if (RT_FAILURE(rc))
    {
        LogRel(("VMXR0SetupVM: hmR0VmxSetupTaggedTlb failed! rc=%Rrc\n", rc));
        return rc;
    }

    for (VMCPUID i = 0; i < pVM->cCpus; i++)
    {
        PVMCPU pVCpu = &pVM->aCpus[i];
        AssertPtr(pVCpu);
        AssertPtr(pVCpu->hm.s.vmx.pvVmcs);

        /* Set revision dword at the beginning of the VMCS structure. */
        *(uint32_t *)pVCpu->hm.s.vmx.pvVmcs = MSR_IA32_VMX_BASIC_INFO_VMCS_ID(pVM->hm.s.vmx.msr.vmx_basic_info);

        /* Initialize our VMCS region in memory, set the VMCS launch state to "clear". */
        rc  = VMXClearVMCS(pVCpu->hm.s.vmx.HCPhysVmcs);
        AssertLogRelMsgRCReturnStmt(rc, ("VMXR0SetupVM: VMXClearVMCS failed! rc=%Rrc (pVM=%p)\n", rc, pVM),
                                    hmR0VmxUpdateErrorRecord(pVM, pVCpu, rc), rc);

        /* Load this VMCS as the current VMCS. */
        rc = VMXActivateVMCS(pVCpu->hm.s.vmx.HCPhysVmcs);
        AssertLogRelMsgRCReturnStmt(rc, ("VMXR0SetupVM: VMXActivateVMCS failed! rc=%Rrc (pVM=%p)\n", rc, pVM),
                                    hmR0VmxUpdateErrorRecord(pVM, pVCpu, rc), rc);

        /* Setup the pin-based VM-execution controls. */
        rc = hmR0VmxSetupPinCtls(pVM, pVCpu);
        AssertLogRelMsgRCReturnStmt(rc, ("VMXR0SetupVM: hmR0VmxSetupPinCtls failed! rc=%Rrc (pVM=%p)\n", rc, pVM),
                                    hmR0VmxUpdateErrorRecord(pVM, pVCpu, rc), rc);

        /* Setup the processor-based VM-execution controls. */
        rc = hmR0VmxSetupProcCtls(pVM, pVCpu);
        AssertLogRelMsgRCReturnStmt(rc, ("VMXR0SetupVM: hmR0VmxSetupProcCtls failed! rc=%Rrc (pVM=%p)\n", rc, pVM),
                                    hmR0VmxUpdateErrorRecord(pVM, pVCpu, rc), rc);

        /* Setup the rest (miscellaneous) VM-execution controls. */
        rc = hmR0VmxSetupMiscCtls(pVM, pVCpu);
        AssertLogRelMsgRCReturnStmt(rc, ("VMXR0SetupVM: hmR0VmxSetupMiscCtls failed! rc=%Rrc (pVM=%p)\n", rc, pVM),
                                    hmR0VmxUpdateErrorRecord(pVM, pVCpu, rc), rc);

        /* Setup the initial exception bitmap. */
        rc = hmR0VmxInitXcptBitmap(pVM, pVCpu);
        AssertLogRelMsgRCReturnStmt(rc, ("VMXR0SetupVM: hmR0VmxInitXcptBitmap failed! rc=%Rrc (pVM=%p)\n", rc, pVM),
                                    hmR0VmxUpdateErrorRecord(pVM, pVCpu, rc), rc);

        /* Setup the initial guest-state mask. */
        rc = hmR0VmxInitUpdatedGuestStateMask(pVCpu);
        AssertLogRelMsgRCReturnStmt(rc, ("VMXR0SetupVM: hmR0VmxInitUpdatedGuestStateMask failed! rc=%Rrc (pVM=%p)\n", rc, pVM),
                                    hmR0VmxUpdateErrorRecord(pVM, pVCpu, rc), rc);

#if HC_ARCH_BITS == 32 && !defined(VBOX_WITH_HYBRID_32BIT_KERNEL)
        /* Setup the VMCS read cache as we queue up certain VMWRITEs that can only be done in 64-bit mode for 64-bit guests. */
        rc = hmR0VmxInitVmcsReadCache(pVM, pVCpu);
        AssertLogRelMsgRCReturnStmt(rc, ("VMXR0SetupVM: hmR0VmxInitVmcsReadCache failed! rc=%Rrc (pVM=%p)\n", rc, pVM),
                                    hmR0VmxUpdateErrorRecord(pVM, pVCpu, rc), rc);
#endif

        /* Re-sync the CPU's internal data into our VMCS memory region & reset the launch state to "clear". */
        rc = VMXClearVMCS(pVCpu->hm.s.vmx.HCPhysVmcs);
        AssertLogRelMsgRCReturnStmt(rc, ("VMXR0SetupVM: VMXClearVMCS(2) failed! rc=%Rrc (pVM=%p)\n", rc, pVM),
                                    hmR0VmxUpdateErrorRecord(pVM, pVCpu, rc), rc);

        /* Update the last error record for this VCPU. */
        hmR0VmxUpdateErrorRecord(pVM, pVCpu, rc);
    }

    return VINF_SUCCESS;
}


/**
 * Saves the host control registers (CR0, CR3, CR4) into the host-state area in
 * the VMCS.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 */
DECLINLINE(int) hmR0VmxSaveHostControlRegs(PVM pVM, PVMCPU pVCpu)
{
    RTCCUINTREG uReg = ASMGetCR0();
    int rc = VMXWriteVmcsHstN(VMX_VMCS_HOST_CR0, uReg);

#ifdef VBOX_WITH_HYBRID_32BIT_KERNEL
    /* For the darwin 32-bit hybrid kernel, we need the 64-bit CR3 as it uses 64-bit paging. */
    if (VMX_IS_64BIT_HOST_MODE())
    {
        uint64_t uReg = hmR0Get64bitCR3();
        rc |= VMXWriteVmcs64(VMX_VMCS_HOST_CR3, uReg);
    }
    else
#endif
    {
        uReg = ASMGetCR3();
        rc |= VMXWriteVmcsHstN(VMX_VMCS_HOST_CR3, uReg);
    }

    uReg = ASMGetCR4();
    rc |= VMXWriteVmcsHstN(VMX_VMCS_HOST_CR4, uReg);
    AssertRCReturn(rc, rc);
    return rc;
}


/**
 * Saves the host segment registers and GDTR, IDTR, (TR, GS and FS bases) into
 * the host-state area in the VMCS.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 */
DECLINLINE(int) hmR0VmxSaveHostSegmentRegs(PVM pVM, PVMCPU pVCpu)
{
    int rc = VERR_INTERNAL_ERROR_5;
    RTSEL uSelCS = 0;
    RTSEL uSelSS = 0;
    RTSEL uSelDS = 0;
    RTSEL uSelES = 0;
    RTSEL uSelFS = 0;
    RTSEL uSelGS = 0;
    RTSEL uSelTR = 0;

    /*
     * Host Selector registers.
     */
#ifdef VBOX_WITH_HYBRID_32BIT_KERNEL
    if (VMX_IS_64BIT_HOST_MODE())
    {
        uSelCS = (RTSEL)(uintptr_t)&SUPR0Abs64bitKernelCS;
        uSelSS = (RTSEL)(uintptr_t)&SUPR0Abs64bitKernelSS;
    }
    else
    {
        /* Seems darwin uses the LDT (TI flag is set) in the CS & SS selectors which VT-x doesn't like. */
        uSelCS = (RTSEL)(uintptr_t)&SUPR0AbsKernelCS;
        uSelSS = (RTSEL)(uintptr_t)&SUPR0AbsKernelSS;
    }
#else
    uSelCS = ASMGetCS();
    uSelSS = ASMGetSS();
#endif

    /* Note: VT-x is picky about the RPL of the selectors here; we'll restore them manually. */
    /** @todo Verify if we have any platform that actually run with DS or ES with
     *        RPL != 0 in kernel space. */
    uSelDS = 0;
    uSelES = 0;
    uSelFS = 0;
    uSelGS = 0;
    uSelTR = ASMGetTR();

    /* Verification based on Intel spec. 26.2.3 "Checks on Host Segment and Descriptor-Table Registers"  */
    Assert(!(uSelCS & X86_SEL_RPL)); Assert(!(uSelCS & X86_SEL_LDT));
    Assert(!(uSelSS & X86_SEL_RPL)); Assert(!(uSelSS & X86_SEL_LDT));
    Assert(!(uSelDS & X86_SEL_RPL)); Assert(!(uSelDS & X86_SEL_LDT));
    Assert(!(uSelES & X86_SEL_RPL)); Assert(!(uSelES & X86_SEL_LDT));
    Assert(!(uSelFS & X86_SEL_RPL)); Assert(!(uSelFS & X86_SEL_LDT));
    Assert(!(uSelGS & X86_SEL_RPL)); Assert(!(uSelGS & X86_SEL_LDT));
    Assert(uSelCS != 0);
    Assert(uSelTR != 0);

    /* Assertion is right but we would not have updated u32ExitCtls yet. */
#if 0
    if (!(pVCpu->hm.s.vmx.u32ExitCtls & VMX_VMCS_CTRL_EXIT_CONTROLS_HOST_ADDR_SPACE_SIZE))
        Assert(uSelSS != 0);
#endif

    /* Write these host selector fields into the host-state area in the VMCS. */
    rc =  VMXWriteVmcs32(VMX_VMCS16_HOST_FIELD_CS, uSelCS);
    rc |= VMXWriteVmcs32(VMX_VMCS16_HOST_FIELD_SS, uSelSS);
    /* Avoid the VMWRITEs as we set the following segments to 0 and the VMCS fields are already  0 (since g_HvmR0 is static) */
#if 0
    rc |= VMXWriteVmcs32(VMX_VMCS16_HOST_FIELD_DS, uSelDS);
    rc |= VMXWriteVmcs32(VMX_VMCS16_HOST_FIELD_ES, uSelES);
    rc |= VMXWriteVmcs32(VMX_VMCS16_HOST_FIELD_FS, uSelFS);
    rc |= VMXWriteVmcs32(VMX_VMCS16_HOST_FIELD_GS, uSelGS);
#endif
    rc |= VMXWriteVmcs32(VMX_VMCS16_HOST_FIELD_TR, uSelTR);
    AssertRCReturn(rc, rc);

    /*
     * Host GDTR and IDTR.
     */
    /** @todo Despite VT-x -not- restoring the limits on GDTR and IDTR it should
     *        be safe to -not- save and restore GDTR and IDTR in the assembly
     *        code and just do it here and don't care if the limits are zapped on
     *        VM-exit. */
    RTGDTR Gdtr;
    RT_ZERO(Gdtr);
#ifdef VBOX_WITH_HYBRID_32BIT_KERNEL
    if (VMX_IS_64BIT_HOST_MODE())
    {
        X86XDTR64 Gtr64;
        X86XDTR64 Idtr64;
        hmR0Get64bitGdtrAndIdtr(&Gdtr64, &Idtr64);
        rc  = VMXWriteVmcs64(VMX_VMCS_HOST_GDTR_BASE, Gdtr64.uAddr);
        rc |= VMXWriteVmcs64(VMX_VMCS_HOST_IDTR_BASE, Idtr64.uAddr);
        Gdtr.cbGdt = Gdtr64.cb;
        Gdtr.pGdt  = (uintptr_t)Gdtr64.uAddr;
    }
    else
#endif
    {
        RTIDTR Idtr;
        ASMGetGDTR(&Gdtr);
        ASMGetIDTR(&Idtr);
        rc  = VMXWriteVmcsHstN(VMX_VMCS_HOST_GDTR_BASE, Gdtr.pGdt);
        rc |= VMXWriteVmcsHstN(VMX_VMCS_HOST_IDTR_BASE, Idtr.pIdt);
    }
    AssertRCReturn(rc, rc);

    /*
     * Host TR base. Verify that TR selector doesn't point past the GDT. Masking off the TI and RPL bits
     * is effectively what the CPU does for "scaling by 8". TI is always 0 and RPL should be too in most cases.
     */
    if ((uSelTR & X86_SEL_MASK) > Gdtr.cbGdt)
    {
        AssertMsgFailed(("hmR0VmxSaveHostSegmentRegs: TR selector exceeds limit.TR=%RTsel Gdtr.cbGdt=%#x\n", uSelTR, Gdtr.cbGdt));
        return VERR_VMX_INVALID_HOST_STATE;
    }

    PCX86DESCHC pDesc = (PCX86DESCHC)(Gdtr.pGdt + (uSelTR & X86_SEL_MASK));
#ifdef VBOX_WITH_HYBRID_32BIT_KERNEL
    if (VMX_IS_64BIT_HOST_MODE())
    {
        /* We need the 64-bit TR base for hybrid darwin. */
        uint64_t u64TRBase = X86DESC64_BASE((PX86DESC64)pDesc);
        rc = VMXWriteVmcsHstN(VMX_VMCS_HOST_TR_BASE, u64TRBase);
    }
    else
#endif
    {
        uintptr_t uTRBase;
#if HC_ARCH_BITS == 64
        uTRBase = X86DESC64_BASE(pDesc);
#else
        uTRBase = X86DESC_BASE(pDesc);
#endif
        rc = VMXWriteVmcsHstN(VMX_VMCS_HOST_TR_BASE, uTRBase);
    }
    AssertRCReturn(rc, rc);

    /*
     * Host FS base and GS base.
     * For 32-bit hosts the base is handled by the assembly code where we push/pop FS and GS which                                                                .
     * would take care of the bases. In 64-bit, the MSRs come into play.
     */
#if HC_ARCH_BITS == 64 || defined(VBOX_WITH_HYBRID_32BIT_KERNEL)
    if (VMX_IS_64BIT_HOST_MODE())
    {
        uint64_t u64FSBase = ASMRdMsr(MSR_K8_FS_BASE);
        uint64_t u64GSBase = ASMRdMsr(MSR_K8_GS_BASE);
        rc  = VMXWriteVmcsHstN(VMX_VMCS_HOST_FS_BASE, u64FSBase);
        rc |= VMXWriteVmcsHstN(VMX_VMCS_HOST_GS_BASE, u64GSBase);
        AssertRCReturn(rc, rc);
    }
#endif
    return rc;
}


/**
 * Saves certain host MSRs in the VM-Exit MSR-load area and some in the
 * host-state area of the VMCS. Theses MSRs will be automatically restored on
 * the host after every successful VM exit.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 */
DECLINLINE(int) hmR0VmxSaveHostMsrs(PVM pVM, PVMCPU pVCpu)
{
    AssertPtr(pVCpu);
    AssertPtr(pVCpu->hm.s.vmx.pvHostMsr);

    PVMXMSR  pHostMsr           = (PVMXMSR)pVCpu->hm.s.vmx.pvHostMsr;
    unsigned idxHostMsr         = 0;
    uint32_t u32HostExtFeatures = pVM->hm.s.cpuid.u32AMDFeatureEDX;

    if (u32HostExtFeatures & (X86_CPUID_EXT_FEATURE_EDX_NX | X86_CPUID_EXT_FEATURE_EDX_LONG_MODE))
    {
        pHostMsr->u32IndexMSR = MSR_K6_EFER;
        pHostMsr->u32Reserved = 0;
#if HC_ARCH_BITS == 32 && defined(VBOX_ENABLE_64_BITS_GUESTS) && !defined(VBOX_WITH_HYBRID_32BIT_KERNEL)
        if (CPUMIsGuestInLongMode(pVCpu))
        {
            /* Must match the EFER value in our 64 bits switcher. */
            pHostMsr->u64Value = ASMRdMsr(MSR_K6_EFER) | MSR_K6_EFER_LME | MSR_K6_EFER_SCE | MSR_K6_EFER_NXE;
        }
        else
#endif
            pHostMsr->u64Value = ASMRdMsr(MSR_K6_EFER);
        pHostMsr++; idxHostMsr++;
    }

#if HC_ARCH_BITS == 64 || defined(VBOX_WITH_HYBRID_32BIT_KERNEL)
    if (VMX_IS_64BIT_HOST_MODE())
    {
        pHostMsr->u32IndexMSR  = MSR_K6_STAR;
        pHostMsr->u32Reserved  = 0;
        pHostMsr->u64Value     = ASMRdMsr(MSR_K6_STAR);              /* legacy syscall eip, cs & ss */
        pHostMsr++; idxHostMsr++;
        pHostMsr->u32IndexMSR  = MSR_K8_LSTAR;
        pHostMsr->u32Reserved  = 0;
        pHostMsr->u64Value     = ASMRdMsr(MSR_K8_LSTAR);             /* 64 bits mode syscall rip */
        pHostMsr++; idxHostMsr++;
        pHostMsr->u32IndexMSR  = MSR_K8_SF_MASK;
        pHostMsr->u32Reserved  = 0;
        pHostMsr->u64Value     = ASMRdMsr(MSR_K8_SF_MASK);           /* syscall flag mask */
        pHostMsr++; idxHostMsr++;
        /* The KERNEL_GS_BASE MSR doesn't work reliably with auto load/store. See @bugref{6208}  */
#if 0
        pMsr->u32IndexMSR = MSR_K8_KERNEL_GS_BASE;
        pMsr->u32Reserved = 0;
        pMsr->u64Value    = ASMRdMsr(MSR_K8_KERNEL_GS_BASE);         /* swapgs exchange value */
        pHostMsr++; idxHostMsr++;
#endif
    }
#endif

    /* Shouldn't ever happen but there -is- a number. We're well within the recommended 512. */
    if (idxHostMsr > MSR_IA32_VMX_MISC_MAX_MSR(pVM->hm.s.vmx.msr.vmx_misc))
        return VERR_HM_UNSUPPORTED_CPU_FEATURE_COMBO;

    int rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_EXIT_MSR_LOAD_COUNT, idxHostMsr);

    /*
     * Host Sysenter MSRs.
     */
    rc |= VMXWriteVmcs32(VMX_VMCS32_HOST_SYSENTER_CS,    ASMRdMsr_Low(MSR_IA32_SYSENTER_CS));
#ifdef VBOX_WITH_HYBRID_32BIT_KERNEL
    if (VMX_IS_64BIT_HOST_MODE())
    {
        rc |= VMXWriteVmcsHstN(VMX_VMCS_HOST_SYSENTER_ESP,   ASMRdMsr(MSR_IA32_SYSENTER_ESP));
        rc |= VMXWriteVmcsHstN(VMX_VMCS_HOST_SYSENTER_EIP,   ASMRdMsr(MSR_IA32_SYSENTER_EIP));
    }
    else
    {
        rc |= VMXWriteVmcsHstN(VMX_VMCS_HOST_SYSENTER_ESP,   ASMRdMsr_Low(MSR_IA32_SYSENTER_ESP));
        rc |= VMXWriteVmcsHstN(VMX_VMCS_HOST_SYSENTER_EIP,   ASMRdMsr_Low(MSR_IA32_SYSENTER_EIP));
    }
#elif HC_ARCH_BITS == 32
    rc |= VMXWriteVmcsHstN(VMX_VMCS_HOST_SYSENTER_ESP,       ASMRdMsr_Low(MSR_IA32_SYSENTER_ESP));
    rc |= VMXWriteVmcsHstN(VMX_VMCS_HOST_SYSENTER_EIP,       ASMRdMsr_Low(MSR_IA32_SYSENTER_EIP));
#else
    rc |= VMXWriteVmcsHstN(VMX_VMCS_HOST_SYSENTER_ESP,       ASMRdMsr(MSR_IA32_SYSENTER_ESP));
    rc |= VMXWriteVmcsHstN(VMX_VMCS_HOST_SYSENTER_EIP,       ASMRdMsr(MSR_IA32_SYSENTER_EIP));
#endif
    AssertRCReturn(rc, rc);

    /** @todo IA32_PERF_GLOBALCTRL, IA32_PAT, IA32_EFER, also see
     *        hmR0VmxSetupExitCtls() !! */
    return rc;
}


/**
 * Sets up VM-entry controls in the VMCS. These controls can affect things done
 * on VM-exit; e.g. "load debug controls", see Intel spec. 24.8.1 "VM-entry
 * controls".
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pCtx        Pointer to the guest-CPU context.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxLoadGuestEntryCtls(PVM pVM, PVMCPU pVCpu, PCPUMCTX pCtx)
{
    int rc = VINF_SUCCESS;
    if (pVCpu->hm.s.fContextUseFlags & HM_CHANGED_VMX_ENTRY_CTLS)
    {
        uint32_t val = pVM->hm.s.vmx.msr.vmx_entry.n.disallowed0;            /* Bits set here must be set in the VMCS. */
        uint32_t zap = pVM->hm.s.vmx.msr.vmx_entry.n.allowed1;               /* Bits cleared here must be cleared in the VMCS. */

        /* Load debug controls (DR7 & IA32_DEBUGCTL_MSR). The first VT-x capable CPUs only supports the 1-setting of this bit. */
        val |= VMX_VMCS_CTRL_ENTRY_CONTROLS_LOAD_DEBUG;

        /* Set if the guest is in long mode. This will set/clear the EFER.LMA bit on VM-entry. */
        if (CPUMIsGuestInLongModeEx(pCtx))
            val |= VMX_VMCS_CTRL_ENTRY_CONTROLS_IA32E_MODE_GUEST;
        else
            Assert(!(val & VMX_VMCS_CTRL_ENTRY_CONTROLS_IA32E_MODE_GUEST));

        /*
         * The following should not be set (since we're not in SMM mode):
         * - VMX_VMCS_CTRL_ENTRY_CONTROLS_ENTRY_SMM
         * - VMX_VMCS_CTRL_ENTRY_CONTROLS_DEACTIVATE_DUALMON
         */

        /** @todo VMX_VMCS_CTRL_ENTRY_CONTROLS_LOAD_GUEST_PERF_MSR,
         *        VMX_VMCS_CTRL_ENTRY_CONTROLS_LOAD_GUEST_PAT_MSR,
         *  VMX_VMCS_CTRL_ENTRY_CONTROLS_LOAD_GUEST_EFER_MSR */

        if ((val & zap) != val)
        {
            LogRel(("hmR0VmxLoadGuestEntryCtls: invalid VM-entry controls combo! cpu=%RX64 val=%RX64 zap=%RX64\n",
                    pVM->hm.s.vmx.msr.vmx_entry.n.disallowed0, val, zap));
            return VERR_HM_UNSUPPORTED_CPU_FEATURE_COMBO;
        }

        val &= zap;
        rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_ENTRY_CONTROLS, val);
        AssertRCReturn(rc, rc);

        /* Update VCPU with the currently set VM-exit controls. */
        pVCpu->hm.s.vmx.u32EntryCtls = val;
        pVCpu->hm.s.fContextUseFlags &= ~HM_CHANGED_VMX_ENTRY_CTLS;
    }
    return rc;
}


/**
 * Sets up the VM-exit controls in the VMCS.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 */
DECLINLINE(int) hmR0VmxLoadGuestExitCtls(PVM pVM, PVMCPU pVCpu)
{
    int rc = VINF_SUCCESS;
    if (pVCpu->hm.s.fContextUseFlags & HM_CHANGED_VMX_EXIT_CTLS)
    {
        uint32_t val = pVM->hm.s.vmx.msr.vmx_exit.n.disallowed0;            /* Bits set here must be set in the VMCS. */
        uint32_t zap = pVM->hm.s.vmx.msr.vmx_exit.n.allowed1;               /* Bits cleared here must be cleared in the VMCS. */

        /* Save debug controls (DR7 & IA32_DEBUGCTL_MSR). The first VT-x CPUs only supported the 1-setting of this bit. */
        val |= VMX_VMCS_CTRL_EXIT_CONTROLS_SAVE_DEBUG;

        /* Set the host long mode active (EFER.LMA) bit (which Intel calls "Host address-space size") if necessary. */
#if HC_ARCH_BITS == 64 || defined(VBOX_WITH_HYBRID_32BIT_KERNEL)
        if (VMX_IS_64BIT_HOST_MODE())
            val |= VMX_VMCS_CTRL_EXIT_CONTROLS_HOST_ADDR_SPACE_SIZE;
        else
            Assert(!(val & VMX_VMCS_CTRL_EXIT_CONTROLS_HOST_ADDR_SPACE_SIZE));
#elif HC_ARCH_BITS == 32 && defined(VBOX_ENABLE_64_BITS_GUESTS)
        if (CPUMIsGuestInLongModeEx(pCtx))
            val |= VMX_VMCS_CTRL_EXIT_CONTROLS_HOST_ADDR_SPACE_SIZE;    /* The switcher goes to long mode. */
        else
            Assert(!(val & VMX_VMCS_CTRL_EXIT_CONTROLS_HOST_ADDR_SPACE_SIZE));
#endif

        /* Don't acknowledge external interrupts on VM-exit. We want to let the host do that. */
        Assert(!(val & VMX_VMCS_CTRL_EXIT_CONTROLS_ACK_EXT_INT));

        /** @todo VMX_VMCS_CTRL_EXIT_CONTROLS_LOAD_PERF_MSR,
         *        VMX_VMCS_CTRL_EXIT_CONTROLS_SAVE_GUEST_PAT_MSR,
         *        VMX_VMCS_CTRL_EXIT_CONTROLS_LOAD_HOST_PAT_MSR,
         *        VMX_VMCS_CTRL_EXIT_CONTROLS_SAVE_GUEST_EFER_MSR,
         *        VMX_VMCS_CTRL_EXIT_CONTROLS_LOAD_HOST_EFER_MSR. */

        if (pVM->hm.s.vmx.msr.vmx_exit.n.allowed1 & VMX_VMCS_CTRL_EXIT_CONTROLS_SAVE_VMX_PREEMPT_TIMER)
            val |= VMX_VMCS_CTRL_EXIT_CONTROLS_SAVE_VMX_PREEMPT_TIMER;

        if ((val & zap) != val)
        {
            LogRel(("hmR0VmxSetupProcCtls: invalid VM-exit controls combo! cpu=%RX64 val=%RX64 zap=%RX64\n",
                    pVM->hm.s.vmx.msr.vmx_exit.n.disallowed0, val, zap));
            return VERR_HM_UNSUPPORTED_CPU_FEATURE_COMBO;
        }

        val &= zap;
        rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_EXIT_CONTROLS, val);

        /* Update VCPU with the currently set VM-exit controls. */
        pVCpu->hm.s.vmx.u32ExitCtls = val;
        pVCpu->hm.s.fContextUseFlags &= ~HM_CHANGED_VMX_EXIT_CTLS;
    }
    return rc;
}


/**
 * Loads the guest APIC and related state.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pCtx        Pointer to the guest-CPU context.
 */
DECLINLINE(int) hmR0VmxLoadGuestApicState(PVM pVM, PVMCPU pVCpu, PCPUMCTX pCtx)
{
    int rc = VINF_SUCCESS;
    if (pVCpu->hm.s.fContextUseFlags & HM_CHANGED_VMX_GUEST_APIC_STATE)
    {
        /* Setup TPR shadowing. Also setup TPR patching for 32-bit guests. */
        if (pVCpu->hm.s.vmx.u32ProcCtls & VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_USE_TPR_SHADOW)
        {
            Assert(pVCpu->hm.s.vmx.HCPhysVirtApic);

            bool    fPendingIntr = false;
            uint8_t u8GuestTpr   = 0;
            rc = PDMApicGetTPR(pVCpu, &u8GuestTpr, &fPendingIntr);
            AssertRCReturn(rc, rc);

            /*
             * If there are external interrupts pending but masked by the TPR value, apply the threshold so that if the guest
             * lowers the TPR, it would cause a VM-exit and we can deliver the interrupt.
             * If there are no external interrupts pending, set threshold to 0 to not cause a VM-exit. We will eventually deliver
             * the interrupt when we VM-exit for other reasons.
             */
            pVCpu->hm.s.vmx.pbVirtApic[0x80] = u8GuestTpr;      /* Offset 0x80 is TPR in the APIC MMIO range. */
            /* Bits 3-0 of the TPR threshold field correspond to bits 7-4 of the TPR (which is the Task-Priority Class). */
            uint32_t u32TprThreshold = fPendingIntr ? (u8GuestTpr >> 4) : 0;
            Assert(!(u32TprThreshold & 0xfffffff0));            /* Bits 31:4 MBZ. */

            rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_TPR_THRESHOLD, u32TprThreshold);
            AssertRCReturn(rc, rc);

            /* 32-bit guests uses LSTAR MSR for patching guest code which touches the TPR. */
            if (pVM->hm.s.fTPRPatchingActive)
            {
                Assert(!CPUMIsGuestInLongModeEx(pCtx));     /* EFER always up-to-date. */
                pCtx->msrLSTAR = u8GuestTpr;
                if (pVCpu->hm.s.vmx.u32ProcCtls & VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_USE_MSR_BITMAPS)
                {
                    /* If there are interrupts pending, intercept CR8 writes, otherwise don't intercept CR8 reads or writes. */
                    if (fPendingIntr)
                        hmR0VmxSetMsrPermission(pVCpu, MSR_K8_LSTAR, VMXMSREXIT_PASSTHRU_READ, VMXMSREXIT_INTERCEPT_WRITE);
                    else
                        hmR0VmxSetMsrPermission(pVCpu, MSR_K8_LSTAR, VMXMSREXIT_PASSTHRU_READ, VMXMSREXIT_PASSTHRU_WRITE);
                }
            }
        }

        pVCpu->hm.s.fContextUseFlags &= ~HM_CHANGED_VMX_GUEST_APIC_STATE;
    }
    return rc;
}


/**
 * Loads the guest's interruptibility-state ("interrupt shadow" as AMD calls it)
 * into the guest-state area in the VMCS.
 *
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pMixedCtx   Pointer to the guest-CPU context. The data may be
 *                      out-of-sync. Make sure to update the required fields
 *                      before using them.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(void) hmR0VmxLoadGuestIntrState(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx)
{
    /*
     * Instructions like STI and MOV SS inhibit interrupts till the next instruction completes. Check if we should
     * inhibit interrupts or clear any existing interrupt-inhibition.
     */
    uint32_t uIntrState = 0;
    if (VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_INHIBIT_INTERRUPTS))
    {
        /* If inhibition is active, RIP & RFLAGS should've been accessed (i.e. read previously from the VMCS or from ring-3). */
        AssertMsg((pVCpu->hm.s.vmx.fUpdatedGuestState & (VMX_UPDATED_GUEST_RIP | VMX_UPDATED_GUEST_RFLAGS)),
                  ("%#x\n", pVCpu->hm.s.vmx.fUpdatedGuestState));
        if (pMixedCtx->rip != EMGetInhibitInterruptsPC(pVCpu))
        {
            /*
             * We can clear the inhibit force flag as even if we go back to the recompiler without executing guest code in
             * VT-x the flag's condition to be cleared is met and thus the cleared state is correct.
             * hmR0VmxInjectPendingInterrupt() relies on us clearing this flag here.
             */
            VMCPU_FF_CLEAR(pVCpu, VMCPU_FF_INHIBIT_INTERRUPTS);
        }
        else if (pMixedCtx->eflags.Bits.u1IF)
            uIntrState = VMX_VMCS_GUEST_INTERRUPTIBILITY_STATE_BLOCK_STI;
        else
            uIntrState = VMX_VMCS_GUEST_INTERRUPTIBILITY_STATE_BLOCK_MOVSS;
    }

    Assert(!(uIntrState & 0xfffffff0));                             /* Bits 31:4 MBZ. */
    Assert((uIntrState & 0x3) != 0x3);                              /* Block-by-STI and MOV SS cannot be simultaneously set. */
    int rc = VMXWriteVmcs32(VMX_VMCS32_GUEST_INTERRUPTIBILITY_STATE, uIntrState);
    AssertRC(rc);
}


/**
 * Loads the guest's RIP into the guest-state area in the VMCS.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pMixedCtx   Pointer to the guest-CPU context. The data may be
 *                      out-of-sync. Make sure to update the required fields
 *                      before using them.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxLoadGuestRip(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx)
{
    int rc = VINF_SUCCESS;
    if (pVCpu->hm.s.fContextUseFlags & HM_CHANGED_GUEST_RIP)
    {
        rc = VMXWriteVmcsGstN(VMX_VMCS_GUEST_RIP, pMixedCtx->rip);
        AssertRCReturn(rc, rc);
        Log(("VMX_VMCS_GUEST_RIP=%#RX64\n", pMixedCtx->rip));
        pVCpu->hm.s.fContextUseFlags &= ~HM_CHANGED_GUEST_RIP;
    }
    return rc;
}


/**
 * Loads the guest's RSP into the guest-state area in the VMCS.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pCtx        Pointer to the guest-CPU context.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxLoadGuestRsp(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx)
{
    int rc = VINF_SUCCESS;
    if (pVCpu->hm.s.fContextUseFlags & HM_CHANGED_GUEST_RSP)
    {
        rc = VMXWriteVmcsGstN(VMX_VMCS_GUEST_RSP, pMixedCtx->rsp);
        AssertRCReturn(rc, rc);
        pVCpu->hm.s.fContextUseFlags &= ~HM_CHANGED_GUEST_RSP;
    }
    return rc;
}


/**
 * Loads the guest's RFLAGS into the guest-state area in the VMCS.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pCtx        Pointer to the guest-CPU context.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxLoadGuestRflags(PVM pVM, PVMCPU pVCpu, PCPUMCTX pCtx)
{
    int rc = VINF_SUCCESS;
    if (pVCpu->hm.s.fContextUseFlags & HM_CHANGED_GUEST_RFLAGS)
    {
        /* Intel spec. 2.3.1 "System Flags and Fields in IA-32e Mode" claims the upper 32-bits of RFLAGS are reserved (MBZ).
           Let us assert it as such and use native-width VMWRITE. */
        X86RFLAGS uRFlags = pCtx->rflags;
        Assert(uRFlags.u64 >> 32 == 0);
        uRFlags.u64 &= VMX_EFLAGS_RESERVED_0;                   /* Bits 22-31, 15, 5 & 3 MBZ. */
        uRFlags.u64 |= VMX_EFLAGS_RESERVED_1;                   /* Bit 1 MB1. */

        /*
         * If we're emulating real-mode using Virtual 8086 mode, save the real-mode eflags so we can restore them on VM exit.
         * Modify the real-mode guest's eflags so that VT-x can run the real-mode guest code under Virtual 8086 mode.
         */
        if (pVCpu->hm.s.vmx.RealMode.fRealOnV86Active)
        {
            Assert(pVM->hm.s.vmx.pRealModeTSS);
            Assert(PDMVmmDevHeapIsEnabled(pVM));
            pVCpu->hm.s.vmx.RealMode.eflags.u32 = uRFlags.u64; /* Save the original eflags of the real-mode guest. */
            uRFlags.Bits.u1VM   = 1;                           /* Set the Virtual 8086 mode bit. */
            uRFlags.Bits.u2IOPL = 0;                           /* Change IOPL to 0, otherwise certain instructions won't fault. */
        }

        rc = VMXWriteVmcsGstN(VMX_VMCS_GUEST_RFLAGS, uRFlags.u64);
        AssertRCReturn(rc, rc);

        Log(("VMX_VMCS_GUEST_RFLAGS=%#RX64\n", uRFlags.u64));
        pVCpu->hm.s.fContextUseFlags &= ~HM_CHANGED_GUEST_RFLAGS;
    }
    return rc;
}


/**
 * Loads the guest's general purpose registers (GPRs) - RIP, RSP and RFLAGS
 * into the guest-state area in the VMCS. The remaining GPRs are handled in the
 * assembly code.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pCtx        Pointer to the guest-CPU context.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxLoadGuestGprs(PVM pVM, PVMCPU pVCpu, PCPUMCTX pCtx)
{
    LogFlowFunc(("pVM=%p pVCpu=%p pCtx=%p\n", pVM, pVCpu, pCtx));
    int rc = hmR0VmxLoadGuestRip(pVM, pVCpu, pCtx);
    rc    |= hmR0VmxLoadGuestRsp(pVM, pVCpu, pCtx);
    rc    |= hmR0VmxLoadGuestRflags(pVM, pVCpu, pCtx);
    return rc;
}


/**
 * Loads the guest control registers (CR0, CR3, CR4) into the guest-state area
 * in the VMCS.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pCtx        Pointer to the guest-CPU context.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxLoadGuestControlRegs(PVM pVM, PVMCPU pVCpu, PCPUMCTX pCtx)
{
    int rc = VINF_SUCCESS;

    /*
     * Guest CR0.
     * Guest FPU.
     */
    if (pVCpu->hm.s.fContextUseFlags & HM_CHANGED_GUEST_CR0)
    {
        uint64_t u64GuestCR0 = pCtx->cr0;

        /* The guest's view (read access) of its CR0 is unblemished. */
        rc  = VMXWriteVmcsGstN(VMX_VMCS_CTRL_CR0_READ_SHADOW, u64GuestCR0);
        AssertRCReturn(rc, rc);
        Log2(("VMX_VMCS_CTRL_CR0_READ_SHADOW=%#RX64\n", u64GuestCR0));

        /* Setup VT-x's view of the guest CR0. */
        /* Minimize VM-exits due to CR3 changes when we have NestedPaging. */
        if (pVM->hm.s.fNestedPaging)
        {
            if (CPUMIsGuestPagingEnabledEx(pCtx))
            {
                /* The guest has paging enabled, let it access CR3 without causing a VM exit if supported. */
                pVCpu->hm.s.vmx.u32ProcCtls &= ~(  VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_CR3_LOAD_EXIT
                                                 | VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_CR3_STORE_EXIT);
            }
            else
            {
                /* The guest doesn't have paging enabled, make CR3 access to cause VM exits to update our shadow. */
                pVCpu->hm.s.vmx.u32ProcCtls |=   VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_CR3_LOAD_EXIT
                                               | VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_CR3_STORE_EXIT;
            }

            rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_PROC_EXEC_CONTROLS, pVCpu->hm.s.vmx.u32ProcCtls);
            AssertRCReturn(rc, rc);
        }
        else
            u64GuestCR0 |= X86_CR0_WP;     /* Guest CPL 0 writes to its read-only pages should cause a VM-exit. */

        /*
         * Guest FPU bits.
         * Intel spec. 23.8 "Restrictions on VMX operation" mentions that CR0.NE bit must always be set on the first
         * CPUs to support VT-x (prob. means all the way up to Nehalem) and no mention of with regards to UX in VM-entry checks.
         */
        u64GuestCR0 |= X86_CR0_NE;
        bool fInterceptNM = false;
        if (CPUMIsGuestFPUStateActive(pVCpu))
        {
            fInterceptNM = false;              /* Guest FPU active, no need to VM-exit on #NM. */
            /* The guest should still get #NM exceptions when it expects it to, so we should not clear TS & MP bits here.
               We're only concerned about -us- not intercepting #NMs when the guest-FPU is active. Not the guest itself! */
        }
        else
        {
            fInterceptNM = true;               /* Guest FPU inactive, VM-exit on #NM for lazy FPU loading. */
            u64GuestCR0 |=  X86_CR0_TS         /* Guest can task switch quickly and do lazy FPU syncing. */
                          | X86_CR0_MP;        /* FWAIT/WAIT should not ignore CR0.TS and should generate #NM. */
        }

        /* Catch floating point exceptions if we need to report them to the guest in a different way. */
        bool fInterceptMF = false;
        if (!(pCtx->cr0 & X86_CR0_NE))
            fInterceptMF = true;

        /* Finally, intercept all exceptions as we cannot directly inject them in real-mode, see hmR0VmxInjectEventVmcs(). */
        if (pVCpu->hm.s.vmx.RealMode.fRealOnV86Active)
        {
            Assert(PDMVmmDevHeapIsEnabled(pVM));
            Assert(pVM->hm.s.vmx.pRealModeTSS);
            pVCpu->hm.s.vmx.u32XcptBitmap |= VMX_REAL_MODE_XCPT_BITMAP;
            fInterceptNM = true;
            fInterceptMF = true;
        }
        else
            pVCpu->hm.s.vmx.u32XcptBitmap &= ~VMX_REAL_MODE_XCPT_BITMAP;

        if (fInterceptNM)
            pVCpu->hm.s.vmx.u32XcptBitmap |= RT_BIT(X86_XCPT_NM);
        else
            pVCpu->hm.s.vmx.u32XcptBitmap &= ~RT_BIT(X86_XCPT_NM);

        if (fInterceptMF)
            pVCpu->hm.s.vmx.u32XcptBitmap |= RT_BIT(X86_XCPT_MF);
        else
            pVCpu->hm.s.vmx.u32XcptBitmap &= ~RT_BIT(X86_XCPT_MF);

        /* Additional intercepts for debugging, define these yourself explicitly. */
#ifdef VBOX_ALWAYS_TRAP_ALL_EXCEPTIONS
        pVCpu->hm.s.vmx.u32XcptBitmap |=   RT_BIT(X86_XCPT_BP)
                                         | RT_BIT(X86_XCPT_DB)
                                         | RT_BIT(X86_XCPT_DE)
                                         | RT_BIT(X86_XCPT_NM)
                                         | RT_BIT(X86_XCPT_UD)
                                         | RT_BIT(X86_XCPT_NP)
                                         | RT_BIT(X86_XCPT_SS)
                                         | RT_BIT(X86_XCPT_GP)
                                         | RT_BIT(X86_XCPT_PF)
                                         | RT_BIT(X86_XCPT_MF);
#elif defined(VBOX_ALWAYS_TRAP_PF)
        pVCpu->hm.s.vmx.u32XcptBitmap    |= RT_BIT(X86_XCPT_PF)
#endif

        Assert(pVM->hm.s.fNestedPaging || (pVCpu->hm.s.vmx.u32XcptBitmap & RT_BIT(X86_XCPT_PF)));

        /* Set/clear the CR0 specific bits along with their exceptions (PE, PG, CD, NW). */
        uint64_t uSetCR0 = (pVM->hm.s.vmx.msr.vmx_cr0_fixed0 & pVM->hm.s.vmx.msr.vmx_cr0_fixed1);
        uint64_t uZapCR0 = (pVM->hm.s.vmx.msr.vmx_cr0_fixed0 | pVM->hm.s.vmx.msr.vmx_cr0_fixed1);
        if (pVM->hm.s.vmx.fUnrestrictedGuest)               /* Exceptions for unrestricted-guests for fixed CR0 bits (PE, PG). */
            uSetCR0 &= ~(X86_CR0_PE | X86_CR0_PG);
        else
            Assert((uSetCR0 & (X86_CR0_PE | X86_CR0_PG)) == (X86_CR0_PE | X86_CR0_PG));

        u64GuestCR0 |= uSetCR0;
        u64GuestCR0 &= uZapCR0;
        u64GuestCR0 &= ~(X86_CR0_CD | X86_CR0_NW);          /* Always enable caching. */

        /* Write VT-x's view of the guest CR0 into the VMCS and update the exception bitmap. */
        rc  = VMXWriteVmcsGstN(VMX_VMCS_GUEST_CR0, u64GuestCR0);
        rc |= VMXWriteVmcs32(VMX_VMCS32_CTRL_EXCEPTION_BITMAP, pVCpu->hm.s.vmx.u32XcptBitmap);
        Log2(("VMX_VMCS_GUEST_CR0=%#RX32\n", (uint32_t)u64GuestCR0));

        /*
         * CR0 is shared between host and guest along with a CR0 read shadow. Therefore, certain bits must not be changed
         * by the guest because VT-x ignores saving/restoring them (namely CD, ET, NW) and for certain other bits
         * we want to be notified immediately of guest CR0 changes (e.g. PG to update our shadow page tables).
         */
        uint64_t u64CR0Mask = 0;
        u64CR0Mask =  X86_CR0_PE
                    | X86_CR0_WP
                    | X86_CR0_PG
                    | X86_CR0_ET    /* Bit ignored on VM-entry and VM-exit. Don't let the guest modify the host CR0.ET */
                    | X86_CR0_CD    /* Bit ignored on VM-entry and VM-exit. Don't let the guest modify the host CR0.CD */
                    | X86_CR0_NW;   /* Bit ignored on VM-entry and VM-exit. Don't let the guest modify the host CR0.NW */

        /* We don't need to intercept changes to CR0.PE with unrestricted guests. */
        if (pVM->hm.s.vmx.fUnrestrictedGuest)
            u64CR0Mask &= ~X86_CR0_PE;

        /* If the guest FPU state is active, don't need to VM-exit on writes to FPU related bits in CR0. */
        if (fInterceptNM)
            u64CR0Mask |=  (X86_CR0_TS | X86_CR0_MP);
        else
            u64CR0Mask &= ~(X86_CR0_TS | X86_CR0_MP);

        /* Write the CR0 mask into the VMCS and update the VCPU's copy of the current CR0 mask. */
        pVCpu->hm.s.vmx.cr0_mask = u64CR0Mask;
        rc |= VMXWriteVmcsHstN(VMX_VMCS_CTRL_CR0_MASK, u64CR0Mask);
        AssertRCReturn(rc, rc);

        pVCpu->hm.s.fContextUseFlags &= ~HM_CHANGED_GUEST_CR0;
    }

    /*
     * Guest CR2.
     * It's always loaded in the assembler code. Nothing to do here.
     */

    /*
     * Guest CR3.
     */
    if (pVCpu->hm.s.fContextUseFlags & HM_CHANGED_GUEST_CR3)
    {
        uint64_t u64GuestCR3 = 0;
        if (pVM->hm.s.fNestedPaging)
        {
            pVCpu->hm.s.vmx.GCPhysEPTP = PGMGetHyperCR3(pVCpu);

            /* Validate. See Intel spec. 28.2.2 "EPT Translation Mechanism" and 24.6.11 "Extended-Page-Table Pointer (EPTP)" */
            Assert(pVCpu->hm.s.vmx.GCPhysEPTP);
            Assert(!(pVCpu->hm.s.vmx.GCPhysEPTP & 0xfff0000000000000ULL));
            Assert(!(pVCpu->hm.s.vmx.GCPhysEPTP & 0xfff));

            /* VMX_EPT_MEMTYPE_WB support is already checked in hmR0VmxSetupTaggedTlb(). */
            pVCpu->hm.s.vmx.GCPhysEPTP |=  VMX_EPT_MEMTYPE_WB
                                         | (VMX_EPT_PAGE_WALK_LENGTH_DEFAULT << VMX_EPT_PAGE_WALK_LENGTH_SHIFT);

            /* Validate. See Intel spec. 26.2.1 "Checks on VMX Controls" */
            AssertMsg(   ((pVCpu->hm.s.vmx.GCPhysEPTP >> 3) & 0x07) == 3      /* Bits 3:5 (EPT page walk length - 1) must be 3. */
                      && ((pVCpu->hm.s.vmx.GCPhysEPTP >> 6) & 0x3f) == 0,     /* Bits 6:11 MBZ. */
                         ("EPTP %#RX64\n", pVCpu->hm.s.vmx.GCPhysEPTP));

            rc = VMXWriteVmcs64(VMX_VMCS64_CTRL_EPTP_FULL, pVCpu->hm.s.vmx.GCPhysEPTP);
            AssertRCReturn(rc, rc);
            Log(("VMX_VMCS64_CTRL_EPTP_FULL=%#RX64\n", pVCpu->hm.s.vmx.GCPhysEPTP));

            if (   pVM->hm.s.vmx.fUnrestrictedGuest
                || CPUMIsGuestPagingEnabledEx(pCtx))
            {
                /* If the guest is in PAE mode, pass the PDPEs to VT-x using the VMCS fields. */
                if (CPUMIsGuestInPAEModeEx(pCtx))
                {
                    rc  = PGMGstGetPaePdpes(pVCpu, &pVCpu->hm.s.aPdpes[0]);
                    rc |= VMXWriteVmcs64(VMX_VMCS64_GUEST_PDPTE0_FULL, pVCpu->hm.s.aPdpes[0].u);
                    rc |= VMXWriteVmcs64(VMX_VMCS64_GUEST_PDPTE1_FULL, pVCpu->hm.s.aPdpes[1].u);
                    rc |= VMXWriteVmcs64(VMX_VMCS64_GUEST_PDPTE2_FULL, pVCpu->hm.s.aPdpes[2].u);
                    rc |= VMXWriteVmcs64(VMX_VMCS64_GUEST_PDPTE3_FULL, pVCpu->hm.s.aPdpes[3].u);
                    AssertRCReturn(rc, rc);
                }

                /* The guest's view of its CR3 is unblemished with Nested Paging when the guest is using paging or we
                   have Unrestricted Execution to handle the guest when it's not using paging. */
                u64GuestCR3 = pCtx->cr3;
            }
            else
            {
                /*
                 * The guest is not using paging, but the CPU (VT-x) has to. While the guest thinks it accesses physical memory
                 * directly, we use our identity-mapped page table to map guest-linear to guest-physical addresses.
                 * EPT takes care of translating it to host-physical addresses.
                 */
                RTGCPHYS GCPhys;
                Assert(pVM->hm.s.vmx.pNonPagingModeEPTPageTable);
                Assert(PDMVmmDevHeapIsEnabled(pVM));

                /* We obtain it here every time as the guest could have relocated this PCI region. */
                rc = PDMVmmDevHeapR3ToGCPhys(pVM, pVM->hm.s.vmx.pNonPagingModeEPTPageTable, &GCPhys);
                AssertRCReturn(rc, rc);

                u64GuestCR3 = GCPhys;
            }
        }
        else
        {
            /* Non-nested paging case, just use the hypervisor's CR3. */
            u64GuestCR3 = PGMGetHyperCR3(pVCpu);
        }

        Log2(("VMX_VMCS_GUEST_CR3=%#RX64\n", u64GuestCR3));
        rc = VMXWriteVmcsGstN(VMX_VMCS_GUEST_CR3, u64GuestCR3);
        AssertRCReturn(rc, rc);

        pVCpu->hm.s.fContextUseFlags &= ~HM_CHANGED_GUEST_CR3;
    }

    /*
     * Guest CR4.
     */
    if (pVCpu->hm.s.fContextUseFlags & HM_CHANGED_GUEST_CR4)
    {
        uint64_t u64GuestCR4 = pCtx->cr4;

        /* The guest's view of its CR4 is unblemished. */
        rc  = VMXWriteVmcsGstN(VMX_VMCS_CTRL_CR4_READ_SHADOW, u64GuestCR4);
        AssertRCReturn(rc, rc);
        Log2(("VMX_VMCS_CTRL_CR4_READ_SHADOW=%#RGv\n", u64GuestCR4));

        /* Setup VT-x's view of the guest CR4. */
        /*
         * If we're emulating real-mode using virtual-8086 mode, we want to redirect software interrupts to the 8086 program
         * interrupt handler. Clear the VME bit (the interrupt redirection bitmap is already all 0, see hmR3InitFinalizeR0())
         * See Intel spec. 20.2 "Software Interrupt Handling Methods While in Virtual-8086 Mode".
         */
        if (pVCpu->hm.s.vmx.RealMode.fRealOnV86Active)
        {
            Assert(pVM->hm.s.vmx.pRealModeTSS);
            Assert(PDMVmmDevHeapIsEnabled(pVM));
            u64GuestCR4 &= ~X86_CR4_VME;
        }

        if (pVM->hm.s.fNestedPaging)
        {
            if (   !CPUMIsGuestPagingEnabledEx(pCtx)
                && !pVM->hm.s.vmx.fUnrestrictedGuest)
            {
                /* We use 4 MB pages in our identity mapping page table when the guest doesn't have paging. */
                u64GuestCR4 |= X86_CR4_PSE;
                /* Our identity mapping is a 32 bits page directory. */
                u64GuestCR4 &= ~X86_CR4_PAE;
            }
            /* else use guest CR4.*/
        }
        else
        {
            /*
             * The shadow paging modes and guest paging modes are different, the shadow is in accordance with the host
             * paging mode and thus we need to adjust VT-x's view of CR4 depending on our shadow page tables.
             */
            switch (pVCpu->hm.s.enmShadowMode)
            {
                case PGMMODE_REAL:              /* Real-mode. */
                case PGMMODE_PROTECTED:         /* Protected mode without paging. */
                case PGMMODE_32_BIT:            /* 32-bit paging. */
                {
                    u64GuestCR4 &= ~X86_CR4_PAE;
                    break;
                }

                case PGMMODE_PAE:               /* PAE paging. */
                case PGMMODE_PAE_NX:            /* PAE paging with NX. */
                {
                    u64GuestCR4 |= X86_CR4_PAE;
                    break;
                }

                case PGMMODE_AMD64:             /* 64-bit AMD paging (long mode). */
                case PGMMODE_AMD64_NX:          /* 64-bit AMD paging (long mode) with NX enabled. */
#ifdef VBOX_ENABLE_64_BITS_GUESTS
                    break;
#endif
                default:
                    AssertFailed();
                    return VERR_PGM_UNSUPPORTED_SHADOW_PAGING_MODE;
            }
        }

        /* We need to set and clear the CR4 specific bits here (mainly the X86_CR4_VMXE bit). */
        uint64_t uSetCR4 = (pVM->hm.s.vmx.msr.vmx_cr4_fixed0 & pVM->hm.s.vmx.msr.vmx_cr4_fixed1);
        uint64_t uZapCR4 = (pVM->hm.s.vmx.msr.vmx_cr4_fixed0 | pVM->hm.s.vmx.msr.vmx_cr4_fixed1);
        u64GuestCR4 |= uSetCR4;
        u64GuestCR4 &= uZapCR4;

        /* Write VT-x's view of the guest CR4 into the VMCS. */
        Log2(("VMX_VMCS_GUEST_CR4=%#RGv (Set=%#RX32 Zap=%#RX32)\n", u64GuestCR4, uSetCR4, uZapCR4));
        rc = VMXWriteVmcsGstN(VMX_VMCS_GUEST_CR4, u64GuestCR4);

        /* Setup CR4 mask. CR4 flags owned by the host, if the guest attempts to change them, that would cause a VM exit. */
        uint64_t u64CR4Mask = 0;
        u64CR4Mask =  X86_CR4_VME
                    | X86_CR4_PAE
                    | X86_CR4_PGE
                    | X86_CR4_PSE
                    | X86_CR4_VMXE;
        pVCpu->hm.s.vmx.cr4_mask = u64CR4Mask;
        rc |= VMXWriteVmcsHstN(VMX_VMCS_CTRL_CR4_MASK, u64CR4Mask);
        AssertRCReturn(rc, rc);

        pVCpu->hm.s.fContextUseFlags &= ~HM_CHANGED_GUEST_CR4;
    }
    return rc;
}


/**
 * Loads the guest debug registers into the guest-state area in the VMCS.
 * This also sets up whether #DB and MOV DRx accesses cause VM exits.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pCtx        Pointer to the guest-CPU context.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxLoadGuestDebugRegs(PVM pVM, PVMCPU pVCpu, PCPUMCTX pCtx)
{
    if (!(pVCpu->hm.s.fContextUseFlags & HM_CHANGED_GUEST_DEBUG))
        return VINF_SUCCESS;

#ifdef DEBUG
    /* Validate. Intel spec. 26.3.1.1 "Checks on Guest Controls Registers, Debug Registers, MSRs" */
    if (pVCpu->hm.s.vmx.u32EntryCtls & VMX_VMCS_CTRL_ENTRY_CONTROLS_LOAD_DEBUG)
    {
        Assert((pCtx->dr[7] & 0xffffffff00000000ULL) == 0);  /* upper 32 bits are reserved (MBZ). */
        /* Validate. Intel spec. 17.2 "Debug Registers", recompiler paranoia checks. */
        Assert((pCtx->dr[7] & 0xd800) == 0);                 /* bits 15, 14, 12, 11 are reserved (MBZ). */
        Assert((pCtx->dr[7] & 0x400) == 0x400);              /* bit 10 is reserved (MB1). */
    }
#endif

    int rc                = VERR_INTERNAL_ERROR_5;
    bool fInterceptDB     = false;
    bool fInterceptMovDRx = false;
    if (DBGFIsStepping(pVCpu))
    {
        /* If the CPU supports the monitor trap flag, use it for single stepping in DBGF. */
        if (pVM->hm.s.vmx.msr.vmx_proc_ctls.n.allowed1 & VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_MONITOR_TRAP_FLAG)
        {
            pVCpu->hm.s.vmx.u32ProcCtls |= VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_MONITOR_TRAP_FLAG;
            rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_PROC_EXEC_CONTROLS, pVCpu->hm.s.vmx.u32ProcCtls);
            AssertRCReturn(rc, rc);
            Assert(fInterceptDB == false);
        }
        else if (pCtx->eflags.Bits.u1TF)    /* If the guest is using its TF bit, we cannot single step in DBGF. */
        {
            Assert(fInterceptDB == false);
            /** @todo can we somehow signal DBGF that it cannot single-step instead of
             *        just continuing? */
        }
        else
            fInterceptDB = true;
    }
    else
        Assert(fInterceptDB == false);      /* If we are not single stepping in DBGF, there is no need to intercept #DB. */

    /*
     * If the guest is using its DRx registers and the host DRx does not yet contain the guest DRx values,
     * load the guest DRx registers into the host and don't cause VM-exits on guest's MOV DRx accesses.
     * The same for the hypervisor DRx registers, priority is for the guest here.
     */
    if (    (pCtx->dr[7] & (X86_DR7_ENABLED_MASK | X86_DR7_GD))
        && !CPUMIsGuestDebugStateActive(pVCpu))
    {
        /* Save the host and load the guest debug registers. This will make the guest debug state active. */
        rc = CPUMR0LoadGuestDebugState(pVM, pVCpu, pCtx, true /* include DR6 */);
        AssertRC(rc);
        Assert(CPUMIsGuestDebugStateActive(pVCpu));
        Assert(fInterceptMovDRx == false);
        STAM_COUNTER_INC(&pVCpu->hm.s.StatDRxArmed);
    }
    else if (    CPUMGetHyperDR7(pVCpu) & (X86_DR7_ENABLED_MASK | X86_DR7_GD)
             && !CPUMIsHyperDebugStateActive(pVCpu))
    {
        /* Save the host and load the hypervisor debug registers. This will make the hyper debug state active. */
        rc = CPUMR0LoadHyperDebugState(pVM, pVCpu, pCtx, true /* include DR6 */);
        AssertRC(rc);
        Assert(CPUMIsHyperDebugStateActive(pVCpu));
        fInterceptMovDRx = true;
    }
    else
        Assert(fInterceptMovDRx == false);  /* No need to intercept MOV DRx if DBGF is not active nor the guest is debugging. */

    /* Update the exception bitmap regarding intercepting #DB generated by the guest. */
    if (fInterceptDB)
        pVCpu->hm.s.vmx.u32XcptBitmap |= RT_BIT(X86_XCPT_DB);
    else if (!pVCpu->hm.s.vmx.RealMode.fRealOnV86Active)
    {
#ifndef VBOX_ALWAYS_TRAP_ALL_EXCEPTIONS
        pVCpu->hm.s.vmx.u32XcptBitmap &= ~RT_BIT(X86_XCPT_DB);
#endif
    }

    /* Update the processor-based VM-execution controls regarding intercepting MOV DRx instructions. */
    if (fInterceptMovDRx)
        pVCpu->hm.s.vmx.u32ProcCtls   |= VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_MOV_DR_EXIT;
    else
        pVCpu->hm.s.vmx.u32ProcCtls   &= ~VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_MOV_DR_EXIT;

    rc  = VMXWriteVmcs32(VMX_VMCS32_CTRL_EXCEPTION_BITMAP,   pVCpu->hm.s.vmx.u32XcptBitmap);
    rc |= VMXWriteVmcs32(VMX_VMCS32_CTRL_PROC_EXEC_CONTROLS, pVCpu->hm.s.vmx.u32ProcCtls);

    /* The guest's view of its DR7 is unblemished. */
    rc |= VMXWriteVmcsGstN(VMX_VMCS_GUEST_DR7, pCtx->dr[7]);

    pVCpu->hm.s.fContextUseFlags &= ~HM_CHANGED_GUEST_DEBUG;
    return rc;
}


#ifdef DEBUG
/**
 * Debug function to validate segment registers.
 */
static void hmR0VmxValidateSegmentRegs(PVM pVM, PVMCPU pVCpu, PCPUMCTX pCtx)
{
    /* Validate segment registers. See Intel spec. 26.3.1.2 "Checks on Guest Segment Registers". */
    if (   !pVM->hm.s.vmx.fUnrestrictedGuest
        && (   !CPUMIsGuestInRealModeEx(pCtx)
            && !CPUMIsGuestInV86ModeEx(pCtx)))
    {
        /* Protected mode checks */
        /* CS */
        Assert(pCtx->cs.Attr.n.u1Present);
        Assert(!(pCtx->cs.Attr.u & 0xf00));
        Assert(!(pCtx->cs.Attr.u & 0xfffe0000));
        Assert(   (pCtx->cs.u32Limit & 0xfff) == 0xfff
               || !(pCtx->cs.Attr.n.u1Granularity));
        Assert(   !(pCtx->cs.u32Limit & 0xfff00000)
               || (pCtx->cs.Attr.n.u1Granularity));
        Assert(pCtx->cs.Attr.u && pCtx->cs.Attr.u != VMX_SEL_UNUSABLE);  /* CS cannot be loaded with NULL in protected mode. */
        if (pCtx->cs.Attr.n.u4Type == 9 || pCtx->cs.Attr.n.u4Type == 11)
            Assert(pCtx->cs.Attr.n.u2Dpl == pCtx->ss.Attr.n.u2Dpl);
        else if (pCtx->cs.Attr.n.u4Type == 13 || pCtx->cs.Attr.n.u4Type == 15)
            Assert(pCtx->cs.Attr.n.u2Dpl <= pCtx->ss.Attr.n.u2Dpl);
        else
            AssertMsgFailed(("Invalid CS Type %#x\n", pCtx->cs.Attr.n.u2Dpl));
        /* SS */
        if (pCtx->ss.Attr.u && pCtx->ss.Attr.u != VMX_SEL_UNUSABLE)
        {
            Assert((pCtx->ss.Sel & X86_SEL_RPL) == (pCtx->cs.Sel & X86_SEL_RPL));
            Assert(pCtx->ss.Attr.n.u4Type == 3 || pCtx->ss.Attr.n.u4Type == 7);
            Assert(pCtx->ss.Attr.n.u1Present);
            Assert(!(pCtx->ss.Attr.u & 0xf00));
            Assert(!(pCtx->ss.Attr.u & 0xfffe0000));
            Assert(   (pCtx->ss.u32Limit & 0xfff) == 0xfff
                   || !(pCtx->ss.Attr.n.u1Granularity));
            Assert(   !(pCtx->ss.u32Limit & 0xfff00000)
                   || (pCtx->ss.Attr.n.u1Granularity));
        }
        Assert(pCtx->ss.Attr.n.u2Dpl == (pCtx->ss.Sel & X86_SEL_RPL));
        /* CR0 might not be up-to-date here always, hence disabled. */
#if 0
        if (!pCtx->cr0 & X86_CR0_PE)
            Assert(!pCtx->ss.Attr.n.u2Dpl);
#endif
        /* DS, ES, FS, GS - only check for usable selectors, see hmR0VmxWriteSegmentReg(). */
        if (pCtx->ds.Attr.u && pCtx->ds.Attr.u != VMX_SEL_UNUSABLE)
        {
            Assert(pCtx->ds.Attr.n.u4Type & X86_SEL_TYPE_ACCESSED);
            Assert(pCtx->ds.Attr.n.u1Present);
            Assert(pCtx->ds.Attr.n.u4Type > 11 || pCtx->ds.Attr.n.u2Dpl >= (pCtx->ds.Sel & X86_SEL_RPL));
            Assert(!(pCtx->ds.Attr.u & 0xf00));
            Assert(!(pCtx->ds.Attr.u & 0xfffe0000));
            Assert(   (pCtx->ds.u32Limit & 0xfff) == 0xfff
                   || !(pCtx->ds.Attr.n.u1Granularity));
            Assert(   !(pCtx->ds.u32Limit & 0xfff00000)
                   || (pCtx->ds.Attr.n.u1Granularity));
            Assert(   !(pCtx->ds.Attr.n.u4Type & X86_SEL_TYPE_CODE)
                   || (pCtx->ds.Attr.n.u4Type & X86_SEL_TYPE_READ));
        }
        if (pCtx->es.Attr.u && pCtx->es.Attr.u != VMX_SEL_UNUSABLE)
        {
            Assert(pCtx->es.Attr.n.u4Type & X86_SEL_TYPE_ACCESSED);
            Assert(pCtx->es.Attr.n.u1Present);
            Assert(pCtx->es.Attr.n.u4Type > 11 || pCtx->es.Attr.n.u2Dpl >= (pCtx->es.Sel & X86_SEL_RPL));
            Assert(!(pCtx->es.Attr.u & 0xf00));
            Assert(!(pCtx->es.Attr.u & 0xfffe0000));
            Assert(   (pCtx->es.u32Limit & 0xfff) == 0xfff
                   || !(pCtx->es.Attr.n.u1Granularity));
            Assert(   !(pCtx->es.u32Limit & 0xfff00000)
                   || (pCtx->es.Attr.n.u1Granularity));
            Assert(   !(pCtx->es.Attr.n.u4Type & X86_SEL_TYPE_CODE)
                   || (pCtx->es.Attr.n.u4Type & X86_SEL_TYPE_READ));
        }
        if (pCtx->fs.Attr.u && pCtx->fs.Attr.u != VMX_SEL_UNUSABLE)
        {
            Assert(pCtx->fs.Attr.n.u4Type & X86_SEL_TYPE_ACCESSED);
            Assert(pCtx->fs.Attr.n.u1Present);
            Assert(pCtx->fs.Attr.n.u4Type > 11 || pCtx->fs.Attr.n.u2Dpl >= (pCtx->fs.Sel & X86_SEL_RPL));
            Assert(!(pCtx->fs.Attr.u & 0xf00));
            Assert(!(pCtx->fs.Attr.u & 0xfffe0000));
            Assert(   (pCtx->fs.u32Limit & 0xfff) == 0xfff
                   || !(pCtx->fs.Attr.n.u1Granularity));
            Assert(   !(pCtx->fs.u32Limit & 0xfff00000)
                   || (pCtx->fs.Attr.n.u1Granularity));
            Assert(   !(pCtx->fs.Attr.n.u4Type & X86_SEL_TYPE_CODE)
                   || (pCtx->fs.Attr.n.u4Type & X86_SEL_TYPE_READ));
        }
        if (pCtx->gs.Attr.u && pCtx->gs.Attr.u != VMX_SEL_UNUSABLE)
        {
            Assert(pCtx->gs.Attr.n.u4Type & X86_SEL_TYPE_ACCESSED);
            Assert(pCtx->gs.Attr.n.u1Present);
            Assert(pCtx->gs.Attr.n.u4Type > 11 || pCtx->gs.Attr.n.u2Dpl >= (pCtx->gs.Sel & X86_SEL_RPL));
            Assert(!(pCtx->gs.Attr.u & 0xf00));
            Assert(!(pCtx->gs.Attr.u & 0xfffe0000));
            Assert(   (pCtx->gs.u32Limit & 0xfff) == 0xfff
                   || !(pCtx->gs.Attr.n.u1Granularity));
            Assert(   !(pCtx->gs.u32Limit & 0xfff00000)
                   || (pCtx->gs.Attr.n.u1Granularity));
            Assert(   !(pCtx->gs.Attr.n.u4Type & X86_SEL_TYPE_CODE)
                   || (pCtx->gs.Attr.n.u4Type & X86_SEL_TYPE_READ));
        }
        /* 64-bit capable CPUs. */
# if HC_ARCH_BITS == 64 || defined(VBOX_WITH_HYBRID_32BIT_KERNEL)
        Assert(!(pCtx->cs.u64Base >> 32));
        Assert(!pCtx->ss.Attr.u || !(pCtx->ss.u64Base >> 32));
        Assert(!pCtx->ds.Attr.u || !(pCtx->ds.u64Base >> 32));
        Assert(!pCtx->es.Attr.u || !(pCtx->es.u64Base >> 32));
# endif
    }
    else if (   CPUMIsGuestInV86ModeEx(pCtx)
             || (   CPUMIsGuestInRealModeEx(pCtx)
                 && !pVM->hm.s.vmx.fUnrestrictedGuest))
    {
        /* Real and v86 mode checks. */
        /* hmR0VmxWriteSegmentReg() writes the modified in VMCS. We want what we're feeding to VT-x. */
        uint32_t u32CSAttr, u32SSAttr, u32DSAttr, u32ESAttr, u32FSAttr, u32GSAttr;
        if (pVCpu->hm.s.vmx.RealMode.fRealOnV86Active)
        {
            u32CSAttr = 0xf3; u32SSAttr = 0xf3; u32DSAttr = 0xf3; u32ESAttr = 0xf3; u32FSAttr = 0xf3; u32GSAttr = 0xf3;
        }
        else
        {
            u32CSAttr = pCtx->cs.Attr.u; u32SSAttr = pCtx->ss.Attr.u; u32DSAttr = pCtx->ds.Attr.u;
            u32ESAttr = pCtx->es.Attr.u; u32FSAttr = pCtx->fs.Attr.u; u32GSAttr = pCtx->gs.Attr.u;
        }

        /* CS */
        AssertMsg((pCtx->cs.u64Base == (uint64_t)pCtx->cs.Sel << 4), ("CS base %#x %#x\n", pCtx->cs.u64Base, pCtx->cs.Sel));
        Assert(pCtx->cs.u32Limit == 0xffff);
        Assert(u32CSAttr == 0xf3);
        /* SS */
        Assert(pCtx->ss.u64Base == (uint64_t)pCtx->ss.Sel << 4);
        Assert(pCtx->ss.u32Limit == 0xffff);
        Assert(u32SSAttr == 0xf3);
        /* DS */
        Assert(pCtx->ds.u64Base == (uint64_t)pCtx->ds.Sel << 4);
        Assert(pCtx->ds.u32Limit == 0xffff);
        Assert(u32DSAttr == 0xf3);
        /* ES */
        Assert(pCtx->es.u64Base == (uint64_t)pCtx->es.Sel << 4);
        Assert(pCtx->es.u32Limit == 0xffff);
        Assert(u32ESAttr == 0xf3);
        /* FS */
        Assert(pCtx->fs.u64Base == (uint64_t)pCtx->fs.Sel << 4);
        Assert(pCtx->fs.u32Limit == 0xffff);
        Assert(u32FSAttr == 0xf3);
        /* GS */
        Assert(pCtx->gs.u64Base == (uint64_t)pCtx->gs.Sel << 4);
        Assert(pCtx->gs.u32Limit == 0xffff);
        Assert(u32GSAttr == 0xf3);
        /* 64-bit capable CPUs. */
# if HC_ARCH_BITS == 64 || defined(VBOX_WITH_HYBRID_32BIT_KERNEL)
        Assert(!(pCtx->cs.u64Base >> 32));
        Assert(!u32SSAttr || !(pCtx->ss.u64Base >> 32));
        Assert(!u32DSAttr || !(pCtx->ds.u64Base >> 32));
        Assert(!u32ESAttr || !(pCtx->es.u64Base >> 32));
# endif
    }
}
#endif /* DEBUG */


/**
 * Writes a guest segment register into the guest-state area in the VMCS.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   idxSel      Index of the selector in the VMCS.
 * @param   idxLimit    Index of the segment limit in the VMCS.
 * @param   idxBase     Index of the segment base in the VMCS.
 * @param   idxAccess   Index of the access rights of the segment in the VMCS.
 * @param   pSelReg     Pointer to the segment selector.
 * @param   pCtx        Pointer to the guest-CPU context.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxWriteSegmentReg(PVM pVM, PVMCPU pVCpu, uint32_t idxSel, uint32_t idxLimit, uint32_t idxBase,
                                       uint32_t idxAccess, PCPUMSELREG pSelReg, PCPUMCTX pCtx)
{
    int rc;
    rc  = VMXWriteVmcs32(idxSel,    pSelReg->Sel);       /* 16-bit guest selector field. */
    rc |= VMXWriteVmcs32(idxLimit,  pSelReg->u32Limit);  /* 32-bit guest segment limit field. */
    rc |= VMXWriteVmcsGstN(idxBase, pSelReg->u64Base);   /* Natural width guest segment base field.*/
    AssertRCReturn(rc, rc);

    uint32_t u32Access = pSelReg->Attr.u;
    if (pVCpu->hm.s.vmx.RealMode.fRealOnV86Active)
    {
        /* VT-x requires our real-using-v86 mode hack to override the segment access-right bits. */
        u32Access = 0xf3;
        Assert(pVM->hm.s.vmx.pRealModeTSS);
        Assert(PDMVmmDevHeapIsEnabled(pVM));
    }
    else
    {
        /*
         * The way to differentiate between whether this is really a null selector or was just a selector loaded with 0 in
         * real-mode is using the segment attributes. A selector loaded in real-mode with the value 0 is valid and usable in
         * protected-mode and we should -not- mark it as an unusable segment. Both the recompiler & VT-x ensures NULL selectors
         * loaded in protected-mode have their attribute as 0.
         */
        if (!u32Access)
            u32Access = VMX_SEL_UNUSABLE;
    }

    /* Validate segment access rights. Refer to Intel spec. "26.3.1.2 Checks on Guest Segment Registers". */
    AssertMsg((u32Access == VMX_SEL_UNUSABLE) || (u32Access & X86_SEL_TYPE_ACCESSED),
              ("Access bit not set for usable segment. idx=%#x sel=%#x attr %#x\n", idxBase, pSelReg, pSelReg->Attr.u));

    rc = VMXWriteVmcs32(idxAccess, u32Access);           /* 32-bit guest segment access-rights field. */
    AssertRCReturn(rc, rc);
    return rc;
}


/**
 * Loads the guest segment registers, GDTR, IDTR, LDTR, (TR, FS and GS bases)
 * into the guest-state area in the VMCS.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCPU       Pointer to the VMCPU.
 * @param   pCtx        Pointer to the guest-CPU context.
 *
 * @remarks No-long-jump zone!!!
 * @remarks Requires RFLAGS (for debug assertions).
 */
DECLINLINE(int) hmR0VmxLoadGuestSegmentRegs(PVM pVM, PVMCPU pVCpu, PCPUMCTX pCtx)
{
    int rc = VERR_INTERNAL_ERROR_5;

    /*
     * Guest Segment registers: CS, SS, DS, ES, FS, GS.
     */
    if (pVCpu->hm.s.fContextUseFlags & HM_CHANGED_GUEST_SEGMENT_REGS)
    {
        /* Save the segment attributes for real-on-v86 mode hack, so we can restore them on VM-exit. */
        if (pVCpu->hm.s.vmx.RealMode.fRealOnV86Active)
        {
            pVCpu->hm.s.vmx.RealMode.uAttrCS.u = pCtx->cs.Attr.u;
            pVCpu->hm.s.vmx.RealMode.uAttrSS.u = pCtx->ss.Attr.u;
            pVCpu->hm.s.vmx.RealMode.uAttrDS.u = pCtx->ds.Attr.u;
            pVCpu->hm.s.vmx.RealMode.uAttrES.u = pCtx->es.Attr.u;
            pVCpu->hm.s.vmx.RealMode.uAttrFS.u = pCtx->fs.Attr.u;
            pVCpu->hm.s.vmx.RealMode.uAttrGS.u = pCtx->gs.Attr.u;
        }

#ifdef VBOX_WITH_REM
        if (!pVM->hm.s.vmx.fUnrestrictedGuest)
        {
            Assert(pVM->hm.s.vmx.pRealModeTSS);
            PGMMODE enmGuestMode = PGMGetGuestMode(pVCpu);
            if (pVCpu->hm.s.vmx.enmLastSeenGuestMode != enmGuestMode)
            {
                AssertCompile(PGMMODE_REAL < PGMMODE_PROTECTED);
                if (   pVCpu->hm.s.vmx.enmLastSeenGuestMode == PGMMODE_REAL
                    && enmGuestMode >= PGMMODE_PROTECTED)
                {
                    /* Signal that recompiler must flush its code-cache as the guest -may- rewrite code it will later execute
                       in real-mode (e.g. OpenBSD 4.0) */
                    REMFlushTBs(pVM);
                    Log2(("Switch to protected mode detected!\n"));
                }
                pVCpu->hm.s.vmx.enmLastSeenGuestMode = enmGuestMode;
            }
        }
#endif
        rc =  hmR0VmxWriteSegmentReg(pVM, pVCpu, VMX_VMCS16_GUEST_FIELD_CS, VMX_VMCS32_GUEST_CS_LIMIT, VMX_VMCS_GUEST_CS_BASE,
                                     VMX_VMCS32_GUEST_CS_ACCESS_RIGHTS, &pCtx->cs, pCtx);
        rc |= hmR0VmxWriteSegmentReg(pVM, pVCpu, VMX_VMCS16_GUEST_FIELD_SS, VMX_VMCS32_GUEST_SS_LIMIT, VMX_VMCS_GUEST_SS_BASE,
                                      VMX_VMCS32_GUEST_SS_ACCESS_RIGHTS, &pCtx->ss, pCtx);
        rc |= hmR0VmxWriteSegmentReg(pVM, pVCpu, VMX_VMCS16_GUEST_FIELD_DS, VMX_VMCS32_GUEST_DS_LIMIT, VMX_VMCS_GUEST_DS_BASE,
                                      VMX_VMCS32_GUEST_DS_ACCESS_RIGHTS, &pCtx->ds, pCtx);
        rc |= hmR0VmxWriteSegmentReg(pVM, pVCpu, VMX_VMCS16_GUEST_FIELD_ES, VMX_VMCS32_GUEST_ES_LIMIT, VMX_VMCS_GUEST_ES_BASE,
                                      VMX_VMCS32_GUEST_ES_ACCESS_RIGHTS, &pCtx->es, pCtx);
        rc |= hmR0VmxWriteSegmentReg(pVM, pVCpu, VMX_VMCS16_GUEST_FIELD_FS, VMX_VMCS32_GUEST_FS_LIMIT, VMX_VMCS_GUEST_FS_BASE,
                                      VMX_VMCS32_GUEST_FS_ACCESS_RIGHTS, &pCtx->fs, pCtx);
        rc |= hmR0VmxWriteSegmentReg(pVM, pVCpu, VMX_VMCS16_GUEST_FIELD_GS, VMX_VMCS32_GUEST_GS_LIMIT, VMX_VMCS_GUEST_GS_BASE,
                                      VMX_VMCS32_GUEST_GS_ACCESS_RIGHTS, &pCtx->gs, pCtx);
        AssertRCReturn(rc, rc);

#ifdef DEBUG
        hmR0VmxValidateSegmentRegs(pVM, pVCpu, pCtx);
#endif
        pVCpu->hm.s.fContextUseFlags &= ~HM_CHANGED_GUEST_SEGMENT_REGS;
    }

    /*
     * Guest TR.
     */
    if (pVCpu->hm.s.fContextUseFlags & HM_CHANGED_GUEST_TR)
    {
        /*
         * Real-mode emulation using virtual-8086 mode with CR4.VME. Interrupt redirection is achieved
         * using the interrupt redirection bitmap (all bits cleared to let the guest handle INT-n's) in the TSS.
         * See hmR3InitFinalizeR0() to see how pRealModeTSS is setup.
         */
        uint16_t u16Sel          = 0;
        uint32_t u32Limit        = 0;
        uint64_t u64Base         = 0;
        uint32_t u32AccessRights = 0;

        if (!pVCpu->hm.s.vmx.RealMode.fRealOnV86Active)
        {
            u16Sel          = pCtx->tr.Sel;
            u32Limit        = pCtx->tr.u32Limit;
            u64Base         = pCtx->tr.u64Base;
            u32AccessRights = pCtx->tr.Attr.u;
        }
        else
        {
            Assert(pVM->hm.s.vmx.pRealModeTSS);
            Assert(PDMVmmDevHeapIsEnabled(pVM));    /* Guaranteed by HMR3CanExecuteGuest() -XXX- what about inner loop changes? */

            /* We obtain it here every time as PCI regions could be reconfigured in the guest, changing the VMMDev base. */
            RTGCPHYS GCPhys;
            rc = PDMVmmDevHeapR3ToGCPhys(pVM, pVM->hm.s.vmx.pRealModeTSS, &GCPhys);
            AssertRCReturn(rc, rc);

            X86DESCATTR DescAttr;
            DescAttr.u           = 0;
            DescAttr.n.u1Present = 1;
            DescAttr.n.u4Type    = X86_SEL_TYPE_SYS_386_TSS_BUSY;

            u16Sel          = 0;
            u32Limit        = HM_VTX_TSS_SIZE;
            u64Base         = GCPhys;   /* in real-mode phys = virt. */
            u32AccessRights = DescAttr.u;
        }

        /* Validate. */
        Assert(!(u16Sel & RT_BIT(2)));
        AssertMsg(   (u32AccessRights & 0xf) == X86_SEL_TYPE_SYS_386_TSS_BUSY
                  || (u32AccessRights & 0xf) == X86_SEL_TYPE_SYS_286_TSS_BUSY, ("TSS is not busy!? %#x\n", u32AccessRights));
        AssertMsg(!(u32AccessRights & VMX_SEL_UNUSABLE), ("TR unusable bit is not clear!? %#x\n", u32AccessRights));
        Assert(!(u32AccessRights & RT_BIT(4)));           /* System MBZ.*/
        Assert(u32AccessRights & RT_BIT(7));              /* Present MB1.*/
        Assert(!(u32AccessRights & 0xf00));               /* 11:8 MBZ. */
        Assert(!(u32AccessRights & 0xfffe0000));          /* 31:17 MBZ. */
        Assert(   (u32Limit & 0xfff) == 0xfff
               || !(u32AccessRights & RT_BIT(15)));       /* Granularity MBZ. */
        Assert(   !(pCtx->tr.u32Limit & 0xfff00000)
               || (u32AccessRights & RT_BIT(15)));        /* Granularity MB1. */

        rc  = VMXWriteVmcs32(VMX_VMCS16_GUEST_FIELD_TR,         u16Sel);
        rc |= VMXWriteVmcs32(VMX_VMCS32_GUEST_TR_LIMIT,         u32Limit);
        rc |= VMXWriteVmcsGstN(VMX_VMCS_GUEST_TR_BASE,          u64Base);
        rc |= VMXWriteVmcs32(VMX_VMCS32_GUEST_TR_ACCESS_RIGHTS, u32AccessRights);
        AssertRCReturn(rc, rc);

        Log2(("VMX_VMCS_GUEST_TR_BASE=%#RX64\n", u64Base));
        pVCpu->hm.s.fContextUseFlags &= ~HM_CHANGED_GUEST_TR;
    }

    /*
     * Guest GDTR.
     */
    if (pVCpu->hm.s.fContextUseFlags & HM_CHANGED_GUEST_GDTR)
    {
        rc  = VMXWriteVmcs32(VMX_VMCS32_GUEST_GDTR_LIMIT, pCtx->gdtr.cbGdt);
        rc |= VMXWriteVmcsGstN(VMX_VMCS_GUEST_GDTR_BASE,  pCtx->gdtr.pGdt);
        AssertRCReturn(rc, rc);

        Assert(!(pCtx->gdtr.cbGdt & 0xffff0000ULL));      /* Bits 31:16 MBZ. */
        Log2(("VMX_VMCS_GUEST_GDTR_BASE=%#RX64\n", pCtx->gdtr.pGdt));
        pVCpu->hm.s.fContextUseFlags &= ~HM_CHANGED_GUEST_GDTR;
    }

    /*
     * Guest LDTR.
     */
    if (pVCpu->hm.s.fContextUseFlags & HM_CHANGED_GUEST_LDTR)
    {
        /* The unusable bit is specific to VT-x, if it's a null selector mark it as an unusable segment. */
        uint32_t u32Access = 0;
        if (!(pCtx->ldtr.Attr.u & VMX_SEL_UNUSABLE))
            u32Access = VMX_SEL_UNUSABLE;
        else
            u32Access = pCtx->ldtr.Attr.u;

        rc  = VMXWriteVmcs32(VMX_VMCS16_GUEST_FIELD_LDTR,         pCtx->ldtr.Sel);
        rc |= VMXWriteVmcs32(VMX_VMCS32_GUEST_LDTR_LIMIT,         pCtx->ldtr.u32Limit);
        rc |= VMXWriteVmcsGstN(VMX_VMCS_GUEST_LDTR_BASE,          pCtx->ldtr.u64Base);
        rc |= VMXWriteVmcs32(VMX_VMCS32_GUEST_LDTR_ACCESS_RIGHTS, u32Access);
        AssertRCReturn(rc, rc);

        /* Validate. */
        if (!(u32Access & VMX_SEL_UNUSABLE))
        {
            Assert(!(pCtx->ldtr.Sel & RT_BIT(2)));              /* TI MBZ. */
            Assert(pCtx->ldtr.Attr.n.u4Type == 2);              /* Type MB2 (LDT). */
            Assert(!pCtx->ldtr.Attr.n.u1DescType);              /* System MBZ. */
            Assert(pCtx->ldtr.Attr.n.u1Present == 1);           /* Present MB1. */
            Assert(!pCtx->ldtr.Attr.n.u4LimitHigh);             /* 11:8 MBZ. */
            Assert(!(pCtx->ldtr.Attr.u & 0xfffe0000));          /* 31:17 MBZ. */
            Assert(   (pCtx->ldtr.u32Limit & 0xfff) == 0xfff
                   || !pCtx->ldtr.Attr.n.u1Granularity);        /* Granularity MBZ. */
            Assert(   !(pCtx->ldtr.u32Limit & 0xfff00000)
                   || pCtx->ldtr.Attr.n.u1Granularity);         /* Granularity MB1. */
        }

        Log2(("VMX_VMCS_GUEST_LDTR_BASE=%#RX64\n",  pCtx->ldtr.u64Base));
        pVCpu->hm.s.fContextUseFlags &= ~HM_CHANGED_GUEST_LDTR;
    }

    /*
     * Guest IDTR.
     */
    if (pVCpu->hm.s.fContextUseFlags & HM_CHANGED_GUEST_IDTR)
    {
        rc  = VMXWriteVmcs32(VMX_VMCS32_GUEST_IDTR_LIMIT, pCtx->idtr.cbIdt);
        rc |= VMXWriteVmcsGstN(VMX_VMCS_GUEST_IDTR_BASE,  pCtx->idtr.pIdt);
        AssertRCReturn(rc, rc);

        Assert(!(pCtx->idtr.cbIdt & 0xffff0000ULL));      /* Bits 31:16 MBZ. */
        Log2(("VMX_VMCS_GUEST_IDTR_BASE=%#RX64\n", pCtx->idtr.pIdt));
        pVCpu->hm.s.fContextUseFlags &= ~HM_CHANGED_GUEST_IDTR;
    }

    /*
     * Guest FS & GS base MSRs.
     * We already initialized the FS & GS base as part of the guest segment registers, but the guest's FS/GS base
     * MSRs might have changed (e.g. due to WRMSR) and we need to update the bases if that happened. These MSRs
     * are only available in 64-bit mode.
     */
    /** @todo Avoid duplication of this code in assembly (see MYPUSHSEGS) - it
     *        should not be necessary to do it in assembly again. */
    if (CPUMIsGuestInLongModeEx(pCtx))
    {
        if (pVCpu->hm.s.fContextUseFlags & HM_CHANGED_GUEST_FS_BASE_MSR)
        {
            rc = VMXWriteVmcsGstN(VMX_VMCS_GUEST_FS_BASE, pCtx->fs.u64Base);
            AssertRCReturn(rc, rc);
            pVCpu->hm.s.fContextUseFlags &= ~HM_CHANGED_GUEST_FS_BASE_MSR;
        }

        if (pVCpu->hm.s.fContextUseFlags & HM_CHANGED_GUEST_GS_BASE_MSR)
        {
            rc = VMXWriteVmcsGstN(VMX_VMCS_GUEST_GS_BASE, pCtx->gs.u64Base);
            AssertRCReturn(rc, rc);
            pVCpu->hm.s.fContextUseFlags &= ~HM_CHANGED_GUEST_GS_BASE_MSR;
        }
    }
    else
        pVCpu->hm.s.fContextUseFlags &= ~(HM_CHANGED_GUEST_FS_BASE_MSR | HM_CHANGED_GUEST_GS_BASE_MSR);

    return VINF_SUCCESS;
}


/**
 * Loads certain guest MSRs into the VM-entry MSR-load and VM-exit MSR-store
 * areas. These MSRs will automatically be loaded to the host CPU on every
 * successful VM entry and stored from the host CPU on every successful VM exit.
 * Also loads the sysenter MSRs into the guest-state area in the VMCS.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pCtx        Pointer to the guest-CPU context.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxLoadGuestMsrs(PVM pVM, PVMCPU pVCpu, PCPUMCTX pCtx)
{
    AssertPtr(pVCpu);
    AssertPtr(pVCpu->hm.s.vmx.pvGuestMsr);

    /*
     * MSRs covered by Auto-load/store: EFER, LSTAR, STAR, SF_MASK, TSC_AUX (RDTSCP).
     */
    int rc = VINF_SUCCESS;
    if (pVCpu->hm.s.fContextUseFlags & HM_CHANGED_VMX_GUEST_AUTO_MSRS)
    {
        PVMXMSR  pGuestMsr  = (PVMXMSR)pVCpu->hm.s.vmx.pvGuestMsr;
        unsigned cGuestMsrs = 0;

        /* See Intel spec. 4.1.4 "Enumeration of Paging Features by CPUID". */
        const bool fSupportsNX       = CPUMGetGuestCpuIdFeature(pVM, CPUMCPUIDFEATURE_NX);
        const bool fSupportsLongMode = CPUMGetGuestCpuIdFeature(pVM, CPUMCPUIDFEATURE_LONG_MODE);
        if (fSupportsNX || fSupportsLongMode)
        {
            /** @todo support save IA32_EFER, i.e.
             *        VMX_VMCS_CTRL_EXIT_CONTROLS_SAVE_GUEST_EFER_MSR, in which case the
             *        guest EFER need not be part of the VM-entry MSR-load area. */
            pGuestMsr->u32IndexMSR = MSR_K6_EFER;
            pGuestMsr->u32Reserved = 0;
            pGuestMsr->u64Value    = pCtx->msrEFER;
            /* VT-x will complain if only MSR_K6_EFER_LME is set. See Intel spec. 26.4 "Loading MSRs" for details. */
            if (!CPUMIsGuestInLongModeEx(pCtx))
                pGuestMsr->u64Value &= ~(MSR_K6_EFER_LMA | MSR_K6_EFER_LME);
            pGuestMsr++; cGuestMsrs++;
            if (fSupportsLongMode)
            {
                pGuestMsr->u32IndexMSR = MSR_K8_LSTAR;
                pGuestMsr->u32Reserved = 0;
                pGuestMsr->u64Value    = pCtx->msrLSTAR;           /* 64 bits mode syscall rip */
                pGuestMsr++; cGuestMsrs++;
                pGuestMsr->u32IndexMSR = MSR_K6_STAR;
                pGuestMsr->u32Reserved = 0;
                pGuestMsr->u64Value    = pCtx->msrSTAR;            /* legacy syscall eip, cs & ss */
                pGuestMsr++; cGuestMsrs++;
                pGuestMsr->u32IndexMSR = MSR_K8_SF_MASK;
                pGuestMsr->u32Reserved = 0;
                pGuestMsr->u64Value    = pCtx->msrSFMASK;          /* syscall flag mask */
                pGuestMsr++; cGuestMsrs++;
                /* The KERNEL_GS_BASE MSR doesn't work reliably with auto load/store. See @bugref{6208}  */
#if 0
                pGuestMsr->u32IndexMSR = MSR_K8_KERNEL_GS_BASE;
                pGuestMsr->u32Reserved = 0;
                pGuestMsr->u64Value    = pCtx->msrKERNELGSBASE;    /* swapgs exchange value */
                pGuestMsr++; cGuestMsrs++;
#endif
            }
        }

        /*
         * RDTSCP requires the TSC_AUX MSR. Host and guest share the physical MSR. So we have to
         * load the guest's copy if the guest can execute RDTSCP without causing VM-exits.
         */
        if (   CPUMGetGuestCpuIdFeature(pVM, CPUMCPUIDFEATURE_RDTSCP)
            && (pVCpu->hm.s.vmx.u32ProcCtls2 & VMX_VMCS_CTRL_PROC_EXEC2_RDTSCP))
        {
            pGuestMsr->u32IndexMSR = MSR_K8_TSC_AUX;
            pGuestMsr->u32Reserved = 0;
            rc = CPUMQueryGuestMsr(pVCpu, MSR_K8_TSC_AUX, &pGuestMsr->u64Value);
            AssertRCReturn(rc, rc);
            pGuestMsr++; cGuestMsrs++;
        }

        /* Shouldn't ever happen but there -is- a number. We're well within the recommended 512. */
        if (cGuestMsrs > MSR_IA32_VMX_MISC_MAX_MSR(pVM->hm.s.vmx.msr.vmx_misc))
        {
            LogRel(("CPU autoload/store MSR count in VMCS exceeded cGuestMsrs=%u.\n", cGuestMsrs));
            return VERR_HM_UNSUPPORTED_CPU_FEATURE_COMBO;
        }

        /* Update the VCPU's copy of the guest MSR count. */
        pVCpu->hm.s.vmx.cGuestMsrs = cGuestMsrs;
        rc  = VMXWriteVmcs32(VMX_VMCS32_CTRL_ENTRY_MSR_LOAD_COUNT, cGuestMsrs);
        rc |= VMXWriteVmcs32(VMX_VMCS32_CTRL_EXIT_MSR_STORE_COUNT, cGuestMsrs);
        AssertRCReturn(rc, rc);

        pVCpu->hm.s.fContextUseFlags &= ~HM_CHANGED_VMX_GUEST_AUTO_MSRS;
    }

    /*
     * Guest Sysenter MSRs.
     * These flags are only set when MSR-bitmaps are not supported by the CPU and we cause
     * VM exits on WRMSRs for these MSRs.
     */
    if (pVCpu->hm.s.fContextUseFlags & HM_CHANGED_GUEST_SYSENTER_CS_MSR)
    {
        rc = VMXWriteVmcs32(VMX_VMCS32_GUEST_SYSENTER_CS,   pCtx->SysEnter.cs);
        pVCpu->hm.s.fContextUseFlags &= ~HM_CHANGED_GUEST_SYSENTER_CS_MSR;
    }
    if (pVCpu->hm.s.fContextUseFlags & HM_CHANGED_GUEST_SYSENTER_EIP_MSR)
    {
        rc |= VMXWriteVmcsGstN(VMX_VMCS_GUEST_SYSENTER_EIP, pCtx->SysEnter.eip);
        pVCpu->hm.s.fContextUseFlags &= ~HM_CHANGED_GUEST_SYSENTER_EIP_MSR;
    }
    if (pVCpu->hm.s.fContextUseFlags & HM_CHANGED_GUEST_SYSENTER_ESP_MSR)
    {
        rc |= VMXWriteVmcsGstN(VMX_VMCS_GUEST_SYSENTER_ESP, pCtx->SysEnter.esp);
        pVCpu->hm.s.fContextUseFlags &= ~HM_CHANGED_GUEST_SYSENTER_ESP_MSR;
    }
    AssertRCReturn(rc, rc);

    return rc;
}


/**
 * Loads the guest activity state into the guest-state area in the VMCS.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pCtx        Pointer to the guest-CPU context.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxLoadGuestActivityState(PVM pVM, PVMCPU pVCpu, PCPUMCTX pCtx)
{
    /** @todo See if we can make use of other states, e.g.
     *        VMX_VMCS_GUEST_ACTIVITY_SHUTDOWN or HLT.  */
    int rc = VINF_SUCCESS;
    if (pVCpu->hm.s.fContextUseFlags & HM_CHANGED_VMX_GUEST_ACTIVITY_STATE)
    {
        rc = VMXWriteVmcs32(VMX_VMCS32_GUEST_ACTIVITY_STATE, VMX_VMCS_GUEST_ACTIVITY_ACTIVE);
        AssertRCReturn(rc, rc);
        pVCpu->hm.s.fContextUseFlags &= ~HM_CHANGED_VMX_GUEST_ACTIVITY_STATE;
    }
    return rc;
}


/**
 * Sets up the appropriate function to run guest code.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pCtx        Pointer to the guest-CPU context.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxSetupVMRunHandler(PVM pVM, PVMCPU pVCpu, PCPUMCTX pCtx)
{
    if (CPUMIsGuestInLongModeEx(pCtx))
    {
#ifndef VBOX_ENABLE_64_BITS_GUESTS
        return VERR_PGM_UNSUPPORTED_SHADOW_PAGING_MODE;
#endif
        Assert(pVM->hm.s.fAllow64BitGuests);    /* Guaranteed by hmR3InitFinalizeR0(). */
#if HC_ARCH_BITS == 32 && !defined(VBOX_WITH_HYBRID_32BIT_KERNEL)
        /* 32-bit host. We need to switch to 64-bit before running the 64-bit guest. */
        pVCpu->hm.s.vmx.pfnStartVM = VMXR0SwitcherStartVM64;
#else
        /* 64-bit host or hybrid host. */
        pVCpu->hm.s.vmx.pfnStartVM = VMXR0StartVM64;
#endif
    }
    else
    {
        /* Guest is not in long mode, use the 32-bit handler. */
        pVCpu->hm.s.vmx.pfnStartVM = VMXR0StartVM32;
    }
    Assert(pVCpu->hm.s.vmx.pfnStartVM);
    return VINF_SUCCESS;
}


/**
 * Wrapper for running the guest code in VT-x.
 *
 * @returns VBox strict status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pCtx        Pointer to the guest-CPU context.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxRunGuest(PVM pVM, PVMCPU pVCpu, PCPUMCTX pCtx)
{
    /*
     * 64-bit Windows uses XMM registers in the kernel as the Microsoft compiler expresses floating-point operations
     * using SSE instructions. Some XMM registers (XMM6-XMM15) are callee-saved and thus the need for this XMM wrapper.
     * Refer MSDN docs. "Configuring Programs for 64-bit / x64 Software Conventions / Register Usage" for details.
     */
#ifdef VBOX_WITH_KERNEL_USING_XMM
    return hmR0VMXStartVMWrapXMM(pVCpu->hm.s.fResumeVM, pCtx, &pVCpu->hm.s.vmx.VMCSCache, pVM, pVCpu, pVCpu->hm.s.vmx.pfnStartVM);
#else
    return pVCpu->hm.s.vmx.pfnStartVM(pVCpu->hm.s.fResumeVM, pCtx, &pVCpu->hm.s.vmx.VMCSCache, pVM, pVCpu);
#endif
}


/**
 * Report world-switch error and dump some useful debug info.
 *
 * @param   pVM             Pointer to the VM.
 * @param   pVCpu           Pointer to the VMCPU.
 * @param   rcVMRun         The return code from VMLAUNCH/VMRESUME.
 * @param   pCtx            Pointer to the guest-CPU context.
 * @param   pVmxTransient   Pointer to the VMX transient structure (only
 *                          exitReason updated).
 */
static void hmR0VmxReportWorldSwitchError(PVM pVM, PVMCPU pVCpu, int rcVMRun, PCPUMCTX pCtx, PVMXTRANSIENT pVmxTransient)
{
    Assert(pVM);
    Assert(pVCpu);
    Assert(pCtx);
    Assert(pVmxTransient);
    Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));

    Log(("VM-entry failure: %Rrc\n", rcVMRun));
    switch (rcVMRun)
    {
        case VERR_VMX_INVALID_VMXON_PTR:
            AssertFailed();
            break;
        case VINF_SUCCESS:      /* VMLAUNCH/VMRESUME succeeded but VM-entry failed... yeah, true story. */
        case VERR_VMX_UNABLE_TO_START_VM:
        case VERR_VMX_UNABLE_TO_RESUME_VM:
        {
            int rc = VMXReadVmcs32(VMX_VMCS32_RO_EXIT_REASON, &pVCpu->hm.s.vmx.lasterror.u32ExitReason);
            rc    |= VMXReadVmcs32(VMX_VMCS32_RO_VM_INSTR_ERROR, &pVCpu->hm.s.vmx.lasterror.u32InstrError);
            rc    |= hmR0VmxReadExitQualificationVmcs(pVmxTransient);
            AssertRC(rc);

#ifdef VBOX_STRICT
                Log(("uExitReason        %#x (VmxTransient %#x)\n", pVCpu->hm.s.vmx.lasterror.u32ExitReason,
                                                                    pVmxTransient->uExitReason));
                Log(("Exit Qualification %#x\n", pVmxTransient->uExitQualification));
                Log(("InstrError         %#x\n", pVCpu->hm.s.vmx.lasterror.u32InstrError));
                if (pVCpu->hm.s.vmx.lasterror.u32InstrError <= VMX_INSTR_ERROR_MAX)
                    Log(("InstrError Desc.  \"%s\"\n", s_apszVmxInstrErrors[pVCpu->hm.s.vmx.lasterror.u32InstrError]));
                else
                    Log(("InstrError Desc.    Range exceeded %u\n", VMX_INSTR_ERROR_MAX));

                /* VMX control bits. */
                uint32_t    u32Val;
                uint64_t    u64Val;
                RTHCUINTREG uHCReg;
                rc = VMXReadVmcs32(VMX_VMCS32_CTRL_PIN_EXEC_CONTROLS, &u32Val);         AssertRC(rc);
                Log(("VMX_VMCS32_CTRL_PIN_EXEC_CONTROLS       %#RX32\n", u32Val));
                rc = VMXReadVmcs32(VMX_VMCS32_CTRL_PROC_EXEC_CONTROLS, &u32Val);        AssertRC(rc);
                Log(("VMX_VMCS32_CTRL_PROC_EXEC_CONTROLS      %#RX32\n", u32Val));
                rc = VMXReadVmcs32(VMX_VMCS32_CTRL_PROC_EXEC_CONTROLS2, &u32Val);       AssertRC(rc);
                Log(("VMX_VMCS32_CTRL_PROC_EXEC_CONTROLS2     %#RX32\n", u32Val));
                rc = VMXReadVmcs32(VMX_VMCS32_CTRL_ENTRY_CONTROLS, &u32Val);            AssertRC(rc);
                Log(("VMX_VMCS32_CTRL_ENTRY_CONTROLS          %#RX32\n", u32Val));
                rc = VMXReadVmcs32(VMX_VMCS32_CTRL_EXIT_CONTROLS, &u32Val);             AssertRC(rc);
                Log(("VMX_VMCS32_CTRL_EXIT_CONTROLS           %#RX32\n", u32Val));
                rc = VMXReadVmcs32(VMX_VMCS32_CTRL_CR3_TARGET_COUNT, &u32Val);          AssertRC(rc);
                Log(("VMX_VMCS32_CTRL_CR3_TARGET_COUNT        %#RX32\n", u32Val));
                rc = VMXReadVmcs32(VMX_VMCS32_CTRL_ENTRY_INTERRUPTION_INFO, &u32Val);   AssertRC(rc);
                Log(("VMX_VMCS32_CTRL_ENTRY_INTERRUPTION_INFO %#RX32\n", u32Val));
                rc = VMXReadVmcs32(VMX_VMCS32_CTRL_ENTRY_EXCEPTION_ERRCODE, &u32Val);   AssertRC(rc);
                Log(("VMX_VMCS32_CTRL_ENTRY_EXCEPTION_ERRCODE %#RX32\n", u32Val));
                rc = VMXReadVmcs32(VMX_VMCS32_CTRL_ENTRY_INSTR_LENGTH, &u32Val);        AssertRC(rc);
                Log(("VMX_VMCS32_CTRL_ENTRY_INSTR_LENGTH      %u\n", u32Val));
                rc = VMXReadVmcs32(VMX_VMCS32_CTRL_TPR_THRESHOLD, &u32Val);             AssertRC(rc);
                Log(("VMX_VMCS32_CTRL_TPR_THRESHOLD           %u\n", u32Val));
                rc = VMXReadVmcs32(VMX_VMCS32_CTRL_EXIT_MSR_STORE_COUNT, &u32Val);      AssertRC(rc);
                Log(("VMX_VMCS32_CTRL_EXIT_MSR_STORE_COUNT    %u (guest MSRs)\n", u32Val));
                rc = VMXReadVmcs32(VMX_VMCS32_CTRL_EXIT_MSR_LOAD_COUNT, &u32Val);       AssertRC(rc);
                Log(("VMX_VMCS32_CTRL_EXIT_MSR_LOAD_COUNT     %u (host MSRs)\n", u32Val));
                rc = VMXReadVmcs32(VMX_VMCS32_CTRL_ENTRY_MSR_LOAD_COUNT, &u32Val);      AssertRC(rc);
                Log(("VMX_VMCS32_CTRL_ENTRY_MSR_LOAD_COUNT    %u (guest MSRs)\n", u32Val));
                rc = VMXReadVmcs32(VMX_VMCS32_CTRL_EXCEPTION_BITMAP, &u32Val);          AssertRC(rc);
                Log(("VMX_VMCS32_CTRL_EXCEPTION_BITMAP        %#RX32\n", u32Val));
                rc = VMXReadVmcs32(VMX_VMCS32_CTRL_PAGEFAULT_ERROR_MASK, &u32Val);      AssertRC(rc);
                Log(("VMX_VMCS32_CTRL_PAGEFAULT_ERROR_MASK    %#RX32\n", u32Val));
                rc = VMXReadVmcs32(VMX_VMCS32_CTRL_PAGEFAULT_ERROR_MATCH, &u32Val);     AssertRC(rc);
                Log(("VMX_VMCS32_CTRL_PAGEFAULT_ERROR_MATCH   %#RX32\n", u32Val));
                rc = VMXReadVmcsHstN(VMX_VMCS_CTRL_CR0_MASK, &uHCReg);                  AssertRC(rc);
                Log(("VMX_VMCS_CTRL_CR0_MASK                  %#RHr\n", uHCReg));
                rc = VMXReadVmcsHstN(VMX_VMCS_CTRL_CR0_READ_SHADOW, &uHCReg);           AssertRC(rc);
                Log(("VMX_VMCS_CTRL_CR4_READ_SHADOW           %#RHr\n", uHCReg));
                rc = VMXReadVmcsHstN(VMX_VMCS_CTRL_CR4_MASK, &uHCReg);                  AssertRC(rc);
                Log(("VMX_VMCS_CTRL_CR4_MASK                  %#RHr\n", uHCReg));
                rc = VMXReadVmcsHstN(VMX_VMCS_CTRL_CR4_READ_SHADOW, &uHCReg);           AssertRC(rc);
                Log(("VMX_VMCS_CTRL_CR4_READ_SHADOW           %#RHr\n", uHCReg));
                rc = VMXReadVmcs64(VMX_VMCS64_CTRL_EPTP_FULL, &u64Val);                 AssertRC(rc);
                Log(("VMX_VMCS64_CTRL_EPTP_FULL               %#RX64\n", u64Val));

                /* Guest bits. */
                RTGCUINTREG uGCReg;
                rc = VMXReadVmcsGstN(VMX_VMCS_GUEST_RIP, &uGCReg);      AssertRC(rc);
                Log(("Old Guest Rip %#RGv New %#RGv\n", (RTGCPTR)pCtx->rip, (RTGCPTR)uGCReg));
                rc = VMXReadVmcsGstN(VMX_VMCS_GUEST_RSP, &uGCReg);      AssertRC(rc);
                Log(("Old Guest Rsp %#RGv New %#RGv\n", (RTGCPTR)pCtx->rsp, (RTGCPTR)uGCReg));
                rc = VMXReadVmcsGstN(VMX_VMCS_GUEST_RFLAGS, &uGCReg);   AssertRC(rc);
                Log(("Old Guest Rflags %#RGr New %#RGr\n", (RTGCPTR)pCtx->rflags.u64, (RTGCPTR)uGCReg));
                rc = VMXReadVmcs32(VMX_VMCS16_GUEST_FIELD_VPID, &u32Val); AssertRC(rc);
                Log(("VMX_VMCS16_GUEST_FIELD_VPID %u\n", u32Val));

                /* Host bits. */
                rc = VMXReadVmcsHstN(VMX_VMCS_HOST_CR0, &uHCReg); AssertRC(rc);
                Log(("Host CR0 %#RHr\n", uHCReg));
                rc = VMXReadVmcsHstN(VMX_VMCS_HOST_CR3, &uHCReg); AssertRC(rc);
                Log(("Host CR3 %#RHr\n", uHCReg));
                rc = VMXReadVmcsHstN(VMX_VMCS_HOST_CR4, &uHCReg); AssertRC(rc);
                Log(("Host CR4 %#RHr\n", uHCReg));

                RTGDTR      HostGdtr;
                PCX86DESCHC pDesc;
                ASMGetGDTR(&HostGdtr);
                rc = VMXReadVmcs32(VMX_VMCS16_HOST_FIELD_CS, &u32Val);
                Log(("Host CS %#08x\n", u32Val));
                if (u32Val < HostGdtr.cbGdt)
                {
                    pDesc  = (PCX86DESCHC)(HostGdtr.pGdt + (u32Val & X86_SEL_MASK));
                    HMR0DumpDescriptor(pDesc, u32Val, "CS: ");
                }

                rc = VMXReadVmcs32(VMX_VMCS16_HOST_FIELD_DS, &u32Val); AssertRC(rc);
                Log(("Host DS %#08x\n", u32Val));
                if (u32Val < HostGdtr.cbGdt)
                {
                    pDesc  = (PCX86DESCHC)(HostGdtr.pGdt + (u32Val & X86_SEL_MASK));
                    HMR0DumpDescriptor(pDesc, u32Val, "DS: ");
                }

                rc = VMXReadVmcs32(VMX_VMCS16_HOST_FIELD_ES, &u32Val); AssertRC(rc);
                Log(("Host ES %#08x\n", u32Val));
                if (u32Val < HostGdtr.cbGdt)
                {
                    pDesc  = (PCX86DESCHC)(HostGdtr.pGdt + (u32Val & X86_SEL_MASK));
                    HMR0DumpDescriptor(pDesc, u32Val, "ES: ");
                }

                rc = VMXReadVmcs32(VMX_VMCS16_HOST_FIELD_FS, &u32Val); AssertRC(rc);
                Log(("Host FS %#08x\n", u32Val));
                if (u32Val < HostGdtr.cbGdt)
                {
                    pDesc  = (PCX86DESCHC)(HostGdtr.pGdt + (u32Val & X86_SEL_MASK));
                    HMR0DumpDescriptor(pDesc, u32Val, "FS: ");
                }

                rc = VMXReadVmcs32(VMX_VMCS16_HOST_FIELD_GS, &u32Val); AssertRC(rc);
                Log(("Host GS %#08x\n", u32Val));
                if (u32Val < HostGdtr.cbGdt)
                {
                    pDesc  = (PCX86DESCHC)(HostGdtr.pGdt + (u32Val & X86_SEL_MASK));
                    HMR0DumpDescriptor(pDesc, u32Val, "GS: ");
                }

                rc = VMXReadVmcs32(VMX_VMCS16_HOST_FIELD_SS, &u32Val); AssertRC(rc);
                Log(("Host SS %#08x\n", u32Val));
                if (u32Val < HostGdtr.cbGdt)
                {
                    pDesc  = (PCX86DESCHC)(HostGdtr.pGdt + (u32Val & X86_SEL_MASK));
                    HMR0DumpDescriptor(pDesc, u32Val, "SS: ");
                }

                rc = VMXReadVmcs32(VMX_VMCS16_HOST_FIELD_TR,  &u32Val); AssertRC(rc);
                Log(("Host TR %#08x\n", u32Val));
                if (u32Val < HostGdtr.cbGdt)
                {
                    pDesc  = (PCX86DESCHC)(HostGdtr.pGdt + (u32Val & X86_SEL_MASK));
                    HMR0DumpDescriptor(pDesc, u32Val, "TR: ");
                }

                rc = VMXReadVmcsHstN(VMX_VMCS_HOST_TR_BASE, &uHCReg); AssertRC(rc);
                Log(("Host TR Base %#RHv\n", uHCReg));
                rc = VMXReadVmcsHstN(VMX_VMCS_HOST_GDTR_BASE, &uHCReg); AssertRC(rc);
                Log(("Host GDTR Base %#RHv\n", uHCReg));
                rc = VMXReadVmcsHstN(VMX_VMCS_HOST_IDTR_BASE, &uHCReg); AssertRC(rc);
                Log(("Host IDTR Base %#RHv\n", uHCReg));
                rc = VMXReadVmcs32(VMX_VMCS32_HOST_SYSENTER_CS, &u32Val); AssertRC(rc);
                Log(("Host SYSENTER CS  %#08x\n", u32Val));
                rc = VMXReadVmcsHstN(VMX_VMCS_HOST_SYSENTER_EIP, &uHCReg); AssertRC(rc);
                Log(("Host SYSENTER EIP %#RHv\n", uHCReg));
                rc = VMXReadVmcsHstN(VMX_VMCS_HOST_SYSENTER_ESP, &uHCReg); AssertRC(rc);
                Log(("Host SYSENTER ESP %#RHv\n", uHCReg));
                rc = VMXReadVmcsHstN(VMX_VMCS_HOST_RSP, &uHCReg); AssertRC(rc);
                Log(("Host RSP %#RHv\n", uHCReg));
                rc = VMXReadVmcsHstN(VMX_VMCS_HOST_RIP, &uHCReg); AssertRC(rc);
                Log(("Host RIP %#RHv\n", uHCReg));
# if HC_ARCH_BITS == 64 || defined(VBOX_WITH_HYBRID_32BIT_KERNEL)
                if (VMX_IS_64BIT_HOST_MODE())
                {
                    Log(("MSR_K6_EFER            = %#RX64\n", ASMRdMsr(MSR_K6_EFER)));
                    Log(("MSR_K6_STAR            = %#RX64\n", ASMRdMsr(MSR_K6_STAR)));
                    Log(("MSR_K8_LSTAR           = %#RX64\n", ASMRdMsr(MSR_K8_LSTAR)));
                    Log(("MSR_K8_CSTAR           = %#RX64\n", ASMRdMsr(MSR_K8_CSTAR)));
                    Log(("MSR_K8_SF_MASK         = %#RX64\n", ASMRdMsr(MSR_K8_SF_MASK)));
                    Log(("MSR_K8_KERNEL_GS_BASE  = %#RX64\n", ASMRdMsr(MSR_K8_KERNEL_GS_BASE)));
                }
# endif
#endif /* VBOX_STRICT */
            break;
        }

        default:
            /* Impossible */
            AssertMsgFailed(("hmR0VmxReportWorldSwitchError %Rrc (%#x)\n", rcVMRun, rcVMRun));
            break;
    }
    NOREF(pVM);
}


#if HC_ARCH_BITS == 32 && defined(VBOX_ENABLE_64_BITS_GUESTS) && !defined(VBOX_WITH_HYBRID_32BIT_KERNEL)
#ifndef VMX_USE_CACHED_VMCS_ACCESSES
# error "VMX_USE_CACHED_VMCS_ACCESSES not defined when it should be!"
#endif

/**
 * Executes the specified handler in 64-bit mode.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pCtx        Pointer to the guest CPU context.
 * @param   pfnHandler  Pointer to the RC handler function.
 * @param   cbParam     Number of parameters.
 * @param   paParam     Array of 32-bit parameters.
 */
VMMR0DECL(int) VMXR0Execute64BitsHandler(PVM pVM, PVMCPU pVCpu, PCPUMCTX pCtx, RTRCPTR pfnHandler, uint32_t cbParam,
                                         uint32_t *paParam)
{
    int             rc, rc2;
    PHMGLOBLCPUINFO pCpu;
    RTHCPHYS        HCPhysCpuPage;
    RTHCUINTREG     uOldEFlags;

    AssertReturn(pVM->hm.s.pfnHost32ToGuest64R0, VERR_HM_NO_32_TO_64_SWITCHER);
    Assert(pfnHandler);
    Assert(pVCpu->hm.s.vmx.VMCSCache.Write.cValidEntries <= RT_ELEMENTS(pVCpu->hm.s.vmx.VMCSCache.Write.aField));
    Assert(pVCpu->hm.s.vmx.VMCSCache.Read.cValidEntries <= RT_ELEMENTS(pVCpu->hm.s.vmx.VMCSCache.Read.aField));

#ifdef VBOX_STRICT
    for (uint32_t i = 0; i < pVCpu->hm.s.vmx.VMCSCache.Write.cValidEntries; i++)
        Assert(hmR0VmxIsValidWriteField(pVCpu->hm.s.vmx.VMCSCache.Write.aField[i]));

    for (uint32_t i = 0; i <pVCpu->hm.s.vmx.VMCSCache.Read.cValidEntries; i++)
        Assert(hmR0VmxIsValidReadField(pVCpu->hm.s.vmx.VMCSCache.Read.aField[i]));
#endif

    /* Disable interrupts. */
    uOldEFlags = ASMIntDisableFlags();

#ifdef VBOX_WITH_VMMR0_DISABLE_LAPIC_NMI
    RTCPUID idHostCpu = RTMpCpuId();
    CPUMR0SetLApic(pVM, idHostCpu);
#endif

    pCpu = HMR0GetCurrentCpu();
    HCPhysCpuPage = RTR0MemObjGetPagePhysAddr(pCpu->hMemObj, 0);

    /* Clear VMCS. Marking it inactive, clearing implementation-specific data and writing VMCS data back to memory. */
    VMXClearVMCS(pVCpu->hm.s.vmx.HCPhysVmcs);

    /* Leave VMX Root Mode. */
    VMXDisable();

    ASMSetCR4(ASMGetCR4() & ~X86_CR4_VMXE);

    CPUMSetHyperESP(pVCpu, VMMGetStackRC(pVCpu));
    CPUMSetHyperEIP(pVCpu, pfnHandler);
    for (int i = (int)cbParam - 1; i >= 0; i--)
        CPUMPushHyper(pVCpu, paParam[i]);

    STAM_PROFILE_ADV_START(&pVCpu->hm.s.StatWorldSwitch3264, z);

    /* Call the switcher. */
    rc = pVM->hm.s.pfnHost32ToGuest64R0(pVM, RT_OFFSETOF(VM, aCpus[pVCpu->idCpu].cpum) - RT_OFFSETOF(VM, cpum));
    STAM_PROFILE_ADV_STOP(&pVCpu->hm.s.StatWorldSwitch3264, z);

    /** @todo replace with hmR0VmxEnterRootMode() and LeaveRootMode(). */
    /* Make sure the VMX instructions don't cause #UD faults. */
    ASMSetCR4(ASMGetCR4() | X86_CR4_VMXE);

    /* Re-enter VMX Root Mode */
    rc2 = VMXEnable(HCPhysCpuPage);
    if (RT_FAILURE(rc2))
    {
        ASMSetCR4(ASMGetCR4() & ~X86_CR4_VMXE);
        ASMSetFlags(uOldEFlags);
        return VERR_VMX_VMXON_FAILED;
    }

    rc2 = VMXActivateVMCS(pVCpu->hm.s.vmx.HCPhysVmcs);
    AssertRC(rc2);
    Assert(!(ASMGetFlags() & X86_EFL_IF));
    ASMSetFlags(uOldEFlags);
    return rc;
}


/**
 * Prepares for and executes VMLAUNCH (64 bits guests) for 32-bit hosts
 * supporting 64-bit guests.
 *
 * @returns VBox status code.
 * @param   fResume     Whether to VMLAUNCH or VMRESUME.
 * @param   pCtx        Pointer to the guest-CPU context.
 * @param   pCache      Pointer to the VMCS cache.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 */
DECLASM(int) VMXR0SwitcherStartVM64(RTHCUINT fResume, PCPUMCTX pCtx, PVMCSCACHE pCache, PVM pVM, PVMCPU pVCpu)
{
    uint32_t        aParam[6];
    PHMGLOBLCPUINFO pCpu          = NULL;
    RTHCPHYS        HCPhysCpuPage = 0;
    int             rc            = VERR_INTERNAL_ERROR_5;

    pCpu = HMR0GetCurrentCpu();
    HCPhysCpuPage = RTR0MemObjGetPagePhysAddr(pCpu->hMemObj, 0);

#ifdef VBOX_WITH_CRASHDUMP_MAGIC
    pCache->uPos = 1;
    pCache->interPD = PGMGetInterPaeCR3(pVM);
    pCache->pSwitcher = (uint64_t)pVM->hm.s.pfnHost32ToGuest64R0;
#endif

#ifdef DEBUG
    pCache->TestIn.HCPhysCpuPage= 0;
    pCache->TestIn.HCPhysVmcs   = 0;
    pCache->TestIn.pCache       = 0;
    pCache->TestOut.HCPhysVmcs  = 0;
    pCache->TestOut.pCache      = 0;
    pCache->TestOut.pCtx        = 0;
    pCache->TestOut.eflags      = 0;
#endif

    aParam[0] = (uint32_t)(HCPhysCpuPage);                              /* Param 1: VMXON physical address - Lo. */
    aParam[1] = (uint32_t)(HCPhysCpuPage >> 32);                        /* Param 1: VMXON physical address - Hi. */
    aParam[2] = (uint32_t)(pVCpu->hm.s.vmx.HCPhysVmcs);                 /* Param 2: VMCS physical address - Lo. */
    aParam[3] = (uint32_t)(pVCpu->hm.s.vmx.HCPhysVmcs >> 32);           /* Param 2: VMCS physical address - Hi. */
    aParam[4] = VM_RC_ADDR(pVM, &pVM->aCpus[pVCpu->idCpu].hm.s.vmx.VMCSCache);
    aParam[5] = 0;

#ifdef VBOX_WITH_CRASHDUMP_MAGIC
    pCtx->dr[4] = pVM->hm.s.vmx.pScratchPhys + 16 + 8;
    *(uint32_t *)(pVM->hm.s.vmx.pScratch + 16 + 8) = 1;
#endif
    rc = VMXR0Execute64BitsHandler(pVM, pVCpu, pCtx, pVM->hm.s.pfnVMXGCStartVM64, 6, &aParam[0]);

#ifdef VBOX_WITH_CRASHDUMP_MAGIC
    Assert(*(uint32_t *)(pVM->hm.s.vmx.pScratch + 16 + 8) == 5);
    Assert(pCtx->dr[4] == 10);
    *(uint32_t *)(pVM->hm.s.vmx.pScratch + 16 + 8) = 0xff;
#endif

#ifdef DEBUG
    AssertMsg(pCache->TestIn.HCPhysCpuPage== HCPhysCpuPage, ("%RHp vs %RHp\n", pCache->TestIn.HCPhysCpuPage, HCPhysCpuPage));
    AssertMsg(pCache->TestIn.HCPhysVmcs   == pVCpu->hm.s.vmx.HCPhysVmcs, ("%RHp vs %RHp\n", pCache->TestIn.HCPhysVmcs,
                                                                              pVCpu->hm.s.vmx.HCPhysVmcs));
    AssertMsg(pCache->TestIn.HCPhysVmcs   == pCache->TestOut.HCPhysVmcs, ("%RHp vs %RHp\n", pCache->TestIn.HCPhysVmcs,
                                                                          pCache->TestOut.HCPhysVmcs));
    AssertMsg(pCache->TestIn.pCache       == pCache->TestOut.pCache, ("%RGv vs %RGv\n", pCache->TestIn.pCache,
                                                                      pCache->TestOut.pCache));
    AssertMsg(pCache->TestIn.pCache       == VM_RC_ADDR(pVM, &pVM->aCpus[pVCpu->idCpu].hm.s.vmx.VMCSCache),
              ("%RGv vs %RGv\n", pCache->TestIn.pCache, VM_RC_ADDR(pVM, &pVM->aCpus[pVCpu->idCpu].hm.s.vmx.VMCSCache)));
    AssertMsg(pCache->TestIn.pCtx         == pCache->TestOut.pCtx, ("%RGv vs %RGv\n", pCache->TestIn.pCtx,
                                                                    pCache->TestOut.pCtx));
    Assert(!(pCache->TestOut.eflags & X86_EFL_IF));
#endif
    return rc;
}


/**
 * Initialize the VMCS-Read cache. The VMCS cache is used for 32-bit hosts
 * running 64-bit guests (except 32-bit Darwin which runs with 64-bit paging in
 * 32-bit mode) for 64-bit fields that cannot be accessed in 32-bit mode. Some
 * 64-bit fields -can- be accessed (those that have a 32-bit FULL & HIGH part).
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 */
static int hmR0VmxInitVmcsReadCache(PVM pVM, PVMCPU pVCpu)
{
#define VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, idxField)                            \
{                                                                                   \
    Assert(pCache->Read.aField[idxField##_CACHE_IDX] == 0);                         \
    pCache->Read.aField[idxField##_CACHE_IDX] = idxField;                           \
    pCache->Read.aFieldVal[idxField##_CACHE_IDX] = 0;                               \
}

#define VMXLOCAL_INIT_VMCS_SELREG(REG, pCache)                                      \
{                                                                                   \
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS16_GUEST_FIELD_##REG);           \
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS32_GUEST_##REG##_LIMIT);         \
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_##REG##_BASE);            \
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS32_GUEST_##REG##_ACCESS_RIGHTS); \
}

    AssertPtr(pVM);
    AssertPtr(pVCpu);
    PVMCSCACHE pCache = &pVCpu->hm.s.vmx.VMCSCache;

    /* 16-bit guest-state fields (16-bit selectors and their corresponding 32-bit limit & 32-bit access-rights fields). */
    VMXLOCAL_INIT_VMCS_SELREG(ES,   pCache);
    VMXLOCAL_INIT_VMCS_SELREG(CS,   pCache);
    VMXLOCAL_INIT_VMCS_SELREG(SS,   pCache);
    VMXLOCAL_INIT_VMCS_SELREG(DS,   pCache);
    VMXLOCAL_INIT_VMCS_SELREG(FS,   pCache);
    VMXLOCAL_INIT_VMCS_SELREG(GS,   pCache);
    VMXLOCAL_INIT_VMCS_SELREG(LDTR, pCache);
    VMXLOCAL_INIT_VMCS_SELREG(TR,   pCache);

    /* 64-bit guest-state fields; unused as we use two 32-bit VMREADs for these 64-bit fields (using "FULL" and "HIGH" fields). */
#if 0
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS64_GUEST_VMCS_LINK_PTR_FULL);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS64_GUEST_DEBUGCTL_FULL);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS64_GUEST_PAT_FULL);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS64_GUEST_EFER_FULL);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS64_GUEST_PERF_GLOBAL_CTRL_FULL);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS64_GUEST_PDPTE0_FULL);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS64_GUEST_PDPTE1_FULL);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS64_GUEST_PDPTE2_FULL);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS64_GUEST_PDPTE3_FULL);
#endif

    /* 32-bit guest-state fields. */
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS32_GUEST_GDTR_LIMIT);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS32_GUEST_IDTR_LIMIT);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS32_GUEST_INTERRUPTIBILITY_STATE);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS32_GUEST_SYSENTER_CS);
    /* Unused 32-bit guest-state fields. */
#if 0
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS32_GUEST_ACTIVITY_STATE);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS32_GUEST_SMBASE);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS32_GUEST_PREEMPTION_TIMER_VALUE);
#endif

    /* Natural width guest-state fields. */
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_CR0);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_CR4);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_ES_BASE);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_CS_BASE);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_SS_BASE);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_DS_BASE);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_FS_BASE);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_GS_BASE);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_LDTR_BASE);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_TR_BASE);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_GDTR_BASE);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_IDTR_BASE);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_DR7);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_RSP);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_RIP);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_RFLAGS);
    /* Unused natural width guest-state fields. */
#if 0
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_DEBUG_EXCEPTIONS);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_CR3); /* Handled in Nested Paging case */
#endif
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_SYSENTER_ESP);
    VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_SYSENTER_EIP);

    if (pVM->hm.s.fNestedPaging)
    {
        VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS_GUEST_CR3);
        VMXLOCAL_INIT_READ_CACHE_FIELD(pCache, VMX_VMCS64_EXIT_GUEST_PHYS_ADDR_FULL);
        pCache->Read.cValidEntries = VMX_VMCS_MAX_NESTED_PAGING_CACHE_IDX;
    }
    else
        pCache->Read.cValidEntries = VMX_VMCS_MAX_CACHE_IDX;

#undef VMXLOCAL_INIT_VMCS_SELREG
#undef VMXLOCAL_INIT_READ_CACHE_FIELD
    return VINF_SUCCESS;
}


/**
 * Writes a field into the VMCS. This can either directly invoke a VMWRITE or
 * queue up the VMWRITE by using the VMCS write cache (on 32-bit hosts, except
 * darwin, running 64-bit guests).
 *
 * @returns VBox status code.
 * @param   pVCpu           Pointer to the VMCPU.
 * @param   idxField        The VMCS field encoding.
 * @param   u64Val          16, 32 or 64 bits value.
 */
VMMR0DECL(int) VMXWriteVmcs64Ex(PVMCPU pVCpu, uint32_t idxField, uint64_t u64Val)
{
    int rc;
    switch (idxField)
    {
        /*
         * These fields consists of a "FULL" and a "HIGH" part which can be written to individually.
         */
        /* 64-bit Control fields. */
        case VMX_VMCS64_CTRL_IO_BITMAP_A_FULL:
        case VMX_VMCS64_CTRL_IO_BITMAP_B_FULL:
        case VMX_VMCS64_CTRL_MSR_BITMAP_FULL:
        case VMX_VMCS64_CTRL_VMEXIT_MSR_STORE_FULL:
        case VMX_VMCS64_CTRL_VMEXIT_MSR_LOAD_FULL:
        case VMX_VMCS64_CTRL_VMENTRY_MSR_LOAD_FULL:
        case VMX_VMCS64_CTRL_EXEC_VMCS_PTR_FULL:
        case VMX_VMCS64_CTRL_TSC_OFFSET_FULL:
        case VMX_VMCS64_CTRL_VAPIC_PAGEADDR_FULL:
        case VMX_VMCS64_CTRL_APIC_ACCESSADDR_FULL:
        case VMX_VMCS64_CTRL_VMFUNC_CTRLS_FULL:
        case VMX_VMCS64_CTRL_EPTP_FULL:
        case VMX_VMCS64_CTRL_EPTP_LIST_FULL:
        /* 64-bit Read-only data fields. */
        case VMX_VMCS64_EXIT_GUEST_PHYS_ADDR_FULL:
        /* 64-bit Guest-state fields. */
        case VMX_VMCS64_GUEST_VMCS_LINK_PTR_FULL:
        case VMX_VMCS64_GUEST_DEBUGCTL_FULL:
        case VMX_VMCS64_GUEST_PAT_FULL:
        case VMX_VMCS64_GUEST_EFER_FULL:
        case VMX_VMCS64_GUEST_PERF_GLOBAL_CTRL_FULL:
        case VMX_VMCS64_GUEST_PDPTE0_FULL:
        case VMX_VMCS64_GUEST_PDPTE1_FULL:
        case VMX_VMCS64_GUEST_PDPTE2_FULL:
        case VMX_VMCS64_GUEST_PDPTE3_FULL:
        /* 64-bit Host-state fields. */
        case VMX_VMCS64_HOST_FIELD_PAT_FULL:
        case VMX_VMCS64_HOST_FIELD_EFER_FULL:
        case VMX_VMCS64_HOST_PERF_GLOBAL_CTRL_FULL:
        {
            rc  = VMXWriteVmcs32(idxField, u64Val);
            rc |= VMXWriteVmcs32(idxField + 1, (uint32_t)(u64Val >> 32ULL));    /* hmpf, do we really need the "ULL" suffix? */
            break;
        }

        /*
         * These fields do not have high and low parts. Queue up the VMWRITE by using the VMCS write-cache (for 64-bit
         * values). When we switch the host to 64-bit mode for running 64-bit guests, these VMWRITEs get executed then.
         */
        /* Natural-width Guest-state fields.  */
        case VMX_VMCS_GUEST_CR0:
        case VMX_VMCS_GUEST_CR3:
        case VMX_VMCS_GUEST_CR4:
        case VMX_VMCS_GUEST_ES_BASE:
        case VMX_VMCS_GUEST_CS_BASE:
        case VMX_VMCS_GUEST_SS_BASE:
        case VMX_VMCS_GUEST_DS_BASE:
        case VMX_VMCS_GUEST_FS_BASE:
        case VMX_VMCS_GUEST_GS_BASE:
        case VMX_VMCS_GUEST_LDTR_BASE:
        case VMX_VMCS_GUEST_TR_BASE:
        case VMX_VMCS_GUEST_GDTR_BASE:
        case VMX_VMCS_GUEST_IDTR_BASE:
        case VMX_VMCS_GUEST_DR7:
        case VMX_VMCS_GUEST_RSP:
        case VMX_VMCS_GUEST_RIP:
        case VMX_VMCS_GUEST_RFLAGS:
        case VMX_VMCS_GUEST_DEBUG_EXCEPTIONS:
        case VMX_VMCS_GUEST_SYSENTER_ESP:
        case VMX_VMCS_GUEST_SYSENTER_EIP:
        {
            if ((u64Val >> 32ULL) == 0)
            {
                /* If this field is 64-bit, VT-x will zero out the top bits. */
                rc = VMXWriteVmcs32(idxField, (uint32_t)u64Val);
            }
            else
            {
                /* Assert that only the 32->64 switcher case should ever come here. */
                Assert(pVM->hm.s.fAllow64BitGuests);
                rc = VMXWriteCachedVmcsEx(pVCpu, idxField, u64Val);
            }
            break;
        }

        default:
        {
            AssertMsgFailed(("VMXWriteVmcs64Ex: invalid field %#x (pVCpu=%p u64Val=%RX64)\n", (unsigned)idxField, pVCpu, u64Val));
            rc = VERR_INVALID_PARAMETER;
            break;
        }
    }
    AssertRCReturn(rc, rc);
    return rc;
}


/**
 * Queue up a VMWRITE by using the VMCS write cache. This is only used on 32-bit
 * hosts (except darwin) for 64-bit guests.
 *
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   idxField    The VMCS field encoding.
 * @param   u64Val      16, 32 or 64 bits value.
 */
VMMR0DECL(int) VMXWriteCachedVmcsEx(PVMCPU pVCpu, uint32_t idxField, uint64_t u64Val)
{
    AssertPtr(pVCpu);
    PVMCSCACHE pCache = &pVCpu->hm.s.vmx.VMCSCache;

    AssertMsgReturn(pCache->Write.cValidEntries < VMCSCACHE_MAX_ENTRY - 1,
                    ("entries=%u\n", pCache->Write.cValidEntries), VERR_ACCESS_DENIED);

    /* Make sure there are no duplicates. */
    for (unsigned i = 0; i < pCache->Write.cValidEntries; i++)
    {
        if (pCache->Write.aField[i] == idxField)
        {
            pCache->Write.aFieldVal[i] = u64Val;
            return VINF_SUCCESS;
        }
    }

    pCache->Write.aField[pCache->Write.cValidEntries]    = idxField;
    pCache->Write.aFieldVal[pCache->Write.cValidEntries] = u64Val;
    pCache->Write.cValidEntries++;
    return VINF_SUCCESS;
}


/**
 * Loads the VMCS write-cache into the CPU (by executing VMWRITEs).
 *
 * @param   pCache          Pointer to the VMCS cache.
 */
VMMR0DECL(void) VMXWriteCachedVmcsLoad(PVMCSCACHE pCache)
{
    AssertPtr(pCache);
    for (uint32_t i = 0; i < pCache->Write.cValidEntries; i++)
    {
        int rc = VMXWriteVmcs64(pCache->Write.aField[i], pCache->Write.aFieldVal[i]);
        AssertRC(rc,  rc);
    }
    pCache->Write.cValidEntries = 0;
}


/**
 * Stores the VMCS read-cache from the CPU (by executing VMREADs).
 *
 * @param   pCache          Pointer to the VMCS cache.
 * @remarks No-long-jump zone!!!
 */
VMMR0DECL(void) VMXReadCachedVmcsStore(PVMCSCACHE pCache)
{
    AssertPtr(pCache);
    for (uint32_t i = 0; i < pCache->Read.cValidEntries; i++)
    {
        int rc = VMXReadVmcs64(pCache->Read.aField[i], &pCache->Read.aFieldVal[i]);
        AssertRC(rc);
    }
}
#endif /* HC_ARCH_BITS == 32 && defined(VBOX_ENABLE_64_BITS_GUESTS) && !defined(VBOX_WITH_HYBRID_32BIT_KERNEL) */


/**
 * Sets up the usage of TSC-offsetting and updates the VMCS. If offsetting is
 * not possible, cause VM-exits on RDTSC(P)s. Also sets up the VMX preemption
 * timer.
 *
 * @returns VBox status code.
 * @param   pVM             Pointer to the VM.
 * @param   pVCpu           Pointer to the VMCPU.
 * @param   pMixedCtx       Pointer to the guest-CPU context. The data may be
 *                          out-of-sync. Make sure to update the required fields
 *                          before using them.
 * @remarks No-long-jump zone!!!
 */
static void hmR0VmxUpdateTscOffsettingAndPreemptTimer(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx)
{
    int rc = VERR_INTERNAL_ERROR_5;
    bool fOffsettedTsc = false;
    if (pVM->hm.s.vmx.fUsePreemptTimer)
    {
        uint64_t cTicksToDeadline = TMCpuTickGetDeadlineAndTscOffset(pVCpu, &fOffsettedTsc, &pVCpu->hm.s.vmx.u64TSCOffset);

        /* Make sure the returned values have sane upper and lower boundaries. */
        uint64_t u64CpuHz  = SUPGetCpuHzFromGIP(g_pSUPGlobalInfoPage);
        cTicksToDeadline   = RT_MIN(cTicksToDeadline, u64CpuHz / 64);      /* 1/64th of a second */
        cTicksToDeadline   = RT_MAX(cTicksToDeadline, u64CpuHz / 2048);    /* 1/2048th of a second */
        cTicksToDeadline >>= pVM->hm.s.vmx.cPreemptTimerShift;

        uint32_t cPreemptionTickCount = (uint32_t)RT_MIN(cTicksToDeadline, UINT32_MAX - 16);
        rc = VMXWriteVmcs32(VMX_VMCS32_GUEST_PREEMPTION_TIMER_VALUE, cPreemptionTickCount);        AssertRC(rc);
    }
    else
        fOffsettedTsc = TMCpuTickCanUseRealTSC(pVCpu, &pVCpu->hm.s.vmx.u64TSCOffset);

    if (fOffsettedTsc)
    {
        uint64_t u64CurTSC = ASMReadTSC();
        if (u64CurTSC + pVCpu->hm.s.vmx.u64TSCOffset >= TMCpuTickGetLastSeen(pVCpu))
        {
            /* Note: VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_RDTSC_EXIT takes precedence over TSC_OFFSET, applies to RDTSCP too. */
            rc = VMXWriteVmcs64(VMX_VMCS64_CTRL_TSC_OFFSET_FULL, pVCpu->hm.s.vmx.u64TSCOffset);    AssertRC(rc);

            pVCpu->hm.s.vmx.u32ProcCtls &= ~VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_RDTSC_EXIT;
            rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_PROC_EXEC_CONTROLS, pVCpu->hm.s.vmx.u32ProcCtls);  AssertRC(rc);
            STAM_COUNTER_INC(&pVCpu->hm.s.StatTscOffset);
        }
        else
        {
            /* VM-exit on RDTSC(P) as we would otherwise pass decreasing TSC values to the guest. */
            pVCpu->hm.s.vmx.u32ProcCtls |= VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_RDTSC_EXIT;
            rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_PROC_EXEC_CONTROLS, pVCpu->hm.s.vmx.u32ProcCtls);  AssertRC(rc);
            STAM_COUNTER_INC(&pVCpu->hm.s.StatTscInterceptOverFlow);
        }
    }
    else
    {
        /* We can't use TSC-offsetting (non-fixed TSC, warp drive active etc.), VM-exit on RDTSC(P). */
        pVCpu->hm.s.vmx.u32ProcCtls |= VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_RDTSC_EXIT;
        rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_PROC_EXEC_CONTROLS, pVCpu->hm.s.vmx.u32ProcCtls);      AssertRC(rc);
        STAM_COUNTER_INC(&pVCpu->hm.s.StatTscIntercept);
    }
}


/**
 * Determines if an exception is a benign exception. Benign exceptions
 * are ones which cannot cause double-faults.
 *
 * @returns true if the exception is benign, false otherwise.
 * @param   uVector     The exception vector.
 */
DECLINLINE(bool) hmR0VmxIsBenignXcpt(const uint32_t uVector)
{
    switch (uVector)
    {
        case X86_XCPT_DB:
        case X86_XCPT_NMI:
        case X86_XCPT_BP:
        case X86_XCPT_OF:
        case X86_XCPT_BR:
        case X86_XCPT_UD:
        case X86_XCPT_NM:
        case X86_XCPT_CO_SEG_OVERRUN:
        case X86_XCPT_MF:
        case X86_XCPT_AC:
        case X86_XCPT_MC:
        case X86_XCPT_XF:
            return true;
        default:
            return false;
    }
}


/**
 * Determines if an exception is a contributory exception. Contributory
 * exceptions are ones which can cause double-faults.
 *
 * @returns true if the exception is contributory, false otherwise.
 * @param   uVector     The exception vector.
 */
DECLINLINE(bool) hmR0VmxIsContributoryXcpt(const uint32_t uVector)
{
    switch (uVector)
    {
        case X86_XCPT_GP:
        case X86_XCPT_SS:
        case X86_XCPT_NP:
        case X86_XCPT_TS:
        case X86_XCPT_DE:
            return true;
        default:
            return false;
    }
    return false;
}


/**
 * Determines if we are intercepting any contributory exceptions.
 *
 * @returns true if we are intercepting any contributory exception, false
 *        otherwise.
 * @param   pVCpu       Pointer to the VMCPU.
 */
DECLINLINE(bool) hmR0VmxInterceptingContributoryXcpts(PVMCPU pVCpu)
{
    if (   (pVCpu->hm.s.vmx.u32XcptBitmap & RT_BIT(X86_XCPT_GP))
        || (pVCpu->hm.s.vmx.u32XcptBitmap & RT_BIT(X86_XCPT_SS))
        || (pVCpu->hm.s.vmx.u32XcptBitmap & RT_BIT(X86_XCPT_NP))
        || (pVCpu->hm.s.vmx.u32XcptBitmap & RT_BIT(X86_XCPT_TS))
        || (pVCpu->hm.s.vmx.u32XcptBitmap & RT_BIT(X86_XCPT_DE)))
    {
        return true;
    }
    return false;
}


/**
 * Handle a condition that occurred while delivering an event through the guest
 * IDT.
 *
 * @returns VBox status code (informational error codes included).
 * @retval VINF_SUCCESS if we should continue handling the VM-exit.
 * @retval VINF_VMX_DOUBLE_FAULT if a #DF condition was detected and we ought to
 *         continue execution of the guest which will delivery the #DF.
 * @retval VINF_EM_RESET if we detected a triple-fault condition.
 *
 * @param   pVM             Pointer to the VM.
 * @param   pVCpu           Pointer to the VMCPU.
 * @param   pMixedCtx       Pointer to the guest-CPU context. The data may be
 *                          out-of-sync. Make sure to update the required fields
 *                          before using them.
 * @param   pVmxTransient   Pointer to the VMX transient structure.
 *
 * @remarks No-long-jump zone!!!
 */
static int hmR0VmxCheckExitDueToEventDelivery(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    int rc = hmR0VmxReadIdtVectoringInfoVmcs(pVmxTransient);
    AssertRCReturn(rc, rc);
    if (VMX_IDT_VECTORING_INFO_VALID(pVmxTransient->uIdtVectoringInfo))
    {
        rc = hmR0VmxReadExitIntrInfoVmcs(pVmxTransient);
        AssertRCReturn(rc, rc);

        uint32_t uIntType    = VMX_IDT_VECTORING_INFO_TYPE(pVmxTransient->uIdtVectoringInfo);
        uint32_t uExitVector = VMX_EXIT_INTERRUPTION_INFO_VECTOR(pVmxTransient->uExitIntrInfo);
        uint32_t uIdtVector  = VMX_IDT_VECTORING_INFO_VECTOR(pVmxTransient->uIdtVectoringInfo);

        typedef enum
        {
            VMXREFLECTXCPT_XCPT,    /* Reflect Idt-vectoring exception. */
            VMXREFLECTXCPT_DF,      /* Reflect a double-fault to the guest. */
            VMXREFLECTXCPT_TF,      /* Reflect a triple fault state to the VMM. */
            VMXREFLECTXCPT_NONE     /* Nothing to reflect. */
        } VMXREFLECTXCPT;

        /* See Intel spec. 30.7.1.1 "Reflecting Exceptions to Guest Software". */
        VMXREFLECTXCPT enmReflect = VMXREFLECTXCPT_NONE;
        if (uIntType == VMX_IDT_VECTORING_INFO_TYPE_HW_XCPT)
        {
            if (   hmR0VmxIsBenignXcpt(uIdtVector)
                || hmR0VmxIsBenignXcpt(uExitVector)
                || (   hmR0VmxIsContributoryXcpt(uIdtVector)
                    && uExitVector == X86_XCPT_PF))
            {
                enmReflect = VMXREFLECTXCPT_XCPT;
            }
            else if (   (pVCpu->hm.s.vmx.u32XcptBitmap & RT_BIT(X86_XCPT_PF))
                     && uIdtVector == X86_XCPT_PF
                     && uExitVector == X86_XCPT_PF)
            {
                pVmxTransient->fVectoringPF = true;
            }
            else if (   hmR0VmxIsContributoryXcpt(uIdtVector)
                     && hmR0VmxIsContributoryXcpt(uExitVector))
            {
                enmReflect = VMXREFLECTXCPT_DF;
            }
            else if (   hmR0VmxInterceptingContributoryXcpts(pVCpu)
                     && uIdtVector == X86_XCPT_PF
                     && hmR0VmxIsContributoryXcpt(uExitVector))
            {
                enmReflect = VMXREFLECTXCPT_DF;
            }
            else if (uIdtVector == X86_XCPT_DF)
                enmReflect = VMXREFLECTXCPT_TF;
        }
        else if (   uIntType != VMX_IDT_VECTORING_INFO_TYPE_SW_INT
                 && uIntType != VMX_IDT_VECTORING_INFO_TYPE_SW_XCPT
                 && uIntType != VMX_IDT_VECTORING_INFO_TYPE_PRIV_SW_XCPT)
        {
            /*
             * Ignore software interrupts (INT n), software exceptions (#BP, #OF) and privileged software exception
             * (whatever they are) as they reoccur when restarting the instruction.
             */
            enmReflect = VMXREFLECTXCPT_XCPT;
        }

        Assert(pVmxTransient->fVectoringPF == false || enmReflect == VMXREFLECTXCPT_NONE);
        switch (enmReflect)
        {
            case VMXREFLECTXCPT_XCPT:
            {
                Assert(!pVCpu->hm.s.Event.fPending);
                pVCpu->hm.s.Event.fPending = true;
                pVCpu->hm.s.Event.u64IntrInfo = VMX_ENTRY_INTR_INFO_FROM_EXIT_IDT_INFO(pVmxTransient->uIdtVectoringInfo);
                if (VMX_IDT_VECTORING_INFO_ERROR_CODE_IS_VALID(pVCpu->hm.s.Event.u64IntrInfo))
                {
                    rc = hmR0VmxReadIdtVectoringErrorCodeVmcs(pVmxTransient);
                    AssertRCReturn(rc, rc);
                    pVCpu->hm.s.Event.u32ErrCode = pVmxTransient->uIdtVectoringErrorCode;
                }
                else
                    pVCpu->hm.s.Event.u32ErrCode = 0;
                Log(("Pending event %#RX64 Err=%#RX32\n", pVCpu->hm.s.Event.u64IntrInfo, pVCpu->hm.s.Event.u32ErrCode));
                break;
            }

            case VMXREFLECTXCPT_DF:
            {
                uint32_t u32IntrInfo;
                u32IntrInfo  = X86_XCPT_DF | (1 << VMX_EXIT_INTERRUPTION_INFO_VALID_SHIFT);
                u32IntrInfo |= (VMX_EXIT_INTERRUPTION_INFO_TYPE_HW_XCPT << VMX_EXIT_INTERRUPTION_INFO_TYPE_SHIFT);
                u32IntrInfo |= VMX_EXIT_INTERRUPTION_INFO_ERROR_CODE_VALID;

                Assert(!pVCpu->hm.s.Event.fPending);
                pVCpu->hm.s.Event.fPending     = true;
                pVCpu->hm.s.Event.u64IntrInfo  = u32IntrInfo;
                pVCpu->hm.s.Event.u32ErrCode   = 0;
                rc = VINF_VMX_DOUBLE_FAULT;
                Log(("Pending #DF %#RX64 uIdt=%#x uExit=%#x\n", pVCpu->hm.s.Event.u64IntrInfo, uIdtVector, uExitVector));
                break;
            }

            case VMXREFLECTXCPT_TF:
            {
                Log(("Pending triple-fault uIdt=%#x uExit=%#x\n", uIdtVector, uExitVector));
                rc = VINF_EM_RESET;
                break;
            }

            default:    /* shut up gcc. */
                break;
        }
    }
    Assert(rc == VINF_SUCCESS || rc == VINF_EM_RESET || rc == VINF_VMX_DOUBLE_FAULT);
    return rc;
}


/**
 * Saves the guest's CR0 register from the VMCS into the guest-CPU context.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pMixedCtx   Pointer to the guest-CPU context. The data maybe
 *                      out-of-sync. Make sure to update the required fields
 *                      before using them.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxSaveGuestCR0(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx)
{
    int rc = VINF_SUCCESS;
    if (   !(pVCpu->hm.s.vmx.fUpdatedGuestState & VMX_UPDATED_GUEST_CR0)
        || !(pVCpu->hm.s.vmx.fUpdatedGuestState & VMX_UPDATED_GUEST_FPU))
    {
        RTGCUINTREG uVal    = 0;
        RTCCUINTREG uShadow = 0;
        rc  = VMXReadVmcsGstN(VMX_VMCS_GUEST_CR0,            &uVal);
        rc |= VMXReadVmcsHstN(VMX_VMCS_CTRL_CR0_READ_SHADOW, &uShadow);
        AssertRCReturn(rc, rc);
        uVal = (uShadow & pVCpu->hm.s.vmx.cr0_mask) | (uVal & ~pVCpu->hm.s.vmx.cr0_mask);
        CPUMSetGuestCR0(pVCpu, uVal);
        pVCpu->hm.s.vmx.fUpdatedGuestState |= VMX_UPDATED_GUEST_CR0 | VMX_UPDATED_GUEST_FPU;
    }
    return rc;
}


/**
 * Saves the guest's CR4 register from the VMCS into the guest-CPU context.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pMixedCtx   Pointer to the guest-CPU context. The data maybe
 *                      out-of-sync. Make sure to update the required fields
 *                      before using them.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxSaveGuestCR4(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx)
{
    int rc = VINF_SUCCESS;
    if (!(pVCpu->hm.s.vmx.fUpdatedGuestState & VMX_UPDATED_GUEST_CR4))
    {
        RTGCUINTREG uVal    = 0;
        RTCCUINTREG uShadow = 0;
        rc  = VMXReadVmcsGstN(VMX_VMCS_GUEST_CR4,            &uVal);
        rc |= VMXReadVmcsHstN(VMX_VMCS_CTRL_CR4_READ_SHADOW, &uShadow);
        AssertRCReturn(rc, rc);
        uVal = (uShadow & pVCpu->hm.s.vmx.cr4_mask) | (uVal & ~pVCpu->hm.s.vmx.cr4_mask);
        CPUMSetGuestCR4(pVCpu, uVal);
        pVCpu->hm.s.vmx.fUpdatedGuestState |= VMX_UPDATED_GUEST_CR4;
    }
    return rc;
}


/**
 * Saves the guest's RIP register from the VMCS into the guest-CPU context.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pMixedCtx   Pointer to the guest-CPU context. The data maybe
 *                      out-of-sync. Make sure to update the required fields
 *                      before using them.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxSaveGuestRip(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx)
{
    if (pVCpu->hm.s.vmx.fUpdatedGuestState & VMX_UPDATED_GUEST_RIP)
        return VINF_SUCCESS;

    RTGCUINTREG uVal = 0;
    int rc = VMXReadVmcsGstN(VMX_VMCS_GUEST_RIP, &uVal);
    AssertRCReturn(rc, rc);
    pMixedCtx->rip = uVal;
    pVCpu->hm.s.vmx.fUpdatedGuestState |= VMX_UPDATED_GUEST_RIP;
    return rc;
}


/**
 * Saves the guest's RSP register from the VMCS into the guest-CPU context.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pMixedCtx   Pointer to the guest-CPU context. The data maybe
 *                      out-of-sync. Make sure to update the required fields
 *                      before using them.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxSaveGuestRsp(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx)
{
    if (pVCpu->hm.s.vmx.fUpdatedGuestState & VMX_UPDATED_GUEST_RSP)
        return VINF_SUCCESS;

    RTGCUINTREG uVal = 0;
    int rc = VMXReadVmcsGstN(VMX_VMCS_GUEST_RSP, &uVal);
    AssertRCReturn(rc, rc);
    pMixedCtx->rsp = uVal;
    pVCpu->hm.s.vmx.fUpdatedGuestState |= VMX_UPDATED_GUEST_RSP;
    return rc;
}


/**
 * Saves the guest's RFLAGS from the VMCS into the guest-CPU context.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pMixedCtx   Pointer to the guest-CPU context. The data maybe
 *                      out-of-sync. Make sure to update the required fields
 *                      before using them.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxSaveGuestRflags(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx)
{
    if (pVCpu->hm.s.vmx.fUpdatedGuestState & VMX_UPDATED_GUEST_RFLAGS)
        return VINF_SUCCESS;

    RTGCUINTREG uVal = 0;
    int rc = VMXReadVmcsGstN(VMX_VMCS_GUEST_RFLAGS, &uVal);
    AssertRCReturn(rc, rc);
    pMixedCtx->rflags.u64 = uVal;

    /* Undo our real-on-v86-mode changes to eflags if necessary. */
    if (pVCpu->hm.s.vmx.RealMode.fRealOnV86Active)
    {
        Assert(pVM->hm.s.vmx.pRealModeTSS);
        Log(("Saving real-mode RFLAGS VT-x view=%#RX64\n", pMixedCtx->rflags.u64));
        pMixedCtx->eflags.Bits.u1VM   = 0;
        pMixedCtx->eflags.Bits.u2IOPL = pVCpu->hm.s.vmx.RealMode.eflags.Bits.u2IOPL;
    }

    pVCpu->hm.s.vmx.fUpdatedGuestState |= VMX_UPDATED_GUEST_RFLAGS;
    return rc;
}


/**
 * Wrapper for saving the guest's RIP, RSP and RFLAGS from the VMCS into the
 * guest-CPU context.
 */
static int hmR0VmxSaveGuestGprs(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx)
{
    int rc = hmR0VmxSaveGuestRip(pVM, pVCpu, pMixedCtx);
    rc    |= hmR0VmxSaveGuestRsp(pVM, pVCpu, pMixedCtx);
    rc    |= hmR0VmxSaveGuestRflags(pVM, pVCpu, pMixedCtx);
    return rc;
}


/**
 * Saves the guest's interruptibility-state ("interrupt shadow" as AMD calls it)
 * from the guest-state area in the VMCS.
 *
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pMixedCtx   Pointer to the guest-CPU context. The data maybe
 *                      out-of-sync. Make sure to update the required fields
 *                      before using them.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(void) hmR0VmxSaveGuestIntrState(PVM pVM,  PVMCPU pVCpu, PCPUMCTX pMixedCtx)
{
    uint32_t uIntrState = 0;
    int rc = VMXReadVmcs32(VMX_VMCS32_GUEST_INTERRUPTIBILITY_STATE, &uIntrState);
    AssertRC(rc);

    if (!uIntrState)
        VMCPU_FF_CLEAR(pVCpu, VMCPU_FF_INHIBIT_INTERRUPTS);
    else
    {
        Assert(   uIntrState == VMX_VMCS_GUEST_INTERRUPTIBILITY_STATE_BLOCK_STI
               || uIntrState == VMX_VMCS_GUEST_INTERRUPTIBILITY_STATE_BLOCK_MOVSS);
        rc  = hmR0VmxSaveGuestRip(pVM, pVCpu, pMixedCtx);
        rc |= hmR0VmxSaveGuestRflags(pVM, pVCpu, pMixedCtx);    /* for hmR0VmxLoadGuestIntrState(). */
        AssertRC(rc);
        EMSetInhibitInterruptsPC(pVCpu, pMixedCtx->rip);
        Assert(VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_INHIBIT_INTERRUPTS));
    }
}


/**
 * Saves the guest's activity state.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pMixedCtx   Pointer to the guest-CPU context. The data maybe
 *                      out-of-sync. Make sure to update the required fields
 *                      before using them.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxSaveGuestActivityState(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx)
{
    /* Nothing to do for now until we make use of different guest-CPU activity state. Just update the flag. */
    pVCpu->hm.s.vmx.fUpdatedGuestState |= VMX_UPDATED_GUEST_ACTIVITY_STATE;
    return VINF_SUCCESS;
}


/**
 * Saves the guest SYSENTER MSRs (SYSENTER_CS, SYSENTER_EIP, SYSENTER_ESP) from
 * the current VMCS into the guest-CPU context.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pMixedCtx   Pointer to the guest-CPU context. The data maybe
 *                      out-of-sync. Make sure to update the required fields
 *                      before using them.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxSaveGuestSysenterMsrs(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx)
{
    int rc = VINF_SUCCESS;
    if (!(pVCpu->hm.s.vmx.fUpdatedGuestState & VMX_UPDATED_GUEST_SYSENTER_CS_MSR))
    {
        uint32_t u32Val = 0;
        rc = VMXReadVmcs32(VMX_VMCS32_GUEST_SYSENTER_CS, &u32Val);     AssertRCReturn(rc, rc);
        pMixedCtx->SysEnter.cs = u32Val;
        pVCpu->hm.s.vmx.fUpdatedGuestState |= VMX_UPDATED_GUEST_SYSENTER_CS_MSR;
    }

    RTGCUINTREG uGCVal = 0;
    if (!(pVCpu->hm.s.vmx.fUpdatedGuestState & VMX_UPDATED_GUEST_SYSENTER_EIP_MSR))
    {
        rc = VMXReadVmcsGstN(VMX_VMCS_GUEST_SYSENTER_EIP, &uGCVal);    AssertRCReturn(rc, rc);
        pMixedCtx->SysEnter.eip = uGCVal;
        pVCpu->hm.s.vmx.fUpdatedGuestState |= VMX_UPDATED_GUEST_SYSENTER_EIP_MSR;
    }
    if (!(pVCpu->hm.s.vmx.fUpdatedGuestState & VMX_UPDATED_GUEST_SYSENTER_ESP_MSR))
    {
        rc = VMXReadVmcsGstN(VMX_VMCS_GUEST_SYSENTER_ESP, &uGCVal);    AssertRCReturn(rc, rc);
        pMixedCtx->SysEnter.esp = uGCVal;
        pVCpu->hm.s.vmx.fUpdatedGuestState |= VMX_UPDATED_GUEST_SYSENTER_ESP_MSR;
    }
    return rc;
}


/**
 * Saves the guest FS_BASE MSRs from the current VMCS into the guest-CPU
 * context.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pMixedCtx   Pointer to the guest-CPU context. The data maybe
 *                      out-of-sync. Make sure to update the required fields
 *                      before using them.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxSaveGuestFSBaseMsr(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx)
{
    RTGCUINTREG uVal = 0;
    int rc = VINF_SUCCESS;
    if (!(pVCpu->hm.s.vmx.fUpdatedGuestState & VMX_UPDATED_GUEST_FS_BASE_MSR))
    {
        rc = VMXReadVmcsGstN(VMX_VMCS_GUEST_FS_BASE, &uVal);   AssertRCReturn(rc, rc);
        pMixedCtx->fs.u64Base = uVal;
        pVCpu->hm.s.vmx.fUpdatedGuestState |= VMX_UPDATED_GUEST_FS_BASE_MSR;
    }
    return rc;
}


/**
 * Saves the guest GS_BASE MSRs from the current VMCS into the guest-CPU
 * context.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pMixedCtx   Pointer to the guest-CPU context. The data maybe
 *                      out-of-sync. Make sure to update the required fields
 *                      before using them.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxSaveGuestGSBaseMsr(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx)
{
    RTGCUINTREG uVal = 0;
    int rc = VINF_SUCCESS;
    if (!(pVCpu->hm.s.vmx.fUpdatedGuestState & VMX_UPDATED_GUEST_GS_BASE_MSR))
    {
        rc = VMXReadVmcsGstN(VMX_VMCS_GUEST_GS_BASE, &uVal);   AssertRCReturn(rc, rc);
        pMixedCtx->gs.u64Base = uVal;
        pVCpu->hm.s.vmx.fUpdatedGuestState |= VMX_UPDATED_GUEST_GS_BASE_MSR;
    }
    return rc;
}


/**
 * Saves the auto load/store'd guest MSRs from the current VMCS into the
 * guest-CPU context. Currently these are LSTAR, STAR, SFMASK and TSC_AUX.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pMixedCtx   Pointer to the guest-CPU context. The data maybe
 *                      out-of-sync. Make sure to update the required fields
 *                      before using them.
 *
 * @remarks No-long-jump zone!!!
 */
static int hmR0VmxSaveGuestAutoLoadStoreMsrs(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx)
{
    if (pVCpu->hm.s.vmx.fUpdatedGuestState & VMX_UPDATED_GUEST_AUTO_LOAD_STORE_MSRS)
        return VINF_SUCCESS;

    for (unsigned i = 0; i < pVCpu->hm.s.vmx.cGuestMsrs; i++)
    {
        PVMXMSR pMsr = (PVMXMSR)pVCpu->hm.s.vmx.pvGuestMsr;
        pMsr += i;
        switch (pMsr->u32IndexMSR)
        {
            case MSR_K8_LSTAR:          pMixedCtx->msrLSTAR = pMsr->u64Value;                    break;
            case MSR_K6_STAR:           pMixedCtx->msrSTAR = pMsr->u64Value;                     break;
            case MSR_K8_SF_MASK:        pMixedCtx->msrSFMASK = pMsr->u64Value;                   break;
            case MSR_K8_TSC_AUX:        CPUMSetGuestMsr(pVCpu, MSR_K8_TSC_AUX, pMsr->u64Value);  break;
#if 0
            /* The KERNEL_GS_BASE MSR doesn't work reliably with auto load/store. See @bugref{6208}  */
            case MSR_K8_KERNEL_GS_BASE: pMixedCtx->msrKERNELGSBASE = pMsr->u64Value;             break;
#endif
            case MSR_K6_EFER:          /* EFER can't be changed without causing a VM-exit. */    break;
            default:
            {
                AssertFailed();
                return VERR_HM_UNEXPECTED_LD_ST_MSR;
            }
        }
    }
    pVCpu->hm.s.vmx.fUpdatedGuestState |= VMX_UPDATED_GUEST_AUTO_LOAD_STORE_MSRS;
    return VINF_SUCCESS;
}


/**
 * Saves the guest control registers from the current VMCS into the guest-CPU
 * context.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pMixedCtx   Pointer to the guest-CPU context. The data maybe
 *                      out-of-sync. Make sure to update the required fields
 *                      before using them.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxSaveGuestControlRegs(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx)
{
    RTGCUINTREG uVal    = 0;
    RTGCUINTREG uShadow = 0;
    int rc              = VINF_SUCCESS;

    /* Guest CR0. Guest FPU. */
    rc = hmR0VmxSaveGuestCR0(pVM, pVCpu, pMixedCtx);

    /* Guest CR4. */
    rc |= hmR0VmxSaveGuestCR4(pVM, pVCpu, pMixedCtx);
    AssertRCReturn(rc, rc);

    /* Guest CR3. Only changes with Nested Paging. This must be done -after- saving CR0 and CR4 from the guest! */
    if (!(pVCpu->hm.s.vmx.fUpdatedGuestState & VMX_UPDATED_GUEST_CR3))
    {
        if (   pVM->hm.s.fNestedPaging
            && CPUMIsGuestPagingEnabledEx(pMixedCtx))
        {
            rc = VMXReadVmcsGstN(VMX_VMCS_GUEST_CR3, &uVal);
            if (pMixedCtx->cr3 != uVal)
            {
                CPUMSetGuestCR3(pVCpu, uVal);
                /* Set the force flag to inform PGM about it when necessary. It is cleared by PGMUpdateCR3().*/
                VMCPU_FF_SET(pVCpu, VMCPU_FF_HM_UPDATE_CR3);
            }

            /* We require EFER to check PAE mode. */
            rc |= hmR0VmxSaveGuestAutoLoadStoreMsrs(pVM, pVCpu, pMixedCtx);

            /* If the guest is in PAE mode, sync back the PDPE's into the guest state. */
            if (CPUMIsGuestInPAEModeEx(pMixedCtx))  /* Reads CR0, CR4 and EFER MSR. */
            {
                rc |= VMXReadVmcs64(VMX_VMCS64_GUEST_PDPTE0_FULL, &pVCpu->hm.s.aPdpes[0].u);
                rc |= VMXReadVmcs64(VMX_VMCS64_GUEST_PDPTE1_FULL, &pVCpu->hm.s.aPdpes[1].u);
                rc |= VMXReadVmcs64(VMX_VMCS64_GUEST_PDPTE2_FULL, &pVCpu->hm.s.aPdpes[2].u);
                rc |= VMXReadVmcs64(VMX_VMCS64_GUEST_PDPTE3_FULL, &pVCpu->hm.s.aPdpes[3].u);
                /* Set the force flag to inform PGM about it when necessary. It is cleared by PGMGstUpdatePaePdpes(). */
                VMCPU_FF_SET(pVCpu, VMCPU_FF_HM_UPDATE_PAE_PDPES);
            }
            AssertRCReturn(rc, rc);
        }
        pVCpu->hm.s.vmx.fUpdatedGuestState |= VMX_UPDATED_GUEST_CR3;
    }
    return rc;
}


/**
 * Reads a guest segment register from the current VMCS into the guest-CPU
 * context.
 *
 * @returns VBox status code.
 * @param   idxSel      Index of the selector in the VMCS.
 * @param   idxLimit    Index of the segment limit in the VMCS.
 * @param   idxBase     Index of the segment base in the VMCS.
 * @param   idxAccess   Index of the access rights of the segment in the VMCS.
 * @param   pSelReg     Pointer to the segment selector.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxReadSegmentReg(uint32_t idxSel, uint32_t idxLimit, uint32_t idxBase, uint32_t idxAccess,
                                    PCPUMSELREG pSelReg)
{
    uint32_t u32Val = 0;
    int rc = VMXReadVmcs32(idxSel, &u32Val);
    pSelReg->Sel      = (uint16_t)u32Val;
    pSelReg->ValidSel = (uint16_t)u32Val;
    pSelReg->fFlags   = CPUMSELREG_FLAGS_VALID;

    rc |= VMXReadVmcs32(idxLimit, &u32Val);
    pSelReg->u32Limit = u32Val;

    RTGCUINTREG uGCVal = 0;
    rc |= VMXReadVmcsGstN(idxBase, &uGCVal);
    pSelReg->u64Base = uGCVal;

    rc |= VMXReadVmcs32(idxAccess, &u32Val);
    pSelReg->Attr.u  = u32Val;
    AssertRCReturn(rc, rc);

    /*
     * If VT-x marks the segment as unusable, the rest of the attributes are undefined.
     * See Intel spec. 27.3.2 "Saving Segment Registers and Descriptor-Table Registers.
     */
    if (pSelReg->Attr.u & VMX_SEL_UNUSABLE)
    {
        Assert(idxSel != VMX_VMCS16_GUEST_FIELD_TR);
        pSelReg->Attr.u = VMX_SEL_UNUSABLE;
    }
    return rc;
}


/**
 * Saves the guest segment registers from the current VMCS into the guest-CPU
 * context.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pMixedCtx   Pointer to the guest-CPU context. The data maybe
 *                      out-of-sync. Make sure to update the required fields
 *                      before using them.
 *
 * @remarks No-long-jump zone!!!
 */
static int hmR0VmxSaveGuestSegmentRegs(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx)
{
    int rc = VINF_SUCCESS;

    /* Guest segment registers. */
    if (!(pVCpu->hm.s.vmx.fUpdatedGuestState & VMX_UPDATED_GUEST_SEGMENT_REGS))
    {
        rc = hmR0VmxSaveGuestCR0(pVM, pVCpu, pMixedCtx);

        rc |= hmR0VmxReadSegmentReg(VMX_VMCS16_GUEST_FIELD_CS, VMX_VMCS32_GUEST_CS_LIMIT, VMX_VMCS_GUEST_CS_BASE,
                                    VMX_VMCS32_GUEST_CS_ACCESS_RIGHTS, &pMixedCtx->cs);
        rc |= hmR0VmxReadSegmentReg(VMX_VMCS16_GUEST_FIELD_SS, VMX_VMCS32_GUEST_SS_LIMIT, VMX_VMCS_GUEST_SS_BASE,
                                    VMX_VMCS32_GUEST_SS_ACCESS_RIGHTS, &pMixedCtx->ss);
        rc |= hmR0VmxReadSegmentReg(VMX_VMCS16_GUEST_FIELD_DS, VMX_VMCS32_GUEST_DS_LIMIT, VMX_VMCS_GUEST_DS_BASE,
                                    VMX_VMCS32_GUEST_DS_ACCESS_RIGHTS, &pMixedCtx->ds);
        rc |= hmR0VmxReadSegmentReg(VMX_VMCS16_GUEST_FIELD_ES, VMX_VMCS32_GUEST_ES_LIMIT, VMX_VMCS_GUEST_ES_BASE,
                                    VMX_VMCS32_GUEST_ES_ACCESS_RIGHTS, &pMixedCtx->es);
        rc |= hmR0VmxReadSegmentReg(VMX_VMCS16_GUEST_FIELD_FS, VMX_VMCS32_GUEST_FS_LIMIT, VMX_VMCS_GUEST_FS_BASE,
                                    VMX_VMCS32_GUEST_FS_ACCESS_RIGHTS, &pMixedCtx->fs);
        rc |= hmR0VmxReadSegmentReg(VMX_VMCS16_GUEST_FIELD_GS, VMX_VMCS32_GUEST_GS_LIMIT, VMX_VMCS_GUEST_GS_BASE,
                                    VMX_VMCS32_GUEST_GS_ACCESS_RIGHTS, &pMixedCtx->gs);
        AssertRCReturn(rc, rc);

        /* Restore segment attributes for real-on-v86 mode hack. */
        if (pVCpu->hm.s.vmx.RealMode.fRealOnV86Active)
        {
            pMixedCtx->cs.Attr.u = pVCpu->hm.s.vmx.RealMode.uAttrCS.u;
            pMixedCtx->ss.Attr.u = pVCpu->hm.s.vmx.RealMode.uAttrSS.u;
            pMixedCtx->ds.Attr.u = pVCpu->hm.s.vmx.RealMode.uAttrDS.u;
            pMixedCtx->es.Attr.u = pVCpu->hm.s.vmx.RealMode.uAttrES.u;
            pMixedCtx->fs.Attr.u = pVCpu->hm.s.vmx.RealMode.uAttrFS.u;
            pMixedCtx->gs.Attr.u = pVCpu->hm.s.vmx.RealMode.uAttrGS.u;
        }

        pVCpu->hm.s.vmx.fUpdatedGuestState |= VMX_UPDATED_GUEST_SEGMENT_REGS;
    }

    /* Guest LDTR. */
    if (!(pVCpu->hm.s.vmx.fUpdatedGuestState & VMX_UPDATED_GUEST_LDTR))
    {
        rc = hmR0VmxReadSegmentReg(VMX_VMCS16_GUEST_FIELD_LDTR, VMX_VMCS32_GUEST_LDTR_LIMIT, VMX_VMCS_GUEST_LDTR_BASE,
                                   VMX_VMCS32_GUEST_LDTR_ACCESS_RIGHTS, &pMixedCtx->ldtr);
        AssertRCReturn(rc, rc);
        pVCpu->hm.s.vmx.fUpdatedGuestState |= VMX_UPDATED_GUEST_LDTR;
    }

    /* Guest GDTR. */
    RTGCUINTREG uGCVal = 0;
    uint32_t    u32Val = 0;
    if (!(pVCpu->hm.s.vmx.fUpdatedGuestState & VMX_UPDATED_GUEST_GDTR))
    {
        rc  = VMXReadVmcsGstN(VMX_VMCS_GUEST_GDTR_BASE, &uGCVal);
        rc |= VMXReadVmcs32(VMX_VMCS32_GUEST_GDTR_LIMIT, &u32Val);  AssertRCReturn(rc, rc);
        pMixedCtx->gdtr.pGdt  = uGCVal;
        pMixedCtx->gdtr.cbGdt = u32Val;
        pVCpu->hm.s.vmx.fUpdatedGuestState |= VMX_UPDATED_GUEST_GDTR;
    }

    /* Guest IDTR. */
    if (!(pVCpu->hm.s.vmx.fUpdatedGuestState & VMX_UPDATED_GUEST_IDTR))
    {
        rc  = VMXReadVmcsGstN(VMX_VMCS_GUEST_IDTR_BASE, &uGCVal);
        rc |= VMXReadVmcs32(VMX_VMCS32_GUEST_IDTR_LIMIT, &u32Val);   AssertRCReturn(rc, rc);
        pMixedCtx->idtr.pIdt  = uGCVal;
        pMixedCtx->idtr.cbIdt = u32Val;
        pVCpu->hm.s.vmx.fUpdatedGuestState |= VMX_UPDATED_GUEST_IDTR;
    }

    /* Guest TR. */
    if (!(pVCpu->hm.s.vmx.fUpdatedGuestState & VMX_UPDATED_GUEST_TR))
    {
        rc = hmR0VmxSaveGuestCR0(pVM, pVCpu, pMixedCtx);

        /* For real-mode emulation using virtual-8086 mode we have the fake TSS (pRealModeTSS) in TR, don't sync the fake one. */
        if (!pVCpu->hm.s.vmx.RealMode.fRealOnV86Active)
        {
            rc |= hmR0VmxReadSegmentReg(VMX_VMCS16_GUEST_FIELD_TR, VMX_VMCS32_GUEST_TR_LIMIT, VMX_VMCS_GUEST_TR_BASE,
                                        VMX_VMCS32_GUEST_TR_ACCESS_RIGHTS, &pMixedCtx->tr);
        }
        AssertRCReturn(rc, rc);
        pVCpu->hm.s.vmx.fUpdatedGuestState |= VMX_UPDATED_GUEST_TR;
    }
    return rc;
}


/**
 * Saves the guest debug registers from the current VMCS into the guest-CPU
 * context.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pMixedCtx   Pointer to the guest-CPU context. The data maybe
 *                      out-of-sync. Make sure to update the required fields
 *                      before using them.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxSaveGuestDebugRegs(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx)
{
    int rc = VINF_SUCCESS;
    if (!(pVCpu->hm.s.vmx.fUpdatedGuestState & VMX_UPDATED_GUEST_DEBUG))
    {
        RTGCUINTREG uVal;
        rc = VMXReadVmcsGstN(VMX_VMCS_GUEST_DR7, &uVal);          AssertRCReturn(rc, rc);
        pMixedCtx->dr[7] = uVal;

        pVCpu->hm.s.vmx.fUpdatedGuestState |= VMX_UPDATED_GUEST_DEBUG;
    }
    return rc;
}


/**
 * Saves the guest APIC state from the currentl VMCS into the guest-CPU context.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pMixedCtx   Pointer to the guest-CPU context. The data maybe
 *                      out-of-sync. Make sure to update the required fields
 *                      before using them.
 *
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(int) hmR0VmxSaveGuestApicState(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx)
{
    /* Updating TPR is already done in hmR0VmxPostRunGuest(). Just update the flag. */
    pVCpu->hm.s.vmx.fUpdatedGuestState |= VMX_UPDATED_GUEST_APIC_STATE;
    return VINF_SUCCESS;
}


/**
 * Saves the entire guest state from the currently active VMCS into the
 * guest-CPU context. This essentially VMREADs all guest-data.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pMixedCtx   Pointer to the guest-CPU context. The data may be
 *                      out-of-sync. Make sure to update the required fields
 *                      before using them.
 */
static int hmR0VmxSaveGuestState(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx)
{
    Assert(pVM);
    Assert(pVCpu);
    Assert(pMixedCtx);

    if (pVCpu->hm.s.vmx.fUpdatedGuestState == VMX_UPDATED_GUEST_ALL)
        return VINF_SUCCESS;

    VMMRZCallRing3Disable(pVCpu);

    int rc = hmR0VmxSaveGuestGprs(pVM, pVCpu, pMixedCtx);
    AssertLogRelMsgRCReturn(rc, ("hmR0VmxSaveGuestGprs failed! rc=%Rrc (pVM=%p pVCpu=%p)\n", rc, pVM, pVCpu), rc);

    rc = hmR0VmxSaveGuestControlRegs(pVM, pVCpu, pMixedCtx);
    AssertLogRelMsgRCReturn(rc, ("hmR0VmxSaveGuestControlRegs failed! rc=%Rrc (pVM=%p pVCpu=%p)\n", rc, pVM, pVCpu), rc);

    rc = hmR0VmxSaveGuestSegmentRegs(pVM, pVCpu, pMixedCtx);
    AssertLogRelMsgRCReturn(rc, ("hmR0VmxSaveGuestSegmentRegs failed! rc=%Rrc (pVM=%p pVCpu=%p)\n", rc, pVM, pVCpu), rc);

    rc = hmR0VmxSaveGuestDebugRegs(pVM, pVCpu, pMixedCtx);
    AssertLogRelMsgRCReturn(rc, ("hmR0VmxSaveGuestDebugRegs failed! rc=%Rrc (pVM=%p pVCpu=%p)\n", rc, pVM, pVCpu), rc);

    rc = hmR0VmxSaveGuestSysenterMsrs(pVM, pVCpu, pMixedCtx);
    AssertLogRelMsgRCReturn(rc, ("hmR0VmxSaveGuestSysenterMsrs failed! rc=%Rrc (pVM=%p pVCpu=%p)\n", rc, pVM, pVCpu), rc);

    rc = hmR0VmxSaveGuestFSBaseMsr(pVM, pVCpu, pMixedCtx);
    AssertLogRelMsgRCReturn(rc, ("hmR0VmxSaveGuestFSBaseMsr failed! rc=%Rrc (pVM=%p pVCpu=%p)\n", rc, pVM, pVCpu), rc);

    rc = hmR0VmxSaveGuestGSBaseMsr(pVM, pVCpu, pMixedCtx);
    AssertLogRelMsgRCReturn(rc, ("hmR0VmxSaveGuestGSBaseMsr failed! rc=%Rrc (pVM=%p pVCpu=%p)\n", rc, pVM, pVCpu), rc);

    rc = hmR0VmxSaveGuestAutoLoadStoreMsrs(pVM, pVCpu, pMixedCtx);
    AssertLogRelMsgRCReturn(rc, ("hmR0VmxSaveGuestAutoLoadStoreMsrs failed! rc=%Rrc (pVM=%p pVCpu=%p)\n", rc, pVM, pVCpu), rc);

    rc = hmR0VmxSaveGuestActivityState(pVM, pVCpu, pMixedCtx);
    AssertLogRelMsgRCReturn(rc, ("hmR0VmxSaveGuestActivityState failed! rc=%Rrc (pVM=%p pVCpu=%p)\n", rc, pVM, pVCpu), rc);

    rc = hmR0VmxSaveGuestApicState(pVM, pVCpu, pMixedCtx);
    AssertLogRelMsgRCReturn(rc, ("hmR0VmxSaveGuestDebugRegs failed! rc=%Rrc (pVM=%p pVCpu=%p)\n", rc, pVM, pVCpu), rc);

    AssertMsg(pVCpu->hm.s.vmx.fUpdatedGuestState == VMX_UPDATED_GUEST_ALL,
              ("Missed guest state bits while saving state; residue %RX32\n", pVCpu->hm.s.vmx.fUpdatedGuestState));

    VMMRZCallRing3Enable(pVCpu);
    return rc;
}


/**
 * Check per-VM and per-VCPU force flag actions that require us to go back to
 * ring-3 for one reason or another.
 *
 * @returns VBox status code (information status code included).
 * @retval VINF_SUCCESS if we don't have any actions that require going back to
 *         ring-3.
 * @retval VINF_PGM_SYNC_CR3 if we have pending PGM CR3 sync.
 * @retval VINF_EM_PENDING_REQUEST if we have pending requests (like hardware
 *         interrupts)
 * @retval VINF_PGM_POOL_FLUSH_PENDING if PGM is doing a pool flush and requires
 *         all EMTs to be in ring-3.
 * @retval VINF_EM_RAW_TO_R3 if there is pending DMA requests.
 * @retval VINF_EM_NO_MEMORY PGM is out of memory, we need to return
 *         to the EM loop.
 *
 * @param   pVM             Pointer to the VM.
 * @param   pVCpu           Pointer to the VMCPU.
 * @param   pMixedCtx       Pointer to the guest-CPU context. The data may be
 *                          out-of-sync. Make sure to update the required fields
 *                          before using them.
 */
static int hmR0VmxCheckForceFlags(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx)
{
    Assert(VMMRZCallRing3IsEnabled(pVCpu));

    int rc = VERR_INTERNAL_ERROR_5;
    if (   VM_FF_IS_PENDING(pVM, VM_FF_HM_TO_R3_MASK | VM_FF_REQUEST | VM_FF_PGM_POOL_FLUSH_PENDING | VM_FF_PDM_DMA)
        || VMCPU_FF_IS_PENDING(pVCpu,  VMCPU_FF_HM_TO_R3_MASK | VMCPU_FF_PGM_SYNC_CR3 | VMCPU_FF_PGM_SYNC_CR3_NON_GLOBAL
                                     | VMCPU_FF_REQUEST | VMCPU_FF_HM_UPDATE_CR3 | VMCPU_FF_HM_UPDATE_PAE_PDPES))
    {
        /* We need the control registers now, make sure the guest-CPU context is updated. */
        rc = hmR0VmxSaveGuestControlRegs(pVM, pVCpu, pMixedCtx);
        AssertRCReturn(rc, rc);

        /* Pending HM CR3 sync. */
        if (VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_HM_UPDATE_CR3))
        {
            rc = PGMUpdateCR3(pVCpu, pMixedCtx->cr3);
            Assert(rc == VINF_SUCCESS || rc == VINF_PGM_SYNC_CR3);
        }
        if (VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_HM_UPDATE_PAE_PDPES))
        {
            rc = PGMGstUpdatePaePdpes(pVCpu, &pVCpu->hm.s.aPdpes[0]);
            AssertRC(rc);
        }

        /* Pending PGM C3 sync. */
        if (VMCPU_FF_IS_PENDING(pVCpu,VMCPU_FF_PGM_SYNC_CR3 | VMCPU_FF_PGM_SYNC_CR3_NON_GLOBAL))
        {
            rc = PGMSyncCR3(pVCpu, pMixedCtx->cr0, pMixedCtx->cr3, pMixedCtx->cr4, VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_PGM_SYNC_CR3));
            if (rc != VINF_SUCCESS)
            {
                AssertRC(rc);
                Log2(("hmR0VmxCheckForceFlags: PGMSyncCR3 forcing us back to ring-3. rc=%d\n", rc));
                return rc;
            }
        }

        /* Pending HM-to-R3 operations (critsects, timers, EMT rendezvous etc.) */
        /* -XXX- what was that about single stepping?  */
        if (   VM_FF_IS_PENDING(pVM, VM_FF_HM_TO_R3_MASK)
            || VMCPU_FF_IS_PENDING(pVCpu, VMCPU_FF_HM_TO_R3_MASK))
        {
            STAM_COUNTER_INC(&pVCpu->hm.s.StatSwitchHmToR3FF);
            rc = RT_UNLIKELY(VM_FF_IS_PENDING(pVM, VM_FF_PGM_NO_MEMORY)) ? VINF_EM_NO_MEMORY : VINF_EM_RAW_TO_R3;
            Log2(("hmR0VmxCheckForceFlags: HM_TO_R3 forcing us back to ring-3. rc=%d\n", rc));
            return rc;
        }

        /* Pending VM request packets, such as hardware interrupts. */
        if (   VM_FF_IS_PENDING(pVM, VM_FF_REQUEST)
            || VMCPU_FF_IS_PENDING(pVCpu, VMCPU_FF_REQUEST))
        {
            Log2(("hmR0VmxCheckForceFlags: Pending VM request forcing us back to ring-3\n"));
            return VINF_EM_PENDING_REQUEST;
        }

        /* Pending PGM pool flushes. */
        if (VM_FF_IS_PENDING(pVM, VM_FF_PGM_POOL_FLUSH_PENDING))
        {
            Log2(("hmR0VmxCheckForceFlags: PGM pool flush pending forcing us back to ring-3\n"));
            return rc = VINF_PGM_POOL_FLUSH_PENDING;
        }

        /* Pending DMA requests. */
        if (VM_FF_IS_PENDING(pVM, VM_FF_PDM_DMA))
        {
            Log2(("hmR0VmxCheckForceFlags: Pending DMA request forcing us back to ring-3\n"));
            return VINF_EM_RAW_TO_R3;
        }
    }

    /* Paranoia. */
    Assert(rc != VERR_EM_INTERPRETER);
    return VINF_SUCCESS;
}


/**
 * Converts any pending VMX event into a TRPM trap. Typically used when leaving
 * VT-x to execute any instruction.
 *
 * @param   pvCpu               Pointer to the VMCPU.
 */
static void hmR0VmxUpdateTRPMTrap(PVMCPU pVCpu)
{
    if (pVCpu->hm.s.Event.fPending)
    {
        uint32_t uVectorType     = VMX_IDT_VECTORING_INFO_TYPE(pVCpu->hm.s.Event.u64IntrInfo);
        uint32_t uVector         = VMX_IDT_VECTORING_INFO_VECTOR(pVCpu->hm.s.Event.u64IntrInfo);
        bool     fErrorCodeValid = VMX_IDT_VECTORING_INFO_ERROR_CODE_IS_VALID(pVCpu->hm.s.Event.u64IntrInfo);
        uint32_t uErrorCode      = pVCpu->hm.s.Event.u32ErrCode;

        /* If a trap was already pending, we did something wrong! */
        Assert(TRPMQueryTrap(pVCpu, NULL, NULL) == VERR_TRPM_NO_ACTIVE_TRAP);

        /* A page-fault exception during a page-fault would become a double-fault. */
        AssertMsg(uVectorType != VMX_IDT_VECTORING_INFO_TYPE_HW_XCPT || uVector != X86_XCPT_PF,
                  ("%#RX64 uVectorType=%#x uVector=%#x\n", pVCpu->hm.s.Event.u64IntrInfo, uVectorType, uVector));

        TRPMEVENT enmTrapType;
        switch (uVectorType)
        {
            case VMX_IDT_VECTORING_INFO_TYPE_EXT_INT:
            case VMX_IDT_VECTORING_INFO_TYPE_NMI:
               enmTrapType = TRPM_HARDWARE_INT;
               break;
            case VMX_IDT_VECTORING_INFO_TYPE_SW_INT:
                enmTrapType = TRPM_SOFTWARE_INT;
                break;
            case VMX_IDT_VECTORING_INFO_TYPE_PRIV_SW_XCPT:
            case VMX_IDT_VECTORING_INFO_TYPE_SW_XCPT:      /* #BP and #OF */
            case VMX_IDT_VECTORING_INFO_TYPE_HW_XCPT:
                enmTrapType = TRPM_TRAP;
                break;
            default:
                AssertMsgFailed(("Invalid trap type %#x\n", uVectorType));
                enmTrapType = TRPM_32BIT_HACK;
                break;
        }
        int rc = TRPMAssertTrap(pVCpu, uVector, enmTrapType);
        AssertRC(rc);
        if (fErrorCodeValid)
            TRPMSetErrorCode(pVCpu, uErrorCode);

        /* Clear the VT-x state bits now that TRPM has the information. */
        pVCpu->hm.s.Event.fPending = false;
        rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_ENTRY_INTERRUPTION_INFO, 0);
        AssertRC(rc);
    }
}


/**
 * Does the necessary state syncing before doing a longjmp to ring-3.
 *
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pMixedCtx   Pointer to the guest-CPU context. The data may be
 *                      out-of-sync. Make sure to update the required fields
 *                      before using them.
 * @param   rcExit      The reason for exiting to ring-3. Can be
 *                      VINF_VMM_UNKNOWN_RING3_CALL.
 *
 * @remarks No-long-jmp zone!!!
 */
static void hmR0VmxLongJmpToRing3(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, int rcExit)
{
    Assert(!VMMRZCallRing3IsEnabled(pVCpu));

    int rc = hmR0VmxSaveGuestState(pVM, pVCpu, pMixedCtx);
    AssertRC(rc);

    /* Restore FPU state if necessary and resync on next R0 reentry .*/
    if (CPUMIsGuestFPUStateActive(pVCpu))
    {
        CPUMR0SaveGuestFPU(pVM, pVCpu, pMixedCtx);
        Assert(!CPUMIsGuestFPUStateActive(pVCpu));
        pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_CR0;
    }

    /* Restore debug registers if necessary and resync on next R0 reentry. */
    if (CPUMIsGuestDebugStateActive(pVCpu))
    {
        CPUMR0SaveGuestDebugState(pVM, pVCpu, pMixedCtx, true /* save DR6 */);
        Assert(!CPUMIsGuestDebugStateActive(pVCpu));
        pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_DEBUG;
    }
    else if (CPUMIsHyperDebugStateActive(pVCpu))
    {
        CPUMR0LoadHostDebugState(pVM, pVCpu);
        Assert(pVCpu->hm.s.vmx.u32ProcCtls & VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_MOV_DR_EXIT);
    }

    STAM_COUNTER_INC(&pVCpu->hm.s.StatSwitchLongJmpToR3);
}


/**
 * An action requires us to go back to ring-3. This function does the necessary
 * steps before we can safely return to ring-3. This is not the same as longjmps
 * to ring-3, this is voluntary.
 *
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pMixedCtx   Pointer to the guest-CPU context. The data may be
 *                      out-of-sync. Make sure to update the required fields
 *                      before using them.
 * @param   rcExit      The reason for exiting to ring-3. Can be
 *                      VINF_VMM_UNKNOWN_RING3_CALL.
 */
static void hmR0VmxExitToRing3(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, int rcExit)
{
    Assert(pVM);
    Assert(pVCpu);
    Assert(pMixedCtx);
    Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));

    /* We want to see what the guest-state was before VM-entry, don't resync here, as we will never continue guest execution.*/
    if (rcExit == VERR_VMX_INVALID_GUEST_STATE)
        return;

    /* Please, no longjumps here (any logging shouldn't flush jump back to ring-3). NO LOGGING BEFORE THIS POINT! */
    VMMRZCallRing3Disable(pVCpu);
    Log(("hmR0VmxExitToRing3: rcExit=%d\n", rcExit));

    /* We need to do this only while truly exiting the "inner loop" back to ring-3 and -not- for any longjmp to ring3. */
    hmR0VmxUpdateTRPMTrap(pVCpu);

    /* Sync. the guest state. */
    hmR0VmxLongJmpToRing3(pVM, pVCpu, pMixedCtx, rcExit);
    STAM_COUNTER_DEC(&pVCpu->hm.s.StatSwitchLongJmpToR3);

    VMCPU_FF_CLEAR(pVCpu, VMCPU_FF_TO_R3);
    CPUMSetChangedFlags(pVCpu,  CPUM_CHANGED_SYSENTER_MSR
                              | CPUM_CHANGED_LDTR
                              | CPUM_CHANGED_GDTR
                              | CPUM_CHANGED_IDTR
                              | CPUM_CHANGED_TR
                              | CPUM_CHANGED_HIDDEN_SEL_REGS);

    /* On our way back from ring-3 the following needs to be done. */
    /** @todo This can change with preemption hooks. */
    if (rcExit == VINF_EM_RAW_INTERRUPT)
        pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_HOST_CONTEXT;
    else
        pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_HOST_CONTEXT | HM_CHANGED_ALL_GUEST;

    STAM_COUNTER_INC(&pVCpu->hm.s.StatSwitchExitToR3);
    VMMRZCallRing3Enable(pVCpu);
}


/**
 *  VMMRZCallRing3 callback wrapper which saves the guest state before we
 *  longjump to ring-3 and possibly get preempted.
 *
 * @param   pVCpu           Pointer to the VMCPU.
 * @param   enmOperation    The operation causing the ring-3 longjump.
 * @param   pvUser          The user argument (pointer to the possibly
 *                          out-of-date guest-CPU context).
 *
 * @remarks Must never be called with @a enmOperation ==
 *          VMMCALLRING3_VM_R0_ASSERTION.
 */
DECLCALLBACK(void) hmR0VmxCallRing3Callback(PVMCPU pVCpu, VMMCALLRING3 enmOperation, void *pvUser)
{
    /* VMMRZCallRing3() already makes sure we never get called as a result of an longjmp due to an assertion, */
    Assert(pVCpu);
    Assert(pvUser);
    Assert(VMMRZCallRing3IsEnabled(pVCpu));
    Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));

    VMMRZCallRing3Disable(pVCpu);
    Log(("hmR0VmxLongJmpToRing3\n"));
    hmR0VmxLongJmpToRing3(pVCpu->CTX_SUFF(pVM), pVCpu, (PCPUMCTX)pvUser, VINF_VMM_UNKNOWN_RING3_CALL);
    VMMRZCallRing3Enable(pVCpu);
}


/**
 * Injects any pending TRPM trap into the VM by updating the VMCS.
 *
 * @returns VBox status code (informational status code included).
 * @param   pVM             Pointer to the VM.
 * @param   pVCpu           Pointer to the VMCPU.
 * @param   pMixedCtx       Pointer to the guest-CPU context. The data may be
 *                          out-of-sync. Make sure to update the required fields
 *                          before using them.
 */
static int hmR0VmxInjectTRPMTrap(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx)
{
    if (!TRPMHasTrap(pVCpu))
        return VINF_SUCCESS;

    uint8_t   u8Vector     = 0;
    TRPMEVENT enmTrpmEvent = TRPM_SOFTWARE_INT;
    RTGCUINT  uErrCode     = 0;

    int rc = TRPMQueryTrapAll(pVCpu, &u8Vector, &enmTrpmEvent, &uErrCode, NULL /* puCr2 */);
    AssertRCReturn(rc, rc);
    Assert(enmTrpmEvent != TRPM_SOFTWARE_INT);

    rc = TRPMResetTrap(pVCpu);
    AssertRCReturn(rc, rc);

    /* Refer Intel spec. 24.8.3 "VM-entry Controls for Event Injection" for the format of u32IntrInfo. */
    uint32_t u32IntrInfo = u8Vector | (1 << VMX_EXIT_INTERRUPTION_INFO_VALID_SHIFT);
    if (enmTrpmEvent == TRPM_TRAP)
    {
        switch (u8Vector)
        {
            case X86_XCPT_BP:
            case X86_XCPT_OF:
            {
                /* These exceptions must be delivered as software exceptions. They have no error codes associated with them. */
                u32IntrInfo |= (VMX_EXIT_INTERRUPTION_INFO_TYPE_SW_XCPT << VMX_EXIT_INTERRUPTION_INFO_TYPE_SHIFT);
                break;
            }

            case X86_XCPT_DF:
            case X86_XCPT_TS:
            case X86_XCPT_NP:
            case X86_XCPT_SS:
            case X86_XCPT_GP:
            case X86_XCPT_PF:
            case X86_XCPT_AC:
                /* These exceptions must be delivered as hardware exceptions. They have error codes associated with
                   them which VT-x/VMM pushes to the guest stack. */
                u32IntrInfo |= VMX_EXIT_INTERRUPTION_INFO_ERROR_CODE_VALID;
                /* no break! */
            default:
            {
                u32IntrInfo |= (VMX_EXIT_INTERRUPTION_INFO_TYPE_HW_XCPT << VMX_EXIT_INTERRUPTION_INFO_TYPE_SHIFT);
                break;
            }
        }
    }
    else if (enmTrpmEvent == TRPM_HARDWARE_INT)
        u32IntrInfo |= (VMX_EXIT_INTERRUPTION_INFO_TYPE_EXT_INT << VMX_EXIT_INTERRUPTION_INFO_TYPE_SHIFT);
    else
        AssertMsgFailed(("Invalid TRPM event type %d\n", enmTrpmEvent));

    STAM_COUNTER_INC(&pVCpu->hm.s.StatIntInject);
    return hmR0VmxInjectEventVmcs(pVM, pVCpu, pMixedCtx, u32IntrInfo, 0 /* cbInstr */, uErrCode);
}


/**
 * Checks if there are any pending guest interrupts to be delivered and injects
 * them into the VM by updating the VMCS.
 *
 * @returns VBox status code (informational status codes included).
 * @param   pVM             Pointer to the VM.
 * @param   pVCpu           Pointer to the VMCPU.
 * @param   pMixedCtx       Pointer to the guest-CPU context. The data may be
 *                          out-of-sync. Make sure to update the required fields
 *                          before using them.
 *
 * @remarks Must be called after hmR0VmxLoadGuestIntrState().
 */
static int hmR0VmxInjectPendingInterrupt(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx)
{
    /* First inject any pending HM interrupts. */
    if (pVCpu->hm.s.Event.fPending)
    {
        int rc = hmR0VmxInjectEventVmcs(pVM, pVCpu, pMixedCtx, pVCpu->hm.s.Event.u64IntrInfo, 0 /* cbInstr */,
                                    pVCpu->hm.s.Event.u32ErrCode);
        AssertRCReturn(rc, rc);
        pVCpu->hm.s.Event.fPending = false;
        return rc;
    }

    /** @todo SMI. SMIs take priority over NMIs. */

    /* NMI. NMIs take priority over regular interrupts . */
    if (VMCPU_FF_TEST_AND_CLEAR(pVCpu, VMCPU_FF_INTERRUPT_NMI))
    {
        /* Construct an NMI interrupt and inject it into the VMCS. */
        RTGCUINTPTR uIntrInfo;
        uIntrInfo  = X86_XCPT_NMI;
        uIntrInfo |= (1 << VMX_EXIT_INTERRUPTION_INFO_VALID_SHIFT);
        uIntrInfo |= (VMX_EXIT_INTERRUPTION_INFO_TYPE_NMI << VMX_EXIT_INTERRUPTION_INFO_TYPE_SHIFT);
        int rc = hmR0VmxInjectEventVmcs(pVM, pVCpu, pMixedCtx, uIntrInfo, 0 /* cbInstr */, 0 /* u32ErrCode */);
        AssertRCReturn(rc, rc);
        return rc;
    }

    /* We need the guests's RFLAGS for sure from this point on, make sure it is updated. */
    int rc = hmR0VmxSaveGuestRflags(pVM, pVCpu, pMixedCtx);
    AssertRCReturn(rc, rc);

    /* If there isn't any active trap, check if we have pending interrupts and convert them to TRPM traps and deliver them. */
    if (!TRPMHasTrap(pVCpu))
    {
        /* Check if there are guest external interrupts (PIC/APIC) pending. */
        if (VMCPU_FF_IS_PENDING(pVCpu, (VMCPU_FF_INTERRUPT_APIC | VMCPU_FF_INTERRUPT_PIC)))
        {
            /*
             * If the guest can receive interrupts now (interrupts enabled and no interrupt inhibition is active) convert
             * the PDM interrupt into a TRPM event and inject it.
             */
            if (   (pMixedCtx->eflags.u32 & X86_EFL_IF)
                && !VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_INHIBIT_INTERRUPTS))
            {
                uint8_t u8Interrupt = 0;
                rc = PDMGetInterrupt(pVCpu, &u8Interrupt);
                if (RT_SUCCESS(rc))
                {
                    /* Convert pending interrupt from PIC/APIC into TRPM and handle it below. */
                    rc = TRPMAssertTrap(pVCpu, u8Interrupt, TRPM_HARDWARE_INT);
                    AssertRCReturn(rc, rc);
                }
                else
                {
                    /** @todo Does this actually happen? If not turn it into an assertion. */
                    Assert(!VMCPU_FF_IS_PENDING(pVCpu, (VMCPU_FF_INTERRUPT_APIC | VMCPU_FF_INTERRUPT_PIC)));
                    STAM_COUNTER_INC(&pVCpu->hm.s.StatSwitchGuestIrq);
                }
            }
            else if (!(pVCpu->hm.s.vmx.u32ProcCtls & VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_INT_WINDOW_EXIT))
            {
                /* Instruct VT-x to cause an interrupt-window exit as soon as the guest is ready to receive interrupts again. */
                pVCpu->hm.s.vmx.u32ProcCtls |= VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_INT_WINDOW_EXIT;
                rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_PROC_EXEC_CONTROLS, pVCpu->hm.s.vmx.u32ProcCtls);
                AssertRCReturn(rc, rc);
            }
            /* else we will deliver interrupts whenever the guest exits next and it's in a state to receive interrupts. */
        }
    }

    /* If interrupts can be delivered, inject it into the VM. */
    if (   (pMixedCtx->eflags.u32 & X86_EFL_IF)
        && !VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_INHIBIT_INTERRUPTS)
        && TRPMHasTrap(pVCpu))
    {
        rc = hmR0VmxInjectTRPMTrap(pVM, pVCpu, pMixedCtx);
        AssertRCReturn(rc, rc);
    }
    return rc;
}

/**
 * Injects an invalid-opcode (#UD) exception into the VM.
 *
 * @returns VBox status code (informational status code included).
 * @param   pVM             Pointer to the VM.
 * @param   pVCpu           Pointer to the VMCPU.
 * @param   pMixedCtx       Pointer to the guest-CPU context. The data may be
 *                          out-of-sync. Make sure to update the required fields
 *                          before using them.
 */
DECLINLINE(int) hmR0VmxInjectXcptUD(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx)
{
    /* Refer Intel spec. 24.8.3 "VM-entry Controls for Event Injection" for the format of u32IntrInfo. */
    uint32_t u32IntrInfo = X86_XCPT_UD | (1 << VMX_EXIT_INTERRUPTION_INFO_VALID_SHIFT);
    STAM_COUNTER_INC(&pVCpu->hm.s.StatIntInject);
    return hmR0VmxInjectEventVmcs(pVM, pVCpu, pMixedCtx, u32IntrInfo, 0 /* cbInstr */, 0 /* u32ErrCode */);
}


/**
 * Injects a double-fault (#DF) exception into the VM.
 *
 * @returns VBox status code (informational status code included).
 * @param   pVM             Pointer to the VM.
 * @param   pVCpu           Pointer to the VMCPU.
 * @param   pMixedCtx       Pointer to the guest-CPU context. The data may be
 *                          out-of-sync. Make sure to update the required fields
 *                          before using them.
 */
DECLINLINE(int) hmR0VmxInjectXcptDF(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx)
{
    /* Inject the double-fault. */
    uint32_t u32IntrInfo = X86_XCPT_DF | (1 << VMX_EXIT_INTERRUPTION_INFO_VALID_SHIFT);
    u32IntrInfo         |= (VMX_EXIT_INTERRUPTION_INFO_TYPE_HW_XCPT << VMX_EXIT_INTERRUPTION_INFO_TYPE_SHIFT);
    u32IntrInfo         |= VMX_EXIT_INTERRUPTION_INFO_ERROR_CODE_VALID;
    STAM_COUNTER_INC(&pVCpu->hm.s.StatIntInject);
    return hmR0VmxInjectEventVmcs(pVM, pVCpu, pMixedCtx, u32IntrInfo, 0 /* cbInstr */, 0 /* u32ErrCode */);
}


/**
 * Injects a debug (#DB) exception into the VM.
 *
 * @returns VBox status code (informational status code included).
 * @param   pVM             Pointer to the VM.
 * @param   pVCpu           Pointer to the VMCPU.
 * @param   pMixedCtx       Pointer to the guest-CPU context. The data may be
 *                          out-of-sync. Make sure to update the required fields
 *                          before using them.
 */
DECLINLINE(int) hmR0VmxInjectXcptDB(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx)
{
    /* Inject the debug-exception. */
    uint32_t u32IntrInfo = X86_XCPT_DB | (1 << VMX_EXIT_INTERRUPTION_INFO_VALID_SHIFT);
    u32IntrInfo         |= (VMX_EXIT_INTERRUPTION_INFO_TYPE_HW_XCPT << VMX_EXIT_INTERRUPTION_INFO_TYPE_SHIFT);
    STAM_COUNTER_INC(&pVCpu->hm.s.StatIntInject);
    return hmR0VmxInjectEventVmcs(pVM, pVCpu, pMixedCtx, u32IntrInfo, 0 /* cbInstr */, 0 /* u32ErrCode */);
}


/**
 * Injects a overflow (#OF) exception into the VM.
 *
 * @returns VBox status code (informational status code included).
 * @param   pVM             Pointer to the VM.
 * @param   pVCpu           Pointer to the VMCPU.
 * @param   pMixedCtx       Pointer to the guest-CPU context. The data may be
 *                          out-of-sync. Make sure to update the required fields
 *                          before using them.
 * @param   cbInstr         The value of RIP that is to be pushed on the guest
 *                          stack.
 */
DECLINLINE(int) hmR0VmxInjectXcptOF(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, uint32_t cbInstr)
{
    /* Inject the overflow exception. */
    uint32_t u32IntrInfo = X86_XCPT_OF | (1 << VMX_EXIT_INTERRUPTION_INFO_VALID_SHIFT);
    u32IntrInfo         |= (VMX_EXIT_INTERRUPTION_INFO_TYPE_SW_INT << VMX_EXIT_INTERRUPTION_INFO_TYPE_SHIFT);
    STAM_COUNTER_INC(&pVCpu->hm.s.StatIntInject);
    return hmR0VmxInjectEventVmcs(pVM, pVCpu, pMixedCtx, u32IntrInfo, cbInstr, 0 /* u32ErrCode */);
}


/**
 * Injects a general-protection (#GP) fault into the VM.
 *
 * @returns VBox status code (informational status code included).
 * @param   pVM             Pointer to the VM.
 * @param   pVCpu           Pointer to the VMCPU.
 * @param   pMixedCtx       Pointer to the guest-CPU context. The data may be
 *                          out-of-sync. Make sure to update the required fields
 *                          before using them.
 * @param   u32ErrorCode    The error code associated with the #GP.
 */
DECLINLINE(int) hmR0VmxInjectXcptGP(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, bool fErrorCodeValid, uint32_t u32ErrorCode)
{
    /* Inject the general-protection fault. */
    uint32_t u32IntrInfo = X86_XCPT_GP | (1 << VMX_EXIT_INTERRUPTION_INFO_VALID_SHIFT);
    u32IntrInfo         |= (VMX_EXIT_INTERRUPTION_INFO_TYPE_HW_XCPT << VMX_EXIT_INTERRUPTION_INFO_TYPE_SHIFT);
    if (fErrorCodeValid)
        u32IntrInfo |= VMX_EXIT_INTERRUPTION_INFO_ERROR_CODE_VALID;
    STAM_COUNTER_INC(&pVCpu->hm.s.StatIntInject);
    return hmR0VmxInjectEventVmcs(pVM, pVCpu, pMixedCtx, u32IntrInfo, 0 /* cbInstr */, u32ErrorCode);
}


/**
 * Injects a software interrupt (INTn) into the VM.
 *
 * @returns VBox status code (informational status code included).
 * @param   pVM             Pointer to the VM.
 * @param   pVCpu           Pointer to the VMCPU.
 * @param   pMixedCtx       Pointer to the guest-CPU context. The data may be
 *                          out-of-sync. Make sure to update the required fields
 *                          before using them.
 * @param   uVector         The software interrupt vector number.
 * @param   cbInstr         The value of RIP that is to be pushed on the guest
 *                          stack.
 */
DECLINLINE(int) hmR0VmxInjectIntN(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, uint16_t uVector, uint32_t cbInstr)
{
    /* Inject the INTn. */
    uint32_t u32IntrInfo = uVector | (1 << VMX_EXIT_INTERRUPTION_INFO_VALID_SHIFT);
    u32IntrInfo         |= (VMX_EXIT_INTERRUPTION_INFO_TYPE_SW_INT << VMX_EXIT_INTERRUPTION_INFO_TYPE_SHIFT);
    STAM_COUNTER_INC(&pVCpu->hm.s.StatIntInject);
    return hmR0VmxInjectEventVmcs(pVM, pVCpu, pMixedCtx, u32IntrInfo, cbInstr, 0 /* u32ErrCode */);
}


/**
 * Pushes a 2-byte value onto the real-mode (in virtual-8086 mode) guest's
 * stack.
 *
 * @returns VBox status code (information status code included).
 * @retval VINF_EM_RESET if pushing a value to the stack caused a triple-fault.
 * @param   pVM         Pointer to the VM.
 * @param   pMixedCtx   Pointer to the guest-CPU context.
 * @param   uValue      The value to push to the guest stack.
 */
DECLINLINE(int) hmR0VmxRealModeGuestStackPush(PVM pVM, PCPUMCTX pMixedCtx, uint16_t uValue)
{
    /*
     * The stack limit is 0xffff in real-on-virtual 8086 mode. Real-mode with weird stack limits cannot be run in
     * virtual 8086 mode in VT-x. See Intel spec. 26.3.1.2 "Checks on Guest Segment Registers".
     * See Intel Instruction reference for PUSH and Intel spec. 22.33.1 "Segment Wraparound".
     */
    if (pMixedCtx->sp == 1)
        return VINF_EM_RESET;
    pMixedCtx->sp -= sizeof(uint16_t);       /* May wrap around which is expected behaviour. */
    int rc = PGMPhysSimpleWriteGCPhys(pVM, pMixedCtx->ss.u64Base + pMixedCtx->sp, &uValue, sizeof(uint16_t));
    AssertRCReturn(rc, rc);
    return rc;
}


/**
 * Injects an event into the guest upon VM-entry by updating the relevant fields
 * in the VM-entry area in the VMCS.
 *
 * @returns VBox status code (informational error codes included).
 * @retval VINF_SUCCESS if the event is successfully injected into the VMCS.
 * @retval VINF_EM_RESET if event injection resulted in a triple-fault.
 *
 * @param   pVM             Pointer to the VM.
 * @param   pVCpu           Pointer to the VMCPU.
 * @param   pMixedCtx       Pointer to the guest-CPU context. The data may be
 *                          out-of-sync. Make sure to update the required fields
 *                          before using them.
 * @param   u64IntrInfo     The VM-entry interruption-information field.
 * @param   cbInstr         The VM-entry instruction length in bytes (for software
 *                          interrupts, exceptions and privileged software
 *                          exceptions).
 * @param   u32ErrCode      The VM-entry exception error code.
 *
 * @remarks No-long-jump zone!!!
 */
static int hmR0VmxInjectEventVmcs(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, uint64_t u64IntrInfo, uint32_t cbInstr,
                                  uint32_t u32ErrCode)
{
    /* Intel spec. 24.8.3 "VM-Entry Controls for Event Injection" specifies the interruption-information field to be 32-bits. */
    AssertMsg(u64IntrInfo >> 32 == 0, ("%#RX64\n", u64IntrInfo));
    uint32_t u32IntrInfo = (uint32_t)u64IntrInfo;

    /* We require CR0 to check if the guest is in real-mode. */
    int rc = hmR0VmxSaveGuestCR0(pVM, pVCpu, pMixedCtx);
    AssertRCReturn(rc, rc);

    const uint32_t uVector = VMX_EXIT_INTERRUPTION_INFO_VECTOR(u32IntrInfo);
    STAM_COUNTER_INC(&pVCpu->hm.s.paStatInjectedIrqsR0[uVector & MASK_INJECT_IRQ_STAT]);

    /*
     * Hardware interrupts & exceptions cannot be delivered through the software interrupt redirection bitmap to the real
     * mode task in virtual-8086 mode. We must jump to the interrupt handler in the (real-mode) guest.
     * See Intel spec. 20.3 "Interrupt and Exception handling in Virtual-8086 Mode" for interrupt & exception classes.
     * See Intel spec. 20.1.4 "Interrupt and Exception Handling" for real-mode interrupt handling.
     */
    if (CPUMIsGuestInRealModeEx(pMixedCtx))
    {
        if (!pVM->hm.s.vmx.fUnrestrictedGuest)
        {
            Assert(PDMVmmDevHeapIsEnabled(pVM));
            Assert(pVM->hm.s.vmx.pRealModeTSS);

            /* Save the required guest state bits from the VMCS. */
            rc  = hmR0VmxSaveGuestSegmentRegs(pVM, pVCpu, pMixedCtx);
            rc |= hmR0VmxSaveGuestGprs(pVM, pVCpu, pMixedCtx);
            AssertRCReturn(rc, rc);

            /* Check if the interrupt handler is present in the IVT (real-mode IDT). IDT limit is (4N - 1). */
            const size_t cbIdtEntry = 4;
            if (uVector * cbIdtEntry + (cbIdtEntry - 1) > pMixedCtx->idtr.cbIdt)
            {
                /* If we are trying to inject a #DF with no valid IDT entry, return a triple-fault. */
                if (uVector == X86_XCPT_DF)
                    return VINF_EM_RESET;
                else if (uVector == X86_XCPT_GP)
                {
                    /* If we're injecting a #GP with no valid IDT entry, inject a double-fault. */
                    return hmR0VmxInjectXcptDF(pVM, pVCpu, pMixedCtx);
                }

                /* If we're injecting an interrupt/exception with no valid IDT entry, inject a general-protection fault. */
                /* No error codes for exceptions in real-mode. See Intel spec. 20.1.4 "Interrupt and Exception Handling" */
                return hmR0VmxInjectXcptGP(pVM, pVCpu, pMixedCtx, false /* fErrCodeValid */, 0 /* u32ErrCode */);
            }

            /* Software exceptions (#BP and #OF exceptions thrown as a result of INT 3 or INTO) */
            uint16_t uGuestIp = pMixedCtx->ip;
            if (VMX_EXIT_INTERRUPTION_INFO_TYPE(u32IntrInfo) == VMX_EXIT_INTERRUPTION_INFO_TYPE_SW_XCPT)
            {
                Assert(uVector == X86_XCPT_BP || uVector == X86_XCPT_OF);
                /* #BP and #OF are both benign traps, we need to resume the next instruction. */
                uGuestIp = pMixedCtx->ip + (uint16_t)cbInstr;
            }
            else if (VMX_EXIT_INTERRUPTION_INFO_TYPE(u32IntrInfo) == VMX_EXIT_INTERRUPTION_INFO_TYPE_SW_INT)
                uGuestIp = pMixedCtx->ip + (uint16_t)cbInstr;

            /* Get the code segment selector and offset from the IDT entry for the interrupt handler. */
            uint16_t offIdtEntry    = 0;
            RTSEL    selIdtEntry    = 0;
            RTGCPHYS GCPhysIdtEntry = (RTGCPHYS)pMixedCtx->idtr.pIdt + uVector * cbIdtEntry;
            rc  = PGMPhysSimpleReadGCPhys(pVM, &offIdtEntry, GCPhysIdtEntry,     sizeof(offIdtEntry));
            rc |= PGMPhysSimpleReadGCPhys(pVM, &selIdtEntry, GCPhysIdtEntry + 2, sizeof(selIdtEntry));
            AssertRCReturn(rc, rc);

            /* Construct the stack frame for the interrupt/exception handler. */
            rc  = hmR0VmxRealModeGuestStackPush(pVM, pMixedCtx, pMixedCtx->eflags.u32);
            rc |= hmR0VmxRealModeGuestStackPush(pVM, pMixedCtx, pMixedCtx->cs.Sel);
            rc |= hmR0VmxRealModeGuestStackPush(pVM, pMixedCtx, uGuestIp);
            AssertRCReturn(rc, rc);

            /* Clear the required eflag bits and jump to the interrupt/exception handler. */
            if (rc == VINF_SUCCESS)
            {
                pMixedCtx->eflags.u32 &= ~(X86_EFL_IF | X86_EFL_TF | X86_EFL_RF | X86_EFL_AC);
                pMixedCtx->rip         = offIdtEntry;
                pMixedCtx->cs.Sel      = selIdtEntry;
                pMixedCtx->cs.u64Base  = selIdtEntry << cbIdtEntry;
                pVCpu->hm.s.fContextUseFlags |=   HM_CHANGED_GUEST_SEGMENT_REGS
                                                | HM_CHANGED_GUEST_RIP
                                                | HM_CHANGED_GUEST_RFLAGS
                                                | HM_CHANGED_GUEST_RSP;
            }
            Assert(rc == VINF_SUCCESS || rc == VINF_EM_RESET);
            return rc;
        }
        else
        {
            /*
             * For unrestricted execution enabled CPUs running real-mode guests, we must not set the deliver-error-code bit.
             * See Intel spec. 26.2.1.3 "VM-Entry Control Fields".
             */
            u32IntrInfo &= ~VMX_EXIT_INTERRUPTION_INFO_ERROR_CODE_VALID;
        }
    }

    /* Add the valid bit, maybe the caller was lazy. */
    u32IntrInfo |= (1 << VMX_EXIT_INTERRUPTION_INFO_VALID_SHIFT);

    Assert(!VMX_EXIT_INTERRUPTION_INFO_NMI_UNBLOCK(u32IntrInfo));       /* Bit 12 MBZ. */
    Assert(!(u32IntrInfo & 0x7ffff000));                                /* Bits 30:12 MBZ. */
    Log(("Injecting u32IntrInfo=%#x u32ErrCode=%#x instrlen=%#x\n", u32IntrInfo, u32ErrCode, cbInstr));

    rc  = VMXWriteVmcs32(VMX_VMCS32_CTRL_ENTRY_INTERRUPTION_INFO, u32IntrInfo);
    if (VMX_EXIT_INTERRUPTION_INFO_ERROR_CODE_IS_VALID(u32IntrInfo))
        rc |= VMXWriteVmcs32(VMX_VMCS32_CTRL_ENTRY_EXCEPTION_ERRCODE, u32ErrCode);
    rc |= VMXWriteVmcs32(VMX_VMCS32_CTRL_ENTRY_INSTR_LENGTH, cbInstr);
    AssertRCReturn(rc, rc);
    return rc;
}


/**
 * Enters the VT-x session.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pCpu        Pointer to the CPU info struct.
 */
VMMR0DECL(int) VMXR0Enter(PVM pVM, PVMCPU pVCpu, PHMGLOBLCPUINFO pCpu)
{
    AssertPtr(pVM);
    AssertPtr(pVCpu);
    Assert(pVM->hm.s.vmx.fSupported);
    Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));
    NOREF(pCpu);

    LogFlowFunc(("pVM=%p pVCpu=%p\n", pVM, pVCpu));

    /* Make sure we're in VMX root mode. */
    RTCCUINTREG u32HostCR4 = ASMGetCR4();
    if (!(u32HostCR4 & X86_CR4_VMXE))
    {
        LogRel(("VMXR0Enter: X86_CR4_VMXE bit in CR4 is not set!\n"));
        return VERR_VMX_X86_CR4_VMXE_CLEARED;
    }

    /* Load the active VMCS as the current one. */
    int rc = VMXActivateVMCS(pVCpu->hm.s.vmx.HCPhysVmcs);
    if (RT_FAILURE(rc))
        return rc;

    /** @todo this will change with preemption hooks where can can VMRESUME as long
     *        as we're no preempted. */
    pVCpu->hm.s.fResumeVM = false;
    return VINF_SUCCESS;
}


/**
 * Leaves the VT-x session.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pCtx        Pointer to the guest-CPU context.
 */
VMMR0DECL(int) VMXR0Leave(PVM pVM, PVMCPU pVCpu, PCPUMCTX pCtx)
{
    AssertPtr(pVCpu);
    Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));
    NOREF(pVM);
    NOREF(pCtx);

    /** @todo this will change with preemption hooks where we only VMCLEAR when
     *        we are actually going to be preempted, not all the time like we
     *        currently do. */
    /*
     * Sync the current VMCS (writes back internal data back into the VMCS region in memory)
     * and mark the VMCS launch-state as "clear".
     */
    int rc = VMXClearVMCS(pVCpu->hm.s.vmx.HCPhysVmcs);
    return rc;
}


/**
 * Saves the host state in the VMCS host-state.
 * Sets up the VM-exit MSR-load area.
 *
 * The CPU state will be loaded from these fields on every successful VM-exit.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 *
 * @remarks No-long-jump zone!!!
 */
VMMR0DECL(int) VMXR0SaveHostState(PVM pVM, PVMCPU pVCpu)
{
    AssertPtr(pVM);
    AssertPtr(pVCpu);
    Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));

    LogFlowFunc(("pVM=%p pVCpu=%p\n", pVM, pVCpu));

    /* Nothing to do if the host-state-changed flag isn't set. This will later be optimized when preemption hooks are in place. */
    if (!(pVCpu->hm.s.fContextUseFlags & HM_CHANGED_HOST_CONTEXT))
        return VINF_SUCCESS;

    int rc = hmR0VmxSaveHostControlRegs(pVM, pVCpu);
    AssertLogRelMsgRCReturn(rc, ("hmR0VmxSaveHostControlRegisters failed! rc=%Rrc (pVM=%p pVCpu=%p)\n", rc, pVM, pVCpu), rc);

    rc = hmR0VmxSaveHostSegmentRegs(pVM, pVCpu);
    AssertLogRelMsgRCReturn(rc, ("hmR0VmxSaveHostSegmentRegisters failed! rc=%Rrc (pVM=%p pVCpu=%p)\n", rc, pVM, pVCpu), rc);

    rc = hmR0VmxSaveHostMsrs(pVM, pVCpu);
    AssertLogRelMsgRCReturn(rc, ("hmR0VmxSaveHostMsrs failed! rc=%Rrc (pVM=%p pVCpu=%p)\n", rc, pVM, pVCpu), rc);

    /* Reset the host-state-changed flag. */
    pVCpu->hm.s.fContextUseFlags &= ~HM_CHANGED_HOST_CONTEXT;
    return rc;
}


/**
 * Loads the guest state into the VMCS guest-state area. The CPU state will be
 * loaded from these fields on every successful VM-entry.
 *
 * Sets up the VM-entry MSR-load and VM-exit MSR-store areas.
 * Sets up the VM-entry controls.
 * Sets up the appropriate VMX non-root function to execute guest code based on
 * the guest CPU mode.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pCtx        Pointer to the guest-CPU context.
 *
 * @remarks No-long-jump zone!!!
 */
VMMR0DECL(int) VMXR0LoadGuestState(PVM pVM, PVMCPU pVCpu, PCPUMCTX pCtx)
{
    AssertPtr(pVM);
    AssertPtr(pVCpu);
    AssertPtr(pCtx);
    Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));

    LogFlowFunc(("pVM=%p pVCpu=%p\n", pVM, pVCpu));

    STAM_PROFILE_ADV_START(&pVCpu->hm.s.StatLoadGuestState, x);

    /* Determine real-on-v86 mode. */
    pVCpu->hm.s.vmx.RealMode.fRealOnV86Active = false;
    if (   !pVM->hm.s.vmx.fUnrestrictedGuest
        && CPUMIsGuestInRealModeEx(pCtx))
    {
        pVCpu->hm.s.vmx.RealMode.fRealOnV86Active = true;
    }

    int rc = hmR0VmxLoadGuestEntryCtls(pVM, pVCpu, pCtx);
    AssertLogRelMsgRCReturn(rc, ("hmR0VmxLoadGuestEntryCtls! rc=%Rrc (pVM=%p pVCpu=%p)\n", rc, pVM, pVCpu), rc);

    rc = hmR0VmxLoadGuestExitCtls(pVM, pVCpu);
    AssertLogRelMsgRCReturn(rc, ("hmR0VmxSetupExitCtls failed! rc=%Rrc (pVM=%p pVCpu=%p)\n", rc, pVM, pVCpu), rc);

    rc = hmR0VmxLoadGuestActivityState(pVM, pVCpu, pCtx);
    AssertLogRelMsgRCReturn(rc, ("hmR0VmxLoadGuestActivityState! rc=%Rrc (pVM=%p pVCpu=%p)\n", rc, pVM, pVCpu), rc);

    rc = hmR0VmxLoadGuestControlRegs(pVM, pVCpu, pCtx);
    AssertLogRelMsgRCReturn(rc, ("hmR0VmxLoadGuestControlRegs: rc=%Rrc (pVM=%p pVCpu=%p)\n", rc, pVM, pVCpu), rc);

    rc = hmR0VmxLoadGuestSegmentRegs(pVM, pVCpu, pCtx);
    AssertLogRelMsgRCReturn(rc, ("hmR0VmxLoadGuestSegmentRegs: rc=%Rrc (pVM=%p pVCpu=%p)\n", rc, pVM, pVCpu), rc);

    rc = hmR0VmxLoadGuestDebugRegs(pVM, pVCpu, pCtx);
    AssertLogRelMsgRCReturn(rc, ("hmR0VmxLoadGuestDebugRegs: rc=%Rrc (pVM=%p pVCpu=%p)\n", rc, pVM, pVCpu), rc);

    rc = hmR0VmxLoadGuestMsrs(pVM, pVCpu, pCtx);
    AssertLogRelMsgRCReturn(rc, ("hmR0VmxLoadGuestMsrs! rc=%Rrc (pVM=%p pVCpu=%p)\n", rc, pVM, pVCpu), rc);

    rc = hmR0VmxLoadGuestApicState(pVM, pVCpu, pCtx);
    AssertLogRelMsgRCReturn(rc, ("hmR0VmxLoadGuestApicState! rc=%Rrc (pVM=%p pVCpu=%p)\n", rc, pVM, pVCpu), rc);

    rc = hmR0VmxLoadGuestGprs(pVM, pVCpu, pCtx);
    AssertLogRelMsgRCReturn(rc, ("hmR0VmxLoadGuestGprs! rc=%Rrc (pVM=%p pVCpu=%p)\n", rc, pVM, pVCpu), rc);

    rc = hmR0VmxSetupVMRunHandler(pVM, pVCpu, pCtx);
    AssertLogRelMsgRCReturn(rc, ("hmR0VmxSetupVMRunHandler! rc=%Rrc (pVM=%p pVCpu=%p)\n", rc, pVM, pVCpu), rc);

    AssertMsg(!pVCpu->hm.s.fContextUseFlags,
             ("Missed updating flags while loading guest state. pVM=%p pVCpu=%p fContextUseFlags=%#RX32\n",
              pVM, pVCpu, pVCpu->hm.s.fContextUseFlags));

    STAM_PROFILE_ADV_STOP(&pVCpu->hm.s.StatLoadGuestState, x);
    return rc;
}


/**
 * Does the preparations before executing guest code in VT-x.
 *
 * This may cause longjmps to ring-3 and may even result in rescheduling to the
 * recompiler. We must be cautious what we do here regarding committing
 * guest-state information into the the VMCS assuming we assuredly execute the
 * guest in VT-x. If we fall back to the recompiler after updating VMCS and
 * clearing the common-state (TRPM/forceflags), we must undo those changes so
 * that the recompiler can (and should) use them when it resumes guest
 * execution. Otherwise such operations must be done when we can no longer
 * exit to ring-3.
 *
 * @returns VBox status code (informational status codes included).
 * @retval VINF_SUCCESS if we can proceed with running the guest.
 * @retval VINF_EM_RESET if a triple-fault occurs while injecting a double-fault
 *         into the guest.
 * @retval VINF_* scheduling changes, we have to go back to ring-3.
 *
 * @param   pVM             Pointer to the VM.
 * @param   pVCpu           Pointer to the VMCPU.
 * @param   pMixedCtx       Pointer to the guest-CPU context. The data may be
 *                          out-of-sync. Make sure to update the required fields
 *                          before using them.
 * @param   pVmxTransient   Pointer to the VMX transient structure.
 *
 * @remarks Called with preemption disabled.
 */
DECLINLINE(int) hmR0VmxPreRunGuest(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    Assert(VMMRZCallRing3IsEnabled(pVCpu));

#ifdef VBOX_WITH_2X_4GB_ADDR_SPACE_IN_R0
    PGMRZDynMapFlushAutoSet(pVCpu);
#endif

    /* Check force flag actions that might require us to go back to ring-3. */
    int rc = hmR0VmxCheckForceFlags(pVM, pVCpu, pMixedCtx);
    if (rc != VINF_SUCCESS)
        return rc;

    /* Setup the Virtualized APIC accesses. pMixedCtx->msrApicBase is always up-to-date. It's not part of the VMCS. */
    if (   pVCpu->hm.s.vmx.u64MsrApicBase != pMixedCtx->msrApicBase
        && (pVCpu->hm.s.vmx.u32ProcCtls2 & VMX_VMCS_CTRL_PROC_EXEC2_VIRT_APIC))
    {
        Assert(pVM->hm.s.vmx.HCPhysApicAccess);
        RTGCPHYS GCPhysApicBase;
        GCPhysApicBase  = pMixedCtx->msrApicBase;
        GCPhysApicBase &= PAGE_BASE_GC_MASK;

        /* Unalias any existing mapping. */
        rc = PGMHandlerPhysicalReset(pVM, GCPhysApicBase);
        AssertRCReturn(rc, rc);

        /* Map the HC APIC-access page into the GC space, this also updates the shadow page tables if necessary. */
        Log2(("Mapped HC APIC-access page into GC: GCPhysApicBase=%#RGv\n", GCPhysApicBase));
        rc = IOMMMIOMapMMIOHCPage(pVM, pVCpu, GCPhysApicBase, pVM->hm.s.vmx.HCPhysApicAccess, X86_PTE_RW | X86_PTE_P);
        AssertRCReturn(rc, rc);

        pVCpu->hm.s.vmx.u64MsrApicBase = pMixedCtx->msrApicBase;
    }

#ifdef VBOX_WITH_VMMR0_DISABLE_PREEMPTION
    /* We disable interrupts so that we don't miss any interrupts that would flag preemption (IPI/timers etc.) */
    pVmxTransient->uEFlags = ASMIntDisableFlags();
    if (RTThreadPreemptIsPending(NIL_RTTHREAD))
    {
        ASMSetFlags(pVmxTransient->uEFlags);
        STAM_COUNTER_INC(&pVCpu->hm.s.StatPendingHostIrq);
        /* Don't use VINF_EM_RAW_INTERRUPT_HYPER as we can't assume the host does kernel preemption. Maybe some day? */
        return VINF_EM_RAW_INTERRUPT;
    }
    VMCPU_SET_STATE(pVCpu, VMCPUSTATE_STARTED_EXEC);
#endif

    /*
     * This clears force-flags, TRPM traps & pending HM events. We cannot safely restore the state if we exit to ring-3
     * (before running guest code) after calling this function (e.g. how do we reverse the effects of calling PDMGetInterrupt()?)
     * This is why this is done after all possible exits-to-ring-3 paths in this code.
     */
    hmR0VmxLoadGuestIntrState(pVM, pVCpu, pMixedCtx);
    rc = hmR0VmxInjectPendingInterrupt(pVM, pVCpu, pMixedCtx);
    AssertRCReturn(rc, rc);
    return rc;
}


/**
 * Prepares to run guest code in VT-x and we've committed to doing so. This
 * means there is no backing out to ring-3 or anywhere else at this
 * point.
 *
 * @param   pVM             Pointer to the VM.
 * @param   pVCpu           Pointer to the VMCPU.
 * @param   pMixedCtx       Pointer to the guest-CPU context. The data may be
 *                          out-of-sync. Make sure to update the required fields
 *                          before using them.
 * @param   pVmxTransient   Pointer to the VMX transient structure.
 *
 * @remarks Called with preemption disabled.
 * @remarks No-long-jump zone!!!
 */
DECLINLINE(void) hmR0VmxPreRunGuestCommitted(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    Assert(!VMMRZCallRing3IsEnabled(pVCpu));
    Assert(VMMR0IsLogFlushDisabled(pVCpu));

#ifndef VBOX_WITH_VMMR0_DISABLE_PREEMPTION
    /** @todo I don't see the point of this, VMMR0EntryFast() already disables interrupts for the entire period. */
    pVmxTransient->uEFlags = ASMIntDisableFlags();
    VMCPU_SET_STATE(pVCpu, VMCPUSTATE_STARTED_EXEC);
#endif

    /* Load the required guest state bits (for guest-state changes in the inner execution loop). */
    Assert(!(pVCpu->hm.s.fContextUseFlags & HM_CHANGED_HOST_CONTEXT));
    Log(("LoadFlags=%#RX32\n", pVCpu->hm.s.fContextUseFlags));
    int rc = VINF_SUCCESS;
    if (pVCpu->hm.s.fContextUseFlags == HM_CHANGED_GUEST_RIP)
    {
        rc = hmR0VmxLoadGuestRip(pVM, pVCpu, pMixedCtx);
        STAM_COUNTER_INC(&pVCpu->hm.s.StatLoadMinimal);
    }
    else if (pVCpu->hm.s.fContextUseFlags)
    {
        rc = VMXR0LoadGuestState(pVM, pVCpu, pMixedCtx);
        STAM_COUNTER_INC(&pVCpu->hm.s.StatLoadFull);
    }
    AssertRC(rc);
    AssertMsg(!pVCpu->hm.s.fContextUseFlags, ("fContextUseFlags =%#x\n", pVCpu->hm.s.fContextUseFlags));

    /* Cache the TPR-shadow for checking on every VM-exit if it might have changed. */
    if (pVCpu->hm.s.vmx.u32ProcCtls & VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_USE_TPR_SHADOW)
        pVmxTransient->u8GuestTpr = pVCpu->hm.s.vmx.pbVirtApic[0x80];

    Assert(pVM->hm.s.vmx.pfnFlushTaggedTlb);
    ASMAtomicWriteBool(&pVCpu->hm.s.fCheckedTLBFlush, true);    /* Used for TLB-shootdowns, set this across the world switch. */
    pVM->hm.s.vmx.pfnFlushTaggedTlb(pVM, pVCpu);                /* Flush the TLB of guest entries as necessary. */

    /* Setup TSC-offsetting or intercept RDTSC(P)s and update the preemption timer. */
    if (pVmxTransient->fUpdateTscOffsettingAndPreemptTimer)
    {
        hmR0VmxUpdateTscOffsettingAndPreemptTimer(pVM, pVCpu, pMixedCtx);
        pVmxTransient->fUpdateTscOffsettingAndPreemptTimer = false;
    }

    /*
     * TPR patching (only active for 32-bit guests on 64-bit capable CPUs) when the CPU does not supported virtualizing
     * APIC accesses feature (VMX_VMCS_CTRL_PROC_EXEC2_VIRT_APIC).
     */
    if (pVM->hm.s.fTPRPatchingActive)
    {
        Assert(!CPUMIsGuestInLongMode(pVCpu));

        /* Need guest's LSTAR MSR (which is part of the auto load/store MSRs in the VMCS), ensure we have the updated one. */
        rc = hmR0VmxSaveGuestAutoLoadStoreMsrs(pVM, pVCpu, pMixedCtx);
        AssertRC(rc);

        /* The patch code uses the LSTAR as it's not used by a guest in 32-bit mode implicitly (i.e. SYSCALL is 64-bit only). */
        pVmxTransient->u64LStarMsr = ASMRdMsr(MSR_K8_LSTAR);
        ASMWrMsr(MSR_K8_LSTAR, pMixedCtx->msrLSTAR);            /* pMixedCtx->msrLSTAR contains the guest's TPR,
                                                                    see hmR0VmxLoadGuestApicState(). */
    }

    STAM_PROFILE_ADV_STOP_START(&pVCpu->hm.s.StatEntry, &pVCpu->hm.s.StatInGC, x);
    TMNotifyStartOfExecution(pVCpu);                            /* Finally, notify TM to resume its clocks as we're about
                                                                    to start executing. */
}


/**
 * Performs some essential restoration of state after running guest code in
 * VT-x.
 *
 * @param   pVM             Pointer to the VM.
 * @param   pVCpu           Pointer to the VMCPU.
 * @param   pMixedCtx       Pointer to the guest-CPU context. The data maybe
 *                          out-of-sync. Make sure to update the required fields
 *                          before using them.
 * @param   pVmxTransient   Pointer to the VMX transient structure.
 * @param   rcVMRun         Return code of VMLAUNCH/VMRESUME.
 *
 * @remarks Called with interrupts disabled.
 * @remarks No-long-jump zone!!! This function will however re-enable longjmps
 *          unconditionally when it is safe to do so.
 */
DECLINLINE(void) hmR0VmxPostRunGuest(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient, int rcVMRun)
{
    Assert(!VMMRZCallRing3IsEnabled(pVCpu));
    STAM_PROFILE_ADV_STOP_START(&pVCpu->hm.s.StatInGC, &pVCpu->hm.s.StatExit1, x);

    ASMAtomicWriteBool(&pVCpu->hm.s.fCheckedTLBFlush, false);   /* See HMInvalidatePageOnAllVCpus(): used for TLB-shootdowns. */
    ASMAtomicIncU32(&pVCpu->hm.s.cWorldSwitchExits);            /* Initialized in vmR3CreateUVM(): used for TLB-shootdowns. */
    pVCpu->hm.s.vmx.fUpdatedGuestState = 0;                     /* Exits/longjmps to ring-3 requires saving the guest state. */
    pVmxTransient->fVmcsFieldsRead     = 0;                     /* Transient fields need to be read from the VMCS. */
    pVmxTransient->fVectoringPF        = false;                 /* Vectoring page-fault needs to be determined later. */

    if (!(pVCpu->hm.s.vmx.u32ProcCtls & VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_RDTSC_EXIT))
    {
        /** @todo Find a way to fix hardcoding a guestimate.  */
        TMCpuTickSetLastSeen(pVCpu, ASMReadTSC()
                             + pVCpu->hm.s.vmx.u64TSCOffset - 0x400 /* guestimate of world switch overhead in clock ticks */);
    }

    TMNotifyEndOfExecution(pVCpu);                              /* Notify TM that the guest is no longer running. */
    Assert(!(ASMGetFlags() & X86_EFL_IF));
    VMCPU_SET_STATE(pVCpu, VMCPUSTATE_STARTED);

    /* Restore the effects of TPR patching if any. */
    if (pVM->hm.s.fTPRPatchingActive)
    {
        int rc = hmR0VmxSaveGuestAutoLoadStoreMsrs(pVM, pVCpu, pMixedCtx);
        AssertRC(rc);
        pMixedCtx->msrLSTAR = ASMRdMsr(MSR_K8_LSTAR);           /* MSR_K8_LSTAR contains the guest TPR. */
        ASMWrMsr(MSR_K8_LSTAR, pVmxTransient->u64LStarMsr);
    }

    ASMSetFlags(pVmxTransient->uEFlags);                        /* Enable interrupts. */
    pVCpu->hm.s.fResumeVM = true;                               /* Use VMRESUME instead of VMLAUNCH in the next run. */

    /* Save the basic VM-exit reason. Refer Intel spec. 24.9.1 "Basic VM-exit Information". */
    uint32_t uExitReason;
    int rc  = VMXReadVmcs32(VMX_VMCS32_RO_EXIT_REASON, &uExitReason);
    rc     |= hmR0VmxReadEntryIntrInfoVmcs(pVmxTransient);
    AssertRC(rc);
    pVmxTransient->uExitReason    = (uint16_t)VMX_EXIT_REASON_BASIC(uExitReason);
    pVmxTransient->fVMEntryFailed = VMX_ENTRY_INTERRUPTION_INFO_VALID(pVmxTransient->uEntryIntrInfo);

    VMMRZCallRing3SetNotification(pVCpu, hmR0VmxCallRing3Callback, pMixedCtx);
    VMMRZCallRing3Enable(pVCpu);                                /* It is now safe to do longjmps to ring-3!!! */

    /* If the VMLAUNCH/VMRESUME failed, we can bail out early. This does -not- cover VMX_EXIT_ERR_*. */
    if (RT_UNLIKELY(rcVMRun != VINF_SUCCESS))
    {
        Log(("VM-entry failure: rcVMRun=%Rrc fVMEntryFailed=%RTbool\n", rcVMRun, pVmxTransient->fVMEntryFailed));
        return;
    }

    if (RT_LIKELY(!pVmxTransient->fVMEntryFailed))
    {
        /* Update the guest interruptibility-state from the VMCS. */
        hmR0VmxSaveGuestIntrState(pVM, pVCpu, pMixedCtx);

        /*
         * If the TPR was raised by the guest, it wouldn't cause a VM-exit immediately. Instead we sync the TPR lazily whenever
         * we eventually get a VM-exit for any reason. This maybe expensive as PDMApicSetTPR() can longjmp to ring-3; also why
         * we do it outside of hmR0VmxSaveGuestState() which must never cause longjmps.
         */
        if (   (pVCpu->hm.s.vmx.u32ProcCtls & VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_USE_TPR_SHADOW)
            && pVmxTransient->u8GuestTpr != pVCpu->hm.s.vmx.pbVirtApic[0x80])
        {
            rc = PDMApicSetTPR(pVCpu, pVCpu->hm.s.vmx.pbVirtApic[0x80]);
            AssertRC(rc);
            pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_VMX_GUEST_APIC_STATE;
        }
    }
}


/**
 * Runs the guest code using VT-x.
 *
 * @returns VBox status code.
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the VMCPU.
 * @param   pCtx        Pointer to the guest-CPU context.
 *
 * @remarks Called with preemption disabled.
 */
VMMR0DECL(int) VMXR0RunGuestCode(PVM pVM, PVMCPU pVCpu, PCPUMCTX pCtx)
{
    Assert(VMMRZCallRing3IsEnabled(pVCpu));
    Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));

    VMXTRANSIENT VmxTransient;
    VmxTransient.fUpdateTscOffsettingAndPreemptTimer = true;
    int          rc     = VERR_INTERNAL_ERROR_5;
    unsigned     cLoops = 0;

    for (;; cLoops++)
    {
        Assert(!HMR0SuspendPending());
        AssertMsg(pVCpu->hm.s.idEnteredCpu == RTMpCpuId(),
                  ("Illegal migration! Entered on CPU %u Current %u cLoops=%u\n", (unsigned)pVCpu->hm.s.idEnteredCpu,
                  (unsigned)RTMpCpuId(), cLoops));

        /* Preparatory work for running guest code, this may return to ring-3 for some last minute updates. */
        STAM_PROFILE_ADV_START(&pVCpu->hm.s.StatEntry, x);
        rc = hmR0VmxPreRunGuest(pVM, pVCpu, pCtx, &VmxTransient);
        if (rc != VINF_SUCCESS)
            break;

        /*
         * No longjmps to ring-3 from this point on!!!
         * Asserts() will still longjmp to ring-3 (but won't return), which is intentional, better than a kernel panic.
         * This also disables flushing of the R0-logger instance (if any).
         */
        VMMRZCallRing3Disable(pVCpu);
        VMMRZCallRing3RemoveNotification(pVCpu);
        hmR0VmxPreRunGuestCommitted(pVM, pVCpu, pCtx, &VmxTransient);

        rc = hmR0VmxRunGuest(pVM, pVCpu, pCtx);
        /* The guest-CPU context is now outdated, 'pCtx' is to be treated as 'pMixedCtx' from this point on!!! */

        /*
         * Restore any residual host-state and save any bits shared between host and guest into the guest-CPU state.
         * This will also re-enable longjmps to ring-3 when it has reached a safe point!!!
         */
        hmR0VmxPostRunGuest(pVM, pVCpu, pCtx, &VmxTransient, rc);
        if (RT_UNLIKELY(rc != VINF_SUCCESS))        /* Check for errors with running the VM (VMLAUNCH/VMRESUME). */
        {
            STAM_PROFILE_ADV_STOP(&pVCpu->hm.s.StatExit1, x);
            hmR0VmxReportWorldSwitchError(pVM, pVCpu, rc, pCtx, &VmxTransient);
            return rc;
        }

        /* Handle the VM-exit. */
        STAM_COUNTER_INC(&pVCpu->hm.s.paStatExitReasonR0[VmxTransient.uExitReason & MASK_EXITREASON_STAT]);
        STAM_PROFILE_ADV_STOP_START(&pVCpu->hm.s.StatExit1, &pVCpu->hm.s.StatExit2, x);
        AssertMsg(VmxTransient.uExitReason <= VMX_EXIT_MAX, ("%#x\n", VmxTransient.uExitReason));
        rc = (*s_apfnVMExitHandlers[VmxTransient.uExitReason])(pVM, pVCpu, pCtx, &VmxTransient);
        STAM_PROFILE_ADV_STOP(&pVCpu->hm.s.StatExit2, x);
        if (rc != VINF_SUCCESS)
            break;
        else if (cLoops > pVM->hm.s.cMaxResumeLoops)
        {
            STAM_COUNTER_INC(&pVCpu->hm.s.StatExitMaxResume);
            rc = VINF_EM_RAW_INTERRUPT;
            break;
        }
    }

    STAM_PROFILE_ADV_STOP(&pVCpu->hm.s.StatEntry, x);
    if (rc == VERR_EM_INTERPRETER)
        rc = VINF_EM_RAW_EMULATE_INSTR;
    hmR0VmxExitToRing3(pVM, pVCpu, pCtx, rc);
    return rc;
}

#if 0
DECLINLINE(int) hmR0VmxHandleExit(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient, unsigned rcReason)
{
    int rc;
    switch (rcReason)
    {
        case VMX_EXIT_EPT_MISCONFIG:           rc = hmR0VmxExitEptMisconfig(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_EPT_VIOLATION:           rc = hmR0VmxExitEptViolation(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_IO_INSTR:                rc = hmR0VmxExitIoInstr(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_CPUID:                   rc = hmR0VmxExitCpuid(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_RDTSC:                   rc = hmR0VmxExitRdtsc(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_RDTSCP:                  rc = hmR0VmxExitRdtscp(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_APIC_ACCESS:             rc = hmR0VmxExitApicAccess(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_XCPT_NMI:                rc = hmR0VmxExitXcptNmi(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_MOV_CRX:                 rc = hmR0VmxExitMovCRx(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_EXT_INT:                 rc = hmR0VmxExitExtInt(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_INT_WINDOW:              rc = hmR0VmxExitIntWindow(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_MWAIT:                   rc = hmR0VmxExitMwait(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_MONITOR:                 rc = hmR0VmxExitMonitor(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_TASK_SWITCH:             rc = hmR0VmxExitTaskSwitch(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_PREEMPTION_TIMER:        rc = hmR0VmxExitPreemptionTimer(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_RDMSR:                   rc = hmR0VmxExitRdmsr(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_WRMSR:                   rc = hmR0VmxExitWrmsr(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_MOV_DRX:                 rc = hmR0VmxExitMovDRx(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_TPR_BELOW_THRESHOLD:     rc = hmR0VmxExitTprBelowThreshold(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_HLT:                     rc = hmR0VmxExitHlt(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_INVD:                    rc = hmR0VmxExitInvd(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_INVLPG:                  rc = hmR0VmxExitInvlpg(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_RSM:                     rc = hmR0VmxExitRsm(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_MTF:                     rc = hmR0VmxExitMtf(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_PAUSE:                   rc = hmR0VmxExitPause(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_XDTR_ACCESS:             rc = hmR0VmxExitXdtrAccess(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_TR_ACCESS:               rc = hmR0VmxExitXdtrAccess(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_WBINVD:                  rc = hmR0VmxExitWbinvd(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_XSETBV:                  rc = hmR0VmxExitXsetbv(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_RDRAND:                  rc = hmR0VmxExitRdrand(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_INVPCID:                 rc = hmR0VmxExitInvpcid(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_GETSEC:                  rc = hmR0VmxExitGetsec(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_RDPMC:                   rc = hmR0VmxExitRdpmc(pVM, pVCpu, pMixedCtx, pVmxTransient); break;

        case VMX_EXIT_TRIPLE_FAULT:            rc = hmR0VmxExitTripleFault(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_NMI_WINDOW:              rc = hmR0VmxExitNmiWindow(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_INIT_SIGNAL:             rc = hmR0VmxExitInitSignal(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_SIPI:                    rc = hmR0VmxExitSipi(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_IO_SMI:                  rc = hmR0VmxExitIoSmi(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_SMI:                     rc = hmR0VmxExitSmi(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_ERR_MSR_LOAD:            rc = hmR0VmxExitErrMsrLoad(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_ERR_INVALID_GUEST_STATE: rc = hmR0VmxExitErrInvalidGuestState(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
        case VMX_EXIT_ERR_MACHINE_CHECK:       rc = hmR0VmxExitErrMachineCheck(pVM, pVCpu, pMixedCtx, pVmxTransient); break;

        case VMX_EXIT_VMCALL:
        case VMX_EXIT_VMCLEAR:
        case VMX_EXIT_VMLAUNCH:
        case VMX_EXIT_VMPTRLD:
        case VMX_EXIT_VMPTRST:
        case VMX_EXIT_VMREAD:
        case VMX_EXIT_VMRESUME:
        case VMX_EXIT_VMWRITE:
        case VMX_EXIT_VMXOFF:
        case VMX_EXIT_VMXON:
        case VMX_EXIT_INVEPT:
        case VMX_EXIT_INVVPID:
        case VMX_EXIT_VMFUNC:
            rc = hmR0VmxExitInjectXcptUD(pVM, pVCpu, pMixedCtx, pVmxTransient);
            break;
        default:
            rc = hmR0VmxExitErrUndefined(pVM, pVCpu, pMixedCtx, pVmxTransient);
            break;
    }
    return rc;
}
#endif

#ifdef DEBUG
/* Is there some generic IPRT define for this that are not in Runtime/internal/\* ?? */
# define VMX_ASSERT_PREEMPT_CPUID_VAR() \
    RTCPUID const idAssertCpu = RTThreadPreemptIsEnabled(NIL_RTTHREAD) ? NIL_RTCPUID : RTMpCpuId()
# define VMX_ASSERT_PREEMPT_CPUID() \
   do \
   { \
        RTCPUID const idAssertCpuNow = RTThreadPreemptIsEnabled(NIL_RTTHREAD) ? NIL_RTCPUID : RTMpCpuId(); \
        AssertMsg(idAssertCpu == idAssertCpuNow,  ("VMX %#x, %#x\n", idAssertCpu, idAssertCpuNow)); \
   } while (0)

# define VMX_VALIDATE_EXIT_HANDLER_PARAMS() \
            do {                                                      \
                AssertPtr(pVM);                                       \
                AssertPtr(pVCpu);                                     \
                AssertPtr(pMixedCtx);                                 \
                AssertPtr(pVmxTransient);                             \
                Assert(pVmxTransient->fVMEntryFailed == false);       \
                Assert(ASMIntAreEnabled() == true);                   \
                Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));      \
                VMX_ASSERT_PREEMPT_CPUID_VAR();                       \
                LogFunc(("\n"));                                      \
                Assert(!RTThreadPreemptIsEnabled(NIL_RTTHREAD));      \
                if (VMMR0IsLogFlushDisabled(pVCpu))                   \
                    VMX_ASSERT_PREEMPT_CPUID();                       \
            } while (0)
# define VMX_VALIDATE_EXIT_XCPT_HANDLER_PARAMS() \
            do { \
                LogFunc(("\n"));                                      \
            } while(0)
#else   /* Release builds */
# define VMX_VALIDATE_EXIT_HANDLER_PARAMS() do { } while(0)
# define VMX_VALIDATE_EXIT_XCPT_HANDLER_PARAMS() do { } while(0)
#endif


/* -=-=-=-=-=-=-=-=--=-=-=-=-=-=-=-=-=-=-=--=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-= */
/* -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=- VM-exit handlers -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=- */
/* -=-=-=-=-=-=-=-=--=-=-=-=-=-=-=-=-=-=-=--=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-= */
/**
 * VM-exit handler for external interrupts (VMX_EXIT_EXT_INT).
 */
static DECLCALLBACK(int) hmR0VmxExitExtInt(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitExtInt);
    return VINF_SUCCESS;
}


/**
 * VM-exit handler for exceptions and NMIs (VMX_EXIT_XCPT_NMI).
 */
static DECLCALLBACK(int) hmR0VmxExitXcptNmi(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();
    int rc = hmR0VmxReadExitIntrInfoVmcs(pVmxTransient);
    AssertRCReturn(rc, rc);

    uint32_t uIntrType = VMX_EXIT_INTERRUPTION_INFO_TYPE(pVmxTransient->uExitIntrInfo);
    Assert(   !(pVCpu->hm.s.vmx.u32ExitCtls & VMX_VMCS_CTRL_EXIT_CONTROLS_ACK_EXT_INT)
           && uIntrType != VMX_EXIT_INTERRUPTION_INFO_TYPE_EXT_INT);

    if (uIntrType == VMX_EXIT_INTERRUPTION_INFO_TYPE_NMI)
        return VINF_EM_RAW_INTERRUPT;

    /* If this VM-exit occurred while delivering an event through the guest IDT, handle it accordingly. */
    rc = hmR0VmxCheckExitDueToEventDelivery(pVM, pVCpu, pMixedCtx, pVmxTransient);
    if (RT_UNLIKELY(rc == VINF_VMX_DOUBLE_FAULT))
        return VINF_SUCCESS;
    else if (RT_UNLIKELY(rc == VINF_EM_RESET))
        return rc;

    uint32_t uExitIntrInfo = pVmxTransient->uExitIntrInfo;
    uint32_t uVector       = VMX_EXIT_INTERRUPTION_INFO_VECTOR(uExitIntrInfo);
    switch (uIntrType)
    {
        case VMX_EXIT_INTERRUPTION_INFO_TYPE_SW_XCPT:   /* Software exception. (#BP or #OF) */
            Assert(uVector == X86_XCPT_DB || uVector == X86_XCPT_BP || uVector == X86_XCPT_OF);
            /* no break */
        case VMX_EXIT_INTERRUPTION_INFO_TYPE_HW_XCPT:   /* Hardware exception. */
        {
            switch (uVector)
            {
                case X86_XCPT_PF: rc = hmR0VmxExitXcptPF(pVM, pVCpu, pMixedCtx, pVmxTransient);      break;
                case X86_XCPT_GP: rc = hmR0VmxExitXcptGP(pVM, pVCpu, pMixedCtx, pVmxTransient);      break;
                case X86_XCPT_NM: rc = hmR0VmxExitXcptNM(pVM, pVCpu, pMixedCtx, pVmxTransient);      break;
                case X86_XCPT_MF: rc = hmR0VmxExitXcptMF(pVM, pVCpu, pMixedCtx, pVmxTransient);      break;
                case X86_XCPT_DB: rc = hmR0VmxExitXcptDB(pVM, pVCpu, pMixedCtx, pVmxTransient);      break;
                case X86_XCPT_BP: rc = hmR0VmxExitXcptBP(pVM, pVCpu, pMixedCtx, pVmxTransient);      break;
#ifdef VBOX_ALWAYS_TRAP_ALL_EXCEPTIONS
                case X86_XCPT_XF: STAM_COUNTER_INC(&pVCpu->hm.s.StatExitGuestXF);
                                  rc = hmR0VmxExitXcptGeneric(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
                case X86_XCPT_DE: STAM_COUNTER_INC(&pVCpu->hm.s.StatExitGuestDE);
                                  rc = hmR0VmxExitXcptGeneric(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
                case X86_XCPT_UD: STAM_COUNTER_INC(&pVCpu->hm.s.StatExitGuestUD);
                                  rc = hmR0VmxExitXcptGeneric(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
                case X86_XCPT_SS: STAM_COUNTER_INC(&pVCpu->hm.s.StatExitGuestSS);
                                  rc = hmR0VmxExitXcptGeneric(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
                case X86_XCPT_NP: STAM_COUNTER_INC(&pVCpu->hm.s.StatExitGuestNP);
                                  rc = hmR0VmxExitXcptGeneric(pVM, pVCpu, pMixedCtx, pVmxTransient); break;
#endif
                default:
                {
                    rc = hmR0VmxSaveGuestCR0(pVM, pVCpu, pMixedCtx);
                    AssertRCReturn(rc, rc);

                    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitGuestXcpUnk);
                    if (pVCpu->hm.s.vmx.RealMode.fRealOnV86Active)
                    {
                        Assert(pVM->hm.s.vmx.pRealModeTSS);
                        Assert(PDMVmmDevHeapIsEnabled(pVM));
                        rc  = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
                        rc |= hmR0VmxReadExitIntrErrorCodeVmcs(pVmxTransient);
                        AssertRCReturn(rc, rc);
                        rc = hmR0VmxInjectEventVmcs(pVM, pVCpu, pMixedCtx,
                                                    VMX_VMCS_CTRL_ENTRY_IRQ_INFO_FROM_EXIT_INT_INFO(uExitIntrInfo),
                                                    pVmxTransient->cbInstr, pVmxTransient->uExitIntrErrorCode);
                        AssertRCReturn(rc, rc);
                    }
                    else
                    {
                        AssertMsgFailed(("Unexpected VM-exit caused by exception %#x\n", uVector));
                        rc = VERR_VMX_UNEXPECTED_EXCEPTION;
                    }
                    break;
                }
            }
            break;
        }

        case VMX_EXIT_INTERRUPTION_INFO_TYPE_DB_XCPT:
        default:
        {
            rc = VERR_VMX_UNEXPECTED_INTERRUPTION_EXIT_CODE;
            AssertMsgFailed(("Unexpected interruption code %#x\n", VMX_EXIT_INTERRUPTION_INFO_TYPE(uExitIntrInfo)));
            break;
        }
    }
    return rc;
}


/**
 * VM-exit handler for interrupt-window exiting (VMX_EXIT_INT_WINDOW).
 */
static DECLCALLBACK(int) hmR0VmxExitIntWindow(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();

    /* Indicate that we no longer need to VM-exit when the guest is ready to receive interrupts, it is now ready. */
    pVCpu->hm.s.vmx.u32ProcCtls &= ~VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_INT_WINDOW_EXIT;
    int rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_PROC_EXEC_CONTROLS, pVCpu->hm.s.vmx.u32ProcCtls);
    AssertRCReturn(rc, rc);

    /* Deliver the pending interrupt via hmR0VmxPreRunGuest()->hmR0VmxInjectPendingInterrupt() and resume guest execution. */
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitIntWindow);
    return VINF_SUCCESS;
}


/**
 * VM-exit handler for NMI-window exiting (VMX_EXIT_NMI_WINDOW).
 */
static DECLCALLBACK(int) hmR0VmxExitNmiWindow(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();
    AssertMsgFailed(("Unexpected NMI-window exit.\n"));
    return VERR_VMX_UNEXPECTED_EXIT_CODE;
}


/**
 * VM-exit handler for WBINVD (VMX_EXIT_WBINVD). Conditional VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitWbinvd(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();
    int rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    rc    |= hmR0VmxSaveGuestRip(pVM, pVCpu, pMixedCtx);
    AssertRCReturn(rc, rc);

    pMixedCtx->rip += pVmxTransient->cbInstr;
    pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_RIP;

    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitWbinvd);
    return VINF_SUCCESS;
}


/**
 * VM-exit handler for WBINVD (VMX_EXIT_INVD). Unconditional VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitInvd(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();
    int rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    rc    |= hmR0VmxSaveGuestRip(pVM, pVCpu, pMixedCtx);
    AssertRCReturn(rc, rc);

    pMixedCtx->rip += pVmxTransient->cbInstr;
    pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_RIP;

    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitInvd);
    return VINF_SUCCESS;
}


/**
 * VM-exit handler for CPUID (VMX_EXIT_CPUID). Unconditional VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitCpuid(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();
    int rc = EMInterpretCpuId(pVM, pVCpu, CPUMCTX2CORE(pMixedCtx));
    if (RT_LIKELY(rc == VINF_SUCCESS))
    {
        rc  = hmR0VmxSaveGuestRip(pVM, pVCpu, pMixedCtx);
        rc |= hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
        AssertRCReturn(rc, rc);
        Assert(pVmxTransient->cbInstr == 2);

        Log(("hmR0VmxExitCpuid: RIP=%#RX64\n", pMixedCtx->rip));
        pMixedCtx->rip += pVmxTransient->cbInstr;
        pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_RIP;
    }
    else
    {
        AssertMsgFailed(("hmR0VmxExitCpuid: EMInterpretCpuId failed with %Rrc\n", rc));
        rc = VERR_EM_INTERPRETER;
    }
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitCpuid);
    return rc;
}


/**
 * VM-exit handler for GETSEC (VMX_EXIT_GETSEC). Unconditional VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitGetsec(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();
    int rc  = hmR0VmxSaveGuestCR4(pVM, pVCpu, pMixedCtx);
    AssertRCReturn(rc, rc);

    if (pMixedCtx->cr4 & X86_CR4_SMXE)
        return VINF_EM_RAW_EMULATE_INSTR;

    AssertMsgFailed(("hmR0VmxExitGetsec: unexpected VM-exit when CR4.SMXE is 0.\n"));
    return VERR_VMX_UNEXPECTED_EXIT_CODE;
}


/**
 * VM-exit handler for RDTSC (VMX_EXIT_RDTSC). Conditional VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitRdtsc(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();
    int rc = hmR0VmxSaveGuestCR4(pVM, pVCpu, pMixedCtx);    /** @todo review if CR4 is really required by EM. */
    AssertRCReturn(rc, rc);

    rc = EMInterpretRdtsc(pVM, pVCpu, CPUMCTX2CORE(pMixedCtx));
    if (RT_LIKELY(rc == VINF_SUCCESS))
    {
        rc  = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
        rc |= hmR0VmxSaveGuestRip(pVM, pVCpu, pMixedCtx);
        AssertRCReturn(rc, rc);
        Assert(pVmxTransient->cbInstr == 2);

        pMixedCtx->rip += pVmxTransient->cbInstr;
        pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_RIP;

        /* If we get a spurious VM-exit when offsetting is enabled, we must reset offsetting on VM-reentry. See @bugref{6634}. */
        if (pVCpu->hm.s.vmx.u32ProcCtls & VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_USE_TSC_OFFSETTING)
            pVmxTransient->fUpdateTscOffsettingAndPreemptTimer = true;
    }
    else
    {
        AssertMsgFailed(("hmR0VmxExitRdtsc: EMInterpretRdtsc failed with %Rrc\n", rc));
        rc = VERR_EM_INTERPRETER;
    }
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitRdtsc);
    return rc;
}


/**
 * VM-exit handler for RDTSCP (VMX_EXIT_RDTSCP). Conditional VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitRdtscp(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();
    int rc = hmR0VmxSaveGuestCR4(pVM, pVCpu, pMixedCtx);    /** @todo review if CR4 is really required by EM. */
    rc    |= hmR0VmxSaveGuestAutoLoadStoreMsrs(pVM, pVCpu, pMixedCtx);  /* For MSR_K8_TSC_AUX */
    AssertRCReturn(rc, rc);

    rc = EMInterpretRdtscp(pVM, pVCpu, pMixedCtx);
    if (RT_LIKELY(rc == VINF_SUCCESS))
    {
        rc  = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
        rc |= hmR0VmxSaveGuestRip(pVM, pVCpu, pMixedCtx);
        AssertRCReturn(rc, rc);
        Assert(pVmxTransient->cbInstr == 3);

        pMixedCtx->rip += pVmxTransient->cbInstr;
        pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_RIP;

        /* If we get a spurious VM-exit when offsetting is enabled, we must reset offsetting on VM-reentry. See @bugref{6634}. */
        if (pVCpu->hm.s.vmx.u32ProcCtls & VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_USE_TSC_OFFSETTING)
            pVmxTransient->fUpdateTscOffsettingAndPreemptTimer = true;
    }
    else
    {
        AssertMsgFailed(("hmR0VmxExitRdtscp: EMInterpretRdtscp failed with %Rrc\n", rc));
        rc = VERR_EM_INTERPRETER;
    }
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitRdtsc);
    return rc;
}


/**
 * VM-exit handler for RDPMC (VMX_EXIT_RDPMC). Conditional VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitRdpmc(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();
    int rc = hmR0VmxSaveGuestCR4(pVM, pVCpu, pMixedCtx);    /** @todo review if CR4 is really required by EM. */
    rc    |= hmR0VmxSaveGuestCR0(pVM, pVCpu, pMixedCtx);    /** @todo review if CR0 is really required by EM. */
    AssertRCReturn(rc, rc);

    rc = EMInterpretRdpmc(pVM, pVCpu, CPUMCTX2CORE(pMixedCtx));
    if (RT_LIKELY(rc == VINF_SUCCESS))
    {
        rc  = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
        rc |= hmR0VmxSaveGuestRip(pVM, pVCpu, pMixedCtx);
        AssertRCReturn(rc, rc);
        Assert(pVmxTransient->cbInstr == 2);

        pMixedCtx->rip += pVmxTransient->cbInstr;
        pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_RIP;
    }
    else
    {
        AssertMsgFailed(("hmR0VmxExitRdpmc: EMInterpretRdpmc failed with %Rrc\n", rc));
        rc = VERR_EM_INTERPRETER;
    }
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitRdpmc);
    return rc;
}


/**
 * VM-exit handler for INVLPG (VMX_EXIT_INVLPG). Conditional VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitInvlpg(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();
    int rc = hmR0VmxReadExitQualificationVmcs(pVmxTransient);
    rc    |= hmR0VmxSaveGuestControlRegs(pVM, pVCpu, pMixedCtx);
    AssertRCReturn(rc, rc);

    VBOXSTRICTRC rc2 = EMInterpretInvlpg(pVM, pVCpu, CPUMCTX2CORE(pMixedCtx), pVmxTransient->uExitQualification);
    rc = VBOXSTRICTRC_VAL(rc2);
    if (RT_LIKELY(rc == VINF_SUCCESS))
    {
        rc  = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
        rc |= hmR0VmxSaveGuestRip(pVM, pVCpu, pMixedCtx);
        AssertRCReturn(rc, rc);

        pMixedCtx->rip += pVmxTransient->cbInstr;
        pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_RIP;
    }
    else
    {
        AssertMsg(rc == VERR_EM_INTERPRETER, ("hmR0VmxExitInvlpg: EMInterpretInvlpg %RGv failed with %Rrc\n",
                                              pVmxTransient->uExitQualification, rc));
        rc = VERR_EM_INTERPRETER;
    }
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitInvlpg);
    return rc;
}


/**
 * VM-exit handler for MONITOR (VMX_EXIT_MONITOR). Conditional VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitMonitor(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();
    int rc = hmR0VmxSaveGuestCR0(pVM, pVCpu, pMixedCtx);
    rc    |= hmR0VmxSaveGuestRflags(pVM, pVCpu, pMixedCtx);
    rc    |= hmR0VmxSaveGuestSegmentRegs(pVM, pVCpu, pMixedCtx);
    AssertRCReturn(rc, rc);

    rc = EMInterpretMonitor(pVM, pVCpu, CPUMCTX2CORE(pMixedCtx));
    if (RT_LIKELY(rc == VINF_SUCCESS))
    {
        rc  = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
        rc |= hmR0VmxSaveGuestRip(pVM, pVCpu, pMixedCtx);
        AssertRCReturn(rc, rc);

        pMixedCtx->rip += pVmxTransient->cbInstr;
        pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_RIP;
    }
    else
    {
        AssertMsg(rc == VERR_EM_INTERPRETER, ("hmR0VmxExitMonitor: EMInterpretMonitor failed with %Rrc\n", rc));
        rc = VERR_EM_INTERPRETER;
    }
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitMonitor);
    return rc;
}


/**
 * VM-exit handler for MWAIT (VMX_EXIT_MWAIT). Conditional VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitMwait(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();
    int rc = hmR0VmxSaveGuestCR0(pVM, pVCpu, pMixedCtx);
    rc    |= hmR0VmxSaveGuestRflags(pVM, pVCpu, pMixedCtx);
    rc    |= hmR0VmxSaveGuestSegmentRegs(pVM, pVCpu, pMixedCtx);
    AssertRCReturn(rc, rc);

    VBOXSTRICTRC rc2 = EMInterpretMWait(pVM, pVCpu, CPUMCTX2CORE(pMixedCtx));
    rc = VBOXSTRICTRC_VAL(rc2);
    if (RT_LIKELY(   rc == VINF_SUCCESS
                  || rc == VINF_EM_HALT))
    {
        int rc3  = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
        rc3     |= hmR0VmxSaveGuestRip(pVM, pVCpu, pMixedCtx);
        AssertRCReturn(rc3, rc3);

        pMixedCtx->rip += pVmxTransient->cbInstr;
        pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_RIP;

        if (   rc == VINF_EM_HALT
            && EMShouldContinueAfterHalt(pVCpu, pMixedCtx))
        {
            rc = VINF_SUCCESS;
        }
    }
    else
    {
        AssertMsg(rc == VERR_EM_INTERPRETER, ("hmR0VmxExitMwait: EMInterpretMWait failed with %Rrc\n", rc));
        rc = VERR_EM_INTERPRETER;
    }
    AssertMsg(rc == VINF_SUCCESS || rc == VINF_EM_HALT || rc == VERR_EM_INTERPRETER,
              ("hmR0VmxExitMwait: failed, invalid error code %Rrc\n", rc));
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitMwait);
    return rc;
}


/**
 * VM-exit handler for RSM (VMX_EXIT_RSM). Unconditional VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitRsm(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    /*
     * Execution of RSM outside of SMM mode causes #UD regardless of VMX root or VMX non-root mode. In theory, we should never
     * get this VM-exit. This can happen only if dual-monitor treatment of SMI and VMX is enabled, which can (only?) be done by
     * executing VMCALL in VMX root operation. If we get here something funny is going on.
     * See Intel spec. "33.15.5 Enabling the Dual-Monitor Treatment".
     */
    AssertMsgFailed(("Unexpected RSM VM-exit. pVM=%p pVCpu=%p pMixedCtx=%p\n", pVM, pVCpu, pMixedCtx));
    return VERR_VMX_UNEXPECTED_EXIT_CODE;
}


/**
 * VM-exit handler for SMI (VMX_EXIT_SMI). Unconditional VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitSmi(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    /*
     * This can only happen if we support dual-monitor treatment of SMI, which can be activated by executing VMCALL in VMX
     * root operation. If we get there there is something funny going on.
     * See Intel spec. "33.15.6 Activating the Dual-Monitor Treatment" and Intel spec. 25.3 "Other Causes of VM-Exits"
     */
    AssertMsgFailed(("Unexpected SMI VM-exit. pVM=%p pVCpu=%p pMixedCtx=%p\n", pVM, pVCpu, pMixedCtx));
    return VERR_VMX_UNEXPECTED_EXIT_CODE;
}


/**
 * VM-exit handler for IO SMI (VMX_EXIT_IO_SMI). Unconditional VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitIoSmi(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    /* Same treatment as VMX_EXIT_SMI. See comment in hmR0VmxExitSmi(). */
    AssertMsgFailed(("Unexpected IO SMI VM-exit. pVM=%p pVCpu=%p pMixedCtx=%p\n", pVM, pVCpu, pMixedCtx));
    return VERR_VMX_UNEXPECTED_EXIT_CODE;
}


/**
 * VM-exit handler for SIPI (VMX_EXIT_SIPI). Conditional VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitSipi(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    /*
     * SIPI exits can only occur in VMX non-root operation when the "wait-for-SIPI" guest activity state is used. We currently
     * don't make use of it (see hmR0VmxLoadGuestActivityState()) as our guests don't have direct access to the host LAPIC.
     * See Intel spec. 25.3 "Other Causes of VM-exits".
     */
    AssertMsgFailed(("Unexpected SIPI VM-exit. pVM=%p pVCpu=%p pMixedCtx=%p\n", pVM, pVCpu, pMixedCtx));
    return VERR_VMX_UNEXPECTED_EXIT_CODE;
}


/**
 * VM-exit handler for INIT signal (VMX_EXIT_INIT_SIGNAL). Unconditional
 * VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitInitSignal(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    /*
     * INIT signals are blocked in VMX root operation by VMXON and by SMI in SMM. See Intel spec. "33.14.1 Default Treatment of
     * SMI Delivery" and "29.3 VMX Instructions" for "VMXON". It is -NOT- blocked in VMX non-root operation so we can potentially
     * still get these exits. See Intel spec. "23.8 Restrictions on VMX operation".
     */
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();
    return VINF_SUCCESS;    /** @todo r=ramshankar: correct?. */
}


/**
 * VM-exit handler for triple faults (VMX_EXIT_TRIPLE_FAULT). Unconditional
 * VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitTripleFault(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();
    return VINF_EM_RESET;
}


/**
 * VM-exit handler for HLT (VMX_EXIT_HLT). Conditional VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitHlt(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();
    Assert(pVCpu->hm.s.vmx.u32ProcCtls & VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_HLT_EXIT);
    int rc = hmR0VmxSaveGuestRip(pVM, pVCpu, pMixedCtx);
    rc    |= hmR0VmxSaveGuestRflags(pVM, pVCpu, pMixedCtx);
    AssertRCReturn(rc, rc);

    pMixedCtx->rip++;
    pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_RIP;
    if (EMShouldContinueAfterHalt(pVCpu, pMixedCtx))    /* Requires eflags. */
        rc = VINF_SUCCESS;
    else
        rc = VINF_EM_HALT;

    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitHlt);
    return rc;
}


/**
 * VM-exit handler for instructions that result in a #UD exception delivered to the guest.
 */
static DECLCALLBACK(int) hmR0VmxExitInjectXcptUD(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();
    return hmR0VmxInjectXcptUD(pVM, pVCpu, pMixedCtx);
}


/**
 * VM-exit handler for expiry of the VMX preemption timer.
 */
static DECLCALLBACK(int) hmR0VmxExitPreemptionTimer(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();

    /* If we're saving the preemption-timer value on every VM-exit & we've reached zero, reset it up on next VM-entry. */
    if (pVCpu->hm.s.vmx.u32ExitCtls & VMX_VMCS_CTRL_EXIT_CONTROLS_SAVE_VMX_PREEMPT_TIMER)
        pVmxTransient->fUpdateTscOffsettingAndPreemptTimer = true;

    /* If there are any timer events pending, fall back to ring-3, otherwise resume guest execution. */
    bool fTimersPending = TMTimerPollBool(pVM, pVCpu);
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitPreemptTimer);
    return fTimersPending ? VINF_EM_RAW_TIMER_PENDING : VINF_SUCCESS;
}


/**
 * VM-exit handler for XSETBV (VMX_EXIT_XSETBV). Unconditional VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitXsetbv(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();
    /* We expose XSETBV to the guest, fallback to the recompiler for emulation. */
    /** @todo check if XSETBV is supported by the recompiler. */
    return VERR_EM_INTERPRETER;
}


/**
 * VM-exit handler for INVPCID (VMX_EXIT_INVPCID). Conditional VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitInvpcid(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();
    /* The guest should not invalidate the host CPU's TLBs, fallback to recompiler. */
    /** @todo implement EMInterpretInvpcid() */
    return VERR_EM_INTERPRETER;
}


/**
 * VM-exit handler for invalid-guest-state (VMX_EXIT_ERR_INVALID_GUEST_STATE).
 * Error VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitErrInvalidGuestState(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    uint32_t    uIntrState;
    RTHCUINTREG uHCReg;
    uint64_t    u64Val;

    int rc  = hmR0VmxReadEntryIntrInfoVmcs(pVmxTransient);
    rc     |= hmR0VmxReadEntryXcptErrorCodeVmcs(pVmxTransient);
    rc     |= hmR0VmxReadEntryInstrLenVmcs(pVmxTransient);
    rc     |= VMXReadVmcs32(VMX_VMCS32_GUEST_INTERRUPTIBILITY_STATE, &uIntrState);
    rc     |= hmR0VmxSaveGuestState(pVM, pVCpu, pMixedCtx);
    AssertRCReturn(rc, rc);

    Log(("VMX_VMCS32_CTRL_ENTRY_INTERRUPTION_INFO    %#RX32\n", pVmxTransient->uEntryIntrInfo));
    Log(("VMX_VMCS32_CTRL_ENTRY_EXCEPTION_ERRCODE    %#RX32\n", pVmxTransient->uEntryXcptErrorCode));
    Log(("VMX_VMCS32_CTRL_ENTRY_INSTR_LENGTH         %#RX32\n", pVmxTransient->cbEntryInstr));
    Log(("VMX_VMCS32_GUEST_INTERRUPTIBILITY_STATE    %#RX32\n", uIntrState));

    rc = VMXReadVmcsGstN(VMX_VMCS_GUEST_CR0, &u64Val);                      AssertRC(rc);
    Log(("VMX_VMCS_GUEST_CR0                         %#RX64\n", u64Val));
    rc = VMXReadVmcsHstN(VMX_VMCS_CTRL_CR0_MASK, &uHCReg);                  AssertRC(rc);
    Log(("VMX_VMCS_CTRL_CR0_MASK                     %#RHr\n", uHCReg));
    rc = VMXReadVmcsHstN(VMX_VMCS_CTRL_CR0_READ_SHADOW, &uHCReg);           AssertRC(rc);
    Log(("VMX_VMCS_CTRL_CR4_READ_SHADOW              %#RHr\n", uHCReg));
    rc = VMXReadVmcsHstN(VMX_VMCS_CTRL_CR4_MASK, &uHCReg);                  AssertRC(rc);
    Log(("VMX_VMCS_CTRL_CR4_MASK                     %#RHr\n", uHCReg));
    rc = VMXReadVmcsHstN(VMX_VMCS_CTRL_CR4_READ_SHADOW, &uHCReg);           AssertRC(rc);
    Log(("VMX_VMCS_CTRL_CR4_READ_SHADOW              %#RHr\n", uHCReg));
    rc = VMXReadVmcs64(VMX_VMCS64_CTRL_EPTP_FULL, &u64Val);                 AssertRC(rc);
    Log(("VMX_VMCS64_CTRL_EPTP_FULL                  %#RX64\n", u64Val));

    HMDumpRegs(pVM, pVCpu, pMixedCtx);

    return VERR_VMX_INVALID_GUEST_STATE;
}


/**
 * VM-exit handler for VM-entry failure due to an MSR-load
 * (VMX_EXIT_ERR_MSR_LOAD). Error VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitErrMsrLoad(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    AssertMsgFailed(("Unexpected MSR-load exit. pVM=%p pVCpu=%p pMixedCtx=%p\n", pVM, pVCpu, pMixedCtx));
    return VERR_VMX_UNEXPECTED_EXIT_CODE;
}


/**
 * VM-exit handler for VM-entry failure due to a machine-check event
 * (VMX_EXIT_ERR_MACHINE_CHECK). Error VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitErrMachineCheck(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    AssertMsgFailed(("Unexpected machine-check event exit. pVM=%p pVCpu=%p pMixedCtx=%p\n", pVM, pVCpu, pMixedCtx));
    return VERR_VMX_UNEXPECTED_EXIT_CODE;
}


/**
 * VM-exit handler for all undefined reasons. Should never ever happen.. in
 * theory.
 */
static DECLCALLBACK(int) hmR0VmxExitErrUndefined(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    AssertMsgFailed(("Huh!? Undefined VM-exit reason %d. pVM=%p pVCpu=%p pMixedCtx=%p\n", pVmxTransient->uExitReason,
                     pVM, pVCpu, pMixedCtx));
    return VERR_VMX_UNDEFINED_EXIT_CODE;
}


/**
 * VM-exit handler for XDTR (LGDT, SGDT, LIDT, SIDT) accesses
 * (VMX_EXIT_XDTR_ACCESS) and LDT and TR access (LLDT, LTR, SLDT, STR).
 * Conditional VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitXdtrAccess(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();
    /* By default, we don't enable VMX_VMCS_CTRL_PROC_EXEC2_DESCRIPTOR_TABLE_EXIT. */
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitXdtrAccess);
    if (pVCpu->hm.s.vmx.u32ProcCtls2 & VMX_VMCS_CTRL_PROC_EXEC2_DESCRIPTOR_TABLE_EXIT)
        return VERR_EM_INTERPRETER;
    AssertMsgFailed(("Unexpected XDTR access. pVM=%p pVCpu=%p pMixedCtx=%p\n", pVM, pVCpu, pMixedCtx));
    return VERR_VMX_UNEXPECTED_EXIT_CODE;
}


/**
 * VM-exit handler for RDRAND (VMX_EXIT_RDRAND). Conditional VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitRdrand(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();
    /* By default, we don't enable VMX_VMCS_CTRL_PROC_EXEC2_RDRAND_EXIT. */
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitRdrand);
    if (pVCpu->hm.s.vmx.u32ProcCtls2 & VMX_VMCS_CTRL_PROC_EXEC2_RDRAND_EXIT)
        return VERR_EM_INTERPRETER;
    AssertMsgFailed(("Unexpected RDRAND exit. pVM=%p pVCpu=%p pMixedCtx=%p\n", pVM, pVCpu, pMixedCtx));
    return VERR_VMX_UNEXPECTED_EXIT_CODE;
}


/**
 * VM-exit handler for RDMSR (VMX_EXIT_RDMSR).
 */
static DECLCALLBACK(int) hmR0VmxExitRdmsr(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();
    /* EMInterpretRdmsr() requires CR0, Eflags and SS segment register. */
    int rc = hmR0VmxSaveGuestCR0(pVM, pVCpu, pMixedCtx);
    rc    |= hmR0VmxSaveGuestRflags(pVM, pVCpu, pMixedCtx);
    rc    |= hmR0VmxSaveGuestSegmentRegs(pVM, pVCpu, pMixedCtx);
    AssertRCReturn(rc, rc);

    rc = EMInterpretRdmsr(pVM, pVCpu, CPUMCTX2CORE(pMixedCtx));
    AssertMsg(rc == VINF_SUCCESS || rc == VERR_EM_INTERPRETER,
              ("hmR0VmxExitRdmsr: failed, invalid error code %Rrc\n", rc));
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitRdmsr);

    /* Update RIP and continue guest execution. */
    if (RT_LIKELY(rc == VINF_SUCCESS))
    {
        rc  = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
        rc |= hmR0VmxSaveGuestRip(pVM, pVCpu, pMixedCtx);
        AssertRCReturn(rc, rc);

        Assert(pVmxTransient->cbInstr == 2);
        pMixedCtx->rip += pVmxTransient->cbInstr;
        pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_RIP;
    }
    return rc;
}


/**
 * VM-exit handler for WRMSR (VMX_EXIT_WRMSR).
 */
static DECLCALLBACK(int) hmR0VmxExitWrmsr(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();
    int rc = hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    AssertRCReturn(rc, rc);
    Assert(pVmxTransient->cbInstr == 2);

    /* If TPR patching is active, LSTAR holds the guest TPR, writes to it must be propagated to the APIC. */
    if (   pVM->hm.s.fTPRPatchingActive
        && pMixedCtx->ecx == MSR_K8_LSTAR)
    {
        Assert(!CPUMIsGuestInLongModeEx(pMixedCtx));    /* Requires EFER but it's always up-to-date. */
        if ((pMixedCtx->eax & 0xff) != pVmxTransient->u8GuestTpr)
        {
            rc = PDMApicSetTPR(pVCpu, pMixedCtx->eax & 0xff);
            AssertRC(rc);
        }

        rc = hmR0VmxSaveGuestRip(pVM, pVCpu, pMixedCtx);
        AssertRCReturn(rc, rc);
        pMixedCtx->rip += pVmxTransient->cbInstr;
        pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_RIP;
        STAM_COUNTER_INC(&pVCpu->hm.s.StatExitWrmsr);
        return VINF_SUCCESS;
    }

    /* Update MSRs that are part of the VMCS when MSR-bitmaps are not supported. */
    if (!(pVCpu->hm.s.vmx.u32ProcCtls & VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_USE_MSR_BITMAPS))
    {
        switch (pMixedCtx->ecx)
        {
            case MSR_IA32_SYSENTER_CS:  pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_SYSENTER_CS_MSR;  break;
            case MSR_IA32_SYSENTER_EIP: pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_SYSENTER_EIP_MSR; break;
            case MSR_IA32_SYSENTER_ESP: pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_SYSENTER_ESP_MSR; break;
            case MSR_K8_FS_BASE:        pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_FS_BASE_MSR;      break;
            case MSR_K8_GS_BASE:        pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_GS_BASE_MSR;      break;
        }
    }
#ifdef DEBUG
    else
    {
        /* Paranoia. Validate that MSRs in the MSR-bitmaps with write-passthru are not intercepted. */
        switch (pMixedCtx->ecx)
        {
            case MSR_IA32_SYSENTER_CS:
            case MSR_IA32_SYSENTER_EIP:
            case MSR_IA32_SYSENTER_ESP:
            case MSR_K8_FS_BASE:
            case MSR_K8_GS_BASE:
                AssertMsgFailed(("Unexpected WRMSR for an MSR in the VMCS. ecx=%RX32\n", pMixedCtx->ecx));
                return VERR_VMX_UNEXPECTED_EXIT_CODE;
            case MSR_K8_LSTAR:
            case MSR_K6_STAR:
            case MSR_K8_SF_MASK:
            case MSR_K8_TSC_AUX:
                AssertMsgFailed(("Unexpected WRMSR for an MSR in the auto-load/store area in the VMCS. ecx=%RX32\n", pMixedCtx->ecx));
                return VERR_VMX_UNEXPECTED_EXIT_CODE;
        }
    }
#endif

    /* EMInterpretWrmsr() requires CR0, EFLAGS and SS segment register. */
    rc  = hmR0VmxSaveGuestCR0(pVM, pVCpu, pMixedCtx);
    rc |= hmR0VmxSaveGuestRflags(pVM, pVCpu, pMixedCtx);
    rc |= hmR0VmxSaveGuestSegmentRegs(pVM, pVCpu, pMixedCtx);
    AssertRCReturn(rc, rc);

    rc = EMInterpretWrmsr(pVM, pVCpu, CPUMCTX2CORE(pMixedCtx));
    AssertMsg(rc == VINF_SUCCESS || rc == VERR_EM_INTERPRETER, ("hmR0VmxExitWrmsr: failed, invalid error code %Rrc\n", rc));
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitWrmsr);

    /* Update guest-state and continue execution. */
    if (RT_LIKELY(rc == VINF_SUCCESS))
    {
        rc = hmR0VmxSaveGuestRip(pVM, pVCpu, pMixedCtx);
        AssertRCReturn(rc, rc);

        pMixedCtx->rip += pVmxTransient->cbInstr;
        pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_RIP;

        /* If this is an X2APIC WRMSR access, update the APIC state as well. */
        if (   pMixedCtx->ecx >= MSR_IA32_X2APIC_START
            && pMixedCtx->ecx <= MSR_IA32_X2APIC_END)
        {
            pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_VMX_GUEST_APIC_STATE;
        }
    }
    return rc;
}


/**
 * VM-exit handler for PAUSE (VMX_EXIT_PAUSE). Conditional VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitPause(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();
    /* By default, we don't enable VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_PAUSE_EXIT. */
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitPause);
    if (pVCpu->hm.s.vmx.u32ProcCtls & VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_PAUSE_EXIT)
        return VERR_EM_INTERPRETER;
    AssertMsgFailed(("Unexpected PAUSE exit. pVM=%p pVCpu=%p pMixedCtx=%p\n", pVM, pVCpu, pMixedCtx));
    return VERR_VMX_UNEXPECTED_EXIT_CODE;
}


/**
 * VM-exit handler for when the TPR value is lowered below the specified
 * threshold (VMX_EXIT_TPR_BELOW_THRESHOLD). Conditional VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitTprBelowThreshold(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();
    Assert(pVCpu->hm.s.vmx.u32ProcCtls & VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_USE_TPR_SHADOW);

    /*
     * The TPR has already been updated, see hmR0VMXPostRunGuest(). RIP is also updated as part of the VM-exit by VT-x. Update
     * the threshold in the VMCS, deliver the pending interrupt via hmR0VmxPreRunGuest()->hmR0VmxInjectPendingInterrupt() and
     * resume guest execution.
     */
    pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_VMX_GUEST_APIC_STATE;
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitTprBelowThreshold);
    return VINF_SUCCESS;
}


/**
 * VM-exit handler for control-register accesses (VMX_EXIT_MOV_CRX). Conditional
 * VM-exit.
 *
 * @retval VINF_SUCCESS when guest execution can continue.
 * @retval VINF_PGM_CHANGE_MODE when shadow paging mode changed, back to ring-3.
 * @retval VINF_PGM_SYNC_CR3 CR3 sync is required, back to ring-3.
 * @retval VERR_EM_INTERPRETER when something unexpected happened, fallback to
 *         recompiler.
 */
static DECLCALLBACK(int) hmR0VmxExitMovCRx(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();
    int rc = hmR0VmxReadExitQualificationVmcs(pVmxTransient);
    AssertRCReturn(rc, rc);

    const RTGCUINTPTR uExitQualification = pVmxTransient->uExitQualification;
    const uint32_t uAccessType           = VMX_EXIT_QUALIFICATION_CRX_ACCESS(uExitQualification);
    switch (uAccessType)
    {
        case VMX_EXIT_QUALIFICATION_CRX_ACCESS_WRITE:       /* MOV to CRx */
        {
#if 0
            /* EMInterpretCRxWrite() references a lot of guest state (EFER, RFLAGS, Segment Registers, etc.) Sync entire state */
            rc = hmR0VmxSaveGuestState(pVM, pVCpu, pMixedCtx);
#else
            rc  = hmR0VmxSaveGuestGprs(pVM, pVCpu, pMixedCtx);
            rc |= hmR0VmxSaveGuestControlRegs(pVM, pVCpu, pMixedCtx);
            rc |= hmR0VmxSaveGuestSegmentRegs(pVM, pVCpu, pMixedCtx);
#endif
            AssertRCReturn(rc, rc);

            rc = EMInterpretCRxWrite(pVM, pVCpu, CPUMCTX2CORE(pMixedCtx),
                                     VMX_EXIT_QUALIFICATION_CRX_REGISTER(uExitQualification),
                                     VMX_EXIT_QUALIFICATION_CRX_GENREG(uExitQualification));
            Assert(rc == VINF_SUCCESS || rc == VERR_EM_INTERPRETER || rc == VINF_PGM_CHANGE_MODE || rc == VINF_PGM_SYNC_CR3);

            switch (VMX_EXIT_QUALIFICATION_CRX_REGISTER(uExitQualification))
            {
                case 0: /* CR0 */
                    Log(("CR0 write rc=%d\n", rc));
                    pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_CR0;
                    break;
                case 2: /* CR2 */
                    Log(("CR2 write rc=%d\n", rc));
                    break;
                case 3: /* CR3 */
                    Assert(!pVM->hm.s.fNestedPaging || !CPUMIsGuestPagingEnabledEx(pMixedCtx));
                    Log(("CR3 write rc=%d\n", rc));
                    pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_CR3;
                    break;
                case 4: /* CR4 */
                    Log(("CR4 write rc=%d\n", rc));
                    pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_CR4;
                    break;
                case 8: /* CR8 */
                    Assert(!(pVCpu->hm.s.vmx.u32ProcCtls & VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_USE_TPR_SHADOW));
                    /* CR8 contains the APIC TPR. Was updated by EMInterpretCRxWrite(). */
                    /* We don't need to update HM_CHANGED_VMX_GUEST_APIC_STATE here as this -cannot- happen with TPR shadowing. */
                    break;
                default:
                    AssertMsgFailed(("Invalid CRx register %#x\n", VMX_EXIT_QUALIFICATION_CRX_REGISTER(uExitQualification)));
                    break;
            }

            STAM_COUNTER_INC(&pVCpu->hm.s.StatExitCRxWrite[VMX_EXIT_QUALIFICATION_CRX_REGISTER(uExitQualification)]);
            break;
        }

        case VMX_EXIT_QUALIFICATION_CRX_ACCESS_READ:        /* MOV from CRx */
        {
            /* EMInterpretCRxRead() requires EFER MSR, CS. */
            rc = hmR0VmxSaveGuestSegmentRegs(pVM, pVCpu, pMixedCtx);
            AssertRCReturn(rc, rc);
            Assert(   !pVM->hm.s.fNestedPaging
                   || !CPUMIsGuestPagingEnabledEx(pMixedCtx)
                   || VMX_EXIT_QUALIFICATION_CRX_REGISTER(uExitQualification) != 3);

            /* CR8 reads only cause a VM-exit when the TPR shadow feature isn't enabled. */
            Assert(   VMX_EXIT_QUALIFICATION_CRX_REGISTER(uExitQualification) != 8
                   || !(pVCpu->hm.s.vmx.u32ProcCtls & VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_USE_TPR_SHADOW));

            rc = EMInterpretCRxRead(pVM, pVCpu, CPUMCTX2CORE(pMixedCtx),
                                    VMX_EXIT_QUALIFICATION_CRX_GENREG(uExitQualification),
                                    VMX_EXIT_QUALIFICATION_CRX_REGISTER(uExitQualification));
            Assert(rc == VINF_SUCCESS || rc == VERR_EM_INTERPRETER);
            STAM_COUNTER_INC(&pVCpu->hm.s.StatExitCRxRead[VMX_EXIT_QUALIFICATION_CRX_REGISTER(uExitQualification)]);
            Log(("CRX CR%d Read access rc=%d\n", VMX_EXIT_QUALIFICATION_CRX_REGISTER(uExitQualification), rc));
            break;
        }

        case VMX_EXIT_QUALIFICATION_CRX_ACCESS_CLTS:        /* CLTS (Clear Task-Switch Flag in CR0) */
        {
            rc = hmR0VmxSaveGuestCR0(pVM, pVCpu, pMixedCtx);
            AssertRCReturn(rc, rc);
            rc = EMInterpretCLTS(pVM, pVCpu);
            AssertRCReturn(rc, rc);
            pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_CR0;
            STAM_COUNTER_INC(&pVCpu->hm.s.StatExitClts);
            Log(("CRX CLTS write rc=%d\n", rc));
            break;
        }

        case VMX_EXIT_QUALIFICATION_CRX_ACCESS_LMSW:        /* LMSW (Load Machine-Status Word into CR0) */
        {
            rc = hmR0VmxSaveGuestCR0(pVM, pVCpu, pMixedCtx);
            AssertRCReturn(rc, rc);
            rc = EMInterpretLMSW(pVM, pVCpu, CPUMCTX2CORE(pMixedCtx), VMX_EXIT_QUALIFICATION_CRX_LMSW_DATA(uExitQualification));
            if (RT_LIKELY(rc == VINF_SUCCESS))
                pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_CR0;
            STAM_COUNTER_INC(&pVCpu->hm.s.StatExitLmsw);
            Log(("CRX LMSW write rc=%d\n", rc));
            break;
        }

        default:
        {
            AssertMsgFailed(("Invalid access-type in Mov CRx exit qualification %#x\n", uAccessType));
            return VERR_VMX_UNEXPECTED_EXCEPTION;
        }
    }

    /* Validate possible error codes. */
    Assert(rc == VINF_SUCCESS || rc == VINF_PGM_CHANGE_MODE || rc == VERR_EM_INTERPRETER || rc == VINF_PGM_SYNC_CR3);
    if (RT_SUCCESS(rc))
    {
        int rc2  = hmR0VmxSaveGuestRip(pVM, pVCpu, pMixedCtx);
        rc2     |= hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
        AssertRCReturn(rc2, rc2);
        pMixedCtx->rip += pVmxTransient->cbInstr;
        pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_RIP;
    }

    return rc;
}


/**
 * VM-exit handler for I/O instructions (VMX_EXIT_IO_INSTR). Conditional
 * VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitIoInstr(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();

    int rc = hmR0VmxReadExitQualificationVmcs(pVmxTransient);
    rc    |= hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    rc    |= hmR0VmxSaveGuestRip(pVM, pVCpu, pMixedCtx);
    rc    |= hmR0VmxSaveGuestRflags(pVM, pVCpu, pMixedCtx);         /* Eflag checks in EMInterpretDisasCurrent(). */
    rc    |= hmR0VmxSaveGuestControlRegs(pVM, pVCpu, pMixedCtx);    /* CR0 checks & PGM* in EMInterpretDisasCurrent(). */
    rc    |= hmR0VmxSaveGuestSegmentRegs(pVM, pVCpu, pMixedCtx);    /* SELM checks in EMInterpretDisasCurrent(). */
    /* EFER also required for longmode checks in EMInterpretDisasCurrent(), but it's always up-to-date. */
    AssertRCReturn(rc, rc);

    /* Refer Intel spec. 27-5. "Exit Qualifications for I/O Instructions" for the format. */
    uint32_t uIOPort   = VMX_EXIT_QUALIFICATION_IO_PORT(pVmxTransient->uExitQualification);
    uint32_t uIOWidth  = VMX_EXIT_QUALIFICATION_IO_WIDTH(pVmxTransient->uExitQualification);
    bool     fIOWrite  = (VMX_EXIT_QUALIFICATION_IO_DIRECTION(pVmxTransient->uExitQualification)
                          == VMX_EXIT_QUALIFICATION_IO_DIRECTION_OUT);
    bool     fIOString = (VMX_EXIT_QUALIFICATION_IO_STRING(pVmxTransient->uExitQualification) == 1);
    Assert(uIOWidth == 0 || uIOWidth == 1 || uIOWidth == 3);

    /* I/O operation lookup arrays. */
    static const uint32_t s_aIOSize[4]  = { 1, 2, 0, 4 };                   /* Size of the I/O Accesses. */
    static const uint32_t s_aIOOpAnd[4] = { 0xff, 0xffff, 0, 0xffffffff };  /* AND masks for saving the result (in AL/AX/EAX). */

    const uint32_t cbSize  = s_aIOSize[uIOWidth];
    const uint32_t cbInstr = pVmxTransient->cbInstr;
    if (fIOString)
    {
        /* INS/OUTS - I/O String instruction. */
        PDISCPUSTATE pDis = &pVCpu->hm.s.DisState;
        /** @todo for now manually disassemble later optimize by getting the fields from
         *        the VMCS. */
        /** @todo VMX_VMCS_RO_EXIT_GUEST_LINEAR_ADDR contains the flat pointer
         *        operand of the instruction. VMX_VMCS32_RO_EXIT_INSTR_INFO contains
         *        segment prefix info. */
        rc = EMInterpretDisasCurrent(pVM, pVCpu, pDis, NULL);
        if (RT_SUCCESS(rc))
        {
            if (fIOWrite)
            {
                VBOXSTRICTRC rc2 = IOMInterpretOUTSEx(pVM, pVCpu, CPUMCTX2CORE(pMixedCtx), uIOPort, pDis->fPrefix,
                                                      (DISCPUMODE)pDis->uAddrMode, cbSize);
                rc = VBOXSTRICTRC_VAL(rc2);
                STAM_COUNTER_INC(&pVCpu->hm.s.StatExitIOStringWrite);
            }
            else
            {
                VBOXSTRICTRC rc2 = IOMInterpretINSEx(pVM, pVCpu, CPUMCTX2CORE(pMixedCtx), uIOPort, pDis->fPrefix,
                                                     (DISCPUMODE)pDis->uAddrMode, cbSize);
                rc = VBOXSTRICTRC_VAL(rc2);
                STAM_COUNTER_INC(&pVCpu->hm.s.StatExitIOStringRead);
            }
        }
        else
        {
            AssertMsg(rc == VERR_EM_INTERPRETER, ("rc=%Rrc RIP %#RX64\n", rc, pMixedCtx->rip));
            rc = VINF_EM_RAW_EMULATE_INSTR;
        }
    }
    else
    {
        /* IN/OUT - I/O instruction. */
        const uint32_t uAndVal = s_aIOOpAnd[uIOWidth];
        Assert(!VMX_EXIT_QUALIFICATION_IO_REP(pVmxTransient->uExitQualification));
        if (fIOWrite)
        {
            VBOXSTRICTRC rc2 = IOMIOPortWrite(pVM, pVCpu, uIOPort, pMixedCtx->eax & uAndVal, cbSize);
            rc = VBOXSTRICTRC_VAL(rc2);
            if (rc == VINF_IOM_R3_IOPORT_WRITE)
                HMR0SavePendingIOPortWrite(pVCpu, pMixedCtx->rip, pMixedCtx->rip + cbInstr, uIOPort, uAndVal, cbSize);
            STAM_COUNTER_INC(&pVCpu->hm.s.StatExitIOWrite);
        }
        else
        {
            uint32_t u32Result = 0;
            VBOXSTRICTRC rc2 = IOMIOPortRead(pVM, pVCpu, uIOPort, &u32Result, cbSize);
            rc = VBOXSTRICTRC_VAL(rc2);
            if (IOM_SUCCESS(rc))
            {
                /* Save result of I/O IN instr. in AL/AX/EAX. */
                pMixedCtx->eax = (pMixedCtx->eax & ~uAndVal) | (u32Result & uAndVal);
            }
            else if (rc == VINF_IOM_R3_IOPORT_READ)
                HMR0SavePendingIOPortRead(pVCpu, pMixedCtx->rip, pMixedCtx->rip + cbInstr, uIOPort, uAndVal, cbSize);
            STAM_COUNTER_INC(&pVCpu->hm.s.StatExitIORead);
        }
    }

    if (IOM_SUCCESS(rc))
    {
        pMixedCtx->rip += cbInstr;
        pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_RIP;
        if (RT_LIKELY(rc == VINF_SUCCESS))
        {
            rc = hmR0VmxSaveGuestDebugRegs(pVM, pVCpu, pMixedCtx);      /* For DR7. */
            AssertRCReturn(rc, rc);

            /* If any IO breakpoints are armed, then we should check if a debug trap needs to be generated. */
            if (pMixedCtx->dr[7] & X86_DR7_ENABLED_MASK)
            {
                STAM_COUNTER_INC(&pVCpu->hm.s.StatDRxIoCheck);
                for (unsigned i = 0; i < 4; i++)
                {
                    unsigned uBPLen = s_aIOSize[X86_DR7_GET_LEN(pMixedCtx->dr[7], i)];
                    if (   (   uIOPort >= pMixedCtx->dr[i]
                            && uIOPort < pMixedCtx->dr[i] + uBPLen)
                        && (pMixedCtx->dr[7] & (X86_DR7_L(i) | X86_DR7_G(i)))
                        && (pMixedCtx->dr[7] & X86_DR7_RW(i, X86_DR7_RW_IO)) == X86_DR7_RW(i, X86_DR7_RW_IO))
                    {
                        Assert(CPUMIsGuestDebugStateActive(pVCpu));
                        uint64_t uDR6 = ASMGetDR6();

                        /* Clear all breakpoint status flags and set the one we just hit. */
                        uDR6 &= ~(X86_DR6_B0 | X86_DR6_B1 | X86_DR6_B2 | X86_DR6_B3);
                        uDR6 |= (uint64_t)RT_BIT(i);

                        /*
                         * Note: AMD64 Architecture Programmer's Manual 13.1:
                         * Bits 15:13 of the DR6 register is never cleared by the processor and must
                         * be cleared by software after the contents have been read.
                         */
                        ASMSetDR6(uDR6);

                        /* X86_DR7_GD will be cleared if DRx accesses should be trapped inside the guest. */
                        pMixedCtx->dr[7] &= ~X86_DR7_GD;

                        /* Paranoia. */
                        pMixedCtx->dr[7] &= 0xffffffff;                                             /* Upper 32 bits reserved. */
                        pMixedCtx->dr[7] &= ~(RT_BIT(11) | RT_BIT(12) | RT_BIT(14) | RT_BIT(15));   /* MBZ. */
                        pMixedCtx->dr[7] |= 0x400;                                                  /* MB1. */

                        /* Resync DR7 */
                        /** @todo probably cheaper to just reload DR7, nothing else needs changing. */
                        pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_DEBUG;

                        /* Inject #DB and get on with guest execution. */
                        rc = hmR0VmxInjectXcptDB(pVM, pVCpu, pMixedCtx);
                        AssertRCReturn(rc, rc);
                        break;
                    }
                }
            }
        }
    }

#ifdef DEBUG
    if (rc == VINF_IOM_R3_IOPORT_READ)
        Assert(!fIOWrite);
    else if (rc == VINF_IOM_R3_IOPORT_WRITE)
        Assert(fIOWrite);
    else
    {
        AssertMsg(   RT_FAILURE(rc)
                  || rc == VINF_SUCCESS
                  || rc == VINF_EM_RAW_EMULATE_INSTR
                  || rc == VINF_EM_RAW_GUEST_TRAP
                  || rc == VINF_TRPM_XCPT_DISPATCHED, ("%Rrc\n", rc));
    }
#endif

    return rc;
}


/**
 * VM-exit handler for task switches (VMX_EXIT_TASK_SWITCH). Unconditional
 * VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitTaskSwitch(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();

    /* Check if this task-switch occurred while delivery an event through the guest IDT. */
    int rc = hmR0VmxReadExitQualificationVmcs(pVmxTransient);
    AssertRCReturn(rc, rc);
    if (VMX_EXIT_QUALIFICATION_TASK_SWITCH_TYPE(pVmxTransient->uExitQualification) == VMX_EXIT_QUALIFICATION_TASK_SWITCH_TYPE_IDT)
    {
        rc = hmR0VmxReadIdtVectoringInfoVmcs(pVmxTransient);
        AssertRCReturn(rc, rc);
        if (VMX_IDT_VECTORING_INFO_VALID(pVmxTransient->uIdtVectoringInfo))
        {
            uint32_t uIntType = VMX_IDT_VECTORING_INFO_TYPE(pVmxTransient->uIdtVectoringInfo);
            if (   uIntType != VMX_IDT_VECTORING_INFO_TYPE_SW_INT
                && uIntType != VMX_IDT_VECTORING_INFO_TYPE_SW_XCPT
                && uIntType != VMX_IDT_VECTORING_INFO_TYPE_PRIV_SW_XCPT)
            {
                /* Save it as a pending event and it'll be converted to a TRPM event on the way out to ring-3. */
                pVCpu->hm.s.Event.fPending = true;
                pVCpu->hm.s.Event.u64IntrInfo = pVmxTransient->uIdtVectoringInfo;
                rc = hmR0VmxReadIdtVectoringErrorCodeVmcs(pVmxTransient);
                AssertRCReturn(rc, rc);
                if (VMX_IDT_VECTORING_INFO_ERROR_CODE_IS_VALID(pVmxTransient->uIdtVectoringErrorCode))
                    pVCpu->hm.s.Event.u32ErrCode = pVmxTransient->uIdtVectoringErrorCode;
                else
                    pVCpu->hm.s.Event.u32ErrCode = 0;
            }
        }
    }
    /** @todo Emulate task switch someday, currently just going back to ring-3 for
     *        emulation. */
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitTaskSwitch);
    return VERR_EM_INTERPRETER;
}


/**
 * VM-exit handler for monitor-trap-flag (VMX_EXIT_MTF). Conditional VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitMtf(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();
    Assert(pVCpu->hm.s.vmx.u32ProcCtls & VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_MONITOR_TRAP_FLAG);
    pVCpu->hm.s.vmx.u32ProcCtls &= ~VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_MONITOR_TRAP_FLAG;
    int rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_PROC_EXEC_CONTROLS, pVCpu->hm.s.vmx.u32ProcCtls);
    AssertRCReturn(rc, rc);
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitMtf);
    return VINF_EM_DBG_STOP;
}


/**
 * VM-exit handler for APIC access (VMX_EXIT_APIC_ACCESS). Conditional VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitApicAccess(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();
    int rc = hmR0VmxReadExitQualificationVmcs(pVmxTransient);

    /* If this VM-exit occurred while delivering an event through the guest IDT, handle it accordingly. */
    rc = hmR0VmxCheckExitDueToEventDelivery(pVM, pVCpu, pMixedCtx, pVmxTransient);
    if (RT_UNLIKELY(rc == VINF_VMX_DOUBLE_FAULT))
        return VINF_SUCCESS;
    else if (RT_UNLIKELY(rc == VINF_EM_RESET))
        return rc;

#if 0
    /** @todo Investigate if IOMMMIOPhysHandler() requires a lot of state, for now
     *   just sync the whole thing. */
    rc = hmR0VmxSaveGuestState(pVM, pVCpu, pMixedCtx);
#else
    /* Aggressive state sync. for now. */
    rc = hmR0VmxSaveGuestGprs(pVM, pVCpu, pMixedCtx);
    rc |= hmR0VmxSaveGuestControlRegs(pVM, pVCpu, pMixedCtx);
    rc |= hmR0VmxSaveGuestSegmentRegs(pVM, pVCpu, pMixedCtx);
#endif
    AssertRCReturn(rc, rc);

    /* See Intel spec. 27-6 "Exit Qualifications for APIC-access VM-exits from Linear Accesses & Guest-Phyiscal Addresses" */
    unsigned uAccessType = VMX_EXIT_QUALIFICATION_APIC_ACCESS_TYPE(pVmxTransient->uExitQualification);
    switch (uAccessType)
    {
        case VMX_APIC_ACCESS_TYPE_LINEAR_WRITE:
        case VMX_APIC_ACCESS_TYPE_LINEAR_READ:
        {
            if (  (pVCpu->hm.s.vmx.u32ProcCtls & VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_USE_TPR_SHADOW)
                && VMX_EXIT_QUALIFICATION_APIC_ACCESS_OFFSET(pVmxTransient->uExitQualification) == 0x80)
            {
                AssertMsgFailed(("hmR0VmxExitApicAccess: can't touch TPR offset while using TPR shadowing.\n"));
            }

            RTGCPHYS GCPhys = pMixedCtx->msrApicBase;   /* Always up-to-date, msrApicBase is not part of the VMCS. */
            GCPhys &= PAGE_BASE_GC_MASK;
            GCPhys += VMX_EXIT_QUALIFICATION_APIC_ACCESS_OFFSET(pVmxTransient->uExitQualification);
            VBOXSTRICTRC rc2 = IOMMMIOPhysHandler(pVM, pVCpu, (uAccessType == VMX_APIC_ACCESS_TYPE_LINEAR_READ) ? 0 : X86_TRAP_PF_RW,
                                                  CPUMCTX2CORE(pMixedCtx), GCPhys);
            rc = VBOXSTRICTRC_VAL(rc2);
            Log(("ApicAccess %RGp %#x\n", GCPhys, VMX_EXIT_QUALIFICATION_APIC_ACCESS_OFFSET(pVmxTransient->uExitQualification)));
            if (   rc == VINF_SUCCESS
                || rc == VERR_PAGE_TABLE_NOT_PRESENT
                || rc == VERR_PAGE_NOT_PRESENT)
            {
                pVCpu->hm.s.fContextUseFlags |=   HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RSP | HM_CHANGED_GUEST_RFLAGS
                                                | HM_CHANGED_VMX_GUEST_APIC_STATE;
                rc = VINF_SUCCESS;
            }
            break;
        }

        default:
            rc = VINF_EM_RAW_EMULATE_INSTR;
            break;
    }

    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitApicAccess);
    return rc;
}


/**
 * VM-exit handler for debug-register accesses (VMX_EXIT_MOV_DRX). Conditional
 * VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitMovDRx(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();

    /* We should -not- get this VM-exit if the guest is debugging. */
    if (CPUMIsGuestDebugStateActive(pVCpu))
    {
        AssertMsgFailed(("Unexpected MOV DRx exit. pVM=%p pVCpu=%p pMixedCtx=%p\n", pVM, pVCpu, pMixedCtx));
        return VERR_VMX_UNEXPECTED_EXIT_CODE;
    }

    int rc = VERR_INTERNAL_ERROR_5;
    if (   !DBGFIsStepping(pVCpu)
        && !CPUMIsHyperDebugStateActive(pVCpu))
    {
        Assert(!CPUMIsGuestDebugStateActive(pVCpu));

        /* Don't intercept MOV DRx. */
        pVCpu->hm.s.vmx.u32ProcCtls &= ~VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_MOV_DR_EXIT;
        rc = VMXWriteVmcs32(VMX_VMCS32_CTRL_PROC_EXEC_CONTROLS, pVCpu->hm.s.vmx.u32ProcCtls);
        AssertRCReturn(rc, rc);

        /* Save the host & load the guest debug state, restart execution of the MOV DRx instruction. */
        rc = CPUMR0LoadGuestDebugState(pVM, pVCpu, pMixedCtx, true /* include DR6 */);
        AssertRC(rc);
        Assert(CPUMIsGuestDebugStateActive(pVCpu));

#ifdef VBOX_WITH_STATISTICS
        rc = hmR0VmxReadExitQualificationVmcs(pVmxTransient);
        AssertRCReturn(rc, rc);
        if (VMX_EXIT_QUALIFICATION_DRX_DIRECTION(pVmxTransient->uExitQualification) == VMX_EXIT_QUALIFICATION_DRX_DIRECTION_WRITE)
            STAM_COUNTER_INC(&pVCpu->hm.s.StatExitDRxWrite);
        else
            STAM_COUNTER_INC(&pVCpu->hm.s.StatExitDRxRead);
#endif
        STAM_COUNTER_INC(&pVCpu->hm.s.StatDRxContextSwitch);
        return VINF_SUCCESS;
    }

    /** @todo clear VMX_VMCS_CTRL_PROC_EXEC_CONTROLS_MOV_DR_EXIT after the first
     *        time and restore DRx registers afterwards */
    /*
     * EMInterpretDRx[Write|Read]() calls CPUMIsGuestIn64BitCode() which requires EFER, CS. EFER is always up-to-date, see
     * hmR0VmxSaveGuestAutoLoadStoreMsrs(). Update only the segment registers from the CPU.
     */
    rc  = hmR0VmxReadExitQualificationVmcs(pVmxTransient);
    rc |= hmR0VmxSaveGuestSegmentRegs(pVM, pVCpu, pMixedCtx);
    AssertRCReturn(rc, rc);

    if (VMX_EXIT_QUALIFICATION_DRX_DIRECTION(pVmxTransient->uExitQualification) == VMX_EXIT_QUALIFICATION_DRX_DIRECTION_WRITE)
    {
        rc = EMInterpretDRxWrite(pVM, pVCpu, CPUMCTX2CORE(pMixedCtx),
                                 VMX_EXIT_QUALIFICATION_DRX_REGISTER(pVmxTransient->uExitQualification),
                                 VMX_EXIT_QUALIFICATION_DRX_GENREG(pVmxTransient->uExitQualification));
        if (RT_SUCCESS(rc))
            pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_DEBUG;
        STAM_COUNTER_INC(&pVCpu->hm.s.StatExitDRxWrite);
    }
    else
    {
        rc = EMInterpretDRxRead(pVM, pVCpu, CPUMCTX2CORE(pMixedCtx),
                                VMX_EXIT_QUALIFICATION_DRX_GENREG(pVmxTransient->uExitQualification),
                                VMX_EXIT_QUALIFICATION_DRX_REGISTER(pVmxTransient->uExitQualification));
        STAM_COUNTER_INC(&pVCpu->hm.s.StatExitDRxRead);
    }

    Assert(rc == VINF_SUCCESS || rc == VERR_EM_INTERPRETER);
    if (RT_SUCCESS(rc))
    {
        int rc2  = hmR0VmxSaveGuestRip(pVM, pVCpu, pMixedCtx);
        rc2     |= hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
        AssertRCReturn(rc2, rc2);
        pMixedCtx->rip += pVmxTransient->cbInstr;
        pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_RIP;
    }
    return rc;
}


/**
 * VM-exit handler for EPT misconfiguration (VMX_EXIT_EPT_MISCONFIG).
 * Conditional VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitEptMisconfig(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();
    Assert(pVM->hm.s.fNestedPaging);

    /* If this VM-exit occurred while delivering an event through the guest IDT, handle it accordingly. */
    int rc = hmR0VmxCheckExitDueToEventDelivery(pVM, pVCpu, pMixedCtx, pVmxTransient);
    if (RT_UNLIKELY(rc == VINF_VMX_DOUBLE_FAULT))
        return VINF_SUCCESS;
    else if (RT_UNLIKELY(rc == VINF_EM_RESET))
        return rc;

    RTGCPHYS GCPhys = 0;
    rc = VMXReadVmcs64(VMX_VMCS64_EXIT_GUEST_PHYS_ADDR_FULL, &GCPhys);

#if 0
    rc = hmR0VmxSaveGuestState(pVM, pVCpu, pMixedCtx);     /** @todo Can we do better?  */
#else
    /* Aggressive state sync. for now. */
    rc |= hmR0VmxSaveGuestGprs(pVM, pVCpu, pMixedCtx);
    rc |= hmR0VmxSaveGuestControlRegs(pVM, pVCpu, pMixedCtx);
    rc |= hmR0VmxSaveGuestSegmentRegs(pVM, pVCpu, pMixedCtx);
#endif
    AssertRCReturn(rc, rc);

    /*
     * If we succeed, resume guest execution.
     * If we fail in interpreting the instruction because we couldn't get the guest physical address
     * of the page containing the instruction via the guest's page tables (we would invalidate the guest page
     * in the host TLB), resume execution which would cause a guest page fault to let the guest handle this
     * weird case. See @bugref{6043}.
     */
    VBOXSTRICTRC rc2 = PGMR0Trap0eHandlerNPMisconfig(pVM, pVCpu, PGMMODE_EPT, CPUMCTX2CORE(pMixedCtx), GCPhys, UINT32_MAX);
    Log(("EPT misconfig at %#RX64 RIP=%#RX64 rc=%d\n", GCPhys, pMixedCtx->rip, rc));
    rc = VBOXSTRICTRC_VAL(rc2);
    if (   rc == VINF_SUCCESS
        || rc == VERR_PAGE_TABLE_NOT_PRESENT
        || rc == VERR_PAGE_NOT_PRESENT)
    {
        pVCpu->hm.s.fContextUseFlags |=   HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RSP | HM_CHANGED_GUEST_RFLAGS
                                        | HM_CHANGED_VMX_GUEST_APIC_STATE;
        return VINF_SUCCESS;
    }
    return rc;
}


/**
 * VM-exit handler for EPT violation (VMX_EXIT_EPT_VIOLATION). Conditional
 * VM-exit.
 */
static DECLCALLBACK(int) hmR0VmxExitEptViolation(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_HANDLER_PARAMS();
    Assert(pVM->hm.s.fNestedPaging);

    /* If this VM-exit occurred while delivering an event through the guest IDT, handle it accordingly. */
    int rc = hmR0VmxCheckExitDueToEventDelivery(pVM, pVCpu, pMixedCtx, pVmxTransient);
    if (RT_UNLIKELY(rc == VINF_VMX_DOUBLE_FAULT))
        return VINF_SUCCESS;
    else if (RT_UNLIKELY(rc == VINF_EM_RESET))
        return rc;

    RTGCPHYS GCPhys = 0;
    rc  = VMXReadVmcs64(VMX_VMCS64_EXIT_GUEST_PHYS_ADDR_FULL, &GCPhys);
    rc |= hmR0VmxReadExitQualificationVmcs(pVmxTransient);
#if 0
    rc |= hmR0VmxSaveGuestState(pVM, pVCpu, pMixedCtx);     /** @todo Can we do better?  */
#else
    /* Aggressive state sync. for now. */
    rc |= hmR0VmxSaveGuestGprs(pVM, pVCpu, pMixedCtx);
    rc |= hmR0VmxSaveGuestControlRegs(pVM, pVCpu, pMixedCtx);
    rc |= hmR0VmxSaveGuestSegmentRegs(pVM, pVCpu, pMixedCtx);
#endif
    AssertRCReturn(rc, rc);

    /* Intel spec. Table 27-7 "Exit Qualifications for EPT violations". */
    AssertMsg(((pVmxTransient->uExitQualification >> 7) & 3) != 2, ("%#RGv", pVmxTransient->uExitQualification));

    RTGCUINT uErrorCode = 0;
    if (pVmxTransient->uExitQualification & VMX_EXIT_QUALIFICATION_EPT_INSTR_FETCH)
        uErrorCode |= X86_TRAP_PF_ID;
    if (pVmxTransient->uExitQualification & VMX_EXIT_QUALIFICATION_EPT_DATA_WRITE)
        uErrorCode |= X86_TRAP_PF_RW;
    if (pVmxTransient->uExitQualification & VMX_EXIT_QUALIFICATION_EPT_ENTRY_PRESENT)
        uErrorCode |= X86_TRAP_PF_P;

    TRPMAssertTrap(pVCpu, X86_XCPT_PF, TRPM_TRAP);
    TRPMSetErrorCode(pVCpu, uErrorCode);
    TRPMSetFaultAddress(pVCpu, GCPhys);

    Log(("EPT violation %#x at %#RGv ErrorCode %#x CS:EIP=%04x:%#RX64\n", (uint32_t)pVmxTransient->uExitQualification, GCPhys,
         uErrorCode, pMixedCtx->cs.Sel, pMixedCtx->rip));

    /* Handle the pagefault trap for the nested shadow table. */
    rc = PGMR0Trap0eHandlerNestedPaging(pVM, pVCpu, PGMMODE_EPT, uErrorCode, CPUMCTX2CORE(pMixedCtx), GCPhys);
    TRPMResetTrap(pVCpu);

    /* Same case as PGMR0Trap0eHandlerNPMisconfig(). See comment above, @bugref{6043}. */
    if (   rc == VINF_SUCCESS
        || rc == VERR_PAGE_TABLE_NOT_PRESENT
        || rc == VERR_PAGE_NOT_PRESENT)
    {
        /* Successfully synced our shadow page tables or emulation MMIO instruction. */
        STAM_COUNTER_INC(&pVCpu->hm.s.StatExitReasonNpf);
        pVCpu->hm.s.fContextUseFlags |=  HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RSP | HM_CHANGED_GUEST_RFLAGS
                                       | HM_CHANGED_VMX_GUEST_APIC_STATE;
        return VINF_SUCCESS;
    }

    Log(("EPT return to ring-3 rc=%d\n"));
    return rc;
}


/* -=-=-=-=-=-=-=-=--=-=-=-=-=-=-=-=-=-=-=--=-=-=-=-=-=-=-=-=-=-=-=-=-= */
/* -=-=-=-=-=-=-=-=-=- VM-exit Exception Handlers -=-=-=-=-=-=-=-=-=-=- */
/* -=-=-=-=-=-=-=-=--=-=-=-=-=-=-=-=-=-=-=--=-=-=-=-=-=-=-=-=-=-=-=-=-= */
/**
 * VM-exit exception handler for #MF (Math Fault: floating point exception).
 */
static DECLCALLBACK(int) hmR0VmxExitXcptMF(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_XCPT_HANDLER_PARAMS();

    int rc = hmR0VmxSaveGuestCR0(pVM, pVCpu, pMixedCtx);
    AssertRCReturn(rc, rc);
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitGuestMF);

    if (!(pMixedCtx->cr0 & X86_CR0_NE))
    {
        /* Old-style FPU error reporting needs some extra work. */
        /** @todo don't fall back to the recompiler, but do it manually. */
        return VERR_EM_INTERPRETER;
    }
    rc = hmR0VmxInjectEventVmcs(pVM, pVCpu, pMixedCtx,
                                 VMX_VMCS_CTRL_ENTRY_IRQ_INFO_FROM_EXIT_INT_INFO(pVmxTransient->uExitIntrInfo),
                                 pVmxTransient->cbInstr, pVmxTransient->uExitIntrErrorCode);
    AssertRCReturn(rc, rc);
    return rc;
}


/**
 * VM-exit exception handler for #BP (Breakpoint exception).
 */
static DECLCALLBACK(int) hmR0VmxExitXcptBP(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_XCPT_HANDLER_PARAMS();

    /** @todo Try optimize this by not saving the entire guest state unless
     *        really needed. */
    int rc = hmR0VmxSaveGuestState(pVM, pVCpu, pMixedCtx);
    AssertRCReturn(rc, rc);
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitGuestBP);

    rc = DBGFRZTrap03Handler(pVM, pVCpu, CPUMCTX2CORE(pMixedCtx));
    if (rc == VINF_EM_RAW_GUEST_TRAP)
    {
        rc  = hmR0VmxReadExitIntrInfoVmcs(pVmxTransient);
        rc |= hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
        rc |= hmR0VmxReadExitIntrErrorCodeVmcs(pVmxTransient);
        AssertRCReturn(rc, rc);

        rc = hmR0VmxInjectEventVmcs(pVM, pVCpu, pMixedCtx,
                                    VMX_VMCS_CTRL_ENTRY_IRQ_INFO_FROM_EXIT_INT_INFO(pVmxTransient->uExitIntrInfo),
                                    pVmxTransient->cbInstr, pVmxTransient->uExitIntrErrorCode);
        AssertRCReturn(rc, rc);
    }

    Assert(rc == VINF_SUCCESS || rc == VINF_EM_RESET);
    return rc;
}


/**
 * VM-exit exception handler for #DB (Debug exception).
 */
static DECLCALLBACK(int) hmR0VmxExitXcptDB(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_XCPT_HANDLER_PARAMS();

    int rc = hmR0VmxReadExitQualificationVmcs(pVmxTransient);
    rc    |= hmR0VmxSaveGuestSegmentRegs(pVM, pVCpu, pMixedCtx);
    rc    |= hmR0VmxSaveGuestRflags(pVM, pVCpu, pMixedCtx);
    AssertRCReturn(rc, rc);

    /* Refer Intel spec. Table 27-1. "Exit Qualifications for debug exceptions" for the format. */
    uint64_t uDR6 = X86_DR6_INIT_VAL;
    uDR6         |= (pVmxTransient->uExitQualification
                     & (X86_DR6_B0 | X86_DR6_B1 | X86_DR6_B2 | X86_DR6_B3 | X86_DR6_BD | X86_DR6_BS));
    rc = DBGFRZTrap01Handler(pVM, pVCpu, CPUMCTX2CORE(pMixedCtx), uDR6);
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitGuestDB);
    if (rc == VINF_EM_RAW_GUEST_TRAP)
    {
        /* DR6, DR7.GD and IA32_DEBUGCTL.LBR are not updated yet. See Intel spec. 27.1 "Architectural State before a VM-Exit". */
        pMixedCtx->dr[6] = uDR6;

        if (CPUMIsGuestDebugStateActive(pVCpu))
            ASMSetDR6(pMixedCtx->dr[6]);

        /* X86_DR7_GD will be cleared if DRx accesses should be trapped inside the guest. */
        pMixedCtx->dr[7] &= ~X86_DR7_GD;

        /* Paranoia. */
        pMixedCtx->dr[7] &= 0xffffffff;                                              /* upper 32 bits reserved */
        pMixedCtx->dr[7] &= ~(RT_BIT(11) | RT_BIT(12) | RT_BIT(14) | RT_BIT(15));    /* must be zero */
        pMixedCtx->dr[7] |= 0x400;                                                   /* must be one */

        /* Resync DR7. */
        rc = VMXWriteVmcsGstN(VMX_VMCS_GUEST_DR7, pMixedCtx->dr[7]);

        rc |= hmR0VmxReadExitIntrInfoVmcs(pVmxTransient);
        rc |= hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
        rc |= hmR0VmxReadExitIntrErrorCodeVmcs(pVmxTransient);
        rc |= hmR0VmxInjectEventVmcs(pVM, pVCpu, pMixedCtx,
                                 VMX_VMCS_CTRL_ENTRY_IRQ_INFO_FROM_EXIT_INT_INFO(pVmxTransient->uExitIntrInfo),
                                 pVmxTransient->cbInstr, pVmxTransient->uExitIntrErrorCode);
        AssertRCReturn(rc,rc);
        return rc;
    }
    /* Return to ring 3 to deal with the debug exit code. */
    return rc;
}


/**
 * VM-exit exception handler for #NM (Device-not-available exception: floating
 * point exception).
 */
static DECLCALLBACK(int) hmR0VmxExitXcptNM(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_XCPT_HANDLER_PARAMS();

#ifndef VBOX_ALWAYS_TRAP_ALL_EXCEPTIONS
    Assert(!CPUMIsGuestFPUStateActive(pVCpu));
#endif

    /* We require CR0 and EFER. EFER is always up-to-date. */
    int rc = hmR0VmxSaveGuestControlRegs(pVM, pVCpu, pMixedCtx);
    AssertRCReturn(rc, rc);

    /* Lazy FPU loading; Load the guest-FPU state transparently and continue execution of the guest. */
    rc = CPUMR0LoadGuestFPU(pVM, pVCpu, pMixedCtx);
    if (rc == VINF_SUCCESS)
    {
        Assert(CPUMIsGuestFPUStateActive(pVCpu));
        pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_CR0;
        STAM_COUNTER_INC(&pVCpu->hm.s.StatExitShadowNM);
        return VINF_SUCCESS;
    }

    /* Forward #NM to the guest. */
    Assert(rc == VINF_EM_RAW_GUEST_TRAP);
    rc = hmR0VmxReadExitIntrInfoVmcs(pVmxTransient);
    AssertRCReturn(rc, rc);
    rc = hmR0VmxInjectEventVmcs(pVM, pVCpu, pMixedCtx,
                                 VMX_VMCS_CTRL_ENTRY_IRQ_INFO_FROM_EXIT_INT_INFO(pVmxTransient->uExitIntrInfo),
                                 pVmxTransient->cbInstr, 0 /* error code */);
    AssertRCReturn(rc, rc);
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitGuestNM);
    return rc;
}


/**
 * VM-exit exception handler for #GP (General-protection exception).
 *
 * @remarks Requires pVmxTransient->uExitIntrInfo to be up-to-date.
 */
static DECLCALLBACK(int) hmR0VmxExitXcptGP(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_XCPT_HANDLER_PARAMS();

    int rc = VERR_INTERNAL_ERROR_5;

    if (!pVCpu->hm.s.vmx.RealMode.fRealOnV86Active)
    {
#ifdef VBOX_ALWAYS_TRAP_ALL_EXCEPTIONS
        /* If the guest is not in real-mode or we have unrestricted execution support, reflect #GP to the guest. */
        rc  = hmR0VmxReadExitIntrInfoVmcs(pVmxTransient);
        rc |= hmR0VmxReadExitIntrErrorCodeVmcs(pVmxTransient);
        rc |= hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
        rc |= hmR0VmxInjectEventVmcs(pVM, pVCpu, pMixedCtx,
                                        VMX_VMCS_CTRL_ENTRY_IRQ_INFO_FROM_EXIT_INT_INFO(pVmxTransient->uExitIntrInfo),
                                        pVmxTransient->cbInstr, pVmxTransient->uExitIntrErrorCode);
        AssertRCReturn(rc, rc);
        return rc;
#else
        /* We don't intercept #GP. */
        AssertMsgFailed(("Unexpected VM-exit caused by #GP exception\n"));
        return VERR_VMX_UNEXPECTED_EXCEPTION;
#endif
    }

    Assert(CPUMIsGuestInRealModeEx(pMixedCtx));
    Assert(!pVM->hm.s.vmx.fUnrestrictedGuest);

    /* EMInterpretDisasCurrent() requires a lot of the state, save the entire state. */
    rc = hmR0VmxSaveGuestState(pVM, pVCpu, pMixedCtx);
    AssertRCReturn(rc, rc);

    PDISCPUSTATE pDis = &pVCpu->hm.s.DisState;
    unsigned int cbOp = 0;
    rc = EMInterpretDisasCurrent(pVM, pVCpu, pDis, &cbOp);
    if (RT_SUCCESS(rc))
    {
        rc = VINF_SUCCESS;
        Assert(cbOp == pDis->cbInstr);
        Log2(("#GP Disas OpCode=%u CS:EIP %04x:%#RX64\n", pDis->pCurInstr->uOpcode, pMixedCtx->cs.Sel, pMixedCtx->rip));
        switch (pDis->pCurInstr->uOpcode)
        {
            case OP_CLI:
                pMixedCtx->eflags.Bits.u1IF = 0;
                pMixedCtx->rip += pDis->cbInstr;
                pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RFLAGS;
                STAM_COUNTER_INC(&pVCpu->hm.s.StatExitCli);
                break;

            case OP_STI:
                pMixedCtx->eflags.Bits.u1IF = 1;
                pMixedCtx->rip += pDis->cbInstr;
                EMSetInhibitInterruptsPC(pVCpu, pMixedCtx->rip);
                Assert(VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_INHIBIT_INTERRUPTS));
                pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RFLAGS;
                STAM_COUNTER_INC(&pVCpu->hm.s.StatExitSti);
                break;

            case OP_HLT:
                rc = VINF_EM_HALT;
                pMixedCtx->rip += pDis->cbInstr;
                pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_RIP;
                STAM_COUNTER_INC(&pVCpu->hm.s.StatExitHlt);
                break;

            case OP_POPF:
            {
                Log(("POPF CS:RIP %04x:%#RX64\n", pMixedCtx->cs.Sel, pMixedCtx->rip));
                size_t   cbParm = 0;
                uint32_t uMask  = 0;
                if (pDis->fPrefix & DISPREFIX_OPSIZE)
                {
                    cbParm = 4;
                    uMask  = 0xffffffff;
                }
                else
                {
                    cbParm = 2;
                    uMask  = 0xffff;
                }

                /* Get the stack pointer & pop the contents of the stack onto EFlags. */
                RTGCPTR   GCPtrStack = 0;
                X86EFLAGS uEflags;
                rc = SELMToFlatEx(pVCpu, DISSELREG_SS, CPUMCTX2CORE(pMixedCtx), pMixedCtx->esp & uMask, SELMTOFLAT_FLAGS_CPL0,
                                  &GCPtrStack);
                if (RT_SUCCESS(rc))
                {
                    Assert(sizeof(uEflags.u32) >= cbParm);
                    uEflags.u32 = 0;
                    rc = PGMPhysRead(pVM, (RTGCPHYS)GCPtrStack, &uEflags.u32, cbParm);
                }
                if (RT_FAILURE(rc))
                {
                    rc = VERR_EM_INTERPRETER;
                    break;
                }
                Log(("POPF %x -> %RGv mask=%x RIP=%#RX64\n", uEflags.u, pMixedCtx->rsp, uMask, pMixedCtx->rip));
                pMixedCtx->eflags.u32 =   (pMixedCtx->eflags.u32 & ~(X86_EFL_POPF_BITS & uMask))
                                        | (uEflags.u32 & X86_EFL_POPF_BITS & uMask);
                /* The RF bit is always cleared by POPF; see Intel Instruction reference for POPF. */
                pMixedCtx->eflags.Bits.u1RF   = 0;
                pMixedCtx->esp               += cbParm;
                pMixedCtx->esp               &= uMask;
                pMixedCtx->rip               += pDis->cbInstr;
                pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RSP | HM_CHANGED_GUEST_RFLAGS;
                STAM_COUNTER_INC(&pVCpu->hm.s.StatExitPopf);
                break;
            }

            case OP_PUSHF:
            {
                size_t   cbParm = 0;
                uint32_t uMask  = 0;
                if (pDis->fPrefix & DISPREFIX_OPSIZE)
                {
                    cbParm = 4;
                    uMask  = 0xffffffff;
                }
                else
                {
                    cbParm = 2;
                    uMask  = 0xffff;
                }

                /* Get the stack pointer & push the contents of eflags onto the stack. */
                RTGCPTR GCPtrStack = 0;
                rc = SELMToFlatEx(pVCpu, DISSELREG_SS, CPUMCTX2CORE(pMixedCtx), (pMixedCtx->esp - cbParm) & uMask,
                                  SELMTOFLAT_FLAGS_CPL0, &GCPtrStack);
                if (RT_FAILURE(rc))
                {
                    rc = VERR_EM_INTERPRETER;
                    break;
                }
                X86EFLAGS uEflags;
                uEflags = pMixedCtx->eflags;
                /* The RF & VM bits are cleared on image stored on stack; see Intel Instruction reference for PUSHF. */
                uEflags.Bits.u1RF = 0;
                uEflags.Bits.u1VM = 0;

                rc = PGMPhysWrite(pVM, (RTGCPHYS)GCPtrStack, &uEflags.u, cbParm);
                if (RT_FAILURE(rc))
                {
                    rc = VERR_EM_INTERPRETER;
                    break;
                }
                Log(("PUSHF %x -> %RGv\n", uEflags.u, GCPtrStack));
                pMixedCtx->esp               -= cbParm;
                pMixedCtx->esp               &= uMask;
                pMixedCtx->rip               += pDis->cbInstr;
                pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RSP;
                STAM_COUNTER_INC(&pVCpu->hm.s.StatExitPushf);
                break;
            }

            case OP_IRET:
            {
                /** @todo Handle 32-bit operand sizes and check stack limits. See Intel
                 *        instruction reference. */
                RTGCPTR  GCPtrStack = 0;
                uint32_t uMask      = 0xffff;
                uint16_t aIretFrame[3];
                if (pDis->fPrefix & (DISPREFIX_OPSIZE | DISPREFIX_ADDRSIZE))
                {
                    rc = VERR_EM_INTERPRETER;
                    break;
                }
                rc = SELMToFlatEx(pVCpu, DISSELREG_SS, CPUMCTX2CORE(pMixedCtx), pMixedCtx->esp & uMask, SELMTOFLAT_FLAGS_CPL0,
                                  &GCPtrStack);
                if (RT_SUCCESS(rc))
                    rc = PGMPhysRead(pVM, (RTGCPHYS)GCPtrStack, &aIretFrame[0], sizeof(aIretFrame));
                if (RT_FAILURE(rc))
                {
                    rc = VERR_EM_INTERPRETER;
                    break;
                }
                pMixedCtx->eip                = 0;
                pMixedCtx->ip                 = aIretFrame[0];
                pMixedCtx->cs.Sel             = aIretFrame[1];
                pMixedCtx->cs.ValidSel        = aIretFrame[1];
                pMixedCtx->cs.u64Base         = (uint64_t)pMixedCtx->cs.Sel << 4;
                pMixedCtx->eflags.u32         = (pMixedCtx->eflags.u32 & ~(X86_EFL_POPF_BITS & uMask))
                                                | (aIretFrame[2] & X86_EFL_POPF_BITS & uMask);
                pMixedCtx->sp                += sizeof(aIretFrame);
                pVCpu->hm.s.fContextUseFlags |=   HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_SEGMENT_REGS | HM_CHANGED_GUEST_RSP
                                                | HM_CHANGED_GUEST_RFLAGS;
                Log(("IRET %#RX32 to %04x:%x\n", GCPtrStack, pMixedCtx->cs.Sel, pMixedCtx->ip));
                STAM_COUNTER_INC(&pVCpu->hm.s.StatExitIret);
                break;
            }

            case OP_INT:
            {
                uint16_t uVector = pDis->Param1.uValue & 0xff;
                rc = hmR0VmxInjectIntN(pVM, pVCpu, pMixedCtx, uVector, pDis->cbInstr);
                AssertRCReturn(rc, rc);
                STAM_COUNTER_INC(&pVCpu->hm.s.StatExitInt);
                break;
            }

            case OP_INTO:
            {
                if (pMixedCtx->eflags.Bits.u1OF)
                {
                    rc = hmR0VmxInjectXcptOF(pVM, pVCpu, pMixedCtx, pDis->cbInstr);
                    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitInt);
                }
                break;
            }

            default:
            {
                VBOXSTRICTRC rc2 = EMInterpretInstructionDisasState(pVCpu, pDis, CPUMCTX2CORE(pMixedCtx), 0 /* pvFault */,
                                                                    EMCODETYPE_SUPERVISOR);
                rc = VBOXSTRICTRC_VAL(rc2);
                pVCpu->hm.s.fContextUseFlags |= HM_CHANGED_ALL_GUEST;
                Log2(("#GP rc=%Rrc\n", rc));
                break;
            }
        }
    }
    else
        rc = VERR_EM_INTERPRETER;

    AssertMsg(rc == VINF_SUCCESS || rc == VERR_EM_INTERPRETER || rc == VINF_PGM_CHANGE_MODE || rc == VINF_EM_HALT
              || rc == VINF_EM_RESET /* injection caused triple fault */,
              ("#GP Unexpected rc=%Rrc\n", rc));
    return rc;
}


/**
 * VM-exit exception handler wrapper for generic exceptions. Simply re-injects
 * the exception reported in the VMX transient structure back into the VM.
 *
 * @remarks Requires uExitIntrInfo, uExitIntrErrorCode, cbInstr fields in the
 *          VMX transient structure to be up-to-date.
 */
static DECLCALLBACK(int) hmR0VmxExitXcptGeneric(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_XCPT_HANDLER_PARAMS();

    /* Re-inject the exception into the guest. This cannot be a double-fault condition which would have been handled in
       hmR0VmxCheckExitDueToEventDelivery(). */
    int rc = hmR0VmxInjectEventVmcs(pVM, pVCpu, pMixedCtx,
                                    VMX_VMCS_CTRL_ENTRY_IRQ_INFO_FROM_EXIT_INT_INFO(pVmxTransient->uExitIntrInfo),
                                    pVmxTransient->cbInstr, pVmxTransient->uExitIntrErrorCode);
    AssertRCReturn(rc, rc);
    return rc;
}


/**
 * VM-exit exception handler for #PF (Page-fault exception).
 */
static DECLCALLBACK(int) hmR0VmxExitXcptPF(PVM pVM, PVMCPU pVCpu, PCPUMCTX pMixedCtx, PVMXTRANSIENT pVmxTransient)
{
    VMX_VALIDATE_EXIT_XCPT_HANDLER_PARAMS();

    int rc = hmR0VmxReadExitQualificationVmcs(pVmxTransient);
    rc    |= hmR0VmxReadExitIntrInfoVmcs(pVmxTransient);
    rc    |= hmR0VmxReadExitInstrLenVmcs(pVmxTransient);
    rc    |= hmR0VmxReadExitIntrErrorCodeVmcs(pVmxTransient);
    AssertRCReturn(rc, rc);

#if defined(VBOX_ALWAYS_TRAP_ALL_EXCEPTIONS) || defined(VBOX_ALWAYS_TRAP_PF)
    if (pVM->hm.s.fNestedPaging)
    {
        if (RT_LIKELY(!pVmxTransient->fVectoringPF))
        {
            pMixedCtx->cr2 = pVmxTransient->uExitQualification;
            rc = hmR0VmxInjectEventVmcs(pVM, pVCpu, pMixedCtx,
                                        VMX_VMCS_CTRL_ENTRY_IRQ_INFO_FROM_EXIT_INT_INFO(pVmxTransient->uExitIntrInfo),
                                        pVmxTransient->cbInstr, pVmxTransient->uExitIntrErrorCode);
            AssertRCReturn(rc, rc);
        }
        else
        {
            /* A guest page-fault occurred during delivery of a page-fault. Inject #DF. */
            Assert(!pVCpu->hm.s.Event.fPending);
            rc = hmR0VmxInjectXcptDF(pVM, pVCpu, pMixedCtx);
        }
        STAM_COUNTER_INC(&pVCpu->hm.s.StatExitGuestPF);
        return rc;
    }
#else
    Assert(!pVM->hm.s.fNestedPaging);
#endif

#ifdef VBOX_HM_WITH_GUEST_PATCHING
    rc  = hmR0VmxSaveGuestControlRegs(pVM, pVCpu, pMixedCtx);
    rc |= hmR0VmxSaveGuestSegmentRegs(pVM, pVCpu, pMixedCtx);
    rc |= hmR0VmxSaveGuestRflags(pVM, pVCpu, pMixedCtx);
    AssertRCReturn(rc, rc);
    /* Shortcut for APIC TPR access, only for 32-bit guests. */
    if (   pVM->hm.s.fTRPPatchingAllowed
        && pVM->hm.s.pGuestPatchMem
        && (pVmxTransient->uExitQualification & 0xfff) == 0x80     /* TPR offset */
        && !(pVmxTransient->uExitIntrErrorCode & X86_TRAP_PF_P)    /* Page not present */
        && CPUMGetGuestCPL(pVCpu) == 0                             /* Requires CR0, EFLAGS, segments. */
        && !CPUMIsGuestInLongModeEx(pMixedCtx)                     /* Requires EFER. */
        && pVM->hm.s.cPatches < RT_ELEMENTS(pVM->hm.s.aPatches))
    {
        RTGCPHYS GCPhys;
        RTGCPHYS GCPhysApicBase = (pMixedCtx->msrApicBase & PAGE_BASE_GC_MASK);
        rc = PGMGstGetPage(pVCpu, (RTGCPTR)pVmxTransient->uExitQualification, NULL /* pfFlags */, &GCPhys);
        if (    rc == VINF_SUCCESS
            &&  GCPhys == GCPhysApicBase)
        {
            rc = hmR0VmxSaveGuestRip(pVM, pVCpu, pMixedCtx);
            AssertRCReturn(rc, rc);

            /* Only attempt to patch the instruction once. */
            PHMTPRPATCH pPatch = (PHMTPRPATCH)RTAvloU32Get(&pVM->hm.s.PatchTree, (AVLOU32KEY)pMixedCtx->eip);
            if (!pPatch)
                return VINF_EM_HM_PATCH_TPR_INSTR;
        }
    }
#endif

    TRPMAssertTrap(pVCpu, X86_XCPT_PF, TRPM_TRAP);
    TRPMSetFaultAddress(pVCpu, pVmxTransient->uExitQualification);
    TRPMSetErrorCode(pVCpu, pVmxTransient->uExitIntrErrorCode);

    rc = hmR0VmxSaveGuestState(pVM, pVCpu, pMixedCtx);
    AssertRCReturn(rc, rc);

    /* Forward it to the trap handler first. */
    rc = PGMTrap0eHandler(pVCpu, pVmxTransient->uExitIntrErrorCode, CPUMCTX2CORE(pMixedCtx),
                          (RTGCPTR)pVmxTransient->uExitQualification);

    Log(("#PF: cr2=%RGv cs:rip=%04x:%RGv errorcode %#RX32 rc=%d\n", pVmxTransient->uExitQualification, pMixedCtx->cs.Sel,
         pMixedCtx->rip, pVmxTransient->uExitIntrErrorCode, rc));

    if (rc == VINF_SUCCESS)
    {
        /* Successfully synced shadow pages tables or emulated an MMIO instruction. */
        /** @todo this isn't quite right, what if guest does lgdt with some MMIO
         *        memory? We don't update the whole state here... */
        pVCpu->hm.s.fContextUseFlags |=   HM_CHANGED_GUEST_RIP | HM_CHANGED_GUEST_RSP | HM_CHANGED_GUEST_RFLAGS
                                        | HM_CHANGED_VMX_GUEST_APIC_STATE;

        TRPMResetTrap(pVCpu);
        STAM_COUNTER_INC(&pVCpu->hm.s.StatExitShadowPF);
        return rc;
    }
    else if (rc == VINF_EM_RAW_GUEST_TRAP)
    {
        if (RT_LIKELY(!pVmxTransient->fVectoringPF))
        {
            /* It's a guest page fault and needs to be reflected to the guest. */
            uint32_t uGstErrorCode = TRPMGetErrorCode(pVCpu);
            TRPMResetTrap(pVCpu);
            pMixedCtx->cr2 = pVmxTransient->uExitQualification;
            rc = hmR0VmxInjectEventVmcs(pVM, pVCpu, pMixedCtx,
                                        VMX_VMCS_CTRL_ENTRY_IRQ_INFO_FROM_EXIT_INT_INFO(pVmxTransient->uExitIntrInfo),
                                        pVmxTransient->cbInstr, uGstErrorCode);
            AssertRCReturn(rc, rc);
        }
        else
        {
            /* A guest page-fault occurred during delivery of a page-fault. Inject #DF. */
            Assert(!pVCpu->hm.s.Event.fPending);
            TRPMResetTrap(pVCpu);
            rc = hmR0VmxInjectXcptDF(pVM, pVCpu, pMixedCtx);
        }
        STAM_COUNTER_INC(&pVCpu->hm.s.StatExitGuestPF);
        Assert(rc == VINF_SUCCESS || rc == VINF_EM_RESET);
        return rc;
    }

    TRPMResetTrap(pVCpu);
    STAM_COUNTER_INC(&pVCpu->hm.s.StatExitShadowPFEM);
    return rc;
}

