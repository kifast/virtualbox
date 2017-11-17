#include "internal/path.h"
        rc = RTNtPathRelativeFromUtf8(&NtName, &hRoot, pszRelFilename, RTDIRREL_NT_GET_ASCENT(pThis),
                                      pThis->enmInfoClass == FileMaximumInformation);
    int rc = RTNtPathRelativeFromUtf8(&NtName, &hRoot, pszDirAndFilter, RTDIRREL_NT_GET_ASCENT(pThis),
                                      pThis->enmInfoClass == FileMaximumInformation);
        rc = rtDirOpenRelativeOrHandle(phDir, pszDirAndFilter, enmFilter, fFlags, (uintptr_t)hRoot, &NtName);
RTDECL(int) RTDirRelDirCreate(PRTDIR hDir, const char *pszRelPath, RTFMODE fMode, uint32_t fCreate, PRTDIR *phSubDir)
    AssertReturn(!(fCreate & ~RTDIRCREATE_FLAGS_VALID_MASK), VERR_INVALID_FLAGS);
    fMode = rtFsModeNormalize(fMode, pszRelPath, 0);
    AssertReturn(rtFsModeIsValidPermissions(fMode), VERR_INVALID_FMODE);
    AssertPtrNullReturn(phSubDir, VERR_INVALID_POINTER);
    /*
     * Convert and normalize the path.
     */
    UNICODE_STRING NtName;
    HANDLE hRoot = pThis->hDir;
    int rc = RTNtPathRelativeFromUtf8(&NtName, &hRoot, pszRelPath, RTDIRREL_NT_GET_ASCENT(pThis),
                                      pThis->enmInfoClass == FileMaximumInformation);
    {
        HANDLE              hNewDir = RTNT_INVALID_HANDLE_VALUE;
        IO_STATUS_BLOCK     Ios     = RTNT_IO_STATUS_BLOCK_INITIALIZER;
        OBJECT_ATTRIBUTES   ObjAttr;
        InitializeObjectAttributes(&ObjAttr, &NtName, 0 /*fAttrib*/, hRoot, NULL);

        ULONG fDirAttribs = (fCreate & RTFS_DOS_MASK_NT) >> RTFS_DOS_SHIFT;
        if (!(fCreate & RTDIRCREATE_FLAGS_NOT_CONTENT_INDEXED_DONT_SET))
            fDirAttribs |= FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
        if (!fDirAttribs)
            fDirAttribs = FILE_ATTRIBUTE_NORMAL;

        NTSTATUS rcNt = NtCreateFile(&hNewDir,
                                     phSubDir
                                     ? FILE_WRITE_ATTRIBUTES | FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY | FILE_TRAVERSE | SYNCHRONIZE
                                     : SYNCHRONIZE,
                                     &ObjAttr,
                                     &Ios,
                                     NULL /*AllocationSize*/,
                                     fDirAttribs,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     FILE_CREATE,
                                     FILE_DIRECTORY_FILE | FILE_OPEN_FOR_BACKUP_INTENT | FILE_SYNCHRONOUS_IO_NONALERT,
                                     NULL /*EaBuffer*/,
                                     0 /*EaLength*/);

        /* Just in case someone takes offence at FILE_ATTRIBUTE_NOT_CONTENT_INDEXED. */
        if (   (   rcNt == STATUS_INVALID_PARAMETER
                || rcNt == STATUS_INVALID_PARAMETER_7)
            && (fDirAttribs & FILE_ATTRIBUTE_NOT_CONTENT_INDEXED)
            && (fCreate & RTDIRCREATE_FLAGS_NOT_CONTENT_INDEXED_NOT_CRITICAL) )
        {
            fDirAttribs &= ~FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
            if (!fDirAttribs)
                fDirAttribs = FILE_ATTRIBUTE_NORMAL;
            rcNt = NtCreateFile(&hNewDir,
                                phSubDir
                                ? FILE_WRITE_ATTRIBUTES | FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY | FILE_TRAVERSE | SYNCHRONIZE
                                : SYNCHRONIZE,
                                &ObjAttr,
                                &Ios,
                                NULL /*AllocationSize*/,
                                fDirAttribs,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                FILE_CREATE,
                                FILE_DIRECTORY_FILE | FILE_OPEN_FOR_BACKUP_INTENT | FILE_SYNCHRONOUS_IO_NONALERT,
                                NULL /*EaBuffer*/,
                                0 /*EaLength*/);
        }

        if (NT_SUCCESS(rcNt))
        {
            if (!phSubDir)
            {
                NtClose(hNewDir);
                rc = VINF_SUCCESS;
            }
            else
            {
                rc = rtDirOpenRelativeOrHandle(phSubDir, pszRelPath, RTDIRFILTER_NONE, 0 /*fFlags*/,
                                               (uintptr_t)hNewDir, NULL /*pvNativeRelative*/);
                if (RT_FAILURE(rc))
                    NtClose(hNewDir);
            }
        }
        else
            rc = RTErrConvertFromNtStatus(rcNt);
        RTNtPathFree(&NtName, NULL);
    }
    /*
     * Convert and normalize the path.
     */
    UNICODE_STRING NtName;
    HANDLE hRoot = pThis->hDir;
    int rc = RTNtPathRelativeFromUtf8(&NtName, &hRoot, pszRelPath, RTDIRREL_NT_GET_ASCENT(pThis),
                                      pThis->enmInfoClass == FileMaximumInformation);
    {
        HANDLE              hSubDir = RTNT_INVALID_HANDLE_VALUE;
        IO_STATUS_BLOCK     Ios     = RTNT_IO_STATUS_BLOCK_INITIALIZER;
        OBJECT_ATTRIBUTES   ObjAttr;
        InitializeObjectAttributes(&ObjAttr, &NtName, 0 /*fAttrib*/, hRoot, NULL);

        NTSTATUS rcNt = NtCreateFile(&hSubDir,
                                     DELETE | SYNCHRONIZE,
                                     &ObjAttr,
                                     &Ios,
                                     NULL /*AllocationSize*/,
                                     FILE_ATTRIBUTE_NORMAL,
                                     FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     FILE_OPEN,
                                     FILE_DIRECTORY_FILE | FILE_OPEN_FOR_BACKUP_INTENT | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT,
                                     NULL /*EaBuffer*/,
                                     0 /*EaLength*/);
        if (NT_SUCCESS(rcNt))
        {
            FILE_DISPOSITION_INFORMATION DispInfo;
            DispInfo.DeleteFile = TRUE;
            RTNT_IO_STATUS_BLOCK_REINIT(&Ios);
            rcNt = NtSetInformationFile(hSubDir, &Ios, &DispInfo, sizeof(DispInfo), FileDispositionInformation);

            NTSTATUS rcNt2 = NtClose(hSubDir);
            if (!NT_SUCCESS(rcNt2) && NT_SUCCESS(rcNt))
                rcNt = rcNt2;
        }

        if (NT_SUCCESS(rcNt))
            rc = VINF_SUCCESS;
        else
            rc = RTErrConvertFromNtStatus(rcNt);

        RTNtPathFree(&NtName, NULL);
    }
    /*
     * Validate and convert flags.
     */
    UNICODE_STRING NtName;
    HANDLE hRoot = pThis->hDir;
    int rc = RTNtPathRelativeFromUtf8(&NtName, &hRoot, pszRelPath, RTDIRREL_NT_GET_ASCENT(pThis),
                                      pThis->enmInfoClass == FileMaximumInformation);
    {
        rc = rtPathNtQueryInfoWorker(hRoot, &NtName, pObjInfo, enmAddAttr, fFlags, pszRelPath);
        RTNtPathFree(&NtName, NULL);
    }
RTAssertMsg2("DBG: RTDirRelPathSetMode(%s)...\n", szPath);
    {
RTAssertMsg2("DBG: RTDirRelPathSetTimes(%s)...\n", szPath);
    }
RTAssertMsg2("DBG: RTDirRelPathSetOwner(%s)...\n", szPath);
        {
RTAssertMsg2("DBG: RTDirRelPathRename(%s,%s)...\n", szSrcPath, szDstPath);
            rc = RTPathRename(szSrcPath, szDstPath, fRename);
        }
    {
RTAssertMsg2("DBG: RTDirRelPathUnlink(%s)...\n", szPath);
    }
    {
RTAssertMsg2("DBG: RTDirRelSymlinkCreate(%s)...\n", szPath);
    }
    {
RTAssertMsg2("DBG: RTDirRelSymlinkRead(%s)...\n", szPath);
    }