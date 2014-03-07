/* $Id$ */
/** @file
 * VirtualBox Main - Guest session handling.
 */

/*
 * Copyright (C) 2012-2013 Oracle Corporation
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
#include "GuestImpl.h"
#include "GuestSessionImpl.h"
#include "GuestCtrlImplPrivate.h"
#include "VirtualBoxErrorInfoImpl.h"

#include "Global.h"
#include "AutoCaller.h"
#include "ProgressImpl.h"
#include "VBoxEvents.h"
#include "VMMDev.h"

#include <memory> /* For auto_ptr. */

#include <iprt/cpp/utils.h> /* For unconst(). */
#include <iprt/env.h>
#include <iprt/file.h> /* For CopyTo/From. */

#include <VBox/com/array.h>
#include <VBox/com/listeners.h>
#include <VBox/version.h>

#ifdef LOG_GROUP
 #undef LOG_GROUP
#endif
#define LOG_GROUP LOG_GROUP_GUEST_CONTROL
#include <VBox/log.h>


/**
 * Base class representing an internal
 * asynchronous session task.
 */
class GuestSessionTaskInternal
{
public:

    GuestSessionTaskInternal(GuestSession *pSession)
        : mSession(pSession),
          mRC(VINF_SUCCESS) { }

    virtual ~GuestSessionTaskInternal(void) { }

    int rc(void) const { return mRC; }
    bool isOk(void) const { return RT_SUCCESS(mRC); }
    const ComObjPtr<GuestSession> &Session(void) const { return mSession; }

protected:

    const ComObjPtr<GuestSession>    mSession;
    int                              mRC;
};

/**
 * Class for asynchronously opening a guest session.
 */
class GuestSessionTaskInternalOpen : public GuestSessionTaskInternal
{
public:

    GuestSessionTaskInternalOpen(GuestSession *pSession)
        : GuestSessionTaskInternal(pSession) { }
};

/**
 * Internal listener class to serve events in an
 * active manner, e.g. without polling delays.
 */
class GuestSessionListener
{
public:

    GuestSessionListener(void)
    {
    }

    HRESULT init(GuestSession *pSession)
    {
        AssertPtrReturn(pSession, E_POINTER);
        mSession = pSession;
        return S_OK;
    }

    void uninit(void)
    {
        mSession = NULL;
    }

    STDMETHOD(HandleEvent)(VBoxEventType_T aType, IEvent *aEvent)
    {
        switch (aType)
        {
            case VBoxEventType_OnGuestSessionStateChanged:
            {
                AssertPtrReturn(mSession, E_POINTER);
                int rc2 = mSession->signalWaitEvent(aType, aEvent);
#ifdef DEBUG_andy
                LogFlowFunc(("Signalling events of type=%RU32, session=%p resulted in rc=%Rrc\n",
                             aType, mSession, rc2));
#endif
                break;
            }

            default:
                AssertMsgFailed(("Unhandled event %RU32\n", aType));
                break;
        }

        return S_OK;
    }

private:

    GuestSession *mSession;
};
typedef ListenerImpl<GuestSessionListener, GuestSession*> GuestSessionListenerImpl;

VBOX_LISTENER_DECLARE(GuestSessionListenerImpl)

// constructor / destructor
/////////////////////////////////////////////////////////////////////////////

DEFINE_EMPTY_CTOR_DTOR(GuestSession)

HRESULT GuestSession::FinalConstruct(void)
{
    LogFlowThisFuncEnter();
    return BaseFinalConstruct();
}

void GuestSession::FinalRelease(void)
{
    LogFlowThisFuncEnter();
    uninit();
    BaseFinalRelease();
    LogFlowThisFuncLeave();
}

// public initializer/uninitializer for internal purposes only
/////////////////////////////////////////////////////////////////////////////

/**
 * Initializes a guest session but does *not* open in on the guest side
 * yet. This needs to be done via the openSession() / openSessionAsync calls.
 *
 * @return  IPRT status code.
 ** @todo Docs!
 */
