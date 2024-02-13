#include "module.h"
#include "script_glue.h"

#include <mono/jit/jit.h>
#include <mono/utils/mono-logger.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/object.h>
#include <mono/metadata/class.h>
#include <mono/metadata/attrdefs.h>
#include <mono/metadata/mono-debug.h>
#include <mono/metadata/mono-config.h>
#include <mono/metadata/threads.h>
#include <mono/metadata/exception.h>

#include <plugify/module.h>
#include <plugify/plugin.h>
#include <plugify/log.h>
#include <plugify/plugin_descriptor.h>
#include <plugify/plugify_provider.h>

#include <glaze/glaze.hpp>
#include <dyncall/dyncall.h>

using namespace csharplm;
using namespace plugify;
using namespace std::string_literals;

namespace csharplm::utils {
	std::string ReadText(const fs::path& filepath) {
		std::ifstream istream(filepath, std::ios::binary);
		if (!istream.is_open())
			return {};
		istream.unsetf(std::ios::skipws);
		return { std::istreambuf_iterator<char>{istream}, std::istreambuf_iterator<char>{} };
	}

	template<typename T>
	bool ReadBytes(const fs::path& file, const std::function<void(std::span<T>)>& callback) {
		std::ifstream istream(file, std::ios::binary);
		if (!istream.is_open())
			return false;
		std::vector<T> buffer{ std::istreambuf_iterator<char>(istream), std::istreambuf_iterator<char>() };
		callback({ buffer.data(), buffer.size() });
		return true;
	}

	MonoAssembly* LoadMonoAssembly(const fs::path& assemblyPath, bool loadPDB, MonoImageOpenStatus& status) {
		MonoImage* image = nullptr;

		ReadBytes<char>(assemblyPath, [&image, &status](std::span<char> buffer) {
			image = mono_image_open_from_data_full(buffer.data(), static_cast<uint32_t>(buffer.size()), 1, &status, 0);
		});

		if (status != MONO_IMAGE_OK)
			return nullptr;

		if (loadPDB) {
			fs::path pdbPath(assemblyPath);
			pdbPath.replace_extension(".pdb");

			ReadBytes<mono_byte>(pdbPath, [&image](std::span<mono_byte> buffer) {
				mono_debug_open_image_from_memory(image, buffer.data(), static_cast<int>(buffer.size()));
			});

			// If pdf not load ?
		}

		MonoAssembly* assembly = mono_assembly_load_from_full(image, assemblyPath.string().c_str(), &status, 0);
		mono_image_close(image);
		return assembly;
	}

	void PrintAssemblyTypes(MonoAssembly* assembly, const std::function<void(std::string)>& out) {
		MonoImage* image = mono_assembly_get_image(assembly);
		const MonoTableInfo* typeDefinitionsTable = mono_image_get_table_info(image, MONO_TABLE_TYPEDEF);
		int numTypes = mono_table_info_get_rows(typeDefinitionsTable);

		for (int i = 0; i < numTypes; ++i) {
			uint32_t cols[MONO_TYPEDEF_SIZE];
			mono_metadata_decode_row(typeDefinitionsTable, i, cols, MONO_TYPEDEF_SIZE);

			const char* nameSpace = mono_metadata_string_heap(image, cols[MONO_TYPEDEF_NAMESPACE]);
			const char* name = mono_metadata_string_heap(image, cols[MONO_TYPEDEF_NAME]);

			out(std::format("{}.{}", nameSpace, ".", + name));
		}
	}

	/*void MonoStringToCStr(MonoString* string, char* dest) {
		char* cStr = mono_string_to_utf8(string);
		strcpy_s(dest, strlen(dest), cStr);
		mono_free(cStr);
	}

	void MonoStringToCWStr(MonoString* string, wchar_t* dest) {
		wchar_t* cWStr = mono_string_to_utf16(string);
		wcscpy_s(dest, wcslen(dest), cWStr);
		mono_free(cWStr);
	}*/

	std::string MonoStringToString(MonoString* string) {
		char* cStr = mono_string_to_utf8(string);
		std::string str(cStr);
		mono_free(cStr);
		return str;
	}

	std::wstring MonoStringToWString(MonoString* string) {
		wchar_t* cWStr = mono_string_to_utf16(string);
		std::wstring str(cWStr);
		mono_free(cWStr);
		return str;
	}

	std::string GetStringProperty(const char* propertyName, MonoClass* classType, MonoObject* classObject) {
		MonoProperty* messageProperty = mono_class_get_property_from_name(classType, propertyName);
		MonoMethod* messageGetter = mono_property_get_get_method(messageProperty);
		MonoString* messageString = (MonoString*) mono_runtime_invoke(messageGetter, classObject, nullptr, nullptr);
		if (messageString != nullptr)
			return MonoStringToString(messageString);
		return {};
	}

	/*template<typename T>
	size_t MonoArrayToCArr(MonoArray* array, T* dest, size_t size) {
		auto length = mono_array_length(array);
		auto minLen = std::min(length, size);
		for (size_t i = 0; i < minLen; ++i) {
			MonoObject* element = mono_array_get(array, MonoObject*, i);
			if constexpr (std::is_same_v<T, char*>) {
				if (dest[i] != nullptr) {
					if (element != nullptr)
						MonoStringToCStr((MonoString*)element, dest[i]);
					else if (strlen(dest[i]))
						dest[i][0] = '\0';
				}
			} else if constexpr (std::is_same_v<T, wchar_t*>) {
				if (dest[i] != nullptr) {
					if (element != nullptr)
						MonoStringToCWStr((MonoString*)element, dest[i]);
					else if (wcslen(dest[i]))
						dest[i][0] = '\0';
				}
			} else {
				if (element != nullptr)
					dest[i] = *(T*) mono_object_unbox(element);
				else
					dest[i] = T{};
			}
		}
		return minLen;
	}*/

	template<typename T>
	void MonoArrayToVector(MonoArray* array, std::vector<T>& dest) {
		auto length = mono_array_length(array);
		dest.resize(length);
		for (size_t i = 0; i < length; ++i) {
			MonoObject* element = mono_array_get(array, MonoObject*, i);
			if constexpr (std::is_same_v<T, std::string>) {
				if (element != nullptr)
					dest[i] = MonoStringToString((MonoString*)element);
				else
					dest[i] = {};
			} else if constexpr (std::is_same_v<T, std::wstring>) {
				if (element != nullptr)
					dest[i] = MonoStringToWString((MonoString*)element);
				else
					dest[i] = {};
			} else {
				if (element != nullptr)
					dest[i] = *(T*) mono_object_unbox(element);
				else
					dest[i] = {};
			}
		}
	}

	ValueType MonoTypeToValueType(const char* typeName) {
		static std::unordered_map<std::string, ValueType> valueTypeMap = {
			{ "System.Void", ValueType::Void },
			{ "System.Boolean", ValueType::Bool },
			{ "System.Char", ValueType::Char8 },
			{ "System.SByte", ValueType::Int8 },
			{ "System.Int16", ValueType::Int16 },
			{ "System.Int32", ValueType::Int32 },
			{ "System.Int64", ValueType::Int64 },
			{ "System.Byte", ValueType::Uint8 },
			{ "System.UInt16", ValueType::Uint16 },
			{ "System.UInt32", ValueType::Uint32 },
			{ "System.UInt64", ValueType::Uint64 },
			{ "System.IntPtr", ValueType::Ptr64 },
			{ "System.UIntPtr", ValueType::Ptr64 },
			{ "System.Single", ValueType::Float },
			{ "System.Double", ValueType::Double },
			{ "System.String", ValueType::String },
			{ "System.Boolean[]", ValueType::ArrayBool },
			{ "System.Char[]", ValueType::ArrayChar8 },
			{ "System.SByte[]", ValueType::ArrayInt8 },
			{ "System.Int16[]", ValueType::ArrayInt16 },
			{ "System.Int32[]", ValueType::ArrayInt32 },
			{ "System.Int64[]", ValueType::ArrayInt64 },
			{ "System.Byte[]", ValueType::ArrayUint8 },
			{ "System.UInt16[]", ValueType::ArrayUint16 },
			{ "System.UInt32[]", ValueType::ArrayUint32 },
			{ "System.UInt64[]", ValueType::ArrayUint64 },
			{ "System.IntPtr[]", ValueType::ArrayPtr64 },
			{ "System.UIntPtr[]", ValueType::ArrayPtr64 },
			{ "System.Single[]", ValueType::ArrayFloat },
			{ "System.Double[]", ValueType::ArrayDouble },
			{ "System.String[]", ValueType::ArrayString },
		};
		auto it = valueTypeMap.find(typeName);
		if (it != valueTypeMap.end())
			return std::get<ValueType>(*it);
		return ValueType::Invalid;
	}

	std::vector<std::string_view> Split(std::string_view strv, std::string_view delims = " ") {
		std::vector<std::string_view> output;
		size_t first = 0;

		while (first < strv.size()) {
			const size_t second = strv.find_first_of(delims, first);

			if (first != second)
				output.emplace_back(strv.substr(first, second-first));

			if (second == std::string_view::npos)
				break;

			first = second + 1;
		}

		return output;
	}
}

