/*
 * Copyright (C) Texas Instruments - http://www.ti.com/
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */


#ifndef DEBUG_UTILS_H
#define DEBUG_UTILS_H

#include <android/log.h>
#include <utils/threads.h>
#include <utils/KeyedVector.h>




namespace Ti {




class Debug
{
public:
    static Debug * instance();

    int offsetForCurrentThread();
    void log(int priority, const char * format, ...);

private:
    class ThreadInfo
    {
    public:
        ThreadInfo() :
            threadId(0), callOffset(0)
        {}

        volatile int32_t threadId;
        int callOffset;
    };

    class Data : public android::RefBase
    {
    public:
        android::Vector<ThreadInfo*> threads;
    };

private:
    // called from FunctionLogger
    void increaseOffsetForCurrentThread();
    void decreaseOffsetForCurrentThread();

private:
    Debug();

    void grow();
    ThreadInfo * registerThread(Data * data, int32_t threadId);
    ThreadInfo * findCurrentThreadInfo();
    void addOffsetForCurrentThread(int offset);

private:
    static Debug sInstance;

    mutable android::Mutex mMutex;
    android::sp<Data> mData;

    friend class FunctionLogger;
};




template <int Size = 1024>
class EmptyBuffer
{
public:
    EmptyBuffer(const int length)
    {
        memset(mBuffer, ' ', length);
        mBuffer[length] = 0;
    }

    const char * string() const
    {
        return mBuffer;
    }

private:
    char mBuffer[Size];
};




class FunctionLogger
{
public:
    FunctionLogger(const char * file, int line, const char * function);
    ~FunctionLogger();

