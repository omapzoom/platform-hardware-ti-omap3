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

#include "DebugUtils.h"




namespace Ti {




static const int kDebugThreadInfoGrowSize = 16;

Debug Debug::sInstance;




Debug::Debug()
{
    grow();
}


void Debug::grow()
{
    android::Mutex::Autolock locker(mMutex);
    (void)locker;

    const int size = kDebugThreadInfoGrowSize;

    const int newSize = (mData.get() ? mData->threads.size() : 0) + size;

    Data * const newData = new Data;
    newData->threads.setCapacity(newSize);

    // insert previous thread info pointers
    if ( mData.get() )
        newData->threads.insertVectorAt(mData->threads, 0);

    // populate data with new thread infos
    for ( int i = 0; i < size; ++i )
        newData->threads.add(new ThreadInfo);

    // replace old data with new one
    mData = newData;
}


Debug::ThreadInfo * Debug::registerThread(Data * const data, const int32_t threadId)
{
    const int size = data->threads.size();
    for ( int i = 0; i < size; ++i )
    {
        ThreadInfo * const threadInfo = data->threads.itemAt(i);
        if ( android_atomic_acquire_cas(0, threadId, &threadInfo->threadId) == 0 )
            return threadInfo;
    }

    // failed to find empty slot for thread
    return 0;
}




} // namespace Ti
