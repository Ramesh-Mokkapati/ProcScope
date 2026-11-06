// DependencyResolver.h : the whole dependency chain of a PE file, worked out statically.
//
// Follows imports, delay-loaded imports and forwarded exports from a root file; finds each DLL on
// disk the way the Windows loader does (API sets, KnownDLLs, then the standard search order); and
// reports DLLs that are missing, functions a DLL doesn't export, and architecture mismatches.
//
// Nothing is loaded or run. That also means anything decided at run time (LoadLibrary calls, changed
// search paths, side-by-side manifests) isn't covered.
#pragma once

#include <windows.h>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Windows' API set table: maps virtual names such as api-ms-win-core-file-l1-1-0.dll to real DLLs.
class ApiSetSchema
{
public:
    bool Load(const std::wstring& schemaDllPath);
    bool IsLoaded() const { return m_loaded; }

    static bool LooksLikeApiSet(const std::wstring& lowerName);

    // True if the name is in the table. host is empty when this system has no DLL for it.
    bool Resolve(const std::wstring& lowerName, const std::wstring& lowerImporter, std::wstring& host) const;

private:
    struct Entry
    {
        std::wstring                                   defaultHost;
        std::unordered_map<std::wstring, std::wstring> byImporter;
    };
    std::unordered_map<std::wstring, Entry> m_entries;
    bool m_loaded = false;
};

enum class DepKind { Static, Delay, Forward };
enum class DepStatus { Ok, NotFound, WrongArchitecture, Invalid, ApiSetNoHost };
enum class DepSeverity { Error, Warning, Info };

struct DepFunction
{
    std::string name;
    WORD        ordinal = 0;
    bool        byOrdinal = false;
    bool        found = true;            // false when the target doesn't export it
    bool        checked = true;          // false when there's no readable target to check against
    bool        required = false;        // something the root can't load without imports this
    std::string forwardedTo;
    size_t      forwardEdge = size_t(-1);       // where forwardedTo was recorded, on the target module
    size_t      forwardFunction = size_t(-1);
};

struct DepEdge
{
    size_t      target = 0;
    DepKind     kind = DepKind::Static;
    std::string importedAs;              // name as written in the importing file
    bool        viaApiSet = false;
    bool        required = false;        // the root can't load unless this edge resolves
    size_t      missingFunctions = 0;
    std::vector<DepFunction> functions;
    std::unordered_map<std::string, size_t> functionIndex;
};

struct DepModule
{
    std::wstring name;
    std::wstring path;                   // empty if not found
    std::wstring foundVia;
    std::wstring detail;                 // why it can't be used
    DepStatus    status = DepStatus::Ok;
    WORD         machine = 0;
    std::wstring version;
    std::wstring company;
    bool         required = false;       // reached from the root without passing a delay-load
    std::deque<DepEdge> edges;
};

struct DepProblem
{
    DepSeverity  severity = DepSeverity::Error;
    size_t       importer = 0;
    size_t       edge = 0;
    std::wstring text;
};

struct DependencyGraph
{
    std::deque<DepModule>   modules;     // [0] is the root
    std::vector<DepProblem> problems;    // most severe first
    size_t errors = 0;
    size_t warnings = 0;
    bool   cancelled = false;
    std::wstring failure;                // set when the root itself couldn't be read

    std::wstring SummaryText() const;
};

// Keeps parsed facts per file between calls, so moving from one library to the next stays fast.
// One instance must only be used by one thread at a time.
class DependencyResolver
{
public:
    DependencyResolver();
    ~DependencyResolver();

    // cancelled is polled between modules; the result is then marked as cancelled.
    std::shared_ptr<DependencyGraph> Resolve(const std::wstring& rootPath, const std::function<bool()>& cancelled = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