    void setExitLine(int line);

private:
    const char * const mFile;
    const int mLine;
    const char * const mFunction;
    const void * const mThreadId;
    int mExitLine;
};




#ifdef TI_FUNCTION_LOGGER_ENABLE
#   define LOG_FUNCTION_NAME Ti::FunctionLogger __function_logger_instance(__FILE__, __LINE__, __FUNCTION__);
#   define LOG_FUNCTION_NAME_EXIT __function_logger_instance.setExitLine(__LINE__);
#else
#   define LOG_FUNCTION_NAME
#   define LOG_FUNCTION_NAME_EXIT
#endif




#define DBGUTILS_LOGV_FULL(priority, file, line, function, format, ...)      \
    do                                                                       \
    {                                                                        \
        Ti::Debug * const debug = Ti::Debug::instance();                     \
        debug->log(priority, format, (int)androidGetThreadId(),              \
                Ti::EmptyBuffer<>(debug->offsetForCurrentThread()).string(), \
                file, line, function, __VA_ARGS__);                          \
    } while (0)

// Defines for debug statements - Macro LOG_TAG needs to be defined in the respective files
#define DBGUTILS_LOGV(...) DBGUTILS_LOGV_FULL(ANDROID_LOG_VERBOSE, __FILE__, __LINE__, __FUNCTION__, "(%x) %s  %s:%d %s - " __VA_ARGS__, "")
#define DBGUTILS_LOGD(...) DBGUTILS_LOGV_FULL(ANDROID_LOG_DEBUG,   __FILE__, __LINE__, __FUNCTION__, "(%x) %s  %s:%d %s - " __VA_ARGS__, "")
#define DBGUTILS_LOGE(...) DBGUTILS_LOGV_FULL(ANDROID_LOG_ERROR,   __FILE__, __LINE__, __FUNCTION__, "(%x) %s  %s:%d %s - " __VA_ARGS__, "")
#define DBGUTILS_LOGF(...) DBGUTILS_LOGV_FULL(ANDROID_LOG_FATAL,   __FILE__, __LINE__, __FUNCTION__, "(%x) %s  %s:%d %s - " __VA_ARGS__, "")

#define DBGUTILS_LOGVA DBGUTILS_LOGV
#define DBGUTILS_LOGVB DBGUTILS_LOGV

#define DBGUTILS_LOGDA DBGUTILS_LOGD
#define DBGUTILS_LOGDB DBGUTILS_LOGD

#define DBGUTILS_LOGEA DBGUTILS_LOGE
#define DBGUTILS_LOGEB DBGUTILS_LOGE

// asserts
#define _DBGUTILS_PLAIN_ASSERT(condition)                              \
    do                                                                 \
    {                                                                  \
        if ( !(condition) )                                            \
        {                                                              \
            __android_log_print(ANDROID_LOG_FATAL, "Ti::Debug",        \
                    "Condition failed: " #condition);                  \
            __android_log_print(ANDROID_LOG_FATAL, "Ti::Debug",        \
                    "Aborting process...");                            \
            abort();                                                   \
        }                                                              \
    } while (0)

#define _DBGUTILS_PLAIN_ASSERT_X(condition, ...)                       \
    do                                                                 \
    {                                                                  \
        if ( !(condition) )                                            \
        {                                                              \
            __android_log_print(ANDROID_LOG_FATAL, "Ti::Debug",        \
                    "Condition failed: " #condition ": " __VA_ARGS__); \
            __android_log_print(ANDROID_LOG_FATAL, "Ti::Debug",        \
                    "Aborting process...");                            \
            abort();                                                   \
        }                                                              \
    } while (0)

#define DBGUTILS_ASSERT(condition)                                           \
    do                                                                       \
    {                                                                        \
        if ( !(condition) )                                                  \
        {                                                                    \
            DBGUTILS_LOGF("Condition failed: " #condition);                  \
            DBGUTILS_LOGF("Aborting process...");                            \
            abort();                                                         \
        }                                                                    \
    } while (0)

#define DBGUTILS_ASSERT_X(condition, ...)                                    \
    do                                                                       \
    {                                                                        \
        if ( !(condition) )                                                  \
        {                                                                    \
            DBGUTILS_LOGF("Condition failed: " #condition ": " __VA_ARGS__); \
            DBGUTILS_LOGF("Aborting process...");                            \
            abort();                                                         \
        }                                                                    \
    } while (0)




inline Debug * Debug::instance()
{ return &sInstance; }


inline Debug::ThreadInfo * Debug::findCurrentThreadInfo()
{
    // retain reference to threads data
    android::sp<Data> data = mData;

    // iterate over threads to locate thread id,
    // this is safe from race conditions because each thread
    // is able to modify only his own ThreadInfo structure
    const int32_t threadId = reinterpret_cast<int32_t>(androidGetThreadId());
    const int size = int(data->threads.size());
    for ( int i = 0; i < size; ++i )
    {
        ThreadInfo * const threadInfo = data->threads.itemAt(i);
        if ( threadInfo->threadId == threadId )
            return threadInfo;
    }

    // this thread has not been registered yet,
    // try to fing empty thread info slot
    while ( true )
    {
        ThreadInfo * const threadInfo = registerThread(data.get(), threadId);
        if ( threadInfo )
            return threadInfo;

        // failed registering thread, because all slots are occupied
        // grow the data and try again
        grow();

        data = mData;
    }

    // should never reach here
    _DBGUTILS_PLAIN_ASSERT(false);
    return 0;
}


inline void Debug::addOffsetForCurrentThread(const int offset)
{
    if ( offset == 0 )
        return;

    ThreadInfo * const threadInfo = findCurrentThreadInfo();
    _DBGUTILS_PLAIN_ASSERT(threadInfo);

    threadInfo->callOffset += offset;

    if ( threadInfo->callOffset == 0 )
    {
        // thread call stack has dropped to zero, unregister it
        android_atomic_acquire_store(0, &threadInfo->threadId);
    }
}


inline int Debug::offsetForCurrentThread()
{
#ifdef TI_FUNCTION_LOGGER_ENABLE
    ThreadInfo * const threadInfo = findCurrentThreadInfo();
    _DBGUTILS_PLAIN_ASSERT(threadInfo);

    return threadInfo->callOffset;
#else
    return 0;
#endif
}


inline void Debug::increaseOffsetForCurrentThread()
{
#ifdef TI_FUNCTION_LOGGER_ENABLE
    addOffsetForCurrentThread(1);
#endif
}


inline void Debug::decreaseOffsetForCurrentThread()
{
#ifdef TI_FUNCTION_LOGGER_ENABLE
    addOffsetForCurrentThread(-1);
#endif
}


inline void Debug::log(const int priority, const char * const format, ...)
{
    va_list args;
    va_start(args, format);
    __android_log_vprint(priority, LOG_TAG, format, args);
    va_end(args);
}




inline FunctionLogger::FunctionLogger(const char * const file, const int line, const char * const function) :
    mFile(file), mLine(line), mFunction(function), mThreadId(androidGetThreadId()), mExitLine(-1)
{
    Debug * const debug = Debug::instance();
    debug->increaseOffsetForCurrentThread();
    LOGD("(%x) %s+ %s:%d %s - ENTER",
            (int)mThreadId, EmptyBuffer<>(debug->offsetForCurrentThread()).string(),
            mFile, mLine, mFunction);
}


inline FunctionLogger::~FunctionLogger()
{
    Debug * const debug = Debug::instance();
    LOGD("(%x) %s- %s:%d %s - EXIT",
            (int)mThreadId, EmptyBuffer<>(debug->offsetForCurrentThread()).string(),
            mFile, mExitLine == -1 ? mLine : mExitLine, mFunction);
    debug->decreaseOffsetForCurrentThread();
}


inline void FunctionLogger::setExitLine(const int line)
{
    if ( mExitLine != -1 )
    {
        Debug * const debug = Debug::instance();
        LOGD("(%x) %s  %s:%d %s - Double function exit trace detected. Previous: %d",
                (int)mThreadId, EmptyBuffer<>(debug->offsetForCurrentThread()).string(),
                mFile, line, mFunction, mExitLine);
    }

    mExitLine = line;
}




} // namespace Ti




#endif //DEBUG_UTILS_H