int GuestSession::init(Guest *pGuest, const GuestSessionStartupInfo &ssInfo,
                       const GuestCredentials &guestCreds)
{
    LogFlowThisFunc(("pGuest=%p, ssInfo=%p, guestCreds=%p\n",
                      pGuest, &ssInfo, &guestCreds));

    /* Enclose the state transition NotReady->InInit->Ready. */
    AutoInitSpan autoInitSpan(this);
    AssertReturn(autoInitSpan.isOk(), VERR_OBJECT_DESTROYED);

#ifndef VBOX_WITH_GUEST_CONTROL
    autoInitSpan.setSucceeded();
    return VINF_SUCCESS;
#else
    AssertPtrReturn(pGuest, VERR_INVALID_POINTER);

    mParent = pGuest;

    /* Copy over startup info. */
    /** @todo Use an overloaded copy operator. Later. */
    mData.mSession.mID = ssInfo.mID;
    mData.mSession.mIsInternal = ssInfo.mIsInternal;
    mData.mSession.mName = ssInfo.mName;
    mData.mSession.mOpenFlags = ssInfo.mOpenFlags;
    mData.mSession.mOpenTimeoutMS = ssInfo.mOpenTimeoutMS;

    /** @todo Use an overloaded copy operator. Later. */
    mData.mCredentials.mUser = guestCreds.mUser;
    mData.mCredentials.mPassword = guestCreds.mPassword;
    mData.mCredentials.mDomain = guestCreds.mDomain;

    mData.mRC = VINF_SUCCESS;
    mData.mStatus = GuestSessionStatus_Undefined;
    mData.mNumObjects = 0;

    HRESULT hr;

    int rc = i_queryInfo();
    if (RT_SUCCESS(rc))
    {
        hr = unconst(mEventSource).createObject();
        if (FAILED(hr))
            rc = VERR_NO_MEMORY;
        else
        {
            hr = mEventSource->init();
            if (FAILED(hr))
                rc = VERR_COM_UNEXPECTED;
        }
    }

    if (RT_SUCCESS(rc))
    {
        try
        {
            GuestSessionListener *pListener = new GuestSessionListener();
            ComObjPtr<GuestSessionListenerImpl> thisListener;
            hr = thisListener.createObject();
            if (SUCCEEDED(hr))
                hr = thisListener->init(pListener, this);

            if (SUCCEEDED(hr))
            {
                com::SafeArray <VBoxEventType_T> eventTypes;
                eventTypes.push_back(VBoxEventType_OnGuestSessionStateChanged);
                hr = mEventSource->RegisterListener(thisListener,
                                                    ComSafeArrayAsInParam(eventTypes),
                                                    TRUE /* Active listener */);
                if (SUCCEEDED(hr))
                {
                    mLocalListener = thisListener;

                    rc = RTCritSectInit(&mWaitEventCritSect);
                    AssertRC(rc);
                }
                else
                    rc = VERR_COM_UNEXPECTED;
            }
            else
                rc = VERR_COM_UNEXPECTED;
        }
        catch(std::bad_alloc &)
        {
            rc = VERR_NO_MEMORY;
        }
    }

    if (RT_SUCCESS(rc))
    {
        /* Confirm a successful initialization when it's the case. */
        autoInitSpan.setSucceeded();
    }
    else
        autoInitSpan.setFailed();

    LogFlowThisFunc(("mName=%s, mID=%RU32, mIsInternal=%RTbool, rc=%Rrc\n",
                     mData.mSession.mName.c_str(), mData.mSession.mID, mData.mSession.mIsInternal, rc));
    return rc;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

/**
 * Uninitializes the instance.
 * Called from FinalRelease().
 */
void GuestSession::uninit(void)
{
    /* Enclose the state transition Ready->InUninit->NotReady. */
    AutoUninitSpan autoUninitSpan(this);
    if (autoUninitSpan.uninitDone())
        return;

    LogFlowThisFuncEnter();

    int rc = VINF_SUCCESS;

#ifdef VBOX_WITH_GUEST_CONTROL
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    LogFlowThisFunc(("Closing directories (%zu total)\n",
                     mData.mDirectories.size()));
    for (SessionDirectories::iterator itDirs = mData.mDirectories.begin();
         itDirs != mData.mDirectories.end(); ++itDirs)
    {
        Assert(mData.mNumObjects);
        mData.mNumObjects--;
        itDirs->second->i_onRemove();
        itDirs->second->uninit();
    }
    mData.mDirectories.clear();

    LogFlowThisFunc(("Closing files (%zu total)\n",
                     mData.mFiles.size()));
    for (SessionFiles::iterator itFiles = mData.mFiles.begin();
         itFiles != mData.mFiles.end(); ++itFiles)
    {
        Assert(mData.mNumObjects);
        mData.mNumObjects--;
        itFiles->second->i_onRemove();
        itFiles->second->uninit();
    }
    mData.mFiles.clear();

    LogFlowThisFunc(("Closing processes (%zu total)\n",
                     mData.mProcesses.size()));
    for (SessionProcesses::iterator itProcs = mData.mProcesses.begin();
         itProcs != mData.mProcesses.end(); ++itProcs)
    {
        Assert(mData.mNumObjects);
        mData.mNumObjects--;
        itProcs->second->i_onRemove();
        itProcs->second->uninit();
    }
    mData.mProcesses.clear();

    AssertMsg(mData.mNumObjects == 0,
              ("mNumObjects=%RU32 when it should be 0\n", mData.mNumObjects));

    baseUninit();
#endif /* VBOX_WITH_GUEST_CONTROL */
    LogFlowFuncLeaveRC(rc);
}

// implementation of public getters/setters for attributes
/////////////////////////////////////////////////////////////////////////////

HRESULT GuestSession::getUser(com::Utf8Str &aUser)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else

    LogFlowThisFuncEnter();

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    aUser = mData.mCredentials.mUser;

    LogFlowThisFuncLeave();
    return S_OK;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::getDomain(com::Utf8Str &aDomain)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    aDomain = mData.mCredentials.mDomain;

    LogFlowThisFuncLeave();
    return S_OK;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::getName(com::Utf8Str &aName)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    aName = mData.mSession.mName;

    LogFlowThisFuncLeave();
    return S_OK;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::getId(ULONG *aId)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    *aId = mData.mSession.mID;

    LogFlowThisFuncLeave();
    return S_OK;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::getStatus(GuestSessionStatus_T *aStatus)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    *aStatus = mData.mStatus;

    LogFlowThisFuncLeave();
    return S_OK;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::getTimeout(ULONG *aTimeout)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    *aTimeout = mData.mTimeout;

    LogFlowThisFuncLeave();
    return S_OK;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::setTimeout(ULONG aTimeout)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    mData.mTimeout = aTimeout;

    LogFlowThisFuncLeave();
    return S_OK;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::getProtocolVersion(ULONG *aProtocolVersion)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    *aProtocolVersion = mData.mProtocolVersion;

    LogFlowThisFuncLeave();
    return S_OK;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::getEnvironment(std::vector<com::Utf8Str> &aEnvironment)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    size_t cEnvVars = mData.mEnvironment.Size();
    LogFlowThisFunc(("[%s]: cEnvVars=%RU32\n",
                     mData.mSession.mName.c_str(), cEnvVars));

    for (size_t i = 0; i < cEnvVars; i++)
        aEnvironment[i] = mData.mEnvironment.Get(i);

    LogFlowThisFuncLeave();
    return S_OK;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::setEnvironment(const std::vector<com::Utf8Str> &aEnvironment)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    int rc = VINF_SUCCESS;
    for (size_t i = 0; i < aEnvironment.size() && RT_SUCCESS(rc); ++i)
        if (!aEnvironment[i].isEmpty()) /* Silently skip empty entries. */
            rc = mData.mEnvironment.Set(aEnvironment[i]);

    HRESULT hr = RT_SUCCESS(rc) ? S_OK : VBOX_E_IPRT_ERROR;
    LogFlowFuncLeaveRC(hr);
    return hr;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::getProcesses(std::vector<ComPtr<IGuestProcess> > &aProcesses)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    aProcesses.resize(mData.mProcesses.size());
    size_t i = 0;
    for(SessionProcesses::iterator it = mData.mProcesses.begin(); it != mData.mProcesses.end(); ++it, ++i)
        it->second.queryInterfaceTo(aProcesses[i].asOutParam());

    LogFlowFunc(("mProcesses=%zu\n", aProcesses.size()));
    return S_OK;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::getDirectories(std::vector<ComPtr<IGuestDirectory> > &aDirectories)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    aDirectories.resize(mData.mDirectories.size());
    size_t i = 0;
    for(SessionDirectories::iterator it = mData.mDirectories.begin(); it != mData.mDirectories.end(); ++it, ++i)
        it->second.queryInterfaceTo(aDirectories[i].asOutParam());

    LogFlowFunc(("mDirectories=%zu\n", aDirectories.size()));
    return S_OK;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::getFiles(std::vector<ComPtr<IGuestFile> > &aFiles)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    aFiles.resize(mData.mFiles.size());
    size_t i = 0;
    for(SessionFiles::iterator it = mData.mFiles.begin(); it != mData.mFiles.end(); ++it, ++i)
        it->second.queryInterfaceTo(aFiles[i].asOutParam());

    LogFlowFunc(("mDirectories=%zu\n", aFiles.size()));

    return S_OK;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::getEventSource(ComPtr<IEventSource> &aEventSource)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    // no need to lock - lifetime constant
    mEventSource.queryInterfaceTo(aEventSource.asOutParam());

    LogFlowThisFuncLeave();
    return S_OK;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

// private methods
///////////////////////////////////////////////////////////////////////////////

int GuestSession::i_closeSession(uint32_t uFlags, uint32_t uTimeoutMS, int *pGuestRc)
{
    LogFlowThisFunc(("uFlags=%x, uTimeoutMS=%RU32\n", uFlags, uTimeoutMS));

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    /* Guest Additions < 4.3 don't support closing dedicated
       guest sessions, skip. */
    if (mData.mProtocolVersion < 2)
    {
        LogFlowThisFunc(("Installed Guest Additions don't support closing dedicated sessions, skipping\n"));
        return VINF_SUCCESS;
    }

    /** @todo uFlags validation. */

    if (mData.mStatus != GuestSessionStatus_Started)
    {
        LogFlowThisFunc(("Session ID=%RU32 not started (anymore), status now is: %RU32\n",
                         mData.mSession.mID, mData.mStatus));
        return VINF_SUCCESS;
    }

    int vrc;

    GuestWaitEvent *pEvent = NULL;
    GuestEventTypes eventTypes;
    try
    {
        eventTypes.push_back(VBoxEventType_OnGuestSessionStateChanged);

        vrc = registerWaitEvent(mData.mSession.mID, 0 /* Object ID */,
                                eventTypes, &pEvent);
    }
    catch (std::bad_alloc)
    {
        vrc = VERR_NO_MEMORY;
    }

    if (RT_FAILURE(vrc))
        return vrc;

    LogFlowThisFunc(("Sending closing request to guest session ID=%RU32, uFlags=%x\n",
                     mData.mSession.mID, uFlags));

    VBOXHGCMSVCPARM paParms[4];
    int i = 0;
    paParms[i++].setUInt32(pEvent->ContextID());
    paParms[i++].setUInt32(uFlags);

    alock.release(); /* Drop the write lock before waiting. */

    vrc = i_sendCommand(HOST_SESSION_CLOSE, i, paParms);
    if (RT_SUCCESS(vrc))
        vrc = i_waitForStatusChange(pEvent, GuestSessionWaitForFlag_Terminate, uTimeoutMS,
                                  NULL /* Session status */, pGuestRc);

    unregisterWaitEvent(pEvent);

    LogFlowFuncLeaveRC(vrc);
    return vrc;
}

int GuestSession::i_directoryCreateInternal(const Utf8Str &strPath, uint32_t uMode,
                                          uint32_t uFlags, int *pGuestRc)
{
    LogFlowThisFunc(("strPath=%s, uMode=%x, uFlags=%x\n",
                     strPath.c_str(), uMode, uFlags));

    int vrc = VINF_SUCCESS;

    GuestProcessStartupInfo procInfo;
    procInfo.mCommand = Utf8Str(VBOXSERVICE_TOOL_MKDIR);
    procInfo.mFlags   = ProcessCreateFlag_Hidden;

    try
    {
        /* Construct arguments. */
        if (uFlags)
        {
            if (uFlags & DirectoryCreateFlag_Parents)
                procInfo.mArguments.push_back(Utf8Str("--parents")); /* We also want to create the parent directories. */
            else
                vrc = VERR_INVALID_PARAMETER;
        }

        if (uMode)
        {
            procInfo.mArguments.push_back(Utf8Str("--mode")); /* Set the creation mode. */

            char szMode[16];
            if (RTStrPrintf(szMode, sizeof(szMode), "%o", uMode))
            {
                procInfo.mArguments.push_back(Utf8Str(szMode));
            }
            else
                vrc = VERR_BUFFER_OVERFLOW;
        }
        procInfo.mArguments.push_back(strPath); /* The directory we want to create. */
    }
    catch (std::bad_alloc)
    {
        vrc = VERR_NO_MEMORY;
    }

    if (RT_SUCCESS(vrc))
        vrc = GuestProcessTool::i_run(this, procInfo, pGuestRc);

    LogFlowFuncLeaveRC(vrc);
    return vrc;
}

inline bool GuestSession::i_directoryExists(uint32_t uDirID, ComObjPtr<GuestDirectory> *pDir)
{
    SessionDirectories::const_iterator it = mData.mDirectories.find(uDirID);
    if (it != mData.mDirectories.end())
    {
        if (pDir)
            *pDir = it->second;
        return true;
    }
    return false;
}

int GuestSession::i_directoryQueryInfoInternal(const Utf8Str &strPath, GuestFsObjData &objData, int *pGuestRc)
{
    LogFlowThisFunc(("strPath=%s\n", strPath.c_str()));

    int vrc = i_fsQueryInfoInternal(strPath, objData, pGuestRc);
    if (RT_SUCCESS(vrc))
    {
        vrc = objData.mType == FsObjType_Directory
            ? VINF_SUCCESS : VERR_NOT_A_DIRECTORY;
    }

    LogFlowFuncLeaveRC(vrc);
    return vrc;
}

int GuestSession::i_directoryRemoveFromList(GuestDirectory *pDirectory)
{
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    int rc = VERR_NOT_FOUND;

    SessionDirectories::iterator itDirs = mData.mDirectories.begin();
    while (itDirs != mData.mDirectories.end())
    {
        if (pDirectory == itDirs->second)
        {
            /* Make sure to consume the pointer before the one of the
             * iterator gets released. */
            ComObjPtr<GuestDirectory> pDir = pDirectory;

            Bstr strName;
            HRESULT hr = itDirs->second->COMGETTER(DirectoryName)(strName.asOutParam());
            ComAssertComRC(hr);

            Assert(mData.mDirectories.size());
            Assert(mData.mNumObjects);
            LogFlowFunc(("Removing directory \"%s\" (Session: %RU32) (now total %zu processes, %RU32 objects)\n",
                         Utf8Str(strName).c_str(), mData.mSession.mID, mData.mDirectories.size() - 1, mData.mNumObjects - 1));

            rc = pDirectory->i_onRemove();
            mData.mDirectories.erase(itDirs);
            mData.mNumObjects--;

            pDir.setNull();
            break;
        }

        itDirs++;
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

int GuestSession::i_directoryRemoveInternal(const Utf8Str &strPath, uint32_t uFlags,
                                            int *pGuestRc)
{
    AssertReturn(!(uFlags & ~DIRREMOVE_FLAG_VALID_MASK), VERR_INVALID_PARAMETER);

    LogFlowThisFunc(("strPath=%s, uFlags=0x%x\n", strPath.c_str(), uFlags));

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    GuestWaitEvent *pEvent = NULL;
    int vrc = registerWaitEvent(mData.mSession.mID, 0 /* Object ID */,
                                &pEvent);
    if (RT_FAILURE(vrc))
        return vrc;

    /* Prepare HGCM call. */
    VBOXHGCMSVCPARM paParms[8];
    int i = 0;
    paParms[i++].setUInt32(pEvent->ContextID());
    paParms[i++].setPointer((void*)strPath.c_str(),
                            (ULONG)strPath.length() + 1);
    paParms[i++].setUInt32(uFlags);

    alock.release(); /* Drop write lock before sending. */

    vrc = i_sendCommand(HOST_DIR_REMOVE, i, paParms);
    if (RT_SUCCESS(vrc))
    {
        vrc = pEvent->Wait(30 * 1000);
        if (   vrc == VERR_GSTCTL_GUEST_ERROR
            && pGuestRc)
            *pGuestRc = pEvent->GuestResult();
    }

    unregisterWaitEvent(pEvent);

    LogFlowFuncLeaveRC(vrc);
    return vrc;
}

int GuestSession::i_objectCreateTempInternal(const Utf8Str &strTemplate, const Utf8Str &strPath,
                                             bool fDirectory, Utf8Str &strName, int *pGuestRc)
{
    LogFlowThisFunc(("strTemplate=%s, strPath=%s, fDirectory=%RTbool\n",
                     strTemplate.c_str(), strPath.c_str(), fDirectory));

    int vrc = VINF_SUCCESS;

    GuestProcessStartupInfo procInfo;
    procInfo.mCommand = Utf8Str(VBOXSERVICE_TOOL_MKTEMP);
    procInfo.mFlags   = ProcessCreateFlag_WaitForStdOut;

    try
    {
        procInfo.mArguments.push_back(Utf8Str("--machinereadable"));
        if (fDirectory)
            procInfo.mArguments.push_back(Utf8Str("-d"));
        if (strPath.length()) /* Otherwise use /tmp or equivalent. */
        {
            procInfo.mArguments.push_back(Utf8Str("-t"));
            procInfo.mArguments.push_back(strPath);
        }
        procInfo.mArguments.push_back(strTemplate);
    }
    catch (std::bad_alloc)
    {
        vrc = VERR_NO_MEMORY;
    }

    /** @todo Use an internal HGCM command for this operation, since
     *        we now can run in a user-dedicated session. */
    int guestRc; GuestCtrlStreamObjects stdOut;
    if (RT_SUCCESS(vrc))
        vrc = GuestProcessTool::i_runEx(this, procInfo,
                                        &stdOut, 1 /* cStrmOutObjects */,
                                        &guestRc);
    if (   RT_SUCCESS(vrc)
        && RT_SUCCESS(guestRc))
    {
        GuestFsObjData objData;
        if (!stdOut.empty())
        {
            vrc = objData.FromMkTemp(stdOut.at(0));
            if (RT_FAILURE(vrc))
            {
                guestRc = vrc;
                vrc = VERR_GSTCTL_GUEST_ERROR;
            }
        }
        else
            vrc = VERR_NO_DATA;

        if (RT_SUCCESS(vrc))
            strName = objData.mName;
    }

    if (pGuestRc)
        *pGuestRc = guestRc;

    LogFlowFuncLeaveRC(vrc);
    return vrc;
}

int GuestSession::i_directoryOpenInternal(const GuestDirectoryOpenInfo &openInfo,
                                          ComObjPtr<GuestDirectory> &pDirectory, int *pGuestRc)
{
    LogFlowThisFunc(("strPath=%s, strPath=%s, uFlags=%x\n",
                     openInfo.mPath.c_str(), openInfo.mFilter.c_str(), openInfo.mFlags));

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    int rc = VERR_MAX_PROCS_REACHED;
    if (mData.mNumObjects >= VBOX_GUESTCTRL_MAX_OBJECTS)
        return rc;

    /* Create a new (host-based) directory ID and assign it. */
    uint32_t uNewDirID = 0;
    ULONG uTries = 0;

    for (;;)
    {
        /* Is the directory ID already used? */
        if (!i_directoryExists(uNewDirID, NULL /* pDirectory */))
        {
            /* Callback with context ID was not found. This means
             * we can use this context ID for our new callback we want
             * to add below. */
            rc = VINF_SUCCESS;
            break;
        }
        uNewDirID++;
        if (uNewDirID == VBOX_GUESTCTRL_MAX_OBJECTS)
            uNewDirID = 0;

        if (++uTries == UINT32_MAX)
            break; /* Don't try too hard. */
    }

    if (RT_FAILURE(rc))
        return rc;

    /* Create the directory object. */
    HRESULT hr = pDirectory.createObject();
    if (FAILED(hr))
        return VERR_COM_UNEXPECTED;

    Console *pConsole = mParent->getConsole();
    AssertPtr(pConsole);

    int vrc = pDirectory->init(pConsole, this /* Parent */,
                               uNewDirID, openInfo);
    if (RT_FAILURE(vrc))
        return vrc;

    /*
     * Since this is a synchronous guest call we have to
     * register the file object first, releasing the session's
     * lock and then proceed with the actual opening command
     * -- otherwise the file's opening callback would hang
     * because the session's lock still is in place.
     */
    try
    {
        /* Add the created directory to our map. */
        mData.mDirectories[uNewDirID] = pDirectory;
        mData.mNumObjects++;
        Assert(mData.mNumObjects <= VBOX_GUESTCTRL_MAX_OBJECTS);

        LogFlowFunc(("Added new guest directory \"%s\" (Session: %RU32) (now total %zu dirs, %RU32 objects)\n",
                     openInfo.mPath.c_str(), mData.mSession.mID, mData.mFiles.size(), mData.mNumObjects));

        alock.release(); /* Release lock before firing off event. */

        /** @todo Fire off a VBoxEventType_OnGuestDirectoryRegistered event? */
    }
    catch (std::bad_alloc &)
    {
        rc = VERR_NO_MEMORY;
    }

    if (RT_SUCCESS(rc))
    {
        /* Nothing further to do here yet. */
        if (pGuestRc)
            *pGuestRc = VINF_SUCCESS;
    }

    LogFlowFuncLeaveRC(vrc);
    return vrc;
}

int GuestSession::i_dispatchToDirectory(PVBOXGUESTCTRLHOSTCBCTX pCtxCb, PVBOXGUESTCTRLHOSTCALLBACK pSvcCb)
{
    LogFlowFunc(("pCtxCb=%p, pSvcCb=%p\n", pCtxCb, pSvcCb));

    AssertPtrReturn(pCtxCb, VERR_INVALID_POINTER);
    AssertPtrReturn(pSvcCb, VERR_INVALID_POINTER);

    if (pSvcCb->mParms < 3)
        return VERR_INVALID_PARAMETER;

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    uint32_t uDirID = VBOX_GUESTCTRL_CONTEXTID_GET_OBJECT(pCtxCb->uContextID);
#ifdef DEBUG
    LogFlowFunc(("uDirID=%RU32 (%zu total)\n",
                 uDirID, mData.mFiles.size()));
#endif
    int rc;
    SessionDirectories::const_iterator itDir
        = mData.mDirectories.find(uDirID);
    if (itDir != mData.mDirectories.end())
    {
        ComObjPtr<GuestDirectory> pDirectory(itDir->second);
        Assert(!pDirectory.isNull());

        alock.release();

        rc = pDirectory->i_callbackDispatcher(pCtxCb, pSvcCb);
    }
    else
        rc = VERR_NOT_FOUND;

    LogFlowFuncLeaveRC(rc);
    return rc;
}

int GuestSession::i_dispatchToFile(PVBOXGUESTCTRLHOSTCBCTX pCtxCb, PVBOXGUESTCTRLHOSTCALLBACK pSvcCb)
{
    LogFlowFunc(("pCtxCb=%p, pSvcCb=%p\n", pCtxCb, pSvcCb));

    AssertPtrReturn(pCtxCb, VERR_INVALID_POINTER);
    AssertPtrReturn(pSvcCb, VERR_INVALID_POINTER);

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    uint32_t uFileID = VBOX_GUESTCTRL_CONTEXTID_GET_OBJECT(pCtxCb->uContextID);
#ifdef DEBUG
    LogFlowFunc(("uFileID=%RU32 (%zu total)\n",
                 uFileID, mData.mFiles.size()));
#endif
    int rc;
    SessionFiles::const_iterator itFile
        = mData.mFiles.find(uFileID);
    if (itFile != mData.mFiles.end())
    {
        ComObjPtr<GuestFile> pFile(itFile->second);
        Assert(!pFile.isNull());

        alock.release();

        rc = pFile->i_callbackDispatcher(pCtxCb, pSvcCb);
    }
    else
        rc = VERR_NOT_FOUND;

    LogFlowFuncLeaveRC(rc);
    return rc;
}

int GuestSession::i_dispatchToObject(PVBOXGUESTCTRLHOSTCBCTX pCtxCb, PVBOXGUESTCTRLHOSTCALLBACK pSvcCb)
{
    LogFlowFunc(("pCtxCb=%p, pSvcCb=%p\n", pCtxCb, pSvcCb));

    AssertPtrReturn(pCtxCb, VERR_INVALID_POINTER);
    AssertPtrReturn(pSvcCb, VERR_INVALID_POINTER);

    int rc;
    uint32_t uObjectID = VBOX_GUESTCTRL_CONTEXTID_GET_OBJECT(pCtxCb->uContextID);

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    /* Since we don't know which type the object is, we need to through all
     * all objects. */
    /** @todo Speed this up by adding an object type to the callback context! */
    SessionProcesses::const_iterator itProc = mData.mProcesses.find(uObjectID);
    if (itProc == mData.mProcesses.end())
    {
        SessionFiles::const_iterator itFile = mData.mFiles.find(uObjectID);
        if (itFile != mData.mFiles.end())
        {
            alock.release();

            rc = i_dispatchToFile(pCtxCb, pSvcCb);
        }
        else
        {
            SessionDirectories::const_iterator itDir = mData.mDirectories.find(uObjectID);
            if (itDir != mData.mDirectories.end())
            {
                alock.release();

                rc = i_dispatchToDirectory(pCtxCb, pSvcCb);
            }
            else
                rc = VERR_NOT_FOUND;
        }
    }
    else
    {
        alock.release();

        rc = i_dispatchToProcess(pCtxCb, pSvcCb);
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

int GuestSession::i_dispatchToProcess(PVBOXGUESTCTRLHOSTCBCTX pCtxCb, PVBOXGUESTCTRLHOSTCALLBACK pSvcCb)
{
    LogFlowFunc(("pCtxCb=%p, pSvcCb=%p\n", pCtxCb, pSvcCb));

    AssertPtrReturn(pCtxCb, VERR_INVALID_POINTER);
    AssertPtrReturn(pSvcCb, VERR_INVALID_POINTER);

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    uint32_t uProcessID = VBOX_GUESTCTRL_CONTEXTID_GET_OBJECT(pCtxCb->uContextID);
#ifdef DEBUG
    LogFlowFunc(("uProcessID=%RU32 (%zu total)\n",
                 uProcessID, mData.mProcesses.size()));
#endif
    int rc;
    SessionProcesses::const_iterator itProc
        = mData.mProcesses.find(uProcessID);
    if (itProc != mData.mProcesses.end())
    {
#ifdef DEBUG_andy
        ULONG cRefs = itProc->second->AddRef();
        Assert(cRefs >= 2);
        LogFlowFunc(("pProcess=%p, cRefs=%RU32\n", &itProc->second, cRefs - 1));
        itProc->second->Release();
#endif
        ComObjPtr<GuestProcess> pProcess(itProc->second);
        Assert(!pProcess.isNull());

        /* Set protocol version so that pSvcCb can
         * be interpreted right. */
        pCtxCb->uProtocol = mData.mProtocolVersion;

        alock.release();
        rc = pProcess->i_callbackDispatcher(pCtxCb, pSvcCb);
    }
    else
        rc = VERR_NOT_FOUND;

    LogFlowFuncLeaveRC(rc);
    return rc;
}

int GuestSession::i_dispatchToThis(PVBOXGUESTCTRLHOSTCBCTX pCbCtx, PVBOXGUESTCTRLHOSTCALLBACK pSvcCb)
{
    AssertPtrReturn(pCbCtx, VERR_INVALID_POINTER);
    AssertPtrReturn(pSvcCb, VERR_INVALID_POINTER);

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

#ifdef DEBUG
    LogFlowThisFunc(("sessionID=%RU32, CID=%RU32, uFunction=%RU32, pSvcCb=%p\n",
                     mData.mSession.mID, pCbCtx->uContextID, pCbCtx->uFunction, pSvcCb));
#endif

    int rc;
    switch (pCbCtx->uFunction)
    {
        case GUEST_DISCONNECTED:
            /** @todo Handle closing all guest objects. */
            rc = VERR_INTERNAL_ERROR;
            break;

        case GUEST_SESSION_NOTIFY: /* Guest Additions >= 4.3.0. */
        {
            rc = i_onSessionStatusChange(pCbCtx, pSvcCb);
            break;
        }

        default:
            /* Silently skip unknown callbacks. */
            rc = VERR_NOT_SUPPORTED;
            break;
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

inline bool GuestSession::i_fileExists(uint32_t uFileID, ComObjPtr<GuestFile> *pFile)
{
    SessionFiles::const_iterator it = mData.mFiles.find(uFileID);
    if (it != mData.mFiles.end())
    {
        if (pFile)
            *pFile = it->second;
        return true;
    }
    return false;
}

int GuestSession::i_fileRemoveFromList(GuestFile *pFile)
{
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    int rc = VERR_NOT_FOUND;

    SessionFiles::iterator itFiles = mData.mFiles.begin();
    while (itFiles != mData.mFiles.end())
    {
        if (pFile == itFiles->second)
        {
            /* Make sure to consume the pointer before the one of thfe
             * iterator gets released. */
            ComObjPtr<GuestFile> pCurFile = pFile;

            Bstr strName;
            HRESULT hr = pCurFile->COMGETTER(FileName)(strName.asOutParam());
            ComAssertComRC(hr);

            Assert(mData.mNumObjects);
            LogFlowThisFunc(("Removing guest file \"%s\" (Session: %RU32) (now total %zu files, %RU32 objects)\n",
                             Utf8Str(strName).c_str(), mData.mSession.mID, mData.mFiles.size() - 1, mData.mNumObjects - 1));

            rc = pFile->i_onRemove();
            mData.mFiles.erase(itFiles);
            mData.mNumObjects--;

            alock.release(); /* Release lock before firing off event. */

            fireGuestFileRegisteredEvent(mEventSource, this, pCurFile,
                                         false /* Unregistered */);
            pCurFile.setNull();
            break;
        }

        itFiles++;
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

int GuestSession::i_fileRemoveInternal(const Utf8Str &strPath, int *pGuestRc)
{
    LogFlowThisFunc(("strPath=%s\n", strPath.c_str()));

    int vrc = VINF_SUCCESS;

    GuestProcessStartupInfo procInfo;
    GuestProcessStream      streamOut;

    procInfo.mCommand = Utf8Str(VBOXSERVICE_TOOL_RM);
    procInfo.mFlags   = ProcessCreateFlag_WaitForStdOut;

    try
    {
        procInfo.mArguments.push_back(Utf8Str("--machinereadable"));
        procInfo.mArguments.push_back(strPath); /* The file we want to remove. */
    }
    catch (std::bad_alloc)
    {
        vrc = VERR_NO_MEMORY;
    }

    if (RT_SUCCESS(vrc))
        vrc = GuestProcessTool::i_run(this, procInfo, pGuestRc);

    LogFlowFuncLeaveRC(vrc);
    return vrc;
}

int GuestSession::i_fileOpenInternal(const GuestFileOpenInfo &openInfo,
                                     ComObjPtr<GuestFile> &pFile, int *pGuestRc)
{
    LogFlowThisFunc(("strPath=%s, strOpenMode=%s, strDisposition=%s, uCreationMode=%x, uOffset=%RU64\n",
                     openInfo.mFileName.c_str(), openInfo.mOpenMode.c_str(), openInfo.mDisposition.c_str(),
                     openInfo.mCreationMode, openInfo.mInitialOffset));

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    /* Guest Additions < 4.3 don't support handling
       guest files, skip. */
    if (mData.mProtocolVersion < 2)
    {
        LogFlowThisFunc(("Installed Guest Additions don't support handling guest files, skipping\n"));
        return VERR_NOT_SUPPORTED;
    }

    int rc = VERR_MAX_PROCS_REACHED;
    if (mData.mNumObjects >= VBOX_GUESTCTRL_MAX_OBJECTS)
        return rc;

    /* Create a new (host-based) file ID and assign it. */
    uint32_t uNewFileID = 0;
    ULONG uTries = 0;

    for (;;)
    {
        /* Is the file ID already used? */
        if (!i_fileExists(uNewFileID, NULL /* pFile */))
        {
            /* Callback with context ID was not found. This means
             * we can use this context ID for our new callback we want
             * to add below. */
            rc = VINF_SUCCESS;
            break;
        }
        uNewFileID++;
        if (uNewFileID == VBOX_GUESTCTRL_MAX_OBJECTS)
            uNewFileID = 0;

        if (++uTries == UINT32_MAX)
            break; /* Don't try too hard. */
    }

    if (RT_FAILURE(rc))
        return rc;

    /* Create the directory object. */
    HRESULT hr = pFile.createObject();
    if (FAILED(hr))
        return VERR_COM_UNEXPECTED;

    Console *pConsole = mParent->getConsole();
    AssertPtr(pConsole);

    rc = pFile->init(pConsole, this /* GuestSession */,
                     uNewFileID, openInfo);
    if (RT_FAILURE(rc))
        return rc;

    /*
     * Since this is a synchronous guest call we have to
     * register the file object first, releasing the session's
     * lock and then proceed with the actual opening command
     * -- otherwise the file's opening callback would hang
     * because the session's lock still is in place.
     */
    try
    {
        /* Add the created file to our vector. */
        mData.mFiles[uNewFileID] = pFile;
        mData.mNumObjects++;
        Assert(mData.mNumObjects <= VBOX_GUESTCTRL_MAX_OBJECTS);

        LogFlowFunc(("Added new guest file \"%s\" (Session: %RU32) (now total %zu files, %RU32 objects)\n",
                     openInfo.mFileName.c_str(), mData.mSession.mID, mData.mFiles.size(), mData.mNumObjects));

        alock.release(); /* Release lock before firing off event. */

        fireGuestFileRegisteredEvent(mEventSource, this, pFile,
                                     true /* Registered */);
    }
    catch (std::bad_alloc &)
    {
        rc = VERR_NO_MEMORY;
    }

    if (RT_SUCCESS(rc))
    {
        int guestRc;
        rc = pFile->i_openFile(30 * 1000 /* 30s timeout */, &guestRc);
        if (   rc == VERR_GSTCTL_GUEST_ERROR
            && pGuestRc)
        {
            *pGuestRc = guestRc;
        }
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

int GuestSession::i_fileQueryInfoInternal(const Utf8Str &strPath, GuestFsObjData &objData, int *pGuestRc)
{
    LogFlowThisFunc(("strPath=%s\n", strPath.c_str()));

    int vrc = i_fsQueryInfoInternal(strPath, objData, pGuestRc);
    if (RT_SUCCESS(vrc))
    {
        vrc = objData.mType == FsObjType_File
            ? VINF_SUCCESS : VERR_NOT_A_FILE;
    }

    LogFlowFuncLeaveRC(vrc);
    return vrc;
}

int GuestSession::i_fileQuerySizeInternal(const Utf8Str &strPath, int64_t *pllSize, int *pGuestRc)
{
    AssertPtrReturn(pllSize, VERR_INVALID_POINTER);

    GuestFsObjData objData;
    int vrc = i_fileQueryInfoInternal(strPath, objData, pGuestRc);
    if (RT_SUCCESS(vrc))
        *pllSize = objData.mObjectSize;

    return vrc;
}

int GuestSession::i_fsQueryInfoInternal(const Utf8Str &strPath, GuestFsObjData &objData, int *pGuestRc)
{
    LogFlowThisFunc(("strPath=%s\n", strPath.c_str()));

    int vrc = VINF_SUCCESS;

    /** @todo Merge this with IGuestFile::queryInfo(). */
    GuestProcessStartupInfo procInfo;
    procInfo.mCommand = Utf8Str(VBOXSERVICE_TOOL_STAT);
    procInfo.mFlags   = ProcessCreateFlag_WaitForStdOut;

    try
    {
        /* Construct arguments. */
        procInfo.mArguments.push_back(Utf8Str("--machinereadable"));
        procInfo.mArguments.push_back(strPath);
    }
    catch (std::bad_alloc)
    {
        vrc = VERR_NO_MEMORY;
    }

    int guestRc; GuestCtrlStreamObjects stdOut;
    if (RT_SUCCESS(vrc))
        vrc = GuestProcessTool::i_runEx(this, procInfo,
                                        &stdOut, 1 /* cStrmOutObjects */,
                                        &guestRc);
    if (   RT_SUCCESS(vrc)
        && RT_SUCCESS(guestRc))
    {
        if (!stdOut.empty())
            vrc = objData.FromStat(stdOut.at(0));
        else
            vrc = VERR_NO_DATA;
    }

    if (   vrc == VERR_GSTCTL_GUEST_ERROR
        && pGuestRc)
        *pGuestRc = guestRc;

    LogFlowThisFunc(("Returning rc=%Rrc, guestRc=%Rrc\n",
                     vrc, guestRc));
    return vrc;
}

const GuestCredentials& GuestSession::i_getCredentials(void)
{
    return mData.mCredentials;
}

const GuestEnvironment& GuestSession::i_getEnvironment(void)
{
    return mData.mEnvironment;
}

Utf8Str GuestSession::i_getName(void)
{
    return mData.mSession.mName;
}

/* static */
Utf8Str GuestSession::i_guestErrorToString(int guestRc)
{
    Utf8Str strError;

    /** @todo pData->u32Flags: int vs. uint32 -- IPRT errors are *negative* !!! */
    switch (guestRc)
    {
        case VERR_INVALID_VM_HANDLE:
            strError += Utf8StrFmt(tr("VMM device is not available (is the VM running?)"));
            break;

        case VERR_HGCM_SERVICE_NOT_FOUND:
            strError += Utf8StrFmt(tr("The guest execution service is not available"));
            break;

        case VERR_AUTHENTICATION_FAILURE:
            strError += Utf8StrFmt(tr("The specified user was not able to logon on guest"));
            break;

        case VERR_TIMEOUT:
            strError += Utf8StrFmt(tr("The guest did not respond within time"));
            break;

        case VERR_CANCELLED:
            strError += Utf8StrFmt(tr("The session operation was canceled"));
            break;

        case VERR_PERMISSION_DENIED:
            strError += Utf8StrFmt(tr("Invalid user/password credentials"));
            break;

        case VERR_MAX_PROCS_REACHED:
            strError += Utf8StrFmt(tr("Maximum number of concurrent guest processes has been reached"));
            break;

        case VERR_NOT_EQUAL: /** @todo Imprecise to the user; can mean anything and all. */
            strError += Utf8StrFmt(tr("Unable to retrieve requested information"));
            break;

        case VERR_NOT_FOUND:
            strError += Utf8StrFmt(tr("The guest execution service is not ready (yet)"));
            break;

        default:
            strError += Utf8StrFmt("%Rrc", guestRc);
            break;
    }

    return strError;
}

/**
 * Checks if this session is ready state where it can handle
 * all session-bound actions (like guest processes, guest files).
 * Only used by official API methods. Will set an external
 * error when not ready.
 */
HRESULT GuestSession::i_isReadyExternal(void)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    /** @todo Be a bit more informative. */
    if (mData.mStatus != GuestSessionStatus_Started)
        return setError(E_UNEXPECTED, tr("Session is not in started state"));

    return S_OK;
}

/**
 * Called by IGuest right before this session gets removed from
 * the public session list.
 */
int GuestSession::i_onRemove(void)
{
    LogFlowThisFuncEnter();

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    int vrc = VINF_SUCCESS;

    /*
     * Note: The event source stuff holds references to this object,
     *       so make sure that this is cleaned up *before* calling uninit.
     */
    if (!mEventSource.isNull())
    {
        mEventSource->UnregisterListener(mLocalListener);

        mLocalListener.setNull();
        unconst(mEventSource).setNull();
    }

    LogFlowFuncLeaveRC(vrc);
    return vrc;
}

/** No locking! */
int GuestSession::i_onSessionStatusChange(PVBOXGUESTCTRLHOSTCBCTX pCbCtx, PVBOXGUESTCTRLHOSTCALLBACK pSvcCbData)
{
    AssertPtrReturn(pCbCtx, VERR_INVALID_POINTER);
    /* pCallback is optional. */
    AssertPtrReturn(pSvcCbData, VERR_INVALID_POINTER);

    if (pSvcCbData->mParms < 3)
        return VERR_INVALID_PARAMETER;

    CALLBACKDATA_SESSION_NOTIFY dataCb;
    /* pSvcCb->mpaParms[0] always contains the context ID. */
    int vrc = pSvcCbData->mpaParms[1].getUInt32(&dataCb.uType);
    AssertRCReturn(vrc, vrc);
    vrc = pSvcCbData->mpaParms[2].getUInt32(&dataCb.uResult);
    AssertRCReturn(vrc, vrc);

    LogFlowThisFunc(("ID=%RU32, uType=%RU32, guestRc=%Rrc\n",
                     mData.mSession.mID, dataCb.uType, dataCb.uResult));

    GuestSessionStatus_T sessionStatus = GuestSessionStatus_Undefined;

    int guestRc = dataCb.uResult; /** @todo uint32_t vs. int. */
    switch (dataCb.uType)
    {
        case GUEST_SESSION_NOTIFYTYPE_ERROR:
            sessionStatus = GuestSessionStatus_Error;
            break;

        case GUEST_SESSION_NOTIFYTYPE_STARTED:
            sessionStatus = GuestSessionStatus_Started;
            break;

        case GUEST_SESSION_NOTIFYTYPE_TEN:
        case GUEST_SESSION_NOTIFYTYPE_TES:
        case GUEST_SESSION_NOTIFYTYPE_TEA:
            sessionStatus = GuestSessionStatus_Terminated;
            break;

        case GUEST_SESSION_NOTIFYTYPE_TOK:
            sessionStatus = GuestSessionStatus_TimedOutKilled;
            break;

        case GUEST_SESSION_NOTIFYTYPE_TOA:
            sessionStatus = GuestSessionStatus_TimedOutAbnormally;
            break;

        case GUEST_SESSION_NOTIFYTYPE_DWN:
            sessionStatus = GuestSessionStatus_Down;
            break;

        case GUEST_SESSION_NOTIFYTYPE_UNDEFINED:
        default:
            vrc = VERR_NOT_SUPPORTED;
            break;
    }

    if (RT_SUCCESS(vrc))
    {
        if (RT_FAILURE(guestRc))
            sessionStatus = GuestSessionStatus_Error;
    }

    /* Set the session status. */
    if (RT_SUCCESS(vrc))
        vrc = i_setSessionStatus(sessionStatus, guestRc);

    LogFlowThisFunc(("ID=%RU32, guestRc=%Rrc\n", mData.mSession.mID, guestRc));

    LogFlowFuncLeaveRC(vrc);
    return vrc;
}

int GuestSession::i_startSessionInternal(int *pGuestRc)
{
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    LogFlowThisFunc(("mID=%RU32, mName=%s, uProtocolVersion=%RU32, openFlags=%x, openTimeoutMS=%RU32\n",
                     mData.mSession.mID, mData.mSession.mName.c_str(), mData.mProtocolVersion,
                     mData.mSession.mOpenFlags, mData.mSession.mOpenTimeoutMS));

    /* Guest Additions < 4.3 don't support opening dedicated
       guest sessions. Simply return success here. */
    if (mData.mProtocolVersion < 2)
    {
        mData.mStatus = GuestSessionStatus_Started;

        LogFlowThisFunc(("Installed Guest Additions don't support opening dedicated sessions, skipping\n"));
        return VINF_SUCCESS;
    }

    if (mData.mStatus != GuestSessionStatus_Undefined)
        return VINF_SUCCESS;

    /** @todo mData.mSession.uFlags validation. */

    /* Set current session status. */
    mData.mStatus = GuestSessionStatus_Starting;
    mData.mRC = VINF_SUCCESS; /* Clear previous error, if any. */

    int vrc;

    GuestWaitEvent *pEvent = NULL;
    GuestEventTypes eventTypes;
    try
    {
        eventTypes.push_back(VBoxEventType_OnGuestSessionStateChanged);

        vrc = registerWaitEvent(mData.mSession.mID, 0 /* Object ID */,
                                eventTypes, &pEvent);
    }
    catch (std::bad_alloc)
    {
        vrc = VERR_NO_MEMORY;
    }

    if (RT_FAILURE(vrc))
        return vrc;

    VBOXHGCMSVCPARM paParms[8];

    int i = 0;
    paParms[i++].setUInt32(pEvent->ContextID());
    paParms[i++].setUInt32(mData.mProtocolVersion);
    paParms[i++].setPointer((void*)mData.mCredentials.mUser.c_str(),
                            (ULONG)mData.mCredentials.mUser.length() + 1);
    paParms[i++].setPointer((void*)mData.mCredentials.mPassword.c_str(),
                            (ULONG)mData.mCredentials.mPassword.length() + 1);
    paParms[i++].setPointer((void*)mData.mCredentials.mDomain.c_str(),
                            (ULONG)mData.mCredentials.mDomain.length() + 1);
    paParms[i++].setUInt32(mData.mSession.mOpenFlags);

    alock.release(); /* Drop write lock before sending. */

    vrc = i_sendCommand(HOST_SESSION_CREATE, i, paParms);
    if (RT_SUCCESS(vrc))
    {
        vrc = i_waitForStatusChange(pEvent, GuestSessionWaitForFlag_Start,
                                    30 * 1000 /* 30s timeout */,
                                    NULL /* Session status */, pGuestRc);
    }
    else
    {
        /*
         * Unable to start guest session - update its current state.
         * Since there is no (official API) way to recover a failed guest session
         * this also marks the end state. Internally just calling this
         * same function again will work though.
         */
        mData.mStatus = GuestSessionStatus_Error;
        mData.mRC = vrc;
    }

    unregisterWaitEvent(pEvent);

    LogFlowFuncLeaveRC(vrc);
    return vrc;
}

int GuestSession::i_startSessionAsync(void)
{
    LogFlowThisFuncEnter();

    int vrc;

    try
    {
        /* Asynchronously open the session on the guest by kicking off a
         * worker thread. */
        std::auto_ptr<GuestSessionTaskInternalOpen> pTask(new GuestSessionTaskInternalOpen(this));
        AssertReturn(pTask->isOk(), pTask->rc());

        vrc = RTThreadCreate(NULL, GuestSession::i_startSessionThread,
                             (void *)pTask.get(), 0,
                             RTTHREADTYPE_MAIN_WORKER, 0,
                             "gctlSesStart");
        if (RT_SUCCESS(vrc))
        {
            /* pTask is now owned by openSessionThread(), so release it. */
            pTask.release();
        }
    }
    catch(std::bad_alloc &)
    {
        vrc = VERR_NO_MEMORY;
    }

    LogFlowFuncLeaveRC(vrc);
    return vrc;
}

/* static */
DECLCALLBACK(int) GuestSession::i_startSessionThread(RTTHREAD Thread, void *pvUser)
{
    LogFlowFunc(("pvUser=%p\n", pvUser));

    std::auto_ptr<GuestSessionTaskInternalOpen> pTask(static_cast<GuestSessionTaskInternalOpen*>(pvUser));
    AssertPtr(pTask.get());

    const ComObjPtr<GuestSession> pSession(pTask->Session());
    Assert(!pSession.isNull());

    AutoCaller autoCaller(pSession);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    int vrc = pSession->i_startSessionInternal(NULL /* Guest rc, ignored */);
    /* Nothing to do here anymore. */

    LogFlowFuncLeaveRC(vrc);
    return vrc;
}

int GuestSession::i_pathRenameInternal(const Utf8Str &strSource, const Utf8Str &strDest,
                                     uint32_t uFlags, int *pGuestRc)
{
    AssertReturn(!(uFlags & ~PATHRENAME_FLAG_VALID_MASK), VERR_INVALID_PARAMETER);

    LogFlowThisFunc(("strSource=%s, strDest=%s, uFlags=0x%x\n",
                     strSource.c_str(), strDest.c_str(), uFlags));

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    GuestWaitEvent *pEvent = NULL;
    int vrc = registerWaitEvent(mData.mSession.mID, 0 /* Object ID */,
                                &pEvent);
    if (RT_FAILURE(vrc))
        return vrc;

    /* Prepare HGCM call. */
    VBOXHGCMSVCPARM paParms[8];
    int i = 0;
    paParms[i++].setUInt32(pEvent->ContextID());
    paParms[i++].setPointer((void*)strSource.c_str(),
                            (ULONG)strSource.length() + 1);
    paParms[i++].setPointer((void*)strDest.c_str(),
                            (ULONG)strDest.length() + 1);
    paParms[i++].setUInt32(uFlags);

    alock.release(); /* Drop write lock before sending. */

    vrc = i_sendCommand(HOST_PATH_RENAME, i, paParms);
    if (RT_SUCCESS(vrc))
    {
        vrc = pEvent->Wait(30 * 1000);
        if (   vrc == VERR_GSTCTL_GUEST_ERROR
            && pGuestRc)
            *pGuestRc = pEvent->GuestResult();
    }

    unregisterWaitEvent(pEvent);

    LogFlowFuncLeaveRC(vrc);
    return vrc;
}

int GuestSession::i_processRemoveFromList(GuestProcess *pProcess)
{
    AssertPtrReturn(pProcess, VERR_INVALID_POINTER);

    LogFlowThisFunc(("pProcess=%p\n", pProcess));

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    int rc = VERR_NOT_FOUND;

    ULONG uPID;
    HRESULT hr = pProcess->COMGETTER(PID)(&uPID);
    ComAssertComRC(hr);

    LogFlowFunc(("Removing process (PID=%RU32) ...\n", uPID));

    SessionProcesses::iterator itProcs = mData.mProcesses.begin();
    while (itProcs != mData.mProcesses.end())
    {
        if (pProcess == itProcs->second)
        {
#ifdef DEBUG_andy
            ULONG cRefs = pProcess->AddRef();
            Assert(cRefs >= 2);
            LogFlowFunc(("pProcess=%p, cRefs=%RU32\n", pProcess, cRefs - 1));
            pProcess->Release();
#endif
            /* Make sure to consume the pointer before the one of the
             * iterator gets released. */
            ComObjPtr<GuestProcess> pProc = pProcess;

            hr = pProc->COMGETTER(PID)(&uPID);
            ComAssertComRC(hr);

            Assert(mData.mProcesses.size());
            Assert(mData.mNumObjects);
            LogFlowFunc(("Removing process ID=%RU32 (Session: %RU32), guest PID=%RU32 (now total %zu processes, %RU32 objects)\n",
                         pProcess->getObjectID(), mData.mSession.mID, uPID, mData.mProcesses.size() - 1, mData.mNumObjects - 1));

            rc = pProcess->i_onRemove();
            mData.mProcesses.erase(itProcs);
            mData.mNumObjects--;

            alock.release(); /* Release lock before firing off event. */

            fireGuestProcessRegisteredEvent(mEventSource, this /* Session */, pProc,
                                            uPID, false /* Process unregistered */);
            pProc.setNull();
            break;
        }

        itProcs++;
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

/**
 * Creates but does *not* start the process yet. See GuestProcess::startProcess() or
 * GuestProcess::startProcessAsync() for that.
 *
 * @return  IPRT status code.
 * @param   procInfo
 * @param   pProcess
 */
int GuestSession::i_processCreateExInteral(GuestProcessStartupInfo &procInfo, ComObjPtr<GuestProcess> &pProcess)
{
    LogFlowFunc(("mCmd=%s, mFlags=%x, mTimeoutMS=%RU32\n",
                 procInfo.mCommand.c_str(), procInfo.mFlags, procInfo.mTimeoutMS));
#ifdef DEBUG
    if (procInfo.mArguments.size())
    {
        LogFlowFunc(("Arguments:"));
        ProcessArguments::const_iterator it = procInfo.mArguments.begin();
        while (it != procInfo.mArguments.end())
        {
            LogFlow((" %s", (*it).c_str()));
            it++;
        }
        LogFlow(("\n"));
    }
#endif

    /* Validate flags. */
    if (procInfo.mFlags)
    {
        if (   !(procInfo.mFlags & ProcessCreateFlag_IgnoreOrphanedProcesses)
            && !(procInfo.mFlags & ProcessCreateFlag_WaitForProcessStartOnly)
            && !(procInfo.mFlags & ProcessCreateFlag_Hidden)
            && !(procInfo.mFlags & ProcessCreateFlag_NoProfile)
            && !(procInfo.mFlags & ProcessCreateFlag_WaitForStdOut)
            && !(procInfo.mFlags & ProcessCreateFlag_WaitForStdErr))
        {
            return VERR_INVALID_PARAMETER;
        }
    }

    if (   (procInfo.mFlags & ProcessCreateFlag_WaitForProcessStartOnly)
        && (   (procInfo.mFlags & ProcessCreateFlag_WaitForStdOut)
            || (procInfo.mFlags & ProcessCreateFlag_WaitForStdErr)
           )
       )
    {
        return VERR_INVALID_PARAMETER;
    }

    /* Adjust timeout. If set to 0, we define
     * an infinite timeout. */
    if (procInfo.mTimeoutMS == 0)
        procInfo.mTimeoutMS = UINT32_MAX;

    /** @tood Implement process priority + affinity. */

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    int rc = VERR_MAX_PROCS_REACHED;
    if (mData.mNumObjects >= VBOX_GUESTCTRL_MAX_OBJECTS)
        return rc;

    /* Create a new (host-based) process ID and assign it. */
    uint32_t uNewProcessID = 0;
    ULONG uTries = 0;

    for (;;)
    {
        /* Is the context ID already used? */
        if (!i_processExists(uNewProcessID, NULL /* pProcess */))
        {
            /* Callback with context ID was not found. This means
             * we can use this context ID for our new callback we want
             * to add below. */
            rc = VINF_SUCCESS;
            break;
        }
        uNewProcessID++;
        if (uNewProcessID == VBOX_GUESTCTRL_MAX_OBJECTS)
            uNewProcessID = 0;

        if (++uTries == VBOX_GUESTCTRL_MAX_OBJECTS)
            break; /* Don't try too hard. */
    }

    if (RT_FAILURE(rc))
        return rc;

    /* Create the process object. */
    HRESULT hr = pProcess.createObject();
    if (FAILED(hr))
        return VERR_COM_UNEXPECTED;

    rc = pProcess->init(mParent->getConsole() /* Console */, this /* Session */,
                        uNewProcessID, procInfo);
    if (RT_FAILURE(rc))
        return rc;

    /* Add the created process to our map. */
    try
    {
        mData.mProcesses[uNewProcessID] = pProcess;
        mData.mNumObjects++;
        Assert(mData.mNumObjects <= VBOX_GUESTCTRL_MAX_OBJECTS);

        LogFlowFunc(("Added new process (Session: %RU32) with process ID=%RU32 (now total %zu processes, %RU32 objects)\n",
                     mData.mSession.mID, uNewProcessID, mData.mProcesses.size(), mData.mNumObjects));

        alock.release(); /* Release lock before firing off event. */

        fireGuestProcessRegisteredEvent(mEventSource, this /* Session */, pProcess,
                                        0 /* PID */, true /* Process registered */);
    }
    catch (std::bad_alloc &)
    {
        rc = VERR_NO_MEMORY;
    }

    return rc;
}

inline bool GuestSession::i_processExists(uint32_t uProcessID, ComObjPtr<GuestProcess> *pProcess)
{
    SessionProcesses::const_iterator it = mData.mProcesses.find(uProcessID);
    if (it != mData.mProcesses.end())
    {
        if (pProcess)
            *pProcess = it->second;
        return true;
    }
    return false;
}

inline int GuestSession::i_processGetByPID(ULONG uPID, ComObjPtr<GuestProcess> *pProcess)
{
    AssertReturn(uPID, false);
    /* pProcess is optional. */

    SessionProcesses::iterator itProcs = mData.mProcesses.begin();
    for (; itProcs != mData.mProcesses.end(); itProcs++)
    {
        ComObjPtr<GuestProcess> pCurProc = itProcs->second;
        AutoCaller procCaller(pCurProc);
        if (procCaller.rc())
            return VERR_COM_INVALID_OBJECT_STATE;

        ULONG uCurPID;
        HRESULT hr = pCurProc->COMGETTER(PID)(&uCurPID);
        ComAssertComRC(hr);

        if (uCurPID == uPID)
        {
            if (pProcess)
                *pProcess = pCurProc;
            return VINF_SUCCESS;
        }
    }

    return VERR_NOT_FOUND;
}

int GuestSession::i_sendCommand(uint32_t uFunction,
                                uint32_t uParms, PVBOXHGCMSVCPARM paParms)
{
    LogFlowThisFuncEnter();

#ifndef VBOX_GUESTCTRL_TEST_CASE
    ComObjPtr<Console> pConsole = mParent->getConsole();
    Assert(!pConsole.isNull());

    /* Forward the information to the VMM device. */
    VMMDev *pVMMDev = pConsole->getVMMDev();
    AssertPtr(pVMMDev);

    LogFlowThisFunc(("uFunction=%RU32, uParms=%RU32\n", uFunction, uParms));
    int vrc = pVMMDev->hgcmHostCall(HGCMSERVICE_NAME, uFunction, uParms, paParms);
    if (RT_FAILURE(vrc))
    {
        /** @todo What to do here? */
    }
#else
    /* Not needed within testcases. */
    int vrc = VINF_SUCCESS;
#endif
    LogFlowFuncLeaveRC(vrc);
    return vrc;
}

/* static */
HRESULT GuestSession::i_setErrorExternal(VirtualBoxBase *pInterface, int guestRc)
{
    AssertPtr(pInterface);
    AssertMsg(RT_FAILURE(guestRc), ("Guest rc does not indicate a failure when setting error\n"));

    return pInterface->setError(VBOX_E_IPRT_ERROR, GuestSession::i_guestErrorToString(guestRc).c_str());
}

/* Does not do locking; caller is responsible for that! */
int GuestSession::i_setSessionStatus(GuestSessionStatus_T sessionStatus, int sessionRc)
{
    LogFlowThisFunc(("oldStatus=%RU32, newStatus=%RU32, sessionRc=%Rrc\n",
                     mData.mStatus, sessionStatus, sessionRc));

    if (sessionStatus == GuestSessionStatus_Error)
    {
        AssertMsg(RT_FAILURE(sessionRc), ("Guest rc must be an error (%Rrc)\n", sessionRc));
        /* Do not allow overwriting an already set error. If this happens
         * this means we forgot some error checking/locking somewhere. */
        AssertMsg(RT_SUCCESS(mData.mRC), ("Guest rc already set (to %Rrc)\n", mData.mRC));
    }
    else
        AssertMsg(RT_SUCCESS(sessionRc), ("Guest rc must not be an error (%Rrc)\n", sessionRc));

    if (mData.mStatus != sessionStatus)
    {
        mData.mStatus = sessionStatus;
        mData.mRC     = sessionRc;

        ComObjPtr<VirtualBoxErrorInfo> errorInfo;
        HRESULT hr = errorInfo.createObject();
        ComAssertComRC(hr);
        int rc2 = errorInfo->initEx(VBOX_E_IPRT_ERROR, sessionRc,
                                    COM_IIDOF(IGuestSession), getComponentName(),
                                    i_guestErrorToString(sessionRc));
        AssertRC(rc2);

        fireGuestSessionStateChangedEvent(mEventSource, this,
                                          mData.mSession.mID, sessionStatus, errorInfo);
    }

    return VINF_SUCCESS;
}

int GuestSession::i_signalWaiters(GuestSessionWaitResult_T enmWaitResult, int rc /*= VINF_SUCCESS */)
{
    /*LogFlowThisFunc(("enmWaitResult=%d, rc=%Rrc, mWaitCount=%RU32, mWaitEvent=%p\n",
                     enmWaitResult, rc, mData.mWaitCount, mData.mWaitEvent));*/

    /* Note: No write locking here -- already done in the caller. */

    int vrc = VINF_SUCCESS;
    /*if (mData.mWaitEvent)
        vrc = mData.mWaitEvent->Signal(enmWaitResult, rc);*/
    LogFlowFuncLeaveRC(vrc);
    return vrc;
}

int GuestSession::i_startTaskAsync(const Utf8Str &strTaskDesc,
                                   GuestSessionTask *pTask, ComObjPtr<Progress> &pProgress)
{
    LogFlowThisFunc(("strTaskDesc=%s, pTask=%p\n", strTaskDesc.c_str(), pTask));

    AssertPtrReturn(pTask, VERR_INVALID_POINTER);

    /* Create the progress object. */
    HRESULT hr = pProgress.createObject();
    if (FAILED(hr))
        return VERR_COM_UNEXPECTED;

    hr = pProgress->init(static_cast<IGuestSession*>(this),
                         Bstr(strTaskDesc).raw(),
                         TRUE /* aCancelable */);
    if (FAILED(hr))
        return VERR_COM_UNEXPECTED;

    /* Initialize our worker task. */
    std::auto_ptr<GuestSessionTask> task(pTask);

    int rc = task->RunAsync(strTaskDesc, pProgress);
    if (RT_FAILURE(rc))
        return rc;

    /* Don't destruct on success. */
    task.release();

    LogFlowFuncLeaveRC(rc);
    return rc;
}

/**
 * Queries/collects information prior to establishing a guest session.
 * This is necessary to know which guest control protocol version to use,
 * among other things (later).
 *
 * @return  IPRT status code.
 */
int GuestSession::i_queryInfo(void)
{
    /*
     * Try querying the guest control protocol version running on the guest.
     * This is done using the Guest Additions version
     */
    ComObjPtr<Guest> pGuest = mParent;
    Assert(!pGuest.isNull());

    uint32_t uVerAdditions = pGuest->getAdditionsVersion();
    uint32_t uVBoxMajor    = VBOX_FULL_VERSION_GET_MAJOR(uVerAdditions);
    uint32_t uVBoxMinor    = VBOX_FULL_VERSION_GET_MINOR(uVerAdditions);

#ifdef DEBUG_andy
    /* Hardcode the to-used protocol version; nice for testing side effects. */
    mData.mProtocolVersion = 2;
#else
    mData.mProtocolVersion = (
                              /* VBox 5.0 and up. */
                                 uVBoxMajor  >= 5
                              /* VBox 4.3 and up. */
                              || (uVBoxMajor == 4 && uVBoxMinor >= 3))
                           ? 2  /* Guest control 2.0. */
                           : 1; /* Legacy guest control (VBox < 4.3). */
    /* Build revision is ignored. */
#endif

    LogFlowThisFunc(("uVerAdditions=%RU32 (%RU32.%RU32), mProtocolVersion=%RU32\n",
                     uVerAdditions, uVBoxMajor, uVBoxMinor, mData.mProtocolVersion));

    /* Tell the user but don't bitch too often. */
    static short s_gctrlLegacyWarning = 0;
    if (   mData.mProtocolVersion < 2
        && s_gctrlLegacyWarning++ < 3) /** @todo Find a bit nicer text. */
        LogRel((tr("Warning: Guest Additions are older (%ld.%ld) than host capabilities for guest control, please upgrade them. Using protocol version %ld now\n"),
                uVBoxMajor, uVBoxMinor, mData.mProtocolVersion));

    return VINF_SUCCESS;
}

int GuestSession::i_waitFor(uint32_t fWaitFlags, ULONG uTimeoutMS, GuestSessionWaitResult_T &waitResult, int *pGuestRc)
{
    LogFlowThisFuncEnter();

    AssertReturn(fWaitFlags, VERR_INVALID_PARAMETER);

    /*LogFlowThisFunc(("fWaitFlags=0x%x, uTimeoutMS=%RU32, mStatus=%RU32, mWaitCount=%RU32, mWaitEvent=%p, pGuestRc=%p\n",
                     fWaitFlags, uTimeoutMS, mData.mStatus, mData.mWaitCount, mData.mWaitEvent, pGuestRc));*/

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    /* Did some error occur before? Then skip waiting and return. */
    if (mData.mStatus == GuestSessionStatus_Error)
    {
        waitResult = GuestSessionWaitResult_Error;
        AssertMsg(RT_FAILURE(mData.mRC), ("No error rc (%Rrc) set when guest session indicated an error\n", mData.mRC));
        if (pGuestRc)
            *pGuestRc = mData.mRC; /* Return last set error. */
        return VERR_GSTCTL_GUEST_ERROR;
    }

    /* Guest Additions < 4.3 don't support session handling, skip. */
    if (mData.mProtocolVersion < 2)
    {
        waitResult = GuestSessionWaitResult_WaitFlagNotSupported;

        LogFlowThisFunc(("Installed Guest Additions don't support waiting for dedicated sessions, skipping\n"));
        return VINF_SUCCESS;
    }

    waitResult = GuestSessionWaitResult_None;
    if (fWaitFlags & GuestSessionWaitForFlag_Terminate)
    {
        switch (mData.mStatus)
        {
            case GuestSessionStatus_Terminated:
            case GuestSessionStatus_Down:
                waitResult = GuestSessionWaitResult_Terminate;
                break;

            case GuestSessionStatus_TimedOutKilled:
            case GuestSessionStatus_TimedOutAbnormally:
                waitResult = GuestSessionWaitResult_Timeout;
                break;

            case GuestSessionStatus_Error:
                /* Handled above. */
                break;

            case GuestSessionStatus_Started:
                waitResult = GuestSessionWaitResult_Start;
                break;

            case GuestSessionStatus_Undefined:
            case GuestSessionStatus_Starting:
                /* Do the waiting below. */
                break;

            default:
                AssertMsgFailed(("Unhandled session status %RU32\n", mData.mStatus));
                return VERR_NOT_IMPLEMENTED;
        }
    }
    else if (fWaitFlags & GuestSessionWaitForFlag_Start)
    {
        switch (mData.mStatus)
        {
            case GuestSessionStatus_Started:
            case GuestSessionStatus_Terminating:
            case GuestSessionStatus_Terminated:
            case GuestSessionStatus_Down:
                waitResult = GuestSessionWaitResult_Start;
                break;

            case GuestSessionStatus_Error:
                waitResult = GuestSessionWaitResult_Error;
                break;

            case GuestSessionStatus_TimedOutKilled:
            case GuestSessionStatus_TimedOutAbnormally:
                waitResult = GuestSessionWaitResult_Timeout;
                break;

            case GuestSessionStatus_Undefined:
            case GuestSessionStatus_Starting:
                /* Do the waiting below. */
                break;

            default:
                AssertMsgFailed(("Unhandled session status %RU32\n", mData.mStatus));
                return VERR_NOT_IMPLEMENTED;
        }
    }

    LogFlowThisFunc(("sessionStatus=%RU32, sessionRc=%Rrc, waitResult=%RU32\n",
                     mData.mStatus, mData.mRC, waitResult));

    /* No waiting needed? Return immediately using the last set error. */
    if (waitResult != GuestSessionWaitResult_None)
    {
        if (pGuestRc)
            *pGuestRc = mData.mRC; /* Return last set error (if any). */
        return RT_SUCCESS(mData.mRC) ? VINF_SUCCESS : VERR_GSTCTL_GUEST_ERROR;
    }

    int vrc;

    GuestWaitEvent *pEvent = NULL;
    GuestEventTypes eventTypes;
    try
    {
        eventTypes.push_back(VBoxEventType_OnGuestSessionStateChanged);

        vrc = registerWaitEvent(mData.mSession.mID, 0 /* Object ID */,
                                eventTypes, &pEvent);
    }
    catch (std::bad_alloc)
    {
        vrc = VERR_NO_MEMORY;
    }

    if (RT_FAILURE(vrc))
        return vrc;

    alock.release(); /* Release lock before waiting. */

    GuestSessionStatus_T sessionStatus;
    vrc = i_waitForStatusChange(pEvent, fWaitFlags,
                                uTimeoutMS, &sessionStatus, pGuestRc);
    if (RT_SUCCESS(vrc))
    {
        switch (sessionStatus)
        {
            case GuestSessionStatus_Started:
                waitResult = GuestSessionWaitResult_Start;
                break;

            case GuestSessionStatus_Terminated:
                waitResult = GuestSessionWaitResult_Terminate;
                break;

            case GuestSessionStatus_TimedOutKilled:
            case GuestSessionStatus_TimedOutAbnormally:
                waitResult = GuestSessionWaitResult_Timeout;
                break;

            case GuestSessionStatus_Down:
                waitResult = GuestSessionWaitResult_Terminate;
                break;

            case GuestSessionStatus_Error:
                waitResult = GuestSessionWaitResult_Error;
                break;

            default:
                waitResult = GuestSessionWaitResult_Status;
                break;
        }
    }

    unregisterWaitEvent(pEvent);

    LogFlowFuncLeaveRC(vrc);
    return vrc;
}

int GuestSession::i_waitForStatusChange(GuestWaitEvent *pEvent, uint32_t fWaitFlags, uint32_t uTimeoutMS,
                                        GuestSessionStatus_T *pSessionStatus, int *pGuestRc)
{
    AssertPtrReturn(pEvent, VERR_INVALID_POINTER);

    VBoxEventType_T evtType;
    ComPtr<IEvent> pIEvent;
    int vrc = waitForEvent(pEvent, uTimeoutMS,
                           &evtType, pIEvent.asOutParam());
    if (RT_SUCCESS(vrc))
    {
        Assert(evtType == VBoxEventType_OnGuestSessionStateChanged);

        ComPtr<IGuestSessionStateChangedEvent> pChangedEvent = pIEvent;
        Assert(!pChangedEvent.isNull());

        GuestSessionStatus_T sessionStatus;
        pChangedEvent->COMGETTER(Status)(&sessionStatus);
        if (pSessionStatus)
            *pSessionStatus = sessionStatus;

        ComPtr<IVirtualBoxErrorInfo> errorInfo;
        HRESULT hr = pChangedEvent->COMGETTER(Error)(errorInfo.asOutParam());
        ComAssertComRC(hr);

        LONG lGuestRc;
        hr = errorInfo->COMGETTER(ResultDetail)(&lGuestRc);
        ComAssertComRC(hr);
        if (RT_FAILURE((int)lGuestRc))
            vrc = VERR_GSTCTL_GUEST_ERROR;
        if (pGuestRc)
            *pGuestRc = (int)lGuestRc;

        LogFlowThisFunc(("Status changed event for session ID=%RU32, new status is: %RU32 (%Rrc)\n",
                         mData.mSession.mID, sessionStatus,
                         RT_SUCCESS((int)lGuestRc) ? VINF_SUCCESS : (int)lGuestRc));
    }

    LogFlowFuncLeaveRC(vrc);
    return vrc;
}

// implementation of public methods
/////////////////////////////////////////////////////////////////////////////

STDMETHODIMP GuestSession::close(void)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    /* Close session on guest. */
    int guestRc = VINF_SUCCESS;
    int rc = i_closeSession(0 /* Flags */, 30 * 1000 /* Timeout */,
                            &guestRc);
    /* On failure don't return here, instead do all the cleanup
     * work first and then return an error. */

    /* Remove ourselves from the session list. */
    int rc2 = mParent->sessionRemove(this);
    if (rc2 == VERR_NOT_FOUND) /* Not finding the session anymore isn't critical. */
        rc2 = VINF_SUCCESS;

    if (RT_SUCCESS(rc))
        rc = rc2;

    LogFlowThisFunc(("Returning rc=%Rrc, guestRc=%Rrc\n",
                     rc, guestRc));
    if (RT_FAILURE(rc))
    {
        if (rc == VERR_GSTCTL_GUEST_ERROR)
            return GuestSession::i_setErrorExternal(this, guestRc);

        return setError(VBOX_E_IPRT_ERROR,
                        tr("Closing guest session failed with %Rrc"), rc);
    }

    return S_OK;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::copyFrom(const com::Utf8Str &aSource, const com::Utf8Str &aDest, const std::vector<CopyFileFlag_T> &aFlags, ComPtr<IProgress> &aProgress)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else

    LogFlowThisFuncEnter();

    if (RT_UNLIKELY((aSource.c_str()) == NULL || *(aSource.c_str()) == '\0'))
        return setError(E_INVALIDARG, tr("No source specified"));
    if (RT_UNLIKELY((aDest.c_str()) == NULL || *(aDest.c_str()) == '\0'))
        return setError(E_INVALIDARG, tr("No destination specified"));

    uint32_t fFlags = CopyFileFlag_None;
    if (aFlags.size())
    {
        for (size_t i = 0; i < aFlags.size(); i++)
            fFlags |= aFlags[i];
    }

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    HRESULT hr = S_OK;

    try
    {
        ComObjPtr<Progress> pProgress;
        SessionTaskCopyFrom *pTask = new SessionTaskCopyFrom(this /* GuestSession */,
                                                             aSource, aDest, fFlags);
        int rc = i_startTaskAsync(Utf8StrFmt(tr("Copying \"%s\" from guest to \"%s\" on the host"), aSource.c_str(), aDest.c_str()),
                                pTask, pProgress);
        if (RT_SUCCESS(rc))
            /* Return progress to the caller. */
            hr = pProgress.queryInterfaceTo(aProgress.asOutParam());
        else
            hr = setError(VBOX_E_IPRT_ERROR,
                          tr("Starting task for copying file \"%s\" from guest to \"%s\" on the host failed: %Rrc"), rc);
    }
    catch(std::bad_alloc &)
    {
        hr = E_OUTOFMEMORY;
    }

    return hr;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::copyTo(const com::Utf8Str &aSource, const com::Utf8Str &aDest, const std::vector<CopyFileFlag_T> &aFlags, ComPtr<IProgress> &aProgress)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else

    LogFlowThisFuncEnter();

    if (RT_UNLIKELY((aSource.c_str()) == NULL || *(aSource.c_str()) == '\0'))
        return setError(E_INVALIDARG, tr("No source specified"));
    if (RT_UNLIKELY((aDest.c_str()) == NULL || *(aDest.c_str()) == '\0'))
        return setError(E_INVALIDARG, tr("No destination specified"));

    uint32_t fFlags = CopyFileFlag_None;
    if (aFlags.size())
    {
        for (size_t i = 0; i < aFlags.size(); i++)
            fFlags |= aFlags[i];
    }

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    HRESULT hr = S_OK;

    try
    {
        ComObjPtr<Progress> pProgress;
        SessionTaskCopyTo *pTask = new SessionTaskCopyTo(this /* GuestSession */,
                                                         aSource, aDest, fFlags);
        AssertPtrReturn(pTask, E_OUTOFMEMORY);
        int rc = i_startTaskAsync(Utf8StrFmt(tr("Copying \"%s\" from host to \"%s\" on the guest"), aSource.c_str(), aDest.c_str()),
                                pTask, pProgress);
        if (RT_SUCCESS(rc))
        {
            /* Return progress to the caller. */
            hr = pProgress.queryInterfaceTo(aProgress.asOutParam());
        }
        else
            hr = setError(VBOX_E_IPRT_ERROR,
                          tr("Starting task for copying file \"%s\" from host to \"%s\" on the guest failed: %Rrc"), rc);
    }
    catch(std::bad_alloc &)
    {
        hr = E_OUTOFMEMORY;
    }

    return hr;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::directoryCreate(const com::Utf8Str &aPath, ULONG aMode,
                                      const std::vector<DirectoryCreateFlag_T> &aFlags)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    if (RT_UNLIKELY((aPath.c_str()) == NULL || *(aPath.c_str()) == '\0'))
        return setError(E_INVALIDARG, tr("No directory to create specified"));

    uint32_t fFlags = DirectoryCreateFlag_None;
    if (aFlags.size())
    {
        for (size_t i = 0; i < aFlags.size(); i++)
            fFlags |= aFlags[i];

        if (fFlags)
            if (!(fFlags & DirectoryCreateFlag_Parents))
                return setError(E_INVALIDARG, tr("Unknown flags (%#x)"), fFlags);
    }

    HRESULT hr = S_OK;

    ComObjPtr <GuestDirectory> pDirectory; int guestRc;
    int rc = i_directoryCreateInternal(aPath, (uint32_t)aMode, fFlags, &guestRc);
    if (RT_FAILURE(rc))
    {
        switch (rc)
        {
            case VERR_GSTCTL_GUEST_ERROR:
                /** @todo Handle VERR_NOT_EQUAL (meaning process exit code <> 0). */
                hr = setError(VBOX_E_IPRT_ERROR, tr("Directory creation failed: Could not create directory"));
                break;

            case VERR_INVALID_PARAMETER:
               hr = setError(VBOX_E_IPRT_ERROR, tr("Directory creation failed: Invalid parameters given"));
               break;

            case VERR_BROKEN_PIPE:
               hr = setError(VBOX_E_IPRT_ERROR, tr("Directory creation failed: Unexpectedly aborted"));
               break;

            default:
               hr = setError(VBOX_E_IPRT_ERROR, tr("Directory creation failed: %Rrc"), rc);
               break;
        }
    }

    return hr;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::directoryCreateTemp(const com::Utf8Str &aTemplateName, ULONG aMode, const com::Utf8Str &aPath, BOOL aSecure, com::Utf8Str &aDirectory)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    if (RT_UNLIKELY((aTemplateName.c_str()) == NULL || *(aTemplateName.c_str()) == '\0'))
        return setError(E_INVALIDARG, tr("No template specified"));
    if (RT_UNLIKELY((aPath.c_str()) == NULL || *(aPath.c_str()) == '\0'))
        return setError(E_INVALIDARG, tr("No directory name specified"));

    HRESULT hr = S_OK;

    int guestRc;
    int rc = i_objectCreateTempInternal(aTemplateName,
                                        aPath,
                                        true /* Directory */, aDirectory, &guestRc);
    if (!RT_SUCCESS(rc))
    {
        switch (rc)
        {
            case VERR_GSTCTL_GUEST_ERROR:
                hr = GuestProcess::i_setErrorExternal(this, guestRc);
                break;

            default:
               hr = setError(VBOX_E_IPRT_ERROR, tr("Temporary directory creation \"%s\" with template \"%s\" failed: %Rrc"),
                             aPath.c_str(), aTemplateName.c_str(), rc);
               break;
        }
    }

    return hr;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::directoryExists(const com::Utf8Str &aPath, BOOL *aExists)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    if (RT_UNLIKELY((aPath.c_str()) == NULL || *(aPath.c_str()) == '\0'))
        return setError(E_INVALIDARG, tr("No directory to check existence for specified"));

    HRESULT hr = S_OK;

    GuestFsObjData objData; int guestRc;
    int rc = i_directoryQueryInfoInternal(aPath, objData, &guestRc);
    if (RT_SUCCESS(rc))
        *aExists = objData.mType == FsObjType_Directory;
    else
    {
        switch (rc)
        {
            case VERR_GSTCTL_GUEST_ERROR:
                hr = GuestProcess::i_setErrorExternal(this, guestRc);
                break;

            default:
               hr = setError(VBOX_E_IPRT_ERROR, tr("Querying directory existence \"%s\" failed: %Rrc"),
                             aPath.c_str(), rc);
               break;
        }
    }

    return hr;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::directoryOpen(const com::Utf8Str &aPath, const com::Utf8Str &aFilter, const std::vector<DirectoryOpenFlag_T> &aFlags, ComPtr<IGuestDirectory> &aDirectory)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    if (RT_UNLIKELY((aPath.c_str()) == NULL || *(aPath.c_str()) == '\0'))
        return setError(E_INVALIDARG, tr("No directory to open specified"));
    if (RT_UNLIKELY((aFilter.c_str()) != NULL && *(aFilter.c_str()) != '\0'))
        return setError(E_INVALIDARG, tr("Directory filters are not implemented yet"));

    uint32_t fFlags = DirectoryOpenFlag_None;
    if (aFlags.size())
    {
        for (size_t i = 0; i < aFlags.size(); i++)
            fFlags |= aFlags[i];

        if (fFlags)
            return setError(E_INVALIDARG, tr("Open flags (%#x) not implemented yet"), fFlags);
    }

    HRESULT hr = S_OK;

    GuestDirectoryOpenInfo openInfo;
    openInfo.mPath = aPath;
    openInfo.mFilter = aFilter;
    openInfo.mFlags = fFlags;

    ComObjPtr <GuestDirectory> pDirectory; int guestRc;
    int rc = i_directoryOpenInternal(openInfo, pDirectory, &guestRc);
    if (RT_SUCCESS(rc))
    {
        /* Return directory object to the caller. */
        hr = pDirectory.queryInterfaceTo(aDirectory.asOutParam());
    }
    else
    {
        switch (rc)
        {
            case VERR_INVALID_PARAMETER:
               hr = setError(VBOX_E_IPRT_ERROR, tr("Opening directory \"%s\" failed; invalid parameters given",
                                                   aPath.c_str()));
               break;

            case VERR_GSTCTL_GUEST_ERROR:
                hr = GuestDirectory::i_setErrorExternal(this, guestRc);
                break;

            default:
               hr = setError(VBOX_E_IPRT_ERROR, tr("Opening directory \"%s\" failed: %Rrc"),
                             aPath.c_str(),rc);
               break;
        }
    }

    return hr;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::directoryQueryInfo(const com::Utf8Str &aPath, ComPtr<IGuestFsObjInfo> &aInfo)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    if (RT_UNLIKELY((aPath.c_str()) == NULL || *(aPath.c_str()) == '\0'))
        return setError(E_INVALIDARG, tr("No directory to query information for specified"));

    HRESULT hr = S_OK;

    GuestFsObjData objData; int guestRc;
    int vrc = i_directoryQueryInfoInternal(aPath, objData, &guestRc);
    if (RT_SUCCESS(vrc))
    {
        if (objData.mType == FsObjType_Directory)
        {
            ComObjPtr<GuestFsObjInfo> pFsObjInfo;
            hr = pFsObjInfo.createObject();
            if (FAILED(hr)) return hr;

            vrc = pFsObjInfo->init(objData);
            if (RT_SUCCESS(vrc))
            {
                hr = pFsObjInfo.queryInterfaceTo(aInfo.asOutParam());
                if (FAILED(hr)) return hr;
            }
        }
    }

    if (RT_FAILURE(vrc))
    {
        switch (vrc)
        {
            case VERR_GSTCTL_GUEST_ERROR:
                hr = GuestProcess::i_setErrorExternal(this, guestRc);
                break;

            case VERR_NOT_A_DIRECTORY:
                hr = setError(VBOX_E_IPRT_ERROR, tr("Element \"%s\" exists but is not a directory",
                                                    aPath.c_str()));
                break;

            default:
                hr = setError(VBOX_E_IPRT_ERROR, tr("Querying directory information for \"%s\" failed: %Rrc"),
                              aPath.c_str(), vrc);
                break;
        }
    }

    return hr;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::directoryRemove(const com::Utf8Str &aPath)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    if (RT_UNLIKELY((aPath.c_str()) == NULL || *(aPath.c_str()) == '\0'))
        return setError(E_INVALIDARG, tr("No directory to remove specified"));

    HRESULT hr = i_isReadyExternal();
    if (FAILED(hr))
        return hr;

    /* No flags; only remove the directory when empty. */
    uint32_t uFlags = 0;

    int guestRc;
    int vrc = i_directoryRemoveInternal(aPath, uFlags, &guestRc);
    if (RT_FAILURE(vrc))
    {
        switch (vrc)
        {
            case VERR_NOT_SUPPORTED:
                hr = setError(VBOX_E_IPRT_ERROR,
                              tr("Handling removing guest directories not supported by installed Guest Additions"));
                break;

            case VERR_GSTCTL_GUEST_ERROR:
                hr = GuestDirectory::i_setErrorExternal(this, guestRc);
                break;

            default:
                hr = setError(VBOX_E_IPRT_ERROR, tr("Removing guest directory \"%s\" failed: %Rrc"),
                              aPath.c_str(), vrc);
                break;
        }
    }

    return hr;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::directoryRemoveRecursive(const com::Utf8Str &aPath, const std::vector<DirectoryRemoveRecFlag_T> &aFlags, ComPtr<IProgress> &aProgress)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    if (RT_UNLIKELY((aPath.c_str()) == NULL || *(aPath.c_str()) == '\0'))
        return setError(E_INVALIDARG, tr("No directory to remove recursively specified"));

    HRESULT hr = i_isReadyExternal();
    if (FAILED(hr))
        return hr;

    ComObjPtr<Progress> pProgress;
    hr = pProgress.createObject();
    if (SUCCEEDED(hr))
        hr = pProgress->init(static_cast<IGuestSession *>(this),
                             Bstr(tr("Removing guest directory")).raw(),
                             TRUE /*aCancelable*/);
    if (FAILED(hr))
        return hr;

    /* Note: At the moment we don't supply progress information while
     *       deleting a guest directory recursively. So just complete
     *       the progress object right now. */
     /** @todo Implement progress reporting on guest directory deletion! */
    hr = pProgress->notifyComplete(S_OK);
    if (FAILED(hr))
        return hr;

    /* Remove the directory + all its contents. */
    uint32_t uFlags = DIRREMOVE_FLAG_RECURSIVE
                    | DIRREMOVE_FLAG_CONTENT_AND_DIR;
    int guestRc;
    int vrc = i_directoryRemoveInternal(aPath, uFlags, &guestRc);
    if (RT_FAILURE(vrc))
    {
        switch (vrc)
        {
            case VERR_NOT_SUPPORTED:
                hr = setError(VBOX_E_IPRT_ERROR,
                              tr("Handling removing guest directories recursively not supported by installed Guest Additions"));
                break;

            case VERR_GSTCTL_GUEST_ERROR:
                hr = GuestFile::i_setErrorExternal(this, guestRc);
                break;

            default:
                hr = setError(VBOX_E_IPRT_ERROR, tr("Recursively removing guest directory \"%s\" failed: %Rrc"),
                              aPath.c_str(), vrc);
                break;
        }
    }
    else
    {
        pProgress.queryInterfaceTo(aProgress.asOutParam());
    }

    return hr;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::directoryRename(const com::Utf8Str &aSource,
                                      const com::Utf8Str &aDest,
                                      const std::vector<PathRenameFlag_T> &aFlags)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    if (RT_UNLIKELY((aSource.c_str()) == NULL || *(aSource.c_str()) == '\0'))
        return setError(E_INVALIDARG, tr("No source directory to rename specified"));

    if (RT_UNLIKELY((aDest.c_str()) == NULL || *(aDest.c_str()) == '\0'))
        return setError(E_INVALIDARG, tr("No destination directory to rename the source to specified"));

    HRESULT hr = i_isReadyExternal();
    if (FAILED(hr))
        return hr;

    /* No flags; only remove the directory when empty. */
    uint32_t uFlags = 0;

    int guestRc;
    int vrc = i_pathRenameInternal(aSource, aDest, uFlags, &guestRc);
    if (RT_FAILURE(vrc))
    {
        switch (vrc)
        {
            case VERR_NOT_SUPPORTED:
                hr = setError(VBOX_E_IPRT_ERROR,
                              tr("Handling renaming guest directories not supported by installed Guest Additions"));
                break;

            case VERR_GSTCTL_GUEST_ERROR:
                hr = setError(VBOX_E_IPRT_ERROR,
                              tr("Renaming guest directory failed: %Rrc"), guestRc);
                break;

            default:
                hr = setError(VBOX_E_IPRT_ERROR, tr("Renaming guest directory \"%s\" failed: %Rrc"),
                              aSource.c_str(), vrc);
                break;
        }
    }

    return hr;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::directorySetACL(const com::Utf8Str &aPath, const com::Utf8Str &aAcl)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    ReturnComNotImplemented();
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::environmentClear()
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    mData.mEnvironment.Clear();

    LogFlowThisFuncLeave();
    return S_OK;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::environmentGet(const com::Utf8Str &aName, com::Utf8Str &aValue)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    if (RT_UNLIKELY(aName.c_str() == NULL) || *(aName.c_str()) == '\0')
        return setError(E_INVALIDARG, tr("No value name specified"));

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    aValue =  mData.mEnvironment.Get(aName);

    LogFlowThisFuncLeave();
    return S_OK;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::environmentSet(const com::Utf8Str &aName, const com::Utf8Str &aValue)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    if (RT_UNLIKELY((aName.c_str() == NULL) || *(aName.c_str()) == '\0'))
        return setError(E_INVALIDARG, tr("No value name specified"));

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    int rc = mData.mEnvironment.Set(aName, aValue);

    HRESULT hr = RT_SUCCESS(rc) ? S_OK : VBOX_E_IPRT_ERROR;
    LogFlowFuncLeaveRC(hr);
    return hr;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::environmentUnset(const com::Utf8Str &aName)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    mData.mEnvironment.Unset(aName);

    LogFlowThisFuncLeave();
    return S_OK;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::fileCreateTemp(const com::Utf8Str &aTemplateName, ULONG aMode, const com::Utf8Str &aPath, BOOL aSecure, ComPtr<IGuestFile> &aFile)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();


    ReturnComNotImplemented();
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::fileExists(const com::Utf8Str &aPath, BOOL *aExists)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    if (RT_UNLIKELY((aPath.c_str()) == NULL || *(aPath.c_str()) == '\0'))
        return setError(E_INVALIDARG, tr("No file to check existence for specified"));

    GuestFsObjData objData; int guestRc;
    int vrc = i_fileQueryInfoInternal(aPath, objData, &guestRc);
    if (RT_SUCCESS(vrc))
    {
        *aExists = TRUE;
        return S_OK;
    }

    HRESULT hr = S_OK;

    switch (vrc)
    {
        case VERR_GSTCTL_GUEST_ERROR:
            hr = GuestProcess::i_setErrorExternal(this, guestRc);
            break;

        case VERR_NOT_A_FILE:
            *aExists = FALSE;
            break;

        default:
            hr = setError(VBOX_E_IPRT_ERROR, tr("Querying file information for \"%s\" failed: %Rrc"),
                          aPath.c_str(), vrc);
            break;
    }

    return hr;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::fileRemove(const com::Utf8Str &aPath)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    if (RT_UNLIKELY((aPath.c_str()) == NULL || *(aPath.c_str()) == '\0'))
        return setError(E_INVALIDARG, tr("No file to remove specified"));

    HRESULT hr = S_OK;

    int guestRc;
    int vrc = i_fileRemoveInternal(aPath, &guestRc);
    if (RT_FAILURE(vrc))
    {
        switch (vrc)
        {
            case VERR_GSTCTL_GUEST_ERROR:
                hr = GuestProcess::i_setErrorExternal(this, guestRc);
                break;

            default:
                hr = setError(VBOX_E_IPRT_ERROR, tr("Removing file \"%s\" failed: %Rrc"),
                              aPath.c_str(), vrc);
                break;
        }
    }

    return hr;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::fileOpen(const com::Utf8Str &aPath, const com::Utf8Str &aOpenMode, const com::Utf8Str &aDisposition, ULONG aCreationMode, ComPtr<IGuestFile> &aFile)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    Bstr strSharingMode = ""; /* Sharing mode is ignored. */

    return fileOpenEx(aPath, aOpenMode, aDisposition, strSharingMode.raw(), aCreationMode,
                      0 /* aOffset */, aFile);
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::fileOpenEx(const com::Utf8Str &aPath, const com::Utf8Str &aOpenMode, const com::Utf8Str &aDisposition, const com::Utf8Str &aSharingMode, ULONG aCreationMode, LONG64 aOffset, ComPtr<IGuestFile> &aFile)

{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    if (RT_UNLIKELY((aPath.c_str()) == NULL || *(aPath.c_str()) == '\0'))
        return setError(E_INVALIDARG, tr("No file to open specified"));
    if (RT_UNLIKELY((aOpenMode.c_str()) == NULL || *(aOpenMode.c_str()) == '\0'))
        return setError(E_INVALIDARG, tr("No open mode specified"));
    if (RT_UNLIKELY((aDisposition.c_str()) == NULL || *(aDisposition.c_str()) == '\0'))
        return setError(E_INVALIDARG, tr("No disposition mode specified"));
    /* aSharingMode is optional. */

    HRESULT hr = i_isReadyExternal();
    if (FAILED(hr))
        return hr;

    /** @todo Validate creation mode. */
    uint32_t uCreationMode = 0;

    GuestFileOpenInfo openInfo;
    openInfo.mFileName = aPath;
    openInfo.mOpenMode = aOpenMode;
    openInfo.mDisposition = aDisposition;
    openInfo.mSharingMode = aSharingMode;
    openInfo.mCreationMode = aCreationMode;
    openInfo.mInitialOffset = aOffset;

    uint64_t uFlagsIgnored;
    int vrc = RTFileModeToFlagsEx(openInfo.mOpenMode.c_str(),
                                  openInfo.mDisposition.c_str(),
                                  openInfo.mSharingMode.c_str(),
                                  &uFlagsIgnored);
    if (RT_FAILURE(vrc))
        return setError(E_INVALIDARG, tr("Invalid open mode / disposition / sharing mode specified"));

    ComObjPtr <GuestFile> pFile; int guestRc;
    vrc = i_fileOpenInternal(openInfo, pFile, &guestRc);
    if (RT_SUCCESS(vrc))
        /* Return directory object to the caller. */
        hr = pFile.queryInterfaceTo(aFile.asOutParam());
    else
    {
        switch (vrc)
        {
            case VERR_NOT_SUPPORTED:
                hr = setError(VBOX_E_IPRT_ERROR,
                              tr("Handling guest files not supported by installed Guest Additions"));
                break;

            case VERR_GSTCTL_GUEST_ERROR:
                hr = GuestFile::i_setErrorExternal(this, guestRc);
                break;

            default:
                hr = setError(VBOX_E_IPRT_ERROR, tr("Opening guest file \"%s\" failed: %Rrc"),
                              aPath.c_str(), vrc);
                break;
        }
    }

    return hr;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::fileQueryInfo(const com::Utf8Str &aPath, ComPtr<IGuestFsObjInfo> &aInfo)

{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    if (RT_UNLIKELY((aPath.c_str()) == NULL || *(aPath.c_str()) == '\0'))
        return setError(E_INVALIDARG, tr("No file to query information for specified"));

    HRESULT hr = S_OK;

    GuestFsObjData objData; int guestRc;
    int vrc = i_fileQueryInfoInternal(aPath, objData, &guestRc);
    if (RT_SUCCESS(vrc))
    {
        ComObjPtr<GuestFsObjInfo> pFsObjInfo;
        hr = pFsObjInfo.createObject();
        if (FAILED(hr)) return hr;

        vrc = pFsObjInfo->init(objData);
        if (RT_SUCCESS(vrc))
        {
            hr = pFsObjInfo.queryInterfaceTo(aInfo.asOutParam());
            if (FAILED(hr)) return hr;
        }
    }

    if (RT_FAILURE(vrc))
    {
        switch (vrc)
        {
            case VERR_GSTCTL_GUEST_ERROR:
                hr = GuestProcess::i_setErrorExternal(this, guestRc);
                break;

            case VERR_NOT_A_FILE:
                hr = setError(VBOX_E_IPRT_ERROR, tr("Element exists but is not a file"));
                break;

            default:
               hr = setError(VBOX_E_IPRT_ERROR, tr("Querying file information failed: %Rrc"), vrc);
               break;
        }
    }

    return hr;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::fileQuerySize(const com::Utf8Str &aPath, LONG64 *aSize)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    if (RT_UNLIKELY((aPath.c_str()) == NULL || *(aPath.c_str()) == '\0'))
        return setError(E_INVALIDARG, tr("No file to query size for specified"));

    HRESULT hr = S_OK;

    int64_t llSize; int guestRc;
    int vrc = i_fileQuerySizeInternal(aPath, &llSize, &guestRc);
    if (RT_SUCCESS(vrc))
        *aSize = llSize;
    else
    {
        switch (vrc)
        {
            case VERR_GSTCTL_GUEST_ERROR:
                hr = GuestProcess::i_setErrorExternal(this, guestRc);
                break;

            default:
               hr = setError(VBOX_E_IPRT_ERROR, tr("Querying file size failed: %Rrc"), vrc);
               break;
        }
    }

    return hr;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::fileRename(const com::Utf8Str &aSource, const com::Utf8Str &aDest, const std::vector<PathRenameFlag_T> &aFlags)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    if (RT_UNLIKELY((aSource.c_str()) == NULL || *(aSource.c_str()) == '\0'))
        return setError(E_INVALIDARG, tr("No source file to rename specified"));

    if (RT_UNLIKELY((aDest.c_str()) == NULL || *(aDest.c_str()) == '\0'))
        return setError(E_INVALIDARG, tr("No destination file to rename the source to specified"));

    HRESULT hr = i_isReadyExternal();
    if (FAILED(hr))
        return hr;

    /* No flags; only remove the directory when empty. */
    uint32_t uFlags = 0;

    int guestRc;
    int vrc = i_pathRenameInternal(aSource, aDest, uFlags, &guestRc);
    if (RT_FAILURE(vrc))
    {
        switch (vrc)
        {
            case VERR_NOT_SUPPORTED:
                hr = setError(VBOX_E_IPRT_ERROR,
                              tr("Handling renaming guest files not supported by installed Guest Additions"));
                break;

            case VERR_GSTCTL_GUEST_ERROR:
                /** @todo Proper guestRc to text translation needed. */
                hr = setError(VBOX_E_IPRT_ERROR,
                              tr("Renaming guest file failed: %Rrc"), guestRc);
                break;

            default:
                hr = setError(VBOX_E_IPRT_ERROR, tr("Renaming guest file \"%s\" failed: %Rrc"),
                              aSource.c_str(), vrc);
                break;
        }
    }

    return hr;
#endif /* VBOX_WITH_GUEST_CONTROL */
}
HRESULT GuestSession::fileSetACL(const com::Utf8Str &aFile, const com::Utf8Str &aAcl)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    ReturnComNotImplemented();
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::processCreate(const com::Utf8Str &aCommand, const std::vector<com::Utf8Str> &aArguments, const std::vector<com::Utf8Str> &aEnvironment, const std::vector<ProcessCreateFlag_T> &aFlags, ULONG aTimeoutMS, ComPtr<IGuestProcess> &aGuestProcess)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    std::vector<LONG> affinityIgnored;

    return processCreateEx(aCommand, aArguments, aEnvironment, aFlags, aTimeoutMS, ProcessPriority_Default, affinityIgnored, aGuestProcess);
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::processCreateEx(const com::Utf8Str &aCommand, const std::vector<com::Utf8Str> &aArguments, const std::vector<com::Utf8Str> &aEnvironment, const std::vector<ProcessCreateFlag_T> &aFlags, ULONG aTimeoutMS, ProcessPriority_T aPriority, const std::vector<LONG> &aAffinity, ComPtr<IGuestProcess> &aGuestProcess)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    if (RT_UNLIKELY((aCommand.c_str()) == NULL || *(aCommand.c_str()) == '\0'))
        return setError(E_INVALIDARG, tr("No command to execute specified"));

    HRESULT hr = i_isReadyExternal();
    if (FAILED(hr))
        return hr;

    GuestProcessStartupInfo procInfo;
    procInfo.mCommand = aCommand;

    if (aArguments.size())
        for (size_t i = 0; i < aArguments.size(); i++)
            procInfo.mArguments.push_back(aArguments[i]);

    int rc = VINF_SUCCESS;

    /*
     * Create the process environment:
     * - Apply the session environment in a first step, and
     * - Apply environment variables specified by this call to
     *   have the chance of overwriting/deleting session entries.
     */
    procInfo.mEnvironment = mData.mEnvironment; /* Apply original session environment. */

    if (aEnvironment.size())
        for (size_t i = 0; i < aEnvironment.size() && RT_SUCCESS(rc); i++)
            rc = procInfo.mEnvironment.Set(aEnvironment[i]);

    if (RT_SUCCESS(rc))
    {
        if (aFlags.size())
            for (size_t i = 0; i < aFlags.size(); i++)
                procInfo.mFlags |= aFlags[i];

        procInfo.mTimeoutMS = aTimeoutMS;

        if (aAffinity.size())
            for (size_t i = 0; i < aAffinity.size(); i++)
                if (aAffinity[i])
                    procInfo.mAffinity |= (uint64_t)1 << i;

        procInfo.mPriority = aPriority;

        ComObjPtr<GuestProcess> pProcess;
        rc = i_processCreateExInteral(procInfo, pProcess);
        if (RT_SUCCESS(rc))
        {
            /* Return guest session to the caller. */
            HRESULT hr2 = pProcess.queryInterfaceTo(aGuestProcess.asOutParam());
            if (FAILED(hr2))
                rc = VERR_COM_OBJECT_NOT_FOUND;

            if (RT_SUCCESS(rc))
                rc = pProcess->i_startProcessAsync();
        }
    }

    if (RT_FAILURE(rc))
    {
        switch (rc)
        {
            case VERR_MAX_PROCS_REACHED:
                hr = setError(VBOX_E_IPRT_ERROR, tr("Maximum number of concurrent guest processes per session (%ld) reached"),
                                                    VBOX_GUESTCTRL_MAX_OBJECTS);
                break;

            /** @todo Add more errors here. */

            default:
                hr = setError(VBOX_E_IPRT_ERROR, tr("Could not create guest process, rc=%Rrc"), rc);
                break;
        }
    }

    LogFlowFuncLeaveRC(rc);
    return hr;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::processGet(ULONG aPid, ComPtr<IGuestProcess> &aGuestProcess)

{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFunc(("PID=%RU32\n", aPid));

    if (aPid == 0)
        return setError(E_INVALIDARG, tr("No valid process ID (PID) specified"));

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    HRESULT hr = S_OK;

    ComObjPtr<GuestProcess> pProcess;
    int rc = i_processGetByPID(aPid, &pProcess);
    if (RT_FAILURE(rc))
        hr = setError(E_INVALIDARG, tr("No process with PID %RU32 found"), aPid);

    /* This will set (*aProcess) to NULL if pProgress is NULL. */
    HRESULT hr2 = pProcess.queryInterfaceTo(aGuestProcess.asOutParam());
    if (SUCCEEDED(hr))
        hr = hr2;

    LogFlowThisFunc(("aProcess=%p, hr=%Rhrc\n", (IGuestProcess*)aGuestProcess, hr));
    return hr;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::symlinkCreate(const com::Utf8Str &aSource, const com::Utf8Str &aTarget, SymlinkType_T aType)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    ReturnComNotImplemented();
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::symlinkExists(const com::Utf8Str &aSymlink, BOOL *aExists)

{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    ReturnComNotImplemented();
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::symlinkRead(const com::Utf8Str &aSymlink, const std::vector<SymlinkReadFlag_T> &aFlags, com::Utf8Str &aTarget)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    ReturnComNotImplemented();
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::symlinkRemoveDirectory(const com::Utf8Str &aPath)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    ReturnComNotImplemented();
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::symlinkRemoveFile(const com::Utf8Str &aFile)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    ReturnComNotImplemented();
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::waitFor(ULONG aWaitFor, ULONG aTimeoutMS, GuestSessionWaitResult_T *aReason)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    /*
     * Note: Do not hold any locks here while waiting!
     */
    HRESULT hr = S_OK;

    int guestRc; GuestSessionWaitResult_T waitResult;
    int vrc = i_waitFor(aWaitFor, aTimeoutMS, waitResult, &guestRc);
    if (RT_SUCCESS(vrc))
        *aReason = waitResult;
    else
    {
        switch (vrc)
        {
            case VERR_GSTCTL_GUEST_ERROR:
                hr = GuestSession::i_setErrorExternal(this, guestRc);
                break;

            case VERR_TIMEOUT:
                *aReason = GuestSessionWaitResult_Timeout;
                break;

            default:
            {
                const char *pszSessionName = mData.mSession.mName.c_str();
                hr = setError(VBOX_E_IPRT_ERROR,
                              tr("Waiting for guest session \"%s\" failed: %Rrc"),
                                 pszSessionName ? pszSessionName : tr("Unnamed"), vrc);
                break;
            }
        }
    }

    LogFlowFuncLeaveRC(vrc);
    return hr;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

HRESULT GuestSession::waitForArray(const std::vector<GuestSessionWaitForFlag_T> &aWaitFor, ULONG aTimeoutMS, GuestSessionWaitResult_T *aReason)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    /*
     * Note: Do not hold any locks here while waiting!
     */
    uint32_t fWaitFor = GuestSessionWaitForFlag_None;
    for (size_t i = 0; i < aWaitFor.size(); i++)
        fWaitFor |= aWaitFor[i];

    return WaitFor(fWaitFor, aTimeoutMS, aReason);
#endif /* VBOX_WITH_GUEST_CONTROL */
}

