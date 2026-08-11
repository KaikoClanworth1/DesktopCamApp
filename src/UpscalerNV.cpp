#include "UpscalerNV.h"

#include <ShlObj.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")

// --- Minimum NVIDIA VFX / NvCVImage / CUDA types we care about --------------
//
// These mirror the public headers (MAXINE-VFX-SDK on GitHub). We avoid
// including NVIDIA headers directly so the project still builds on systems
// without the SDK installed.

extern "C" {

using NvCV_Status = int;
static constexpr NvCV_Status NVCV_SUCCESS        =  0;
static constexpr NvCV_Status NVCV_ERR_UNIMPLEMENTED = -11;
static constexpr NvCV_Status NVCV_ERR_FEATURENOTFOUND = -12;
static constexpr NvCV_Status NVCV_ERR_MODEL      = -27;

typedef struct CUstream_st* CUstream;

// Pixel format + component type enum values MUST match the NVIDIA SDK's
// nvCVImage.h exactly — NvCVImage_Alloc reads them as raw ints.
enum : int {
    NVCV_FORMAT_UNKNOWN = 0,
    NVCV_Y              = 1,
    NVCV_A              = 2,
    NVCV_YA             = 3,
    NVCV_RGB            = 4,
    NVCV_BGR            = 5,
    NVCV_RGBA           = 6,
    NVCV_BGRA           = 7,
};
enum : int {
    NVCV_TYPE_UNKNOWN = 0,
    NVCV_U8           = 1,
    NVCV_U16          = 2,
    NVCV_S16          = 3,
    NVCV_F16          = 4,
    NVCV_U32          = 5,
    NVCV_S32          = 6,
    NVCV_F32          = 7,   // <-- previously wrong (was 5), SR rejected the buffer
    NVCV_U64          = 8,
    NVCV_S64          = 9,
    NVCV_F64          = 10,
};
enum : uint8_t {
    NVCV_CPU          = 0,
    NVCV_GPU          = 1,
    NVCV_CUDA         = 1, // alias
    NVCV_CPU_PINNED   = 2,
};
enum : uint8_t {
    NVCV_INTERLEAVED  = 0,
    NVCV_PLANAR       = 1,
};

struct NvCVImage {
    uint32_t width;
    uint32_t height;
    int32_t  pitch;
    uint8_t  pixelFormat;
    uint8_t  componentType;
    uint8_t  pixelBytes;
    uint8_t  componentBytes;
    uint8_t  numComponents;
    uint8_t  planar;
    uint8_t  gpuMem; // NVCV_CPU / NVCV_CUDA ...
    uint8_t  reserved[3];
    void*    pixels;
    void*    deletePtr;
    void   (*deleteProc)(void*);
    uint64_t bufferBytes;
    // There are a couple more reserved words in newer SDKs but they
    // always follow the used fields — safe to leave this header slightly
    // shorter than the SDK struct; the SDK writes its own fields itself.
    uint64_t _pad[4];
};

typedef void* NvVFX_Handle;
typedef const char* NvVFX_EffectSelector;
typedef const char* NvVFX_ParameterSelector;

// -- Function pointer types ------------------------------------------------
typedef NvCV_Status (*PFN_NvVFX_GetVersion)(unsigned int* version);
typedef NvCV_Status (*PFN_NvVFX_CreateEffect)(NvVFX_EffectSelector code, NvVFX_Handle* effect);
typedef NvCV_Status (*PFN_NvVFX_DestroyEffect)(NvVFX_Handle effect);
typedef NvCV_Status (*PFN_NvVFX_SetU32)(NvVFX_Handle effect, NvVFX_ParameterSelector param, unsigned int val);
typedef NvCV_Status (*PFN_NvVFX_SetF32)(NvVFX_Handle effect, NvVFX_ParameterSelector param, float val);
typedef NvCV_Status (*PFN_NvVFX_SetString)(NvVFX_Handle effect, NvVFX_ParameterSelector param, const char* val);
typedef NvCV_Status (*PFN_NvVFX_SetImage)(NvVFX_Handle effect, NvVFX_ParameterSelector param, NvCVImage* im);
typedef NvCV_Status (*PFN_NvVFX_SetCudaStream)(NvVFX_Handle effect, NvVFX_ParameterSelector param, CUstream stream);
typedef NvCV_Status (*PFN_NvVFX_Load)(NvVFX_Handle effect);
typedef NvCV_Status (*PFN_NvVFX_Run)(NvVFX_Handle effect, int async);

typedef NvCV_Status (*PFN_NvCVImage_Alloc)(NvCVImage* im, uint32_t w, uint32_t h, int pixelFormat, int componentType, int planar, int memSpace, uint32_t alignment);
typedef void        (*PFN_NvCVImage_Dealloc)(NvCVImage* im);
typedef NvCV_Status (*PFN_NvCVImage_InitFromD3D11Texture)(NvCVImage* im, ID3D11Texture2D* tex);
typedef NvCV_Status (*PFN_NvCVImage_Transfer)(const NvCVImage* src, NvCVImage* dst, float scale, CUstream stream, NvCVImage* tempBuf);
typedef NvCV_Status (*PFN_NvCVImage_MapResource)(NvCVImage* im, CUstream stream);
typedef NvCV_Status (*PFN_NvCVImage_UnmapResource)(NvCVImage* im, CUstream stream);

typedef int (*PFN_cudaStreamCreate)(CUstream* stream);
typedef int (*PFN_cudaStreamDestroy)(CUstream stream);
typedef int (*PFN_cudaStreamSynchronize)(CUstream stream);

} // extern "C"

