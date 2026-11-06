// ProcessLoader.cpp
#include "pch.h"
#include "ProcessLoader.h"

// ---------------------------------------------------------------------------
// CSampleThread

void CSampleThread::Start(HWND notify, DWORD intervalMs)
{
    m_notify = notify;
    m_intervalMs = intervalMs;
    m_stop = false;
    m_thread = std::thread(&CSampleThread::Run, this);
}

void CSampleThread::Stop()
{
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

void CSampleThread::SetInterval(DWORD intervalMs)
{
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_intervalMs = intervalMs;
    }
    m_cv.notify_all();
}

void CSampleThread::SetPaused(bool paused)
{
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_paused = paused;
    }
    m_cv.notify_all();
}

void CSampleThread::SampleNow()
{
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_now = true;
    }
    m_cv.notify_all();
}

std::shared_ptr<ProcessSnapshot> CSampleThread::Take()
{
    std::lock_guard<std::mutex> lock(m_lock);
    return std::move(m_latest);
}

void CSampleThread::Run()
{
    SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, nullptr);

    ProcessSampler sampler;
    for (;;)
    {
        auto snapshot = std::make_shared<ProcessSnapshot>();
        sampler.Sample(*snapshot);

        std::unique_lock<std::mutex> lock(m_lock);
        if (m_stop)
            return;
        m_latest = std::move(snapshot);   // an unread one is simply replaced
        ::PostMessage(m_notify, WM_SNAPSHOT_READY, 0, 0);

        const auto wake = [this] { return m_stop || m_now; };
        if (m_paused)
            m_cv.wait(lock, [this] { return m_stop || m_now || !m_paused; });
        else
            m_cv.wait_for(lock, std::chrono::milliseconds(m_intervalMs), wake);
        if (m_stop)
            return;
        m_now = false;
    }
}

// ---------------------------------------------------------------------------
// CProcessLoader

void CProcessLoader::Start(HWND notify)
{
    m_notify = notify;
    m_stop = false;
    m_thread = std::thread(&CProcessLoader::Run, this);
}

void CProcessLoader::Stop()
{
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

void CProcessLoader::Request(DWORD pid, ULONGLONG createTime, const std::wstring& path, UINT id)
{
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_pid = pid;
        m_createTime = createTime;
        m_path = path;
        m_id = id;
        m_hasRequest = true;
    }
    m_cv.notify_all();
}

std::shared_ptr<ProcessDetails> CProcessLoader::TakeDetails()
{
    std::lock_guard<std::mutex> lock(m_lock);
    return std::move(m_details);
}

std::shared_ptr<SignatureInfo> CProcessLoader::TakeSignature()
{
    std::lock_guard<std::mutex> lock(m_lock);
    return std::move(m_signature);
}

std::shared_ptr<DependencyGraph> CProcessLoader::TakeDependencies()
{
    std::lock_guard<std::mutex> lock(m_lock);
    return std::move(m_dependencies);
}

void CProcessLoader::Run()
{
    SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, nullptr);

    for (;;)
    {
        DWORD pid = 0;
        ULONGLONG createTime = 0;
        std::wstring path;
        UINT id = 0;
        {
            std::unique_lock<std::mutex> lock(m_lock);
            m_cv.wait(lock, [this] { return m_stop || m_hasRequest; });
            if (m_stop)
                return;
            pid = m_pid;
            createTime = m_createTime;
            path = m_path;
            id = m_id;
            m_hasRequest = false;
        }

        auto details = std::make_shared<ProcessDetails>();
        details->pid = pid;
        details->createTime = createTime;
        details->path = path;
        EnumerateModules(pid, details->modules, details->modulesError);
        EnumerateThreads(pid, details->modules, details->threads);
        if (!path.empty())
        {
            LoadDetails(path, details->file);
            details->hasFile = true;
        }
        const bool isPe = details->hasFile && details->file.isPe;
        {
            std::lock_guard<std::mutex> lock(m_lock);
            if (m_stop)
                return;
            if (id == m_id)
            {
                m_details = std::move(details);
                ::PostMessage(m_notify, WM_PROCESS_READY, id, 0);
            }
            // Arrow-keying through the list: skip the slow stages for rows already left behind.
            if (m_hasRequest || path.empty())
                continue;
        }

        auto signature = std::make_shared<SignatureInfo>();
        FileSignatureVerifier::Verify(path, *signature);
        {
            std::lock_guard<std::mutex> lock(m_lock);
            if (m_stop)
                return;
            if (id == m_id)
            {
                m_signature = std::move(signature);
                ::PostMessage(m_notify, WM_SIGNATURE_READY, id, 0);
            }
            if (m_hasRequest || !isPe)
                continue;
        }

        // Stops early as soon as the selection moves on, or the window closes.
        auto graph = m_resolver.Resolve(path, [this] {
            std::lock_guard<std::mutex> lock(m_lock);
            return m_stop || m_hasRequest;
        });

        std::lock_guard<std::mutex> lock(m_lock);
        if (m_stop)
            return;
        if (id == m_id && !graph->cancelled)
        {
            m_dependencies = std::move(graph);
            ::PostMessage(m_notify, WM_DEPENDENCIES_READY, id, 0);
        }
    }
}