InitResult CSharpLanguageModule::Initialize(std::weak_ptr<IPlugifyProvider> provider, const IModule& module) {
	if (!(_provider = provider.lock()))
		return ErrorData{ "Provider not exposed" };

	auto json = utils::ReadText(module.GetBaseDir() / "config.json");
	auto config = glz::read_json<MonoConfig>(json);
	if (!config.has_value())
		return ErrorData{ std::format("MonoConfig: 'config.json' has JSON parsing error: {}", glz::format_error(config.error(), json)) };
	_config = std::move(*config);

	fs::path monoPath(module.GetBaseDir() / "mono/lib");
	if (!fs::exists(monoPath))
		return ErrorData{ std::format("Path to mono assemblies not exist '{}'", monoPath.string()) };

	if (!InitMono(monoPath))
		return ErrorData{ "Initialization of mono failed" };

	ScriptGlue::RegisterFunctions();

	_rt = std::make_shared<asmjit::JitRuntime>();

	// Create an app domain
	char appName[] = "PlugifyMonoRuntime";
	_appDomain = mono_domain_create_appdomain(appName, nullptr);
	mono_domain_set(_appDomain, true);

	fs::path coreAssemblyPath{ module.GetBaseDir() / "bin/Plugify.dll" };

	// Load a core assembly
	MonoImageOpenStatus status = MONO_IMAGE_IMAGE_INVALID;
	_coreAssembly = utils::LoadMonoAssembly(coreAssemblyPath, _config.enableDebugging, status);
	if (!_coreAssembly)
		return ErrorData{ std::format("Failed to load '{}' core assembly. Reason: {}", coreAssemblyPath.string(), mono_image_strerror(status)) };

	_coreImage = mono_assembly_get_image(_coreAssembly);
	if (!_coreImage)
		return ErrorData{ std::format("Failed to load '{}' core image.", coreAssemblyPath.string()) };

	// Retrieve and cache core classes/methods

	/// Plugin
	MonoClass* pluginClass = mono_class_from_name(_coreImage, "Plugify", "Plugin");
	if (!pluginClass)
		return ErrorData{ std::format("Failed to find 'Plugin' core class! Check '{}' assembly!", coreAssemblyPath.string()) };

	MonoClass* subscribeAttribute = mono_class_from_name(_coreImage, "Plugify", "SubscribeAttribute");
	if (!subscribeAttribute)
		return ErrorData{ std::format("Failed to find 'SubscribeAttribute' core class! Check '{}' assembly!", coreAssemblyPath.string()) };

	MonoMethod* pluginCtor = mono_class_get_method_from_name(pluginClass, ".ctor", 8);
	if (!pluginCtor)
		return ErrorData{ std::format("Failed to find 'Plugin' .ctor method! Check '{}' assembly!", coreAssemblyPath.string()) };

	_provider->Log("[csharplm] Inited!", Severity::Debug);

	return InitResultData{};
}

void CSharpLanguageModule::Shutdown() {
	_exportMethods.clear();
	_importMethods.clear();
	_functions.clear();
	_methods.clear();
	_scripts.clear();
	_provider.reset();
	_rt.reset();

	ShutdownMono();
}

/*MonoAssembly* CSharpLanguageModule::OnMonoAssemblyPreloadHook(MonoAssemblyName* aname, char** assemblies_path, void* user_data) {
	return OnMonoAssemblyLoad(mono_assembly_name_get_name(aname));
}*/

bool CSharpLanguageModule::InitMono(const fs::path& monoPath) {
	mono_trace_set_print_handler(OnPrintCallback);
	mono_trace_set_printerr_handler(OnPrintErrorCallback);
	mono_trace_set_log_handler(OnLogCallback, nullptr);

	mono_set_assemblies_path(monoPath.string().c_str());

	mono_config_parse(nullptr);

	// Seems we can write custom assembly loader here
	//mono_install_assembly_preload_hook(OnMonoAssemblyPreloadHook, nullptr);

	if (!_config.options.empty()) {
		std::vector<char*> options;
		options.reserve(_config.options.size());
		for (auto& opt : _config.options) {
			if (std::find(options.begin(), options.end(), opt.data()) == options.end()) {
				options.push_back(opt.data());
				if (opt.starts_with("--debugger"))
					_provider->Log(std::format("[csharplm] Mono debugger: {}", opt), Severity::Info);
			}
		}
		mono_jit_parse_options(static_cast<int>(options.size()), options.data());
	}

	if (!_config.level.empty())
		mono_trace_set_level_string(_config.level.c_str());
	if (!_config.mask.empty())
		mono_trace_set_mask_string(_config.mask.c_str());
	if (_config.enableDebugging)
		mono_debug_init(MONO_DEBUG_FORMAT_MONO);

	_rootDomain = mono_jit_init("PlugifyJITRuntime");
	if (!_rootDomain)
		return false;

	if (_config.enableDebugging)
		mono_debug_domain_create(_rootDomain);

	mono_thread_set_main(mono_thread_current());

	mono_install_unhandled_exception_hook(HandleException, nullptr);
	//mono_set_crash_chaining(true);

	char* buildInfo = mono_get_runtime_build_info();
	_provider->Log(std::format("[csharplm] Mono: Runtime version: {}", buildInfo), Severity::Debug);
	mono_free(buildInfo);

	return true;
}

void CSharpLanguageModule::ShutdownMono() {
	mono_domain_set(mono_get_root_domain(), false);

	if (_appDomain) {
		mono_domain_unload(_appDomain);
		_appDomain = nullptr;
	}

	if (_rootDomain) {
		mono_jit_cleanup(_rootDomain);
		_rootDomain = nullptr;
	}

	_coreAssembly = nullptr;
	_coreImage = nullptr;
}

template<typename T, typename C>
MonoArray* CreateArrayT(const std::vector<T>& source, C& klass) {
	MonoArray* array = g_csharplm.CreateArray(klass(), source.size());
	for (size_t i = 0; i < source.size(); ++i) {
		mono_array_set(array, T, i, source[i]);
	}
	return array;
}

template<typename T>
void* MonoArrayToArg(MonoArray* source, std::vector<void*>& args) {
	auto* dest = new std::vector<T>();
	if (source != nullptr) {
		utils::MonoArrayToVector(source, *dest);
	}
	args.push_back((void*) dest);
	return dest;
}

void* MonoStringToArg(MonoString* source, std::vector<void*>& args) {
	std::string* dest;
	if (source != nullptr) {
		char* cStr = mono_string_to_utf8(source);
		dest = new std::string(cStr);
		mono_free(cStr);
	} else {
		dest = new std::string();
	}
	args.push_back((void*) dest);
	return dest;
}

void StoreRefParam(const Parameters* p, DCCallVM* vm, std::vector<void*>& args, uint8_t i, ValueType type) {
	switch (type) {
		case ValueType::Invalid:
		case ValueType::Void:
			break;
		case ValueType::Bool:
		case ValueType::Char8:
		case ValueType::Char16:
		case ValueType::Int8:
		case ValueType::Int16:
		case ValueType::Int32:
		case ValueType::Int64:
		case ValueType::Uint8:
		case ValueType::Uint16:
		case ValueType::Uint32:
		case ValueType::Uint64:
		case ValueType::Ptr64:
		case ValueType::Float:
		case ValueType::Double:
		case ValueType::Function:
			dcArgPointer(vm, p->GetArgumentPtr(i));
			break;
		// MonoString*
		case ValueType::String:
			dcArgPointer(vm, MonoStringToArg(p->GetArgument<MonoString*>(i), args));
			break;
		// MonoArray*
		case ValueType::ArrayBool:
			dcArgPointer(vm, MonoArrayToArg<bool>(p->GetArgument<MonoArray*>(i), args));
			break;
		case ValueType::ArrayChar8:
			dcArgPointer(vm, MonoArrayToArg<char>(p->GetArgument<MonoArray*>(i), args));
			break;
		case ValueType::ArrayChar16:
			dcArgPointer(vm, MonoArrayToArg<wchar_t>(p->GetArgument<MonoArray*>(i), args));
			break;
		case ValueType::ArrayInt8:
			dcArgPointer(vm, MonoArrayToArg<int8_t>(p->GetArgument<MonoArray*>(i), args));
			break;
		case ValueType::ArrayInt16:
			dcArgPointer(vm, MonoArrayToArg<int16_t>(p->GetArgument<MonoArray*>(i), args));
			break;
		case ValueType::ArrayInt32:
			dcArgPointer(vm, MonoArrayToArg<int32_t>(p->GetArgument<MonoArray*>(i), args));
			break;
		case ValueType::ArrayInt64:
			dcArgPointer(vm, MonoArrayToArg<int64_t>(p->GetArgument<MonoArray*>(i), args));
			break;
		case ValueType::ArrayUint8:
			dcArgPointer(vm, MonoArrayToArg<uint8_t>(p->GetArgument<MonoArray*>(i), args));
			break;
		case ValueType::ArrayUint16:
			dcArgPointer(vm, MonoArrayToArg<uint16_t>(p->GetArgument<MonoArray*>(i), args));
			break;
		case ValueType::ArrayUint32:
			dcArgPointer(vm, MonoArrayToArg<uint32_t>(p->GetArgument<MonoArray*>(i), args));
			break;
		case ValueType::ArrayUint64:
			dcArgPointer(vm, MonoArrayToArg<uint64_t>(p->GetArgument<MonoArray*>(i), args));
			break;
		case ValueType::ArrayPtr64:
			dcArgPointer(vm, MonoArrayToArg<uintptr_t>(p->GetArgument<MonoArray*>(i), args));
			break;
		case ValueType::ArrayFloat:
			dcArgPointer(vm, MonoArrayToArg<float>(p->GetArgument<MonoArray*>(i), args));
			break;
		case ValueType::ArrayDouble:
			dcArgPointer(vm, MonoArrayToArg<double>(p->GetArgument<MonoArray*>(i), args));
			break;
		case ValueType::ArrayString:
			dcArgPointer(vm, MonoArrayToArg<std::string>(p->GetArgument<MonoArray*>(i), args));
			break;
	}
}

void StoreValueParam(const Parameters* p, DCCallVM* vm, std::vector<void*>& args, uint8_t i, ValueType type) {
	switch (type) {
		case ValueType::Invalid:
		case ValueType::Void:
			break;

		// No mono
		case ValueType::Bool:
			dcArgBool(vm, p->GetArgument<bool>(i));
			break;
		case ValueType::Char8:
			dcArgChar(vm, p->GetArgument<char>(i));
			break;
		case ValueType::Char16:
			dcArgShort(vm, static_cast<short>(p->GetArgument<wchar_t>(i)));
			break;
		case ValueType::Int8:
		case ValueType::Uint8:
			dcArgChar(vm, p->GetArgument<int8_t>(i));
			break;
		case ValueType::Int16:
		case ValueType::Uint16:
			dcArgShort(vm, p->GetArgument<int16_t>(i));
			break;
		case ValueType::Int32:
		case ValueType::Uint32:
			dcArgInt(vm, p->GetArgument<int32_t>(i));
			break;
		case ValueType::Int64:
		case ValueType::Uint64:
			dcArgLongLong(vm, p->GetArgument<int64_t>(i));
		case ValueType::Function:
		case ValueType::Ptr64:
			dcArgPointer(vm, p->GetArgument<void*>(i));
			break;
		case ValueType::Float:
			dcArgFloat(vm, p->GetArgument<float>(i));
			break;
		case ValueType::Double:
			dcArgDouble(vm, p->GetArgument<double>(i));
			break;
		// MonoString*
		case ValueType::String:
			dcArgPointer(vm, MonoStringToArg(p->GetArgument<MonoString*>(i), args));
			break;
		// MonoArray*
		case ValueType::ArrayBool:
			dcArgPointer(vm, MonoArrayToArg<bool>(p->GetArgument<MonoArray*>(i), args));
			break;
		case ValueType::ArrayChar8:
			dcArgPointer(vm, MonoArrayToArg<char>(p->GetArgument<MonoArray*>(i), args));
			break;
		case ValueType::ArrayChar16:
			dcArgPointer(vm, MonoArrayToArg<wchar_t>(p->GetArgument<MonoArray*>(i), args));
			break;
		case ValueType::ArrayInt8:
			dcArgPointer(vm, MonoArrayToArg<int8_t>(p->GetArgument<MonoArray*>(i), args));
			break;
		case ValueType::ArrayInt16:
			dcArgPointer(vm, MonoArrayToArg<int16_t>(p->GetArgument<MonoArray*>(i), args));
			break;
		case ValueType::ArrayInt32:
			dcArgPointer(vm, MonoArrayToArg<int32_t>(p->GetArgument<MonoArray*>(i), args));
			break;
		case ValueType::ArrayInt64:
			dcArgPointer(vm, MonoArrayToArg<int64_t>(p->GetArgument<MonoArray*>(i), args));
			break;
		case ValueType::ArrayUint8:
			dcArgPointer(vm, MonoArrayToArg<uint8_t>(p->GetArgument<MonoArray*>(i), args));
			break;
		case ValueType::ArrayUint16:
			dcArgPointer(vm, MonoArrayToArg<uint16_t>(p->GetArgument<MonoArray*>(i), args));
			break;
		case ValueType::ArrayUint32:
			dcArgPointer(vm, MonoArrayToArg<uint32_t>(p->GetArgument<MonoArray*>(i), args));
			break;
		case ValueType::ArrayUint64:
			dcArgPointer(vm, MonoArrayToArg<uint64_t>(p->GetArgument<MonoArray*>(i), args));
			break;
		case ValueType::ArrayPtr64:
			dcArgPointer(vm, MonoArrayToArg<uintptr_t>(p->GetArgument<MonoArray*>(i), args));
			break;
		case ValueType::ArrayFloat:
			dcArgPointer(vm, MonoArrayToArg<float>(p->GetArgument<MonoArray*>(i), args));
			break;
		case ValueType::ArrayDouble:
			dcArgPointer(vm, MonoArrayToArg<double>(p->GetArgument<MonoArray*>(i), args));
			break;
		case ValueType::ArrayString:
			dcArgPointer(vm, MonoArrayToArg<std::string>(p->GetArgument<MonoArray*>(i), args));
			break;
	}
}

void DeleteParam(std::vector<void*>& args, uint8_t& i, ValueType type) {
	switch (type) {
		case ValueType::String: {
			delete (std::string*) args[i++];
			break;
		}
		case ValueType::ArrayBool: {
			delete (std::vector<bool>*) args[i++];
			break;
		}
		case ValueType::ArrayChar8: {
			delete (std::vector<char>*) args[i++];
			break;
		}
		case ValueType::ArrayChar16: {
			delete (std::vector<wchar_t>*) args[i++];
			break;
		}
		case ValueType::ArrayInt8: {
			delete (std::vector<int16_t>*) args[i++];
			break;
		}
		case ValueType::ArrayInt16: {
			delete (std::vector<int16_t>*) args[i++];
			break;
		}
		case ValueType::ArrayInt32: {
			delete (std::vector<int32_t>*) args[i++];
			break;
		}
		case ValueType::ArrayInt64: {
			delete (std::vector<int64_t>*) args[i++];
			break;
		}
		case ValueType::ArrayUint8: {
			delete (std::vector<uint8_t>*) args[i++];
			break;
		}
		case ValueType::ArrayUint16: {
			delete (std::vector<uint16_t>*) args[i++];
			break;
		}
		case ValueType::ArrayUint32: {
			delete (std::vector<uint32_t>*) args[i++];
			break;
		}
		case ValueType::ArrayUint64: {
			delete (std::vector<uint64_t>*) args[i++];
			break;
		}
		case ValueType::ArrayPtr64: {
			delete (std::vector<uintptr_t>*) args[i++];
			break;
		}
		case ValueType::ArrayFloat: {
			delete (std::vector<float>*) args[i++];
			break;
		}
		case ValueType::ArrayDouble: {
			delete (std::vector<double>*) args[i++];
			break;
		}
		case ValueType::ArrayString: {
			delete (std::vector<std::string>*) args[i++];
			break;
		}
		default: {
			break;
		}
	}
}

// Call from C# to C++
void CSharpLanguageModule::ExternalCall(const Method* method, void* addr, const Parameters* p, uint8_t count, const ReturnValue* ret) {
	std::vector<void*> args;

	DCCallVM* vm = dcNewCallVM(4096);
	dcMode(vm, DC_CALL_C_DEFAULT);
	dcReset(vm);

	ValueType returnType = method->retType.type;
	bool hasRet = returnType >= ValueType::String && returnType <= ValueType::ArrayString;
	bool hasRefs = false;

	/* branchless */
	using StoreParams = decltype(&StoreValueParam);
	static StoreParams storeParam[2] = { &StoreValueParam, &StoreRefParam };

	uint8_t j = 0;

	// Store parameters

	if (hasRet) {
		storeParam[1](p, vm, args, j++, returnType);
	}

	for (uint8_t i = 0; i < count; ++i) {
		auto& param = method->paramTypes[i];
		storeParam[param.ref](p, vm, args, j++, param.type);
		hasRefs |= param.ref;
	}

	// Call function and store return

	switch (returnType) {
		case ValueType::Invalid:
			break;

		case ValueType::Void: {
			dcCallVoid(vm, addr);
			break;
		}
		case ValueType::Bool: {
			bool val = dcCallBool(vm, addr);
			ret->SetReturnPtr(val);
			break;
		}
		case ValueType::Char8: {
			char val = dcCallChar(vm, addr);
			ret->SetReturnPtr(val);
			break;
		}
		case ValueType::Char16: {
			wchar_t val = static_cast<wchar_t>(dcCallShort(vm, addr));
			ret->SetReturnPtr(val);
			break;
		}
		case ValueType::Int8: {
			int8_t val = dcCallChar(vm, addr);
			ret->SetReturnPtr(val);
			break;
		}
		case ValueType::Int16: {
			int16_t val = dcCallShort(vm, addr);
			ret->SetReturnPtr(val);
			break;
		}
		case ValueType::Int32: {
			int32_t val = dcCallInt(vm, addr);
			ret->SetReturnPtr(val);
			break;
		}
		case ValueType::Int64: {
			int64_t val = dcCallLongLong(vm, addr);
			ret->SetReturnPtr(val);
			break;
		}
		case ValueType::Uint8: {
			uint8_t val = static_cast<uint8_t>(dcCallChar(vm, addr));
			ret->SetReturnPtr(val);
			break;
		}
		case ValueType::Uint16: {
			uint16_t val = static_cast<uint16_t>(dcCallShort(vm, addr));
			ret->SetReturnPtr(val);
			break;
		}
		case ValueType::Uint32: {
			uint32_t val = static_cast<uint32_t>(dcCallInt(vm, addr));
			ret->SetReturnPtr(val);
			break;
		}
		case ValueType::Uint64: {
			uint64_t val = static_cast<uint64_t>(dcCallLongLong(vm, addr));
			ret->SetReturnPtr(val);
			break;
		}
		case ValueType::Function:
		case ValueType::Ptr64: {
			void* val = dcCallPointer(vm, addr);
			ret->SetReturnPtr(val);
			break;
		}
		case ValueType::Float: {
			float val = dcCallFloat(vm, addr);
			ret->SetReturnPtr(val);
			break;
		}
		case ValueType::Double: {
			double val = dcCallDouble(vm, addr);
			ret->SetReturnPtr(val);
			break;
		}
		case ValueType::String: {
			dcCallVoid(vm, addr);
			ret->SetReturnPtr(g_csharplm.CreateString(*(std::string*)args[0]));
			break;
		}
		case ValueType::ArrayBool: {
			dcCallVoid(vm, addr);
			ret->SetReturnPtr(CreateArrayT<bool>(*(std::vector<bool>*)args[0], mono_get_char_class));
			break;
		}
		case ValueType::ArrayChar8: {
			dcCallVoid(vm, addr);
			ret->SetReturnPtr(CreateArrayT<char>(*(std::vector<char>*)args[0], mono_get_char_class));
			break;
		}
		case ValueType::ArrayChar16: {
			dcCallVoid(vm, addr);
			ret->SetReturnPtr(CreateArrayT<wchar_t>(*(std::vector<wchar_t>*)args[0], mono_get_int16_class));
			break;
		}
		case ValueType::ArrayInt8: {
			dcCallVoid(vm, addr);
			ret->SetReturnPtr(CreateArrayT<int8_t>(*(std::vector<int8_t>*)args[0], mono_get_sbyte_class));
			break;
		}
		case ValueType::ArrayInt16: {
			dcCallVoid(vm, addr);
			ret->SetReturnPtr(CreateArrayT<int16_t>(*(std::vector<int16_t>*)args[0], mono_get_int16_class));
			break;
		}
		case ValueType::ArrayInt32: {
			dcCallVoid(vm, addr);
			ret->SetReturnPtr(CreateArrayT<int32_t>(*(std::vector<int32_t>*)args[0], mono_get_int32_class));
			break;
		}
		case ValueType::ArrayInt64: {
			dcCallVoid(vm, addr);
			ret->SetReturnPtr(CreateArrayT<int64_t>(*(std::vector<int64_t>*)args[0], mono_get_int64_class));
			break;
		}
		case ValueType::ArrayUint8: {
			dcCallVoid(vm, addr);
			ret->SetReturnPtr(CreateArrayT<uint8_t>(*(std::vector<uint8_t>*)args[0], mono_get_byte_class));
			break;
		}
		case ValueType::ArrayUint16: {
			dcCallVoid(vm, addr);
			ret->SetReturnPtr(CreateArrayT<uint16_t>(*(std::vector<uint16_t>*)args[0], mono_get_uint16_class));
			break;
		}
		case ValueType::ArrayUint32: {
			dcCallVoid(vm, addr);
			ret->SetReturnPtr(CreateArrayT<uint32_t>(*(std::vector<uint32_t>*)args[0], mono_get_uint32_class));
			break;
		}
		case ValueType::ArrayUint64: {
			dcCallVoid(vm, addr);
			ret->SetReturnPtr(CreateArrayT<uint64_t>(*(std::vector<uint64_t>*)args[0], mono_get_uint64_class));
			break;
		}
		case ValueType::ArrayPtr64: {
			dcCallVoid(vm, addr);
			ret->SetReturnPtr(CreateArrayT<uintptr_t>(*(std::vector<uintptr_t>*)args[0], mono_get_uintptr_class));
			break;
		}
		case ValueType::ArrayFloat: {
			dcCallVoid(vm, addr);
			ret->SetReturnPtr(CreateArrayT<float>(*(std::vector<float>*)args[0], mono_get_single_class));
			break;
		}
		case ValueType::ArrayDouble: {
			dcCallVoid(vm, addr);
			ret->SetReturnPtr(CreateArrayT<double>(*(std::vector<double>*)args[0], mono_get_double_class));
			break;
		}
		case ValueType::ArrayString: {
			dcCallVoid(vm, addr);
			ret->SetReturnPtr(g_csharplm.CreateStringArray(*(std::vector<std::string>*)args[0]));
			break;
		}
	}

	// Pull back references into provided arguments

	if (hasRefs) {
		j = hasRet; // skip first param if has return

		if (j < args.size()) {
			for (uint8_t i = 0; i < count; ++i) {
				auto& param = method->paramTypes[i];
				if (param.ref) {
					switch (param.type) {
						case ValueType::String: {
							p->SetArgument(i, g_csharplm.CreateString(*(std::string*)args[j++]));
							break;
						}
						case ValueType::ArrayBool: {
							p->SetArgument(i, CreateArrayT<bool>(*(std::vector<bool>*)args[j++], mono_get_char_class));
							break;
						}
						case ValueType::ArrayChar8: {
							p->SetArgument(i, CreateArrayT<char>(*(std::vector<char>*)args[j++], mono_get_char_class));
							break;
						}
						case ValueType::ArrayChar16: {
							p->SetArgument(i, CreateArrayT<wchar_t>(*(std::vector<wchar_t>*)args[j++], mono_get_int16_class));
							break;
						}
						case ValueType::ArrayInt8: {
							p->SetArgument(i, CreateArrayT<int8_t>(*(std::vector<int8_t>*)args[j++], mono_get_sbyte_class));
							break;
						}
						case ValueType::ArrayInt16: {
							p->SetArgument(i, CreateArrayT<int16_t>(*(std::vector<int16_t>*)args[j++], mono_get_int16_class));
							break;
						}
						case ValueType::ArrayInt32: {
							p->SetArgument(i, CreateArrayT<int32_t>(*(std::vector<int32_t>*)args[j++], mono_get_int32_class));
							break;
						}
						case ValueType::ArrayInt64: {
							p->SetArgument(i, CreateArrayT<int64_t>(*(std::vector<int64_t>*)args[j++], mono_get_int64_class));
							break;
						}
						case ValueType::ArrayUint8: {
							p->SetArgument(i, CreateArrayT<uint8_t>(*(std::vector<uint8_t>*)args[j++], mono_get_byte_class));
							break;
						}
						case ValueType::ArrayUint16: {
							p->SetArgument(i, CreateArrayT<uint16_t>(*(std::vector<uint16_t>*)args[j++], mono_get_uint16_class));
							break;
						}
						case ValueType::ArrayUint32: {
							p->SetArgument(i, CreateArrayT<uint32_t>(*(std::vector<uint32_t>*)args[j++], mono_get_uint32_class));
							break;
						}
						case ValueType::ArrayUint64: {
							p->SetArgument(i, CreateArrayT<uint64_t>(*(std::vector<uint64_t>*)args[j++], mono_get_uint64_class));
							break;
						}
						case ValueType::ArrayPtr64: {
							p->SetArgument(i, CreateArrayT<uintptr_t>(*(std::vector<uintptr_t>*)args[j++], mono_get_uintptr_class));
							break;
						}
						case ValueType::ArrayFloat: {
							p->SetArgument(i, CreateArrayT<float>(*(std::vector<float>*)args[j++], mono_get_single_class));
							break;
						}
						case ValueType::ArrayDouble: {
							p->SetArgument(i, CreateArrayT<double>(*(std::vector<double>*)args[j++], mono_get_double_class));
							break;
						}
						case ValueType::ArrayString: {
							p->SetArgument(i, g_csharplm.CreateStringArray(*(std::vector<std::string>*)args[j++]));
							break;
						}
						default:
							break;
					}
				}
				if (j == args.size())
					break;
			}
		}
	}

	if (!args.empty()) {
		j = 0;

		if (hasRet) {
			DeleteParam(args, j, returnType);
		}

		if (j < args.size()) {
			for (uint8_t i = 0; i < count; ++i) {
				auto& param = method->paramTypes[i];
				DeleteParam(args, j, param.type);
				if (j == args.size())
					break;
			}
		}
	}

	dcFree(vm);
}

// Call from C++ to C#
void CSharpLanguageModule::InternalCall(const Method* method, void* data, const Parameters* p, uint8_t count, const ReturnValue* ret) {
	const auto& [monoMethod, monoObject] = *reinterpret_cast<ExportMethod*>(data);

	/// We not create param vector, and use Parameters* params directly if passing primitives
	std::vector<void*> args;
	args.reserve(count);
	bool hasRefs = false;
	bool hasRet = method->retType.type >= ValueType::String;

	std::vector<void> args(hasRet ? count - 1 : count);

	for (uint8_t i = hasRet, j = 0; i < count; ++i) {
		auto& param = method->paramTypes[j];
		hasRefs |= param.ref;
		switch (param.type) {
			case ValueType::Invalid:
			case ValueType::Void:
				// Should not trigger!
				break;
			case ValueType::Bool:
			case ValueType::Char8:
			case ValueType::Char16:
			case ValueType::Int8:
			case ValueType::Int16:
			case ValueType::Int32:
			case ValueType::Int64:
			case ValueType::Uint8:
			case ValueType::Uint16:
			case ValueType::Uint32:
			case ValueType::Uint64:
			case ValueType::Ptr64:
			case ValueType::Float:
			case ValueType::Double:
			case ValueType::Function:
				args[j] = p->GetArgumentPtr(i);
				break;
			case ValueType::String: {
				auto source = p->GetArgument<std::string*>(i);
				args[j] = source != nullptr ? g_csharplm.CreateString(*source) : nullptr;
				break;
			}
			case ValueType::ArrayBool: {
				auto source = p->GetArgument<std::vector<bool>*>(i);
				args[j] = source != nullptr ? CreateArrayT<bool>(*source, mono_get_char_class) : nullptr;
				break;
			}
			case ValueType::ArrayChar8: {
				auto source = p->GetArgument<std::vector<char>*>(i);
				args[j] = source != nullptr ? CreateArrayT<char>(*source, mono_get_char_class) : nullptr;
				break;
			}
			case ValueType::ArrayChar16: {
				auto source = p->GetArgument<std::vector<char16_t>*>(i);
				args[j] = source != nullptr ? CreateArrayT<char16_t>(*source, mono_get_int16_class) : nullptr;
				break;
			}
			case ValueType::ArrayInt8: {
				auto source = p->GetArgument<std::vector<int8_t>*>(i);
				args[j] = source != nullptr ? CreateArrayT<int8_t>(*source, mono_get_sbyte_class) : nullptr;
				break;
			}
			case ValueType::ArrayInt16: {
				auto source = p->GetArgument<std::vector<int16_t>*>(i);
				args[j] = source != nullptr ? CreateArrayT<int16_t>(*source, mono_get_int16_class) : nullptr;
				break;
			}
			case ValueType::ArrayInt32: {
				auto source = p->GetArgument<std::vector<int32_t>*>(i);
				args[j] = source != nullptr ? CreateArrayT<int32_t>(*source, mono_get_int32_class) : nullptr;
				break;
			}
			case ValueType::ArrayInt64: {
				auto source = p->GetArgument<std::vector<int64_t>*>(i);
				args[j] = source != nullptr ? CreateArrayT<int64_t>(*source, mono_get_int64_class) : nullptr;
				break;
			}
			case ValueType::ArrayUint8: {
				auto source = p->GetArgument<std::vector<uint8_t>*>(i);
				args[j] = source != nullptr ? CreateArrayT<uint8_t>(*source, mono_get_byte_class) : nullptr;
				break;
			}
			case ValueType::ArrayUint16: {
				auto source = p->GetArgument<std::vector<uint16_t>*>(i);
				args[j] = source != nullptr ? CreateArrayT<uint16_t>(*source, mono_get_uint16_class) : nullptr;
				break;
			}
			case ValueType::ArrayUint32: {
				auto source = p->GetArgument<std::vector<uint32_t>*>(i);
				args[j] = source != nullptr ? CreateArrayT<uint32_t>(*source, mono_get_uint32_class) : nullptr;
				break;
			}
			case ValueType::ArrayUint64: {
				auto source = p->GetArgument<std::vector<uint64_t>*>(i);
				args[j] = source != nullptr ? CreateArrayT<uint64_t>(*source, mono_get_uint64_class) : nullptr;
				break;
			}
			case ValueType::ArrayPtr64: {
				auto source = p->GetArgument<std::vector<uintptr_t>*>(i);
				args[j] = source != nullptr ? CreateArrayT<uintptr_t>(*source, mono_get_uintptr_class) : nullptr;
				break;
			}
			case ValueType::ArrayFloat: {
				auto source = p->GetArgument<std::vector<float>*>(i);
				args[j] = source != nullptr ? CreateArrayT<float>(*source, mono_get_single_class) : nullptr;
				break;
			}
			case ValueType::ArrayDouble: {
				auto source = p->GetArgument<std::vector<double>*>(i);
				args[j] = source != nullptr ? CreateArrayT<double>(*source, mono_get_double_class) : nullptr;
				break;
			}
			case ValueType::ArrayString: {
				auto source = p->GetArgument<std::vector<std::string>*>(i);
				args[j] = source != nullptr ? g_csharplm.CreateStringArray(*source) : nullptr;
				break;
			}
		}
		++j;
	}

	MonoObject* exception = nullptr;
	MonoObject* result = mono_runtime_invoke(monoMethod, monoObject, args.data(), &exception);
	if (exception) {
		HandleException(exception, nullptr);
		ret->SetReturnPtr<uintptr_t>({});
		return;
	}

	if (hasRefs) {
		for (uint8_t i = hasRet, j = 0; i < count; ++i) {
			auto& param = method->paramTypes[j];
			if (param.ref) {
				switch (param.type) {
					case ValueType::String: {
						auto source = (MonoString*) args[j];
						if (source != nullptr)  {
							auto dest = p->GetArgument<std::string*>(i);
							*dest = utils::MonoStringToString(source);
						}
						break;
					}
					case ValueType::ArrayBool: {
						auto source = (MonoArray*) args[j];
						if (source != nullptr) {
							auto dest = p->GetArgument<std::vector<bool>*>(i);
							utils::MonoArrayToVector(source, *dest);
						}
						break;
					}
					case ValueType::ArrayChar8: {
						auto source = (MonoArray*) args[j];
						if (source != nullptr) {
							auto dest = p->GetArgument<std::vector<char>*>(i);
							utils::MonoArrayToVector(source, *dest);
						}
						break;
					}
					case ValueType::ArrayChar16: {
						auto source = (MonoArray*) args[j];
						if (source != nullptr) {
							auto dest = p->GetArgument<std::vector<wchar_t>*>(i);
							utils::MonoArrayToVector(source, *dest);
						}
						break;
					}
					case ValueType::ArrayInt8: {
						auto source = (MonoArray*) args[j];
						if (source != nullptr) {
							auto dest = p->GetArgument<std::vector<int8_t>*>(i);
							utils::MonoArrayToVector(source, *dest);
						}
						break;
					}
					case ValueType::ArrayInt16: {
						auto source = (MonoArray*) args[j];
						if (source != nullptr) {
							auto dest = p->GetArgument<std::vector<int16_t>*>(i);
							utils::MonoArrayToVector(source, *dest);
						}
						break;
					}
					case ValueType::ArrayInt32: {
						auto source = (MonoArray*) args[j];
						if (source != nullptr) {
							auto dest = p->GetArgument<std::vector<int32_t>*>(i);
							utils::MonoArrayToVector(source, *dest);
						}
						break;
					}
					case ValueType::ArrayInt64: {
						auto source = (MonoArray*) args[j];
						if (source != nullptr) {
							auto dest = p->GetArgument<std::vector<int64_t>*>(i);
							utils::MonoArrayToVector(source, *dest);
						}
						break;
					}
					case ValueType::ArrayUint8: {
						auto source = (MonoArray*) args[j];
						if (source != nullptr) {
							auto dest = p->GetArgument<std::vector<uint8_t>*>(i);
							utils::MonoArrayToVector(source, *dest);
						}
						break;
					}
					case ValueType::ArrayUint16: {
						auto source = (MonoArray*) args[j];
						if (source != nullptr) {
							auto dest = p->GetArgument<std::vector<uint16_t>*>(i);
							utils::MonoArrayToVector(source, *dest);
						}
						break;
					}
					case ValueType::ArrayUint32: {
						auto source = (MonoArray*) args[j];
						if (source != nullptr) {
							auto dest = p->GetArgument<std::vector<uint32_t>*>(i);
							utils::MonoArrayToVector(source, *dest);
						}
						break;
					}
					case ValueType::ArrayUint64: {
						auto source = (MonoArray*) args[j];
						if (source != nullptr) {
							auto dest = p->GetArgument<std::vector<uint64_t>*>(i);
							utils::MonoArrayToVector(source, *dest);
						}
						break;
					}
					case ValueType::ArrayPtr64: {
						auto source = (MonoArray*) args[j];
						if (source != nullptr) {
							auto dest = p->GetArgument<std::vector<uintptr_t>*>(i);
							utils::MonoArrayToVector(source, *dest);
						}
						break;
					}
					case ValueType::ArrayFloat: {
						auto source = (MonoArray*) args[j];
						if (source != nullptr) {
							auto dest = p->GetArgument<std::vector<float>*>(i);
							utils::MonoArrayToVector(source, *dest);
						}
						break;
					}
					case ValueType::ArrayDouble: {
						auto source = (MonoArray*) args[j];
						if (source != nullptr) {
							auto dest = p->GetArgument<std::vector<double>*>(i);
							utils::MonoArrayToVector(source, *dest);
						}
						break;
					}
					case ValueType::ArrayString: {
						auto source = (MonoArray*) args[j];
						if (source != nullptr) {
							auto dest = p->GetArgument<std::vector<std::string>*>(i);
							utils::MonoArrayToVector(source, *dest);
						}
						break;
					}
					default:
						break;
				}
			}
			++j;
		}
	}

	switch (method->retType.type) {
		case ValueType::Invalid:
		case ValueType::Void:
			break;
		case ValueType::Bool: {
			bool val = *(bool*) mono_object_unbox(result);
			ret->SetReturnPtr<bool>(val);
			break;
		}
		case ValueType::Char8: {
			char val = *(char*) mono_object_unbox(result);
			ret->SetReturnPtr<char>(val);
			break;
		}
		case ValueType::Char16: {
			wchar_t val = *(wchar_t*) mono_object_unbox(result);
			ret->SetReturnPtr<wchar_t>(val);
			break;
		}
		case ValueType::Int8: {
			int8_t val = *(int8_t*) mono_object_unbox(result);
			ret->SetReturnPtr<int8_t>(val);
			break;
		}
		case ValueType::Int16: {
			int16_t val = *(int16_t*) mono_object_unbox(result);
			ret->SetReturnPtr<int16_t>(val);
			break;
		}
		case ValueType::Int32: {
			int32_t val = *(int32_t*) mono_object_unbox(result);
			ret->SetReturnPtr<int32_t>(val);
			break;
		}
		case ValueType::Int64: {
			int64_t val = *(int64_t*) mono_object_unbox(result);
			ret->SetReturnPtr<int64_t>(val);
			break;
		}
		case ValueType::Uint8: {
			uint8_t val = *(uint8_t*) mono_object_unbox(result);
			ret->SetReturnPtr<uint8_t>(val);
			break;
		}
		case ValueType::Uint16: {
			uint16_t val = *(uint16_t*) mono_object_unbox(result);
			ret->SetReturnPtr<uint16_t>(val);
			break;
		}
		case ValueType::Uint32: {
			uint32_t val = *(uint32_t*) mono_object_unbox(result);
			ret->SetReturnPtr<uint32_t>(val);
			break;
		}
		case ValueType::Uint64: {
			uint64_t val = *(uint64_t*) mono_object_unbox(result);
			ret->SetReturnPtr<uint64_t>(val);
			break;
		}
		case ValueType::Function:
		case ValueType::Ptr64: {
			uintptr_t val = *(uintptr_t*) mono_object_unbox(result);
			ret->SetReturnPtr<uintptr_t>(val);
			break;
		}
		case ValueType::Float: {
			float val = *(float*) mono_object_unbox(result);
			ret->SetReturnPtr<float>(val);
			break;
		}
		case ValueType::Double: {
			double val = *(double*) mono_object_unbox(result);
			ret->SetReturnPtr<double>(val);
			break;
		}
		case ValueType::String: {
			MonoString* source = (MonoString*) result;
			if (source != nullptr) {
				auto dest = p->GetArgument<std::string*>(0);
				*dest = utils::MonoStringToString(source);
			}
			break;
		}
		case ValueType::ArrayBool: {
			MonoArray* source = (MonoArray*) result;
			if (source != nullptr) {
				auto dest = p->GetArgument<std::vector<bool>*>(0);
				utils::MonoArrayToVector(source, *dest);
			}
			break;
		}
		case ValueType::ArrayChar8: {
			MonoArray* source = (MonoArray*) result;
			if (source != nullptr) {
				auto dest = p->GetArgument<std::vector<char>*>(0);
				utils::MonoArrayToVector(source, *dest);
			}
			break;
		}
		case ValueType::ArrayChar16: {
			MonoArray* source = (MonoArray*) result;
			if (source != nullptr) {
				auto dest = p->GetArgument<std::vector<wchar_t>*>(0);
				utils::MonoArrayToVector(source, *dest);
			}
			break;
		}
		case ValueType::ArrayInt8: {
			MonoArray* source = (MonoArray*) result;
			if (source != nullptr) {
				auto dest = p->GetArgument<std::vector<int8_t>*>(0);
				utils::MonoArrayToVector(source, *dest);
			}
			break;
		}
		case ValueType::ArrayInt16: {
			MonoArray* source = (MonoArray*) result;
			if (source != nullptr) {
				auto dest = p->GetArgument<std::vector<int16_t>*>(0);
				utils::MonoArrayToVector(source, *dest);
			}
			break;
		}
		case ValueType::ArrayInt32: {
			MonoArray* source = (MonoArray*) result;
			if (source != nullptr) {
				auto dest = p->GetArgument<std::vector<int32_t>*>(0);
				utils::MonoArrayToVector(source, *dest);
			}
			break;
		}
		case ValueType::ArrayInt64: {
			MonoArray* source = (MonoArray*) result;
			if (source != nullptr) {
				auto dest = p->GetArgument<std::vector<int64_t>*>(0);
				utils::MonoArrayToVector(source, *dest);
			}
			break;
		}
		case ValueType::ArrayUint8: {
			MonoArray* source = (MonoArray*) result;
			if (source != nullptr) {
				auto dest = p->GetArgument<std::vector<uint8_t>*>(0);
				utils::MonoArrayToVector(source, *dest);
			}
			break;
		}
		case ValueType::ArrayUint16: {
			MonoArray* source = (MonoArray*) result;
			if (source != nullptr) {
				auto dest = p->GetArgument<std::vector<uint16_t>*>(0);
				utils::MonoArrayToVector(source, *dest);
			}
			break;
		}
		case ValueType::ArrayUint32: {
			MonoArray* source = (MonoArray*) result;
			if (source != nullptr) {
				auto dest = p->GetArgument<std::vector<uint32_t>*>(0);
				utils::MonoArrayToVector(source, *dest);
			}
			break;
		}
		case ValueType::ArrayUint64: {
			MonoArray* source = (MonoArray*) result;
			if (source != nullptr) {
				auto dest = p->GetArgument<std::vector<uint64_t>*>(0);
				utils::MonoArrayToVector(source, *dest);
			}
			break;
		}
		case ValueType::ArrayPtr64: {
			MonoArray* source = (MonoArray*) result;
			if (source != nullptr) {
				auto dest = p->GetArgument<std::vector<uintptr_t>*>(0);
				utils::MonoArrayToVector(source, *dest);
			}
			break;
		}
		case ValueType::ArrayFloat: {
			MonoArray* source = (MonoArray*) result;
			if (source != nullptr) {
				auto dest = p->GetArgument<std::vector<float>*>(0);
				utils::MonoArrayToVector(source, *dest);
			}
			break;
		}
		case ValueType::ArrayDouble: {
			MonoArray* source = (MonoArray*) result;
			if (source != nullptr) {
				auto dest = p->GetArgument<std::vector<double>*>(0);
				utils::MonoArrayToVector(source, *dest);
			}
			break;
		}
		case ValueType::ArrayString: {
			MonoArray* source = (MonoArray*) result;
			if (source != nullptr) {
				auto dest = p->GetArgument<std::vector<std::string>*>(0);
				utils::MonoArrayToVector(source, *dest);
			}
			break;
		}
	}
}

LoadResult CSharpLanguageModule::OnPluginLoad(const IPlugin& plugin) {
	MonoImageOpenStatus status = MONO_IMAGE_IMAGE_INVALID;
	MonoAssembly* assembly = utils::LoadMonoAssembly(plugin.GetBaseDir() / plugin.GetDescriptor().entryPoint, _config.enableDebugging, status);
	if (!assembly)
		return ErrorData{ std::format("Failed to load assembly: '{}'",mono_image_strerror(status)) };

	MonoImage* image = mono_assembly_get_image(assembly);
	if (!image)
		return ErrorData{ std::format("Failed to load assembly image") };

	auto scriptRef = CreateScriptInstance(plugin, image);
	if (!scriptRef.has_value())
		return ErrorData{ std::format("Failed to find 'Plugin' class implementation") };
	auto& script = scriptRef->get();

	std::vector<std::string> methodErrors;

	const auto& exportedMethods = plugin.GetDescriptor().exportedMethods;
	std::vector<MethodData> methods;
	methods.reserve(exportedMethods.size());

	for (const auto& method : exportedMethods) {
		auto seperated = utils::Split(method.funcName, ".");
		if (seperated.size() != 4) {
			methodErrors.emplace_back(std::format("Invalid function name: '{}'. Please provide name in that format: 'Plugin.Namespace.Class.Method'", method.funcName));
			continue;
		}

		std::string nameSpace(seperated[1]);
		std::string className(seperated[2]);
		std::string methodName(seperated[3]);

		MonoClass* monoClass = mono_class_from_name(image, nameSpace.c_str(), className.c_str());
		if (!monoClass) {
			methodErrors.emplace_back(std::format("Failed to find class '{}.{}'", nameSpace, className));
			continue;
		}
		
		MonoMethod* monoMethod = mono_class_get_method_from_name(monoClass, methodName.c_str(), -1);
		if (!monoMethod) {
			methodErrors.emplace_back(std::format("Failed to find method '{}.{}::{}'", nameSpace, className, methodName));
			continue;
		}
		
		MonoObject* monoInstance = monoClass == script._klass ? script._instance : nullptr;

		void* methodAddr = ValidateMethod(method, methodErrors, monoInstance, monoMethod, nameSpace.c_str(), className.c_str(), methodName.c_str());
		if (methodAddr) {
			methods.emplace_back(method.name, methodAddr);
		}
	}

	if (!methodErrors.empty()) {
		std::string funcs(methodErrors[0]);
		for (auto it = std::next(methodErrors.begin()); it != methodErrors.end(); ++it) {
			std::format_to(std::back_inserter(funcs), ", {}", *it);
		}
		return ErrorData{ std::move(funcs) };
	}

	return LoadResultData{ std::move(methods) };
}

void CSharpLanguageModule::OnMethodExport(const IPlugin& plugin) {
	for (const auto& [name, addr] : plugin.GetMethods()) {
		auto funcName = std::format("{}.{}::{}", plugin.GetName(), plugin.GetName(), name);

		if (_importMethods.contains(funcName)) {
			_provider->Log(std::format("[csharplm] Method name duplicate: {}", funcName), Severity::Error);
			continue;
		}

		for (const auto& method : plugin.GetDescriptor().exportedMethods) {
			if (name == method.name) {

				Function function(_rt);
				void* methodAddr = function.GetJitFunc(method, &ExternalCall, addr);
				if (!methodAddr) {
					_provider->Log(std::format("[csharplm] Method JIT generation error: {}", function.GetError()), Severity::Error);
					continue;
				}
				_functions.emplace_back(std::move(function));
				
				mono_add_internal_call(funcName.c_str(), methodAddr);
				
				_importMethods.emplace(std::move(funcName), ImportMethod{method, addr});
				break;
			}
		}

		//mono_add_internal_call(funcName.c_str(), addr);
	}
}

void CSharpLanguageModule::OnPluginStart(const IPlugin& plugin) {
	ScriptOpt scriptRef = FindScript(plugin.GetName());
	if (scriptRef.has_value()) {
		auto& script = scriptRef->get();

		if (_config.subscribeFeature) {
			std::vector<std::string> methodErrors;

			MonoClass* subscribeClass = mono_class_from_name(_coreImage, "Plugify", "SubscribeAttribute");

			const MonoTableInfo* typeDefinitionsTable = mono_image_get_table_info(script._image, MONO_TABLE_TYPEDEF);
			int numTypes = mono_table_info_get_rows(typeDefinitionsTable);

			for (int i = 0; i < numTypes; ++i) {
				uint32_t cols[MONO_TYPEDEF_SIZE];
				mono_metadata_decode_row(typeDefinitionsTable, i, cols, MONO_TYPEDEF_SIZE);

				const char* nameSpace = mono_metadata_string_heap(script._image, cols[MONO_TYPEDEF_NAMESPACE]);
				const char* className = mono_metadata_string_heap(script._image, cols[MONO_TYPEDEF_NAME]);

				MonoClass* monoClass = mono_class_from_name(script._image, nameSpace, className);
				MonoObject* monoInstance = monoClass == script._klass ? script._instance : nullptr;

				MonoMethod* monoMethod;
				void* iter = nullptr;
				while ((monoMethod = mono_class_get_methods(monoClass, &iter)) != nullptr) {
					const char* methodName = mono_method_get_name(monoMethod);
					MonoCustomAttrInfo* attributes = mono_custom_attrs_from_method(monoMethod);
					if (attributes) {
						for (int j = 0; j < attributes->num_attrs; ++j) {
							MonoCustomAttrEntry& entry = attributes->attrs[j];
							if (subscribeClass != mono_method_get_class(entry.ctor))
								continue;

							std::string methodToFind;

							MonoObject* instance = mono_custom_attrs_get_attr(attributes, subscribeClass);
							void* iterator = nullptr;
							while (MonoClassField* field = mono_class_get_fields(subscribeClass, &iterator)) {
								const char* fieldName = mono_field_get_name(field);
								MonoObject* fieldValue = mono_field_get_value_object(_appDomain, field, instance);
								if (!strcmp(fieldName, "_method")) {
									methodToFind = utils::MonoStringToString((MonoString*) fieldValue);
									break;
								}
							}

							auto it = _importMethods.find(methodToFind);
							if (it != _importMethods.end()) {
								const auto& [ref, addr] = it->second;
								auto& method = ref.get();
								if (method.paramTypes.size() != 1) {
									methodErrors.emplace_back(std::format("Destination method '{}' should have only 1 argument to subscribe", methodToFind));
									continue;
								}

								const auto& [type, _, prototype] = *method.paramTypes.begin();
								if (type != ValueType::Function) {
									methodErrors.emplace_back(std::format("Parameter at index '1' of destination method '{}' should be 'function' type. Current type '{}' not supported", methodToFind, ValueTypeToString(type)));
									continue;
								}

								if (!prototype) {
									methodErrors.emplace_back(std::format("Could not subscribe to destination method '{}' which does not have prototype information", methodToFind));
									continue;
								}

								// Generate a new method with same prototype
								auto newMethod = std::make_unique<Method>(
									methodName,
									std::format("{}.{}.{}.{}", plugin.GetName(), nameSpace, className, methodName),
									prototype->callConv,
									prototype->paramTypes,
									prototype->retType,
									prototype->varIndex
								);

								void* methodAddr = ValidateMethod(*newMethod, methodErrors, monoInstance, monoMethod, nameSpace, className, methodName);
								if (methodAddr) {
									using RegisterCallbackFn = void(*)(void*);
									auto func = reinterpret_cast<RegisterCallbackFn>(addr);
									func(methodAddr);
									_methods.emplace_back(std::move(newMethod));
								}
							} else {
								methodErrors.emplace_back(std::format("Failed to find destination method '{}' to subscribe", methodToFind));
							}
							break;
						}
						mono_custom_attrs_free(attributes);
					}
				}
			}

			if (!methodErrors.empty()) {
				std::string funcs(methodErrors[0]);
				for (auto it = std::next(methodErrors.begin()); it != methodErrors.end(); ++it) {
					std::format_to(std::back_inserter(funcs), ", {}", *it);
				}
				_provider->Log(std::format("[csharplm] Plugin '{}' has problems related to subscribe method(s): {}", plugin.GetName(), funcs), Severity::Warning);
			}
		}

		script.InvokeOnStart();
	}
}

void CSharpLanguageModule::OnPluginEnd(const IPlugin& plugin) {
	ScriptOpt scriptRef = FindScript(plugin.GetName());
	if (scriptRef.has_value()) {
		auto& script = scriptRef->get();

		script.InvokeOnEnd();
	}
}

ScriptOpt CSharpLanguageModule::CreateScriptInstance(const IPlugin& plugin, MonoImage* image) {
	MonoClass* pluginClass = mono_class_from_name(_coreImage, "Plugify", "Plugin");

	const MonoTableInfo* typeDefinitionsTable = mono_image_get_table_info(image, MONO_TABLE_TYPEDEF);
	int numTypes = mono_table_info_get_rows(typeDefinitionsTable);

	for (int i = 0; i < numTypes; ++i) {
		uint32_t cols[MONO_TYPEDEF_SIZE];
		mono_metadata_decode_row(typeDefinitionsTable, i, cols, MONO_TYPEDEF_SIZE);

		const char* nameSpace = mono_metadata_string_heap(image, cols[MONO_TYPEDEF_NAMESPACE]);
		const char* className = mono_metadata_string_heap(image, cols[MONO_TYPEDEF_NAME]);

		MonoClass* monoClass = mono_class_from_name(image, nameSpace, className);
		if (monoClass == pluginClass)
			continue;

		bool isPlugin = mono_class_is_subclass_of(monoClass, pluginClass, false);
		if (!isPlugin)
			continue;

		const auto [it, result] = _scripts.try_emplace(plugin.GetName(), ScriptInstance(plugin, image, monoClass));
		if (result)
			return std::get<ScriptInstance>(*it);
	}

	return std::nullopt;
}