// --- Effect / parameter constants (straight out of nvVideoEffects.h) -------
static constexpr const char* NVVFX_FX_SUPER_RES     = "SuperRes";
static constexpr const char* NVVFX_INPUT_IMAGE_0    = "SrcImage0";
static constexpr const char* NVVFX_OUTPUT_IMAGE_0   = "DstImage0";
static constexpr const char* NVVFX_MODEL_DIRECTORY  = "ModelDir";
static constexpr const char* NVVFX_CUDA_STREAM      = "CudaStream";
static constexpr const char* NVVFX_MODE             = "Mode";
static constexpr const char* NVVFX_SCALE            = "Scale";

// --- Static function pointers (filled by ResolveFunctions) -----------------
namespace nv {
    PFN_NvVFX_GetVersion                GetVersion                = nullptr;
    PFN_NvVFX_CreateEffect              CreateEffect              = nullptr;
    PFN_NvVFX_DestroyEffect             DestroyEffect             = nullptr;
    PFN_NvVFX_SetU32                    SetU32                    = nullptr;
    PFN_NvVFX_SetF32                    SetF32                    = nullptr;
    PFN_NvVFX_SetString                 SetString                 = nullptr;
    PFN_NvVFX_SetImage                  SetImage                  = nullptr;
    PFN_NvVFX_SetCudaStream             SetCudaStream             = nullptr;
    PFN_NvVFX_Load                      Load                      = nullptr;
    PFN_NvVFX_Run                       Run                       = nullptr;
    PFN_NvCVImage_Alloc                 Image_Alloc               = nullptr;
    PFN_NvCVImage_Dealloc               Image_Dealloc             = nullptr;
    PFN_NvCVImage_InitFromD3D11Texture  Image_InitFromD3D11       = nullptr;
    PFN_NvCVImage_Transfer              Image_Transfer            = nullptr;
    PFN_NvCVImage_MapResource           Image_MapResource         = nullptr;
    PFN_NvCVImage_UnmapResource         Image_UnmapResource       = nullptr;
    PFN_cudaStreamCreate                cudaStreamCreate          = nullptr;
    PFN_cudaStreamDestroy               cudaStreamDestroy         = nullptr;
    PFN_cudaStreamSynchronize           cudaStreamSynchronize     = nullptr;
}

// --- Helpers ---------------------------------------------------------------
static std::wstring DefaultModelDir()
{
    // Broadcast SDK installer default — `C:\Program Files\NVIDIA Corporation\
    // NVIDIA Video Effects\models\`.
    wchar_t* pf = nullptr;
    SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &pf);
    std::wstring p = pf ? pf : L"C:\\Program Files";
    if (pf) CoTaskMemFree(pf);
    p += L"\\NVIDIA Corporation\\NVIDIA Video Effects\\models";
    return p;
}

static std::string ToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// --- Ctor/dtor -------------------------------------------------------------
UpscalerNV::UpscalerNV() { SetStatus(Status::NotInitialized, L"not initialized"); }
UpscalerNV::~UpscalerNV() { Shutdown(); }

std::wstring UpscalerNV::StatusMessage() const
{
    std::lock_guard<std::mutex> lk(statusMutex_);
    return statusMsg_;
}

void UpscalerNV::SetStatus(Status s, const std::wstring& msg)
{
    std::lock_guard<std::mutex> lk(statusMutex_);
    status_ = s;
    statusMsg_ = msg;
}

// --- DLL loading -----------------------------------------------------------
static HMODULE LoadNvDll(const std::wstring& path)
{
    // LOAD_WITH_ALTERED_SEARCH_PATH lets Windows look for the DLL's
    // dependencies (cuDNN, ONNXRuntime, TensorRT, etc.) in the same
    // directory as NVVideoEffects.dll. Without this flag, the SDK fails
    // to load on most installations because its deps aren't on PATH.
    return LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

static std::wstring ProgramFilesW()
{
    wchar_t* pf = nullptr;
    SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &pf);
    std::wstring p = pf ? pf : L"C:\\Program Files";
    if (pf) CoTaskMemFree(pf);
    return p;
}

