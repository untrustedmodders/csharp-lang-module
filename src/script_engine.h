#pragma once

#include <mono/jit/jit.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/object.h>
#include <mono/metadata/attrdefs.h>
#include <mono/metadata/mono-debug.h>
#include <mono/metadata/threads.h>

extern "C" {
    typedef struct _MonoClass MonoClass;
    typedef struct _MonoObject MonoObject;
    typedef struct _MonoMethod MonoMethod;
    typedef struct _MonoAssembly MonoAssembly;
    typedef struct _MonoImage MonoImage;
    typedef struct _MonoClassField MonoClassField;
    typedef struct _MonoString MonoString;
    typedef struct _MonoDomain MonoDomain;
}

namespace wizard {
    class IModule;
    class IPlugin;
}

namespace csharplm {
    using ScriptId = uint64_t;

    namespace utils {
        MonoAssembly* LoadMonoAssembly(const fs::path& assemblyPath, bool loadPDB = false);
        void PrintAssemblyTypes(MonoAssembly* assembly);
        std::string MonoStringToString(MonoString* string);
    }

    class ScriptEngine;

    class ScriptInstance {
    public:
        ScriptInstance() = delete;
        ScriptInstance(const ScriptEngine& engine, const wizard::IPlugin& plugin, MonoAssembly* assembly, MonoImage* image, MonoClass* klass);
        ScriptInstance(ScriptInstance&& other) noexcept = default;
        ~ScriptInstance();

        bool Invoke(const std::string& name) const;

        operator bool() const { return _instance != nullptr; }
        operator MonoObject*() const { return _instance; }
        MonoObject* GetManagedObject() const { return _instance; }

    private:
        MonoMethod* CacheMethod(std::string name, int params);
        MonoMethod* CacheVirtualMethod(std::string name);

    private:
        const ScriptEngine& _engine;
        const wizard::IPlugin& _plugin;

        MonoAssembly* _assembly{ nullptr };
        MonoImage* _image{ nullptr };
        MonoClass* _klass{ nullptr };
        MonoObject* _instance{ nullptr };
        std::unordered_map<std::string, MonoMethod*> _methods;

        friend class ScriptEngine;
    };

    using ScriptMap = std::unordered_map<ScriptId, ScriptInstance>;
    using ScriptRef = std::optional<std::reference_wrapper<ScriptInstance>>;

    class ScriptEngine {
    public:
        ScriptEngine() = default;
        ~ScriptEngine() = default;

        bool Initialize(const wizard::IModule& module);
        void Shutdown();

        const ScriptMap& GetScriptMap() const { return _scripts; }
        size_t GetScriptCount() const { return _scripts.size(); }

        ScriptRef FindScript(ScriptId id);

        bool LoadScript(const wizard::IPlugin& plugin);
        void StartScript(const wizard::IPlugin& plugin);
        void EndScript(const wizard::IPlugin& plugin);

        MonoString* CreateString(std::string_view string) const;
        MonoObject* InstantiateClass(MonoClass* klass) const;

    private:
        void InitMono(const fs::path& monoPath);
        void ShutdownMono();

        MonoClass* CacheCoreClass(std::string name);
        MonoMethod* CacheCoreMethod(MonoClass* klass, std::string name, int params);

        MonoClass* FindCoreClass(const std::string& name) const;
        MonoMethod* FindCoreMethod(const std::string& name) const;

    private:
        MonoDomain* _rootDomain{ nullptr };
        MonoDomain* _appDomain{ nullptr };

        MonoAssembly* _coreAssembly{ nullptr };
        MonoImage* _coreImage{ nullptr };

        std::unordered_map<std::string, MonoClass*> _coreClasses;
        std::unordered_map<std::string, MonoMethod*> _coreMethods;

        ScriptMap _scripts;

#ifdef _DEBUG
        bool _enableDebugging{ true };
#else
        bool _enableDebugging{ false };
#endif

        friend class ScriptInstance;
    };
}