/*
* Copyright: JessMA Open Source (ldcsaa@gmail.com)
*
* Author	: Bruce Liang
* Website	: https://github.com/ldcsaa
* Project	: https://github.com/ldcsaa/HP-Socket
* Blog		: http://www.cnblogs.com/ldcsaa
* Wiki		: http://www.oschina.net/p/hp-socket
* QQ Group	: 44636872, 75375912
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/
 
#include "HPThreadPool.h"

#include "common/FuncHelper.h"

#include <pthread.h>

LPTSocketTask CreateSocketTaskObj(	Fn_SocketTaskProc fnTaskProc,
									PVOID pSender, CONNID dwConnID,
									LPCBYTE pBuffer, INT iBuffLen, EnTaskBufferType enBuffType,
									WPARAM wParam, LPARAM lParam)
{
	ASSERT(fnTaskProc != nullptr);
	ASSERT(iBuffLen >= 0);

	LPTSocketTask pTask = new TSocketTask;

	pTask->fn		= fnTaskProc;
	pTask->sender	= pSender;
	pTask->connID	= dwConnID;
	pTask->bufLen	= iBuffLen;
	pTask->bufType	= enBuffType;
	pTask->wparam	= wParam;
	pTask->lparam	= lParam;

	if(enBuffType != TBT_COPY || !pBuffer)
		pTask->buf = pBuffer;
	else
	{
		pTask->buf = MALLOC(BYTE, iBuffLen);
		::CopyMemory((LPBYTE)pTask->buf, pBuffer, iBuffLen);
	}

	return pTask;
}

void DestroySocketTaskObj(LPTSocketTask pTask)
{
	if(pTask)
	{
		if(pTask->bufType != TBT_REFER && pTask->buf)
			FREE(pTask->buf);

		delete pTask;
	}
}

BOOL CHPThreadPool::Start(DWORD dwThreadCount, DWORD dwMaxQueueSize, EnRejectedPolicy enRejectedPolicy, DWORD dwStackSize)
{
	if(!CheckStarting())
		return FALSE;

	m_dwStackSize		= dwStackSize;
	m_dwMaxQueueSize	= dwMaxQueueSize;
	m_enRejectedPolicy	= enRejectedPolicy;

	if(!InternalAdjustThreadCount(dwThreadCount))
	{
		EXECUTE_RESTORE_ERROR(Stop());
		return FALSE;
	}

	m_enState = SS_STARTED;

	return TRUE;;
}

BOOL CHPThreadPool::Stop(DWORD dwMaxWait)
{
	if(!CheckStoping())
		return FALSE;

	::WaitFor(15);

	Shutdown(dwMaxWait);

	Reset();

	return TRUE;
}

BOOL CHPThreadPool::Shutdown(DWORD dwMaxWait)
{
	BOOL isOK		= TRUE;
	BOOL bLimited	= (m_dwMaxQueueSize != 0);
	BOOL bInfinite	= (dwMaxWait == (DWORD)INFINITE || dwMaxWait == 0);

	if(m_enRejectedPolicy == TRP_WAIT_FOR && bLimited)
	{
		CMutexLock2 lock(m_mtx);
		m_cvQueue.notify_all();
	}

	VERIFY(DoAdjustThreadCount(0));

	if(bInfinite)
		m_sem.Wait(CShutdownPredicate(this));
	else
		m_sem.WaitFor(dwMaxWait, CShutdownPredicate(this));

	ASSERT(m_lsTasks.size()	  == 0);
	ASSERT(m_stThreads.size() == 0);

	if(!m_lsTasks.empty())
	{
		CMutexLock2 lock(m_mtx);

		if(!m_lsTasks.empty())
		{
			do
			{
				TTask* pTask = m_lsTasks.front();
				m_lsTasks.pop();

				TTask::Destruct(pTask);

			} while(!m_lsTasks.empty());

			::SetLastError(ERROR_CANCELLED);
			isOK = FALSE;
		}
	}

	if(!m_stThreads.empty())
	{
		CCriSecLock lock(m_cs);

		if(!m_stThreads.empty())
		{
#if !defined(__ANDROID__)
			for(auto it = m_stThreads.begin(), end = m_stThreads.end(); it != end; ++it)
				pthread_cancel(*it);
#endif
			m_stThreads.clear();

			::SetLastError(ERROR_CANCELLED);
			isOK = FALSE;
		}
	}

	return isOK;
}

BOOL CHPThreadPool::Submit(Fn_TaskProc fnTaskProc, PVOID pvArg, DWORD dwMaxWait)
{
	return DoSubmit(fnTaskProc, pvArg, FALSE, dwMaxWait);
}

BOOL CHPThreadPool::Submit(LPTSocketTask pTask, DWORD dwMaxWait)
{
	return DoSubmit((Fn_TaskProc)pTask->fn, (PVOID)pTask, TRUE, dwMaxWait);
}

BOOL CHPThreadPool::DoSubmit(Fn_TaskProc fnTaskProc, PVOID pvArg, BOOL bFreeArg, DWORD dwMaxWait)
{
	EnSubmitResult sr = DirectSubmit(fnTaskProc, pvArg, bFreeArg);

	if(sr != SUBMIT_FULL)
		return (sr == SUBMIT_OK);

	if(m_enRejectedPolicy == TRP_CALL_FAIL)
	{
		::SetLastError(ERROR_DESTINATION_ELEMENT_FULL);
		return FALSE;
	}
	else if(m_enRejectedPolicy == TRP_WAIT_FOR)
	{
		return CycleWaitSubmit(fnTaskProc, pvArg, dwMaxWait, bFreeArg);
	}
	else if(m_enRejectedPolicy == TRP_CALLER_RUN)
	{
		DoRunTaskProc(fnTaskProc, pvArg);
	}
	else
	{
		ASSERT(FALSE);

		::SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	return TRUE;
}

void CHPThreadPool::DoRunTaskProc(Fn_TaskProc fnTaskProc, PVOID pvArg)
{
	::InterlockedIncrement(&m_dwTaskCount);
	fnTaskProc(pvArg);
	::InterlockedDecrement(&m_dwTaskCount);
}

CHPThreadPool::EnSubmitResult CHPThreadPool::DirectSubmit(Fn_TaskProc fnTaskProc, PVOID pvArg, BOOL bFreeArg)
{
	CMutexLock2 lock(m_mtx);

	return DoDirectSubmit(fnTaskProc, pvArg, bFreeArg);
}

CHPThreadPool::EnSubmitResult CHPThreadPool::DoDirectSubmit(Fn_TaskProc fnTaskProc, PVOID pvArg, BOOL bFreeArg)
{
	BOOL bLimited = (m_dwMaxQueueSize != 0);

	if(!CheckStarted())
		return SUBMIT_ERROR;

	if(bLimited && (DWORD)m_lsTasks.size() >= m_dwMaxQueueSize)
		return SUBMIT_FULL;
	else
	{
		TTask* pTask = TTask::Construct(fnTaskProc, pvArg, bFreeArg);

		m_lsTasks.push(pTask);
		m_cvTask.notify_one();
	}

	return SUBMIT_OK;
}

BOOL CHPThreadPool::CycleWaitSubmit(Fn_TaskProc fnTaskProc, PVOID pvArg, DWORD dwMaxWait, BOOL bFreeArg)
{
	ASSERT(m_dwMaxQueueSize != 0);

	DWORD dwTime	= ::TimeGetTime();
	BOOL bInfinite	= (dwMaxWait == (DWORD)INFINITE || dwMaxWait == 0);

	while(CheckStarted()) 
	{
		CMutexLock2 lock(m_mtx);

		EnSubmitResult sr = DoDirectSubmit(fnTaskProc, pvArg, bFreeArg);

		if(sr == SUBMIT_OK)
			return TRUE;
		else if(sr == SUBMIT_ERROR)
			return FALSE;
		{
			if(bInfinite)
				m_cvQueue.wait(lock);
			else
			{
				DWORD dwNow = ::GetTimeGap32(dwTime);

				if(dwNow > dwMaxWait || m_cvQueue.wait_for(lock, chrono::milliseconds(dwMaxWait - dwNow)) == cv_status::timeout)
				{
					::SetLastError(ERROR_TIMEOUT);
					break;
				}
			}
		}
	}

	return FALSE;
}

BOOL CHPThreadPool::AdjustThreadCount(DWORD dwNewThreadCount)
{
	if(!CheckStarted())
		return FALSE;

	return InternalAdjustThreadCount(dwNewThreadCount);
}

BOOL CHPThreadPool::InternalAdjustThreadCount(DWORD dwNewThreadCount)
{
	int iNewThreadCount = (int)dwNewThreadCount;

	if(iNewThreadCount == 0)
		iNewThreadCount = ::GetDefaultWorkerThreadCount();
	else if(iNewThreadCount < 0)
		iNewThreadCount = PROCESSOR_COUNT * (-iNewThreadCount);

	return DoAdjustThreadCount((DWORD)iNewThreadCount);
}

BOOL CHPThreadPool::DoAdjustThreadCount(DWORD dwNewThreadCount)
{
	ASSERT((int)dwNewThreadCount >= 0);

	BOOL bRemove		= FALSE;
	DWORD dwThreadCount	= 0;

	{
		CCriSecLock lock(m_cs);

		if(dwNewThreadCount > m_dwThreadCount)
		{
			dwThreadCount = dwNewThreadCount - m_dwThreadCount;
			return CreateWorkerThreads(dwThreadCount);
		}
		else if(dwNewThreadCount < m_dwThreadCount)
		{
			bRemove			 = TRUE;
			dwThreadCount	 = m_dwThreadCount - dwNewThreadCount;
			m_dwThreadCount -= dwThreadCount;
		}
	}

	if(bRemove)
	{
		CMutexLock2 lock(m_mtx);

		for(DWORD i = 0; i < dwThreadCount; i++)
			m_cvTask.notify_one();
	}

	return TRUE;
}

BOOL CHPThreadPool::CreateWorkerThreads(DWORD dwThreadCount)
{
	unique_ptr<pthread_attr_t> pThreadAttr;

	if(m_dwStackSize != 0)
	{
		pThreadAttr = make_unique<pthread_attr_t>();
		VERIFY_IS_NO_ERROR(pthread_attr_init(pThreadAttr.get()));

		int rs = pthread_attr_setstacksize(pThreadAttr.get(), m_dwStackSize);

		if(!IS_NO_ERROR(rs))
		{
			pthread_attr_destroy(pThreadAttr.get());
			::SetLastError(rs);

			return FALSE;
		}
	}

	BOOL isOK = TRUE;

	for(DWORD i = 0; i < dwThreadCount; i++)
	{
		THR_ID dwThreadID;
		int rs = pthread_create(&dwThreadID, pThreadAttr.get(), ThreadProc, (PVOID)this);

		if(!IS_NO_ERROR(rs))
		{
			::SetLastError(rs);
			isOK = FALSE;

			break;
		}

		m_stThreads.emplace(dwThreadID);
		++m_dwThreadCount;
	}

	if(pThreadAttr != nullptr)
		pthread_attr_destroy(pThreadAttr.get());

	return isOK;
}

PVOID CHPThreadPool::ThreadProc(LPVOID pv)
{
	return (PVOID)(UINT_PTR)(((CHPThreadPool*)pv)->WorkerProc());
}

int CHPThreadPool::WorkerProc()
{
	BOOL bLimited = (m_dwMaxQueueSize != 0);

	while(TRUE)
	{
		BOOL bExit	 = FALSE;
		TTask* pTask = nullptr;

		{
			CMutexLock2 lock(m_mtx);

			do
			{
				if(!m_lsTasks.empty())
				{
					pTask = m_lsTasks.front();
					m_lsTasks.pop();

					if(m_enRejectedPolicy == TRP_WAIT_FOR && bLimited && pTask != nullptr)
						m_cvQueue.notify_one();
				}
				else if(m_dwThreadCount < m_stThreads.size())
					bExit = TRUE;
				else
					m_cvTask.wait(lock);

			} while(pTask == nullptr && !bExit);
		}

		if(pTask != nullptr)
		{
			DoRunTaskProc(pTask->fn, pTask->arg);

			if(pTask->freeArg)
				::DestroySocketTaskObj((LPTSocketTask)pTask->arg);

			TTask::Destruct(pTask);
		}
		else if(bExit)
		{
			if(CheckWorkerThreadExit())
				break;
		}
	}

	return 0;
}

BOOL CHPThreadPool::CheckWorkerThreadExit()
{
	BOOL bExit		= FALSE;
	BOOL bShutdown	= FALSE;

	if(m_dwThreadCount < m_stThreads.size())
	{
		CCriSecLock lock(m_cs);

		if(m_dwThreadCount < m_stThreads.size())
		{
			VERIFY(m_stThreads.erase(SELF_THREAD_ID) == 1);

			bExit		= TRUE;
			bShutdown	= m_stThreads.empty();
		}
	}

	if(bExit)
	{
		pthread_detach(SELF_THREAD_ID);

		if(bShutdown)
			m_sem.NotifyOne();
	}

	return bExit;
}

BOOL CHPThreadPool::CheckStarting()
{
	if(::InterlockedCompareExchange(&m_enState, SS_STARTING, SS_STOPPED) != SS_STOPPED)
	{
		::SetLastError(ERROR_INVALID_STATE);
		return FALSE;
	}

	return TRUE;
}

BOOL CHPThreadPool::CheckStarted()
{
	if(m_enState != SS_STARTED)
	{
		::SetLastError(ERROR_INVALID_STATE);
		return FALSE;
	}

	return TRUE;
}

BOOL CHPThreadPool::CheckStoping()
{
	if( ::InterlockedCompareExchange(&m_enState, SS_STOPPING, SS_STARTED) != SS_STARTED &&
		::InterlockedCompareExchange(&m_enState, SS_STOPPING, SS_STARTING) != SS_STARTING)
	{
		while(m_enState != SS_STOPPED)
			::WaitFor(5);

		::SetLastError(ERROR_INVALID_STATE);
		return FALSE;
	}

	return TRUE;
}

void CHPThreadPool::Reset(BOOL bSetWaitEvent)
{
	m_dwStackSize		= 0;
	m_dwTaskCount		= 0;
	m_dwThreadCount		= 0;
	m_dwMaxQueueSize	= 0;
	m_enRejectedPolicy	= TRP_CALL_FAIL;
	m_enState			= SS_STOPPED;

	if(bSetWaitEvent)
		m_evWait.SyncNotifyAll();
}