static std::wstring ToLowerW(std::wstring s)
{
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// Breadth-first scan under `root` (up to `maxDepth` dirs deep). Returns any
// file whose name matches any of the provided suffix-insensitive patterns.
static std::vector<std::wstring> FindDllsByPattern(
    const std::wstring& root,
    const std::vector<std::wstring>& patternsLower,
    int maxDepth)
{
    std::vector<std::wstring> out;
    if (maxDepth < 0) return out;

    std::vector<std::pair<std::wstring, int>> queue;
    queue.emplace_back(root, 0);

    while (!queue.empty()) {
        auto [dir, depth] = queue.back();
        queue.pop_back();

        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring full = dir + L"\\" + name;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (depth + 1 <= maxDepth) queue.emplace_back(full, depth + 1);
            } else {
                const std::wstring nameLower = ToLowerW(name);
                for (const auto& pat : patternsLower) {
                    if (nameLower.find(pat) != std::wstring::npos) {
                        out.push_back(full);
                        break;
                    }
                }
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return out;
}

static std::wstring ReadRegString(HKEY root, const wchar_t* subkey, const wchar_t* name)
{
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ | KEY_WOW64_64KEY, &h) != ERROR_SUCCESS)
        return {};
    wchar_t buf[MAX_PATH]{};
    DWORD cb = sizeof(buf);
    DWORD type = 0;
    std::wstring out;
    if (RegQueryValueExW(h, name, nullptr, &type, (LPBYTE)buf, &cb) == ERROR_SUCCESS && type == REG_SZ)
        out = buf;
    RegCloseKey(h);
    return out;
}

bool UpscalerNV::LoadDlls()
{
    if (!vfxDll_) {
        std::vector<std::wstring> candidates;

        // 1. Unqualified name — hits PATH first.
        candidates.emplace_back(L"NVVideoEffects.dll");

        // 2. Registry — SDK installer writes its path here.
        for (auto sub : {
                L"SOFTWARE\\NVIDIA Corporation\\NVIDIA Video Effects",
                L"SOFTWARE\\NVIDIA Corporation\\NVIDIA Broadcast SDK",
                L"SOFTWARE\\NVIDIA Corporation\\Video Effects" })
        {
            std::wstring root = ReadRegString(HKEY_LOCAL_MACHINE, sub, L"InstallDir");
            if (root.empty())
                root = ReadRegString(HKEY_LOCAL_MACHINE, sub, L"Path");
            if (!root.empty()) {
                if (root.back() != L'\\' && root.back() != L'/') root += L'\\';
                candidates.push_back(root + L"NVVideoEffects.dll");
            }
        }

        // 3. Known install locations for the Broadcast / VFX / AR SDKs.
        const std::wstring pf = ProgramFilesW();
        const wchar_t* dirs[] = {
            L"\\NVIDIA Corporation\\NVIDIA Video Effects\\",
            L"\\NVIDIA Corporation\\NVIDIA Video Effects SDK\\",
            L"\\NVIDIA Corporation\\NVIDIA Broadcast SDK\\Video Effects\\",
            L"\\NVIDIA Corporation\\NVIDIA Broadcast\\Video Effects\\",
            L"\\NVIDIA Corporation\\NVIDIA Maxine\\VFX\\",
        };
        for (auto d : dirs)
            candidates.push_back(pf + d + L"NVVideoEffects.dll");

        DWORD lastErr = 0;
        std::wstring lastPath;
        for (const auto& path : candidates) {
            vfxDll_ = LoadNvDll(path);
            if (vfxDll_) {
                wprintf(L"[nvsr] loaded %s\n", path.c_str());
                break;
            }
            lastErr  = GetLastError();
            lastPath = path;
            wprintf(L"[nvsr] tried  %s : err=%lu\n", path.c_str(), lastErr);
        }

        // Fallback: scan the whole NVIDIA Corporation tree for any
        // *VideoEffects*.dll (catches installers that drop it in bin\,
        // x64\, Win64\, Redist\, etc.).
        if (!vfxDll_) {
            const std::wstring nvRoot = pf + L"\\NVIDIA Corporation";
            wprintf(L"[nvsr] scanning %s for *VideoEffects*.dll ...\n", nvRoot.c_str());
            const auto found = FindDllsByPattern(nvRoot, { L"videoeffects" }, 4);
            for (const auto& p : found) {
                wprintf(L"[nvsr] scan hit  %s\n", p.c_str());
                vfxDll_ = LoadNvDll(p);
                if (vfxDll_) {
                    wprintf(L"[nvsr] loaded   %s\n", p.c_str());
                    break;
                }
                lastErr  = GetLastError();
                lastPath = p;
                wprintf(L"[nvsr] load fail %s : err=%lu\n", p.c_str(), lastErr);
            }

            // Still nothing? Dump the directory listing so the user can
            // see what's actually in the install.
            if (!vfxDll_) {
                wprintf(L"[nvsr] directory listing of %s:\n", nvRoot.c_str());
                WIN32_FIND_DATAW fd{};
                HANDLE h = FindFirstFileW((nvRoot + L"\\*").c_str(), &fd);
                if (h != INVALID_HANDLE_VALUE) {
                    do {
                        if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L".."))
                            wprintf(L"  %s%s\n",
                                (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? L"<dir> " : L"      ",
                                fd.cFileName);
                    } while (FindNextFileW(h, &fd));
                    FindClose(h);
                }
            }
        }

        if (!vfxDll_) {
            wchar_t msg[512];
            swprintf_s(msg,
                L"NVVideoEffects.dll not found (err=%lu at %ls). "
                L"Enable debug console to see every path tried. Install "
                L"the NVIDIA Broadcast SDK from nvidia.com/en-us/geforce/"
                L"broadcasting/broadcast-sdk/resources",
                lastErr, lastPath.c_str());
            SetStatus(Status::DllMissing, msg);
            return false;
        }
    }

    // NVCVImage.dll is a SEPARATE module shipped alongside NVVideoEffects.dll
    // — the NvCVImage_* helpers live there, not in NVVideoEffects.dll.
    if (!cvDll_) {
        // Derive the directory of the already-loaded NVVideoEffects.dll and
        // look right beside it first.
        wchar_t vfxFull[MAX_PATH]{};
        std::wstring vfxDir;
        if (GetModuleFileNameW(vfxDll_, vfxFull, MAX_PATH)) {
            vfxDir = vfxFull;
            const auto slash = vfxDir.find_last_of(L"\\/");
            if (slash != std::wstring::npos) vfxDir.resize(slash + 1);
        }

        std::vector<std::wstring> cvCandidates;
        cvCandidates.emplace_back(L"NVCVImage.dll"); // PATH
        if (!vfxDir.empty())
            cvCandidates.push_back(vfxDir + L"NVCVImage.dll");

        DWORD lastErr = 0;
        std::wstring lastPath;
        for (const auto& path : cvCandidates) {
            cvDll_ = LoadNvDll(path);
            if (cvDll_) { wprintf(L"[nvsr] loaded %s\n", path.c_str()); break; }
            lastErr = GetLastError();
            lastPath = path;
            wprintf(L"[nvsr] tried  %s : err=%lu\n", path.c_str(), lastErr);
        }

        if (!cvDll_) {
            wchar_t msg[512];
            swprintf_s(msg,
                L"NVCVImage.dll not found (err=%lu at %ls) — re-run the "
                L"Broadcast SDK installer; it's shipped alongside "
                L"NVVideoEffects.dll.",
                lastErr, lastPath.c_str());
            SetStatus(Status::DllMissing, msg);
            return false;
        }
    }

    if (!cudaDll_) {
        // Try common CUDA runtime names on PATH, then inside the VFX SDK
        // directory we just found (SDK installs ship their own cudart).
        static const wchar_t* cudaNames[] = {
            L"cudart64_12.dll", L"cudart64_11.dll", L"cudart64_10.dll",
            L"cudart64_110.dll", L"cudart64.dll",
        };
        for (auto name : cudaNames) {
            cudaDll_ = LoadLibraryW(name);
            if (cudaDll_) { wprintf(L"[nvsr] loaded %s (PATH)\n", name); break; }
        }

        if (!cudaDll_) {
            // Look in the VFX install directory.
            wchar_t vfxPath[MAX_PATH]{};
            if (GetModuleFileNameW(vfxDll_, vfxPath, MAX_PATH)) {
                std::wstring dir = vfxPath;
                const auto slash = dir.find_last_of(L"\\/");
                if (slash != std::wstring::npos) dir.resize(slash + 1);
                for (auto name : cudaNames) {
                    std::wstring full = dir + name;
                    cudaDll_ = LoadNvDll(full);
                    if (cudaDll_) { wprintf(L"[nvsr] loaded %s\n", full.c_str()); break; }
                }
            }
        }

        if (!cudaDll_) {
            SetStatus(Status::CudaMissing,
                L"CUDA runtime (cudart64_*.dll) not loadable. Update your NVIDIA driver "
                L"or reinstall the Broadcast SDK.");
            return false;
        }
    }
    return true;
}

void UpscalerNV::UnloadDlls()
{
    if (vfxDll_)  { FreeLibrary(vfxDll_);  vfxDll_  = nullptr; }
    if (cvDll_)   { FreeLibrary(cvDll_);   cvDll_   = nullptr; }
    if (cudaDll_) { FreeLibrary(cudaDll_); cudaDll_ = nullptr; }
}

bool UpscalerNV::ResolveFunctions()
{
    wprintf(L"[nvsr] resolving function exports...\n");
    auto resolveVfx = [this](const char* name) -> FARPROC {
        FARPROC p = GetProcAddress(vfxDll_, name);
        if (!p) wprintf(L"[nvsr]   MISSING vfx export: %hs\n", name);
        return p;
    };
    // NvCVImage_* helpers live in a separate module (NVCVImage.dll). If we
    // don't have that one for some reason, fall back to vfxDll_ — older
    // SDK builds bundled them together.
    auto resolveCv = [this](const char* name) -> FARPROC {
        FARPROC p = cvDll_ ? GetProcAddress(cvDll_, name) : nullptr;
        if (!p) p = GetProcAddress(vfxDll_, name);
        if (!p) wprintf(L"[nvsr]   MISSING cv export: %hs\n", name);
        return p;
    };
    auto resolveCuda = [this](const char* name) -> FARPROC {
        FARPROC p = GetProcAddress(cudaDll_, name);
        if (!p) wprintf(L"[nvsr]   MISSING cuda export: %hs\n", name);
        return p;
    };

    // NVIDIA VFX
    nv::GetVersion          = (PFN_NvVFX_GetVersion)           resolveVfx("NvVFX_GetVersion");
    nv::CreateEffect        = (PFN_NvVFX_CreateEffect)         resolveVfx("NvVFX_CreateEffect");
    nv::DestroyEffect       = (PFN_NvVFX_DestroyEffect)        resolveVfx("NvVFX_DestroyEffect");
    nv::SetU32              = (PFN_NvVFX_SetU32)               resolveVfx("NvVFX_SetU32");
    nv::SetF32              = (PFN_NvVFX_SetF32)               resolveVfx("NvVFX_SetF32");
    nv::SetString           = (PFN_NvVFX_SetString)            resolveVfx("NvVFX_SetString");
    nv::SetImage            = (PFN_NvVFX_SetImage)             resolveVfx("NvVFX_SetImage");
    nv::SetCudaStream       = (PFN_NvVFX_SetCudaStream)        resolveVfx("NvVFX_SetCudaStream");
    nv::Load                = (PFN_NvVFX_Load)                 resolveVfx("NvVFX_Load");
    nv::Run                 = (PFN_NvVFX_Run)                  resolveVfx("NvVFX_Run");
    nv::Image_Alloc         = (PFN_NvCVImage_Alloc)            resolveCv("NvCVImage_Alloc");
    nv::Image_Dealloc       = (PFN_NvCVImage_Dealloc)          resolveCv("NvCVImage_Dealloc");
    nv::Image_InitFromD3D11 = (PFN_NvCVImage_InitFromD3D11Texture) resolveCv("NvCVImage_InitFromD3D11Texture");
    nv::Image_Transfer      = (PFN_NvCVImage_Transfer)         resolveCv("NvCVImage_Transfer");
    nv::Image_MapResource   = (PFN_NvCVImage_MapResource)      resolveCv("NvCVImage_MapResource");
    nv::Image_UnmapResource = (PFN_NvCVImage_UnmapResource)    resolveCv("NvCVImage_UnmapResource");

    // CUDA runtime
    nv::cudaStreamCreate      = (PFN_cudaStreamCreate)      resolveCuda("cudaStreamCreate");
    nv::cudaStreamDestroy     = (PFN_cudaStreamDestroy)     resolveCuda("cudaStreamDestroy");
    nv::cudaStreamSynchronize = (PFN_cudaStreamSynchronize) resolveCuda("cudaStreamSynchronize");

    const bool vfxOk =
        nv::CreateEffect && nv::DestroyEffect && nv::SetU32 && nv::SetF32 &&
        nv::SetString && nv::SetImage && nv::SetCudaStream && nv::Load && nv::Run &&
        nv::Image_Alloc && nv::Image_Dealloc && nv::Image_InitFromD3D11 &&
        nv::Image_Transfer;
    const bool cudaOk =
        nv::cudaStreamCreate && nv::cudaStreamDestroy && nv::cudaStreamSynchronize;

    if (!vfxOk)  { SetStatus(Status::DllMissing,  L"NVVideoEffects.dll missing expected exports"); return false; }
    if (!cudaOk) { SetStatus(Status::CudaMissing, L"cudart64 missing expected exports"); return false; }
    wprintf(L"[nvsr] all function exports resolved\n");
    return true;
}

// --- Effect / stream -------------------------------------------------------
bool UpscalerNV::CreateEffectAndStream()
{
    // CUDA stream
    CUstream stream = nullptr;
    int cs = nv::cudaStreamCreate(&stream);
    wprintf(L"[nvsr] cudaStreamCreate -> rc=%d stream=%p\n", cs, stream);
    if (cs != 0 || !stream) {
        SetStatus(Status::CudaMissing, L"cudaStreamCreate failed — check driver + SDK versions");
        return false;
    }
    cudaStream_ = stream;

    // Effect
    NvVFX_Handle eff = nullptr;
    NvCV_Status rc = nv::CreateEffect(NVVFX_FX_SUPER_RES, &eff);
    wprintf(L"[nvsr] NvVFX_CreateEffect(SuperRes) -> rc=%d handle=%p\n", rc, eff);
    if (rc != NVCV_SUCCESS || !eff) {
        SetStatus(Status::EffectLoadFailed, L"NvVFX_CreateEffect(SuperRes) failed");
        return false;
    }
    effect_ = eff;

    const std::wstring modelDirW = DefaultModelDir();
    const std::string  modelDirA = ToUtf8(modelDirW);
    wprintf(L"[nvsr] ModelDir = %s\n", modelDirW.c_str());

    rc = nv::SetString(effect_, NVVFX_MODEL_DIRECTORY, modelDirA.c_str());
    wprintf(L"[nvsr] NvVFX_SetString(ModelDir) -> rc=%d\n", rc);
    if (rc != NVCV_SUCCESS) {
        SetStatus(Status::EffectLoadFailed,
            L"NvVFX_SetString(ModelDir) failed — re-run the Broadcast SDK installer");
        return false;
    }

    rc = nv::SetCudaStream(effect_, NVVFX_CUDA_STREAM, (CUstream)cudaStream_);
    wprintf(L"[nvsr] NvVFX_SetCudaStream -> rc=%d\n", rc);
    if (rc != NVCV_SUCCESS) {
        SetStatus(Status::EffectLoadFailed, L"NvVFX_SetCudaStream failed");
        return false;
    }

    return true;
}

void UpscalerNV::DestroyEffectAndStream()
{
    if (effect_ && nv::DestroyEffect) {
        nv::DestroyEffect(effect_);
        effect_ = nullptr;
    }
    if (cudaStream_ && nv::cudaStreamDestroy) {
        nv::cudaStreamDestroy((CUstream)cudaStream_);
        cudaStream_ = nullptr;
    }
}

// --- Buffers ---------------------------------------------------------------
bool UpscalerNV::EnsureBuffers(int srcW, int srcH)
{
    const int dstW = (int)(srcW * scale_ + 0.5f);
    const int dstH = (int)(srcH * scale_ + 0.5f);

    if (!dirty_ && outTex_ && inW_ == srcW && inH_ == srcH && outW_ == dstW && outH_ == dstH)
        return true;

    ReleaseBuffers();

    // Alloc NvCVImage blobs on the heap (we don't know exact SDK struct size).
    auto alloc = []() -> NvCVImage* {
        auto* p = (NvCVImage*)std::calloc(1, sizeof(NvCVImage));
        return p;
    };
    auto free = [](void*& p) {
        if (!p) return;
        std::free(p);
        p = nullptr;
    };

    srcD3DImg_  = alloc();
    srcGpuImg_  = alloc();
    dstGpuImg_  = alloc();
    dstTmpImg_  = alloc();
    dstD3DImg_  = alloc();
    stagingImg_ = alloc();

    // SuperRes expects BGR-f32 PLANAR (three separate float planes per
    // channel) in CUDA memory. Interleaved BGRBGR layout returns
    // NVCV_ERR_BUFFER (rc=-6) from NvVFX_Load.
    NvCV_Status rc;
    rc = nv::Image_Alloc((NvCVImage*)srcGpuImg_,
        srcW, srcH, NVCV_BGR, NVCV_F32, NVCV_PLANAR, NVCV_CUDA, 1);
    if (rc != NVCV_SUCCESS) {
        SetStatus(Status::EffectLoadFailed, L"NvCVImage_Alloc (src CUDA) failed");
        ReleaseBuffers(); return false;
    }
    rc = nv::Image_Alloc((NvCVImage*)dstGpuImg_,
        dstW, dstH, NVCV_BGR, NVCV_F32, NVCV_PLANAR, NVCV_CUDA, 1);
    if (rc != NVCV_SUCCESS) {
        SetStatus(Status::EffectLoadFailed, L"NvCVImage_Alloc (dst CUDA) failed");
        ReleaseBuffers(); return false;
    }

    // Intermediate CUDA buffer in the same format as the D3D output texture
    // (BGRA u8 chunky). NvCVImage_Transfer can't go directly from
    // BGR-f32-planar-CUDA to BGRA-u8-chunky-D3D in one call
    // (NVCV_ERR_PIXELFORMAT). We convert inside CUDA first, then transfer
    // to D3D as a plain memory move.
    rc = nv::Image_Alloc((NvCVImage*)dstTmpImg_,
        dstW, dstH, NVCV_BGRA, NVCV_U8, NVCV_INTERLEAVED, NVCV_CUDA, 32);
    if (rc != NVCV_SUCCESS) {
        SetStatus(Status::EffectLoadFailed, L"NvCVImage_Alloc (dst tmp CUDA) failed");
        ReleaseBuffers(); return false;
    }

    // Create matching D3D11 output texture + SRV.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)dstW;
    td.Height = (UINT)dstH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc = { 1, 0 };
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, outTex_.ReleaseAndGetAddressOf()))) {
        SetStatus(Status::EffectLoadFailed, L"ID3D11Device::CreateTexture2D (upscaled) failed");
        ReleaseBuffers(); return false;
    }
    if (FAILED(device_->CreateShaderResourceView(outTex_.Get(), nullptr, outSRV_.ReleaseAndGetAddressOf()))) {
        SetStatus(Status::EffectLoadFailed, L"CreateShaderResourceView (upscaled) failed");
        ReleaseBuffers(); return false;
    }

    // Wire dstD3DImg to our output D3D texture (for the tail transfer).
    rc = nv::Image_InitFromD3D11((NvCVImage*)dstD3DImg_, outTex_.Get());
    if (rc != NVCV_SUCCESS) {
        SetStatus(Status::EffectLoadFailed, L"NvCVImage_InitFromD3D11Texture (dst) failed");
        ReleaseBuffers(); return false;
    }

    // (The src D3D image is initialized per-frame since the source texture
    // changes identity every time SubmitVideoTexture replaces it.)

    inW_  = srcW;  inH_  = srcH;
    outW_ = dstW;  outH_ = dstH;

    // Apply current parameters to the effect. SuperRes derives the scale
    // factor from the ratio of output/input dims — NVVFX_SCALE is only for
    // the separate "Upscale" (non-AI) effect, and passing it here returns
    // NVCV_ERR_BUFFER (-6) from NvVFX_Load.
    if (effect_) {
        nv::SetU32(effect_, NVVFX_MODE, (unsigned)mode_);
        nv::SetImage(effect_, NVVFX_INPUT_IMAGE_0,  (NvCVImage*)srcGpuImg_);
        nv::SetImage(effect_, NVVFX_OUTPUT_IMAGE_0, (NvCVImage*)dstGpuImg_);

        wprintf(L"[nvsr] EnsureBuffers: in=%dx%d out=%dx%d scale=%.2f mode=%d\n",
                srcW, srcH, dstW, dstH, scale_, mode_);
        rc = nv::Load(effect_);
        wprintf(L"[nvsr] NvVFX_Load -> rc=%d\n", rc);
        if (rc != NVCV_SUCCESS) {
            const wchar_t* why;
            switch (rc) {
                case -10: case -13:
                    why = L"model files missing (re-run the Broadcast SDK installer)";
                    break;
                case -14:
                    why = L"this SR scale isn't installed as a model";
                    break;
                case -16:
                    why = L"output resolution too large (try a lower scale, or pick a smaller camera mode)";
                    break;
                case -17:
                    why = L"this GPU isn't supported by SuperRes (need NVIDIA RTX)";
                    break;
                case -18:
                    why = L"GPU mismatch - app and SDK CUDA contexts differ";
                    break;
                case -19:
                    why = L"NVIDIA driver too old - update to the latest Game Ready / Studio driver";
                    break;
                default:
                    why = L"NvVFX_Load failed";
                    break;
            }
            wchar_t buf[256];
            swprintf_s(buf, L"NvVFX_Load rc=%d: %s", rc, why);
            SetStatus(Status::EffectLoadFailed, buf);
            ReleaseBuffers();
            return false;
        }
    }

    dirty_ = false;
    SetStatus(Status::Ready, L"ready");
    return true;
}

void UpscalerNV::ReleaseBuffers()
{
    auto freeImg = [](void*& p) {
        if (!p) return;
        if (nv::Image_Dealloc) nv::Image_Dealloc((NvCVImage*)p);
        std::free(p);
        p = nullptr;
    };
    freeImg(srcGpuImg_);
    freeImg(dstGpuImg_);
    freeImg(dstTmpImg_);
    // srcD3DImg/dstD3DImg are wrappers over external textures — Image_Dealloc
    // on them only zeros the struct, which is fine.
    freeImg(srcD3DImg_);
    freeImg(dstD3DImg_);
    freeImg(stagingImg_);

    outSRV_.Reset();
    outTex_.Reset();
    inW_ = inH_ = outW_ = outH_ = 0;
    lastSrcTex_ = nullptr; // wrapper was freed, must re-register on next Process
}

// --- Initialize / Shutdown -------------------------------------------------
bool UpscalerNV::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    wprintf(L"[nvsr] Initialize begin\n");
    device_  = device;
    context_ = context;
    if (!LoadDlls())              { wprintf(L"[nvsr] Initialize FAILED: LoadDlls\n"); return false; }
    if (!ResolveFunctions())      { wprintf(L"[nvsr] Initialize FAILED: ResolveFunctions\n"); return false; }
    if (!CreateEffectAndStream()) { wprintf(L"[nvsr] Initialize FAILED: CreateEffectAndStream\n"); return false; }
    SetStatus(Status::Ready, L"ready (no frame processed yet)");
    wprintf(L"[nvsr] Initialize SUCCESS - status: Ready\n");
    return true;
}

