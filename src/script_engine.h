#pragma once

#include <wizard/language_module.h>
#include <wizard/function.h>

#include "fwd.h"

namespace csharplm {
	class ScriptEngine;

	class ScriptInstance {
	public:
		ScriptInstance(const wizard::IPlugin& plugin, MonoImage* image, MonoClass* klass);
		~ScriptInstance();

		operator bool() const { return _instance != nullptr; }
		operator MonoObject*() const { return _instance; }
		MonoObject* GetManagedObject() const { return _instance; }

	private:
		void InvokeOnStart() const;
		void InvokeOnEnd() const;

	private:
		MonoImage* _image{ nullptr };
		MonoClass* _klass{ nullptr };
		MonoObject* _instance{ nullptr };
		MonoMethod* _onStartMethod{ nullptr };
		MonoMethod* _onEndMethod{ nullptr };

		friend class ScriptEngine;
	};

	using ScriptMap = std::unordered_map<std::string, ScriptInstance>;
	using ScriptOpt = std::optional<std::reference_wrapper<ScriptInstance>>;
	using PluginRef = std::reference_wrapper<const wizard::IPlugin>;
	using MethodRef = std::reference_wrapper<const wizard::Method>;
	using MethodOpt = std::optional<MethodRef>;
	//using AttributeMap = std::vector<std::pair<const char*, MonoObject*>>;

	struct ImportMethod {
		MethodRef method;
		void* addr{ nullptr };
	};
	
	struct ExportMethod {
		MonoMethod* method{ nullptr };
		MonoObject* instance{ nullptr };
	};

	class ScriptEngine :  public wizard::ILanguageModule {
	public:
		ScriptEngine() = default;
		~ScriptEngine() = default;

		// ILanguageModule
		wizard::InitResult Initialize(std::weak_ptr<wizard::IWizardProvider> provider, const wizard::IModule& module) override;
		void Shutdown() override;
		wizard::LoadResult OnPluginLoad(const wizard::IPlugin& plugin) override;
		void OnPluginStart(const wizard::IPlugin& plugin) override;
		void OnPluginEnd(const wizard::IPlugin& plugin) override;
		void OnMethodExport(const wizard::IPlugin& plugin) override;

		const ScriptMap& GetScripts() const { return _scripts; }
		ScriptOpt FindScript(const std::string& name);

		//template<typename T, typename C>
		//MonoArray* CreateArrayT(T* data, size_t size, C& klass) const;
		MonoString* CreateString(const char* source) const;
		MonoArray* CreateArray(MonoClass* klass, size_t count) const;
		MonoArray* CreateStringArray(const char** source, size_t size) const;
		MonoObject* InstantiateClass(MonoClass* klass) const;

	private:
		bool InitMono(const fs::path& monoPath);
		void ShutdownMono();

		ScriptOpt CreateScriptInstance(const wizard::IPlugin& plugin, MonoImage* image);
		void* ValidateMethod(const wizard::Method& method, std::vector<std::string>& methodErrors, MonoObject* monoInstance, MonoMethod* monoMethod, const char* nameSpace, const char* className, const char* methodName);
		
	private:
		static void HandleException(MonoObject* exc, void* userData);
		static void OnLogCallback(const char* logDomain, const char* logLevel, const char* message, mono_bool fatal, void* userData);
		static void OnPrintCallback(const char* message, mono_bool isStdout);
		static void OnPrintErrorCallback(const char* message, mono_bool isStdout);

		static void MethodCall(const wizard::Method* method, const wizard::Parameters* params, const uint8_t count, const wizard::ReturnValue* retVal);

	private:
		MonoDomain* _rootDomain{ nullptr };
		MonoDomain* _appDomain{ nullptr };

		MonoAssembly* _coreAssembly{ nullptr };
		MonoImage* _coreImage{ nullptr };

		std::shared_ptr<asmjit::JitRuntime> _rt;
		std::shared_ptr<wizard::IWizardProvider> _provider;
		std::unordered_map<std::string, ExportMethod> _exportMethods;
		std::unordered_map<std::string, ImportMethod> _importMethods;
		std::vector<std::unique_ptr<wizard::Method>> _methods;
		std::vector<wizard::Function> _functions;

		ScriptMap _scripts;

		struct MonoConfig {
			bool enableDebugging{ false };
			bool subscribeFeature{ true };
			std::string level;
			std::string mask;
			std::vector<std::string> options;
		} _config;

		friend class ScriptInstance;
	};
}