#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "logging.h"
#include "DllLoader.h"
#include <filesystem>
#include <mutex>
#include <map>
#include "exports.h"
#include "util.h"
using namespace std::filesystem;

#define GAME_MODULE_NAME "FactoryGame-Win64-Shipping.exe"

static DllLoader* dllLoader;

extern "C" __declspec(dllexport) const wchar_t* bootstrapperVersion = L"2.0.4";

bool EXPORTS_IsLoaderModuleLoaded(const char* moduleName) {
    return GetModuleHandleA(moduleName) != nullptr;
}

MODULE_PTR EXPORTS_LoadModule(const char* moduleName, const wchar_t* filePath) {
    Logging::logFile << "Attempting to load module: " << moduleName << " from " << filePath << std::endl;
    return dllLoader->LoadModule(filePath);
}

FUNCTION_PTR EXPORTS_GetModuleProcAddress(MODULE_PTR module, const char* symbolName) {
    return GetProcAddress(reinterpret_cast<HMODULE>(module), symbolName);
}

FUNCTION_PTR EXPORTS_ResolveModuleSymbol(const char* symbolName) {
    return reinterpret_cast<FUNCTION_PTR>(dllLoader->resolver->ResolveSymbol(symbolName));
}

void EXPORTS_FlushDebugSymbols() {
    dllLoader->FlushDebugSymbols();
}

std::string GetLastErrorAsString();

void discoverLoaderMods(std::map<std::string, HMODULE>& discoveredModules, const std::filesystem::path& rootGameDirectory) {
    std::filesystem::path directoryPath = rootGameDirectory / "loaders";
    std::filesystem::create_directories(directoryPath);
    for (auto& file : std::filesystem::directory_iterator(directoryPath)) {
        if (file.is_regular_file() && file.path().extension() == ".dll") {
            Logging::logFile << "Discovering loader module candidate tetest " << file.path().filename() << std::endl;
            HMODULE loadedModule = dllLoader->LoadModule(file.path().wstring().c_str());
            if (loadedModule != nullptr) {
                Logging::logFile << "Successfully loaded module " << file.path().filename() << std::endl;
                discoveredModules.insert({file.path().filename().string(), loadedModule});
            } else {
                Logging::logFile << "Failed to load module " << file.path().filename() << ": " << std::endl;
                Logging::logFile << "Last Error Message: " << GetLastErrorAsString() << std::endl;
                exit(1);
            }
        }
    }
}

void bootstrapLoaderMods(const std::map<std::string, HMODULE>& discoveredModules, const std::wstring& gameRootDirectory) {
    for (auto& loaderModule : discoveredModules) {
        FUNCTION_PTR bootstrapFunc = GetProcAddress(loaderModule.second, "BootstrapModule");
        if (bootstrapFunc == nullptr) {
            Logging::logFile << "[WARNING]: BootstrapModule() not found in loader module " << loaderModule.first << "!" << std::endl;
            return;
        }
        BootstrapAccessors accessors{
            gameRootDirectory.c_str(),
            &EXPORTS_LoadModule,
            &EXPORTS_GetModuleProcAddress,
            &EXPORTS_IsLoaderModuleLoaded,
            &EXPORTS_ResolveModuleSymbol,
            bootstrapperVersion,
            &EXPORTS_FlushDebugSymbols
        };
        Logging::logFile << "Bootstrapping module " << loaderModule.first << std::endl;
        ((BootstrapModuleFunc) bootstrapFunc)(accessors);
    }
}

static std::mutex setupHookMutex;
static volatile bool hookAlreadySetup = false;

std::filesystem::path resolveGameRootDir() {
    wchar_t pathBuffer[2048]; //just to be sure it will always fit
    GetModuleFileNameW(GetModuleHandleA(GAME_MODULE_NAME), pathBuffer, 2048);
    std::filesystem::path rootDirPath{pathBuffer};
    std::string gameFolderName(GAME_MODULE_NAME);
    gameFolderName.erase(gameFolderName.find('-'));
    //we go up the directory tree until we find the folder called
    //FactoryGame, which denotes the root of the game directory
    while (!std::filesystem::exists(rootDirPath / gameFolderName)) {
        rootDirPath = rootDirPath.parent_path();
    }
    return rootDirPath;
}

void setupExecutableHook(HMODULE selfModuleHandle) {
    //fast route to exit before locking on mutex
    if (hookAlreadySetup) return;
    std::lock_guard guard(setupHookMutex);
    //check if we have loaded while we loaded on mutex
    if (hookAlreadySetup) return;
    hookAlreadySetup = true; //mark as loaded

    //initialize systems, load symbols, call bootstrapper modules
    Logging::initializeLogging();
    Logging::logFile << "Setting up hooking" << std::endl;

    path rootGameDirectory = resolveGameRootDir();
    path bootstrapperDirectory = path(getModuleFileName(selfModuleHandle)).parent_path();
    Logging::logFile << "Game Root Directory: " << rootGameDirectory << std::endl;
    Logging::logFile << "Bootstrapper Directory: " << bootstrapperDirectory << std::endl;

    HMODULE gameModule = GetModuleHandleA(GAME_MODULE_NAME);
    if (gameModule == nullptr) {
        Logging::logFile << "Failed to find primary game module with name: " << GAME_MODULE_NAME << std::endl;
        exit(1);
    }
    path diaDllPath = bootstrapperDirectory / "msdia140.dll";
    HMODULE diaDllHandle = LoadLibraryW(diaDllPath.wstring().c_str());
    if (diaDllHandle == nullptr) {
        Logging::logFile << "Failed to load DIA SDK implementation DLL." << std::endl;
        Logging::logFile << "Expected to find it at: " << diaDllPath.string() << std::endl;
        Logging::logFile << "Make sure it is here and restart. Exiting now." << std::endl;
        exit(1);
    }
    //TODO strict mode where missing symbols result in aborting?
    auto *resolver = new SymbolResolver(gameModule, diaDllHandle, false);
    dllLoader = new DllLoader(resolver);
    Logging::logFile << "Discovering loader modules..." << std::endl;
    std::map<std::string, HMODULE> discoveredMods;
    discoverLoaderMods(discoveredMods, rootGameDirectory);

    Logging::logFile << "Bootstrapping loader modules..." << std::endl;
    bootstrapLoaderMods(discoveredMods, rootGameDirectory.wstring());

    Logging::logFile << "Successfully performed bootstrapping." << std::endl;
}