void UpscalerNV::Shutdown()
{
    ReleaseBuffers();
    DestroyEffectAndStream();
    UnloadDlls();
    device_  = nullptr;
    context_ = nullptr;
    SetStatus(Status::NotInitialized, L"shut down");
}

// --- Process ---------------------------------------------------------------
ID3D11ShaderResourceView* UpscalerNV::Process(ID3D11Texture2D* src, int srcW, int srcH)
{
    if (!enabled_ || !src || srcW <= 0 || srcH <= 0) return nullptr;
    // Gate on "SDK initialized", not "last Load succeeded" — that lets the
    // next Process retry with updated parameters (e.g. user picked a valid
    // scale after a -16 resolution error).
    if (!effect_ || !cudaStream_) return nullptr;

    static bool announced = false;
    if (!announced) {
        wprintf(L"[nvsr] Process: first frame %dx%d (scale=%.2fx mode=%d)\n",
                srcW, srcH, scale_, mode_);
        announced = true;
    }

    if (!EnsureBuffers(srcW, srcH)) return nullptr;

    static uint64_t s_okFrames   = 0;
    static uint64_t s_failStage  = 0;
    auto logOnce = [&](const wchar_t* where, NvCV_Status rc) {
        ++s_failStage;
        if (s_failStage <= 5 || (s_failStage % 120) == 0)
            wprintf(L"[nvsr] %s rc=%d (fail#%llu)\n", where, rc,
                    (unsigned long long)s_failStage);
    };

    // 1. Init the D3D-backed source wrapper exactly once per texture
    //    identity. NvCVImage_InitFromD3D11Texture registers the texture
    //    with CUDA; calling it again on a resource that's already
    //    registered returns cudaErrorInvalidResourceHandle (manifests as
    //    NvCV rc=-1400). The renderer recycles one videoTex_, so the
    //    pointer is stable across frames.
    NvCV_Status rc;
    if (lastSrcTex_ != src) {
        if (lastSrcTex_ && nv::Image_Dealloc)
            nv::Image_Dealloc((NvCVImage*)srcD3DImg_);
        std::memset(srcD3DImg_, 0, sizeof(NvCVImage));
        rc = nv::Image_InitFromD3D11((NvCVImage*)srcD3DImg_, src);
        if (rc != NVCV_SUCCESS) { logOnce(L"InitFromD3D11", rc); return nullptr; }
        lastSrcTex_ = src;
    }

    // 2. D3D(src) → CUDA(srcGpu), BGRA-u8 → BGR-f32-planar, scale 1/255.
    rc = nv::Image_Transfer((NvCVImage*)srcD3DImg_, (NvCVImage*)srcGpuImg_,
                            1.0f / 255.0f, (CUstream)cudaStream_,
                            (NvCVImage*)stagingImg_);
    if (rc != NVCV_SUCCESS) { logOnce(L"Transfer(D3D->CUDA)", rc); return nullptr; }

    // 3. Run SR.
    rc = nv::Run(effect_, 0);
    if (rc != NVCV_SUCCESS) { logOnce(L"NvVFX_Run", rc); return nullptr; }

    // 4a. CUDA(dstGpu BGR-f32-planar) → CUDA(dstTmp BGRA-u8-chunky).
    //     Format/type/layout conversion stays entirely in CUDA memory where
    //     NvCVImage_Transfer's scratch buffer can do the heavy lifting.
    rc = nv::Image_Transfer((NvCVImage*)dstGpuImg_, (NvCVImage*)dstTmpImg_,
                            255.0f, (CUstream)cudaStream_,
                            (NvCVImage*)stagingImg_);
    if (rc != NVCV_SUCCESS) { logOnce(L"Transfer(CUDA->CUDA)", rc); return nullptr; }

    // 4b. CUDA(dstTmp BGRA-u8-chunky) → D3D(outTex BGRA-u8-chunky).
    //     Same format on both sides, so this is just a memory-space move.
    rc = nv::Image_Transfer((NvCVImage*)dstTmpImg_, (NvCVImage*)dstD3DImg_,
                            1.0f, (CUstream)cudaStream_,
                            (NvCVImage*)stagingImg_);
    if (rc != NVCV_SUCCESS) { logOnce(L"Transfer(CUDA->D3D)", rc); return nullptr; }

    if (nv::cudaStreamSynchronize)
        nv::cudaStreamSynchronize((CUstream)cudaStream_);

    ++s_okFrames;
    if (s_okFrames == 1 || s_okFrames == 30 || (s_okFrames % 300) == 0)
        wprintf(L"[nvsr] upscaled frame #%llu\n", (unsigned long long)s_okFrames);

    return outSRV_.Get();
}