void* CSharpLanguageModule::ValidateMethod(const plugify::Method& method, std::vector<std::string>& methodErrors, MonoObject* monoInstance, MonoMethod* monoMethod, const char* nameSpace, const char* className, const char* methodName) {
	uint32_t methodFlags = mono_method_get_flags(monoMethod, nullptr);
	if (!(methodFlags & MONO_METHOD_ATTR_STATIC) && !monoInstance) {
		methodErrors.emplace_back(std::format("Method '{}.{}::{}' is not static", nameSpace, className, methodName));
		return nullptr;
	}

	MonoMethodSignature* sig = mono_method_signature(monoMethod);

	uint32_t paramCount = mono_signature_get_param_count(sig);
	if (paramCount != method.paramTypes.size()) {
		methodErrors.emplace_back(std::format("Invalid parameter count {} when it should have {}", method.paramTypes.size(), paramCount));
		return nullptr;
	}

	char* returnTypeName = mono_type_get_name(mono_signature_get_return_type(sig));
	ValueType returnType = utils::MonoTypeToValueType(returnTypeName);
	if (returnType == ValueType::Invalid) {
		methodErrors.emplace_back(std::format("Return of method '{}.{}::{}' not supported '{}'", nameSpace, className, methodName, returnTypeName));
		return nullptr;
	}

	if (method.retType.type == ValueType::Function && returnType == ValueType::Ptr64) {
		returnType = ValueType::Function; // special case
	}

	if (returnType != method.retType.type) {
		methodErrors.emplace_back(std::format("Method '{}.{}::{}' has invalid return type '{}' when it should have '{}'", nameSpace, className, methodName, ValueTypeToString(method.retType.type), ValueTypeToString(returnType)));
		return nullptr;
	}

	size_t i = 0;
	void* iter = nullptr;
	while (MonoType* type = mono_signature_get_params(sig, &iter)) {
		char* paramTypeName = mono_type_get_name(type);
		ValueType paramType = utils::MonoTypeToValueType(paramTypeName);
		if (paramType == ValueType::Invalid) {
			methodErrors.emplace_back(std::format("Parameter at index '{}' of method '{}.{}::{}' not supported '{}'", i, nameSpace, className, methodName, paramTypeName));
			continue;
		}

		if (method.paramTypes[i].type == ValueType::Function && paramType == ValueType::Ptr64) {
			paramType = ValueType::Function; // special case
		}

		if (paramType != method.paramTypes[i].type) {
			methodErrors.emplace_back(std::format("Method '{}.{}::{}' has invalid param type '{}' at index {} when it should have '{}'", nameSpace, className, methodName, ValueTypeToString(method.paramTypes[i].type), i, ValueTypeToString(paramType)));
			continue;
		}

		i++;
	}

	if (!methodErrors.empty())
		return nullptr;

	auto& exportMethod = _exportMethods.emplace_back(std::make_unique<ExportMethod>(monoMethod, monoInstance));

	Function function(_rt);
	void* methodAddr = function.GetJitFunc(method, &InternalCall, exportMethod.get());
	if (!methodAddr) {
		methodErrors.emplace_back(std::format("Method JIT generation error: ", function.GetError()));
		return nullptr;
	}
	_functions.emplace_back(std::move(function));
	return methodAddr;
};


ScriptOpt CSharpLanguageModule::FindScript(const std::string& name) {
	auto it = _scripts.find(name);
	if (it != _scripts.end())
		return std::get<ScriptInstance>(*it);
	return std::nullopt;
}

MonoString* CSharpLanguageModule::CreateString(const std::string& source) const {
	return source.empty() ? mono_string_empty(_appDomain) : mono_string_new(_appDomain, source.c_str());
}

MonoArray* CSharpLanguageModule::CreateArray(MonoClass* klass, size_t count) const {
	return mono_array_new(_appDomain, klass, count);
}

MonoArray* CSharpLanguageModule::CreateStringArray(const std::vector<std::string>& source) const {
	MonoArray* array = CreateArray(mono_get_string_class(), source.size());
	for (size_t i = 0; i < source.size(); ++i) {
		mono_array_set(array, MonoString*, i, CreateString(source[i]));
	}
	return array;
}

MonoObject* CSharpLanguageModule::InstantiateClass(MonoClass* klass) const {
	MonoObject* instance = mono_object_new(_appDomain, klass);
	mono_runtime_object_init(instance);
	return instance;
}

void CSharpLanguageModule::HandleException(MonoObject* exc, void* /* userData*/) {
	if (!exc || !g_csharplm._provider)
		return;

	MonoClass* exceptionClass = mono_object_get_class(exc);

	std::string result("[csharplm] [Exception] ");

	std::string message = utils::GetStringProperty("Message", exceptionClass, exc);
	if (!message.empty()) {
		std::format_to(std::back_inserter(result), " | Message: {}", message);
	}
	
	std::string source = utils::GetStringProperty("Source", exceptionClass, exc);
	if (!source.empty()) {
		std::format_to(std::back_inserter(result), " | Source: {}", source);
	}
	
	std::string stackTrace = utils::GetStringProperty("StackTrace", exceptionClass, exc);
	if (!stackTrace.empty()) {
		std::format_to(std::back_inserter(result), " | StackTrace: {}", stackTrace);
	}
	
	std::string targetSite = utils::GetStringProperty("TargetSite", exceptionClass, exc);
	if (!targetSite.empty()) {
		std::format_to(std::back_inserter(result), " | TargetSite: {}", targetSite);
	}
	
	g_csharplm._provider->Log(result, Severity::Error);
}

void CSharpLanguageModule::OnLogCallback(const char* logDomain, const char* logLevel, const char* message, mono_bool fatal, void* /* userData*/) {
	if (!g_csharplm._provider)
		return;

	Severity severity = Severity::None;
	if (logLevel != nullptr) {
		switch (std::tolower(logLevel[0], std::locale{})) {
			case 'e': // "error"
				severity = Severity::Error;
				break;
			case 'c': // "critical"
				severity = Severity::Fatal;
				break;
			case 'w': // "warning"
				severity = Severity::Warning;
				break;
			case 'm': // "message"
				severity = Severity::Verbose;
				break;
			case 'i': // "info"
				severity = Severity::Info;
				break;
			case 'd': // "debug"
				severity = Severity::Debug;
				break;
			default:
				break;
		}
	}

	if (!logDomain || strlen(logDomain) == 0) {
		g_csharplm._provider->Log(std::format("[csharplm] {}", message), fatal ? Severity::Fatal : severity);
	} else {
		g_csharplm._provider->Log(std::format("[csharplm] [{}] {}", logDomain, message), fatal ? Severity::Fatal : severity);
	}
}

void CSharpLanguageModule::OnPrintCallback(const char* message, mono_bool /*isStdout*/) {
	if (g_csharplm._provider)
		g_csharplm._provider->Log(std::format("[csharplm] {}", message), Severity::Warning);
}

void CSharpLanguageModule::OnPrintErrorCallback(const char* message, mono_bool /*isStdout*/) {
	if (g_csharplm._provider)
		g_csharplm._provider->Log(std::format("[csharplm] {}", message), Severity::Error);
}

/*_________________________________________________*/

ScriptInstance::ScriptInstance(const IPlugin& plugin, MonoImage* image, MonoClass* klass) : _image{image}, _klass{klass} {
	_instance = g_csharplm.InstantiateClass(_klass);

	// Call Script (base) constructor
	{
		MonoClass* pluginClass = mono_class_from_name(g_csharplm._coreImage, "Plugify", "Plugin");
		MonoMethod* constructor = mono_class_get_method_from_name(pluginClass, ".ctor", 8);
		
		const auto& desc = plugin.GetDescriptor();
		auto id = plugin.GetId();
		std::vector<std::string> deps;
		deps.reserve(desc.dependencies.size());
		for (const auto& dependency : desc.dependencies) {
			deps.emplace_back(dependency.name);
		}
		std::array<void*, 8> args {
			&id,
			g_csharplm.CreateString(plugin.GetName()),
			g_csharplm.CreateString(plugin.GetFriendlyName()),
			g_csharplm.CreateString(desc.friendlyName),
			g_csharplm.CreateString(desc.versionName),
			g_csharplm.CreateString(desc.createdBy),
			g_csharplm.CreateString(desc.createdByURL),
			g_csharplm.CreateStringArray(deps),
		};
		mono_runtime_invoke(constructor, _instance, args.data(), nullptr);
	}

	_onStartMethod = mono_class_get_method_from_name(_klass, "OnStart", 0);
	_onEndMethod  = mono_class_get_method_from_name(_klass, "OnEnd", 0);
}

ScriptInstance::~ScriptInstance() = default;

void ScriptInstance::InvokeOnStart() const {
	if (_onStartMethod) {
		MonoObject* exception = nullptr;
		mono_runtime_invoke(_onStartMethod, _instance, nullptr, &exception);
		if (exception) {
			CSharpLanguageModule::HandleException(exception, nullptr);
		}
	}
}

void ScriptInstance::InvokeOnEnd() const {
	if (_onEndMethod) {
		MonoObject* exception = nullptr;
		mono_runtime_invoke(_onEndMethod, _instance, nullptr, &exception);
		if (exception) {
			CSharpLanguageModule::HandleException(exception, nullptr);
		}
	}
}

namespace csharplm {
	CSharpLanguageModule g_csharplm;
}

plugify::ILanguageModule* GetLanguageModule() {
	return &csharplm::g_csharplm;
}