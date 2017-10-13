#include "ocrlockedfile.h"
#include <qt_windows.h>
#include <QFileInfo>

#define MUTEX_PREFIX "OCRLockedFile mutex "
// Maximum number of concurrent read locks. Must not be greater than MAXIMUM_WAIT_OBJECTS
#define MAX_READERS MAXIMUM_WAIT_OBJECTS

Qt::HANDLE OCRLockedFile::getMutexHandle(int idx, bool doCreate)
{
    if (mutexname.isEmpty()) {
        QFileInfo fi(*this);
        mutexname = QString::fromLatin1(MUTEX_PREFIX)
                    + fi.absoluteFilePath().toLower();
    }
    QString mname(mutexname);
    if (idx >= 0)
        mname += QString::number(idx);

    Qt::HANDLE mutex;
    if (doCreate) {
        mutex = CreateMutexA(NULL, FALSE, mname.toLocal8Bit().constData());
        if (!mutex) {
            qErrnoWarning("OCRLockedFile::lock(): CreateMutex failed");
            return 0;
        }
    }
    else {
        mutex = OpenMutexA(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, mname.toLocal8Bit().constData());
        if (!mutex) {
            if (GetLastError() != ERROR_FILE_NOT_FOUND)
                qErrnoWarning("OCRLockedFile::lock(): OpenMutex failed");
            return 0;
        }
    }
    return mutex;
}

bool OCRLockedFile::waitMutex(Qt::HANDLE mutex, bool doBlock)
{
    Q_ASSERT(mutex);
    DWORD res = WaitForSingleObject(mutex, doBlock ? INFINITE : 0);
    switch (res) {
    case WAIT_OBJECT_0:
    case WAIT_ABANDONED:
        return true;
        break;
    case WAIT_TIMEOUT:
        break;
    default:
        qErrnoWarning("OCRLockedFile::lock(): WaitForSingleObject failed");
    }
    return false;
}



bool OCRLockedFile::lock(LockMode mode, bool block)
{
    if (!isOpen()) {
        qWarning("OCRLockedFile::lock(): file is not opened");
        return false;
    }

    if (mode == NoLock)
        return unlock();

    if (mode == m_lock_mode)
        return true;

    if (m_lock_mode != NoLock)
        unlock();

    if (!wmutex && !(wmutex = getMutexHandle(-1, true)))
        return false;

    if (!waitMutex(wmutex, block))
        return false;

    if (mode == ReadLock) {
        int idx = 0;
        for (; idx < MAX_READERS; idx++) {
            rmutex = getMutexHandle(idx, false);
            if (!rmutex || waitMutex(rmutex, false))
                break;
            CloseHandle(rmutex);
        }
        bool ok = true;
        if (idx >= MAX_READERS) {
            qWarning("OCRLockedFile::lock(): too many readers");
            rmutex = 0;
            ok = false;
        }
        else if (!rmutex) {
            rmutex = getMutexHandle(idx, true);
            if (!rmutex || !waitMutex(rmutex, false))
                ok = false;
        }
        if (!ok && rmutex) {
            CloseHandle(rmutex);
            rmutex = 0;
        }
        ReleaseMutex(wmutex);
        if (!ok)
            return false;
    }
    else {
        Q_ASSERT(rmutexes.isEmpty());
        for (int i = 0; i < MAX_READERS; i++) {
            Qt::HANDLE mutex = getMutexHandle(i, false);
            if (mutex)
                rmutexes.append(mutex);
        }
        if (rmutexes.size()) {
            DWORD res = WaitForMultipleObjects(rmutexes.size(), rmutexes.constData(),
                                               TRUE, block ? INFINITE : 0);
            if (res != WAIT_OBJECT_0 && res != WAIT_ABANDONED) {
                if (res != WAIT_TIMEOUT)
                    qErrnoWarning("OCRLockedFile::lock(): WaitForMultipleObjects failed");
                m_lock_mode = WriteLock;  // trick unlock() to clean up - semiyucky
                unlock();
                return false;
            }
        }
    }

    m_lock_mode = mode;
    return true;
}

bool OCRLockedFile::unlock()
{
    if (!isOpen()) {
        qWarning("OCRLockedFile::unlock(): file is not opened");
        return false;
    }

    if (!isLocked())
        return true;

    if (m_lock_mode == ReadLock) {
        ReleaseMutex(rmutex);
        CloseHandle(rmutex);
        rmutex = 0;
    }
    else {
        foreach(Qt::HANDLE mutex, rmutexes) {
            ReleaseMutex(mutex);
            CloseHandle(mutex);
        }
        rmutexes.clear();
        ReleaseMutex(wmutex);
    }

    m_lock_mode = OCRLockedFile::NoLock;
    return true;
}

OCRLockedFile::~OCRLockedFile()
{
    if (isOpen())
        unlock();
    if (wmutex)
        CloseHandle(wmutex);
}
