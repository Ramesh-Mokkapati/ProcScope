// ProcessLoader.h : background work for the UI.
//   CSampleThread  - takes a process snapshot on a timer, so a slow process can't stall the window.
//   CProcessLoader - loads the selected process in stages: loaded modules and threads, then the
//                    signature, then the dependency chain. Each tab fills in as its stage ends.
#pragma once

#include "DependencyResolver.h"
#include "FileSignatureVerifier.h"
#include "LibraryAnalyzer.h"
#include "ProcessSampler.h"

#define WM_SNAPSHOT_READY     (WM_APP + 40)
// wParam: request id; the UI ignores any id that is not its latest request.
#define WM_PROCESS_READY      (WM_APP + 41)
#define WM_SIGNATURE_READY    (WM_APP + 42)
#define WM_DEPENDENCIES_READY (WM_APP + 43)

class CSampleThread
{
public:
    ~CSampleThread() { Stop(); }

    void Start(HWND notify, DWORD intervalMs);
    void Stop();
    void SetInterval(DWORD intervalMs);
    void SetPaused(bool paused);
    void SampleNow();

    std::shared_ptr<ProcessSnapshot> Take();

private:
    void Run();

    HWND                    m_notify = nullptr;
    std::thread             m_thread;
    std::mutex              m_lock;
    std::condition_variable m_cv;
    bool                    m_stop = false;
    bool                    m_paused = false;
    bool                    m_now = false;
    DWORD                   m_intervalMs = 1000;
    std::shared_ptr<ProcessSnapshot> m_latest;
};

struct ProcessDetails
{
    DWORD                     pid = 0;
    ULONGLONG                 createTime = 0;
    std::wstring              path;
    std::vector<LoadedModule> modules;
    std::wstring              modulesError;
    std::vector<ThreadRow>    threads;
    bool                      hasFile = false;
    LibraryDetails            file;
};

class CProcessLoader
{
public:
    ~CProcessLoader() { Stop(); }

    void Start(HWND notify);
    void Stop();

    // A newer request replaces one that hasn't started yet. path may be empty for a protected process.
    void Request(DWORD pid, ULONGLONG createTime, const std::wstring& path, UINT id);

    std::shared_ptr<ProcessDetails>  TakeDetails();
    std::shared_ptr<SignatureInfo>   TakeSignature();
    std::shared_ptr<DependencyGraph> TakeDependencies();

private:
    void Run();

    HWND                    m_notify = nullptr;
    std::thread             m_thread;
    std::mutex              m_lock;
    std::condition_variable m_cv;
    bool                    m_stop = false;
    bool                    m_hasRequest = false;
    DWORD                   m_pid = 0;
    ULONGLONG               m_createTime = 0;
    std::wstring            m_path;
    UINT                    m_id = 0;

    std::shared_ptr<ProcessDetails>  m_details;
    std::shared_ptr<SignatureInfo>   m_signature;
    std::shared_ptr<DependencyGraph> m_dependencies;
    DependencyResolver               m_resolver;   // worker thread only; its cache speeds up later requests
};
