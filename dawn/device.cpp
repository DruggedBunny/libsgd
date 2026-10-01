#include "device.h"

#include "dawn/dawn_proc.h"
#include "dawn/native/DawnNative.h"

#include <GLFW/glfw3.h>

#if SGD_OS_WINDOWS
#define GLFW_EXPOSE_NATIVE_WIN32 1
#include <GLFW/glfw3native.h>
#undef min
#undef max
#elif SGD_OS_LINUX
#define GLFW_EXPOSE_NATIVE_X11 1
#define GLFW_EXPOSE_NATIVE_GLX 1
#include <GLFW/glfw3native.h>
#undef Success
#undef Always
#undef None
#endif

namespace sgd {

#if SGD_OS_MACOS
// Defined in device.mm
void* createMetalLayer(GLFWwindow* window);
#endif

namespace {

bool g_validationEnabled = true;
auto init0 =
	configVarChanged("dawn.validationEnabled").connect(nullptr, [](CString value) { g_validationEnabled = truthiness(value); });

// Off by default: with the current Dawn/Tint, shader robustness (bounds clamping) costs ~8ms/frame on the complex_demo
// benchmark with FXC, vs ~0.5ms with it off. LibSGD's shaders are internal. Set dawn.robustnessEnabled=true to restore.
bool g_robustnessEnabled = false;
auto init1 =
	configVarChanged("dawn.robustnessEnabled").connect(nullptr, [](CString value) { g_robustnessEnabled = truthiness(value); });

String wgpuStringViewToString(const wgpu::StringView& sv) {
	if(!sv.IsUndefined()) return String(sv);
	return "???";
};

Vector<const char*> g_enabledToggles;
Vector<const char*> g_disabledToggles;

// Debug aid: extra Dawn toggles from the environment, comma separated.
void addEnvToggles(const char* envName, Vector<const char*>& toggles) {
	static Vector<String> storage;
	storage.reserve(64);
	auto env = std::getenv(envName);
	if (!env || !*env) return;
	String list = env;
	size_t pos = 0;
	while (pos <= list.size()) {
		auto end = list.find(',', pos);
		if (end == String::npos) end = list.size();
		if (end > pos && storage.size() < 64) {
			storage.push_back(list.substr(pos, end - pos));
			toggles.push_back(storage.back().c_str());
		}
		pos = end + 1;
	}
}
Vector<wgpu::FeatureName> g_requiredFeatures;

void dawnDeviceLostCallback(const wgpu::Device& device, wgpu::DeviceLostReason reason, wgpu::StringView message) {
	static const char* names[] = {"Undefined", "Unknown", "Destroyed", "CallbackCancelled", "FailedCreation"};

	// Device is lost when the instance goes away at shutdown, not an error.
	if (reason == wgpu::DeviceLostReason::CallbackCancelled) return;

	auto rname = (uint32_t)reason < std::size(names) ? names[(uint32_t)reason] : "OOPS";

	//	SGD_LOG << "Dawn device lost:" << rname;
	//	SGD_LOG << message;
	SGD_ERROR(String("Dawn device lost: ") + rname + "\n" + wgpuStringViewToString(message));
}

void dawnErrorCallback(const wgpu::Device& device, wgpu::ErrorType type, wgpu::StringView message) {
	static const char* names[] = {"Undefined", "NoError", "Validation", "Out of memory", "Internal", "Unknown"};

	auto tname = (uint32_t)type < std::size(names) ? names[(uint32_t)type] : "OOPS";

	SGD_LOG << "Dawn device error message:" << tname;
	SGD_LOG << wgpuStringViewToString(message);
	alert(String("Dawn Device error:\n") + tname + "\n" + wgpuStringViewToString(message));
	SGD_ABORT();
}

void dawnLoggingCallback(wgpu::LoggingType type, wgpu::StringView message) {
	static const char* names[] = {"???", "Verbose", "Info", "Warning", "Error"};

	auto tname = (uint32_t)type < std::size(names) ? names[(uint32_t)type] : "OOPS";

	switch (type) {
	case wgpu::LoggingType::Verbose:
#if SGD_CONFIG_DEBUG
		SGD_LOG << "Dawn:" << wgpuStringViewToString(message);
#endif
		break;
	case wgpu::LoggingType::Info:
		SGD_LOG << "Dawn info:" << wgpuStringViewToString(message);
		break;
	case wgpu::LoggingType::Warning:
		SGD_LOG << "Dawn warning:" << wgpuStringViewToString(message);
		break;
	case wgpu::LoggingType::Error:
		SGD_LOG << "Dawn error:" << wgpuStringViewToString(message);
		alert(String("Dawn error: ") + wgpuStringViewToString(message));
		SGD_ABORT();
		break;
	default:
		SGD_ERROR("Dawn logging error");
	}
}

#define LIMIT(X) SGD_LOG << #X ":" << limits.limits.X;

void logAdapterProps(const wgpu::Adapter& adapter) {

	wgpu::AdapterInfo info{};

	adapter.GetInfo(&info);
	SGD_LOG << "Dawn WGPU adapter Info:";
	SGD_LOG << "Vender name:" << wgpuStringViewToString(info.vendor);
	SGD_LOG << "Architecture:" << wgpuStringViewToString(info.architecture);
	SGD_LOG << "Name:" << wgpuStringViewToString(info.device);
	SGD_LOG << "Driver description:" << wgpuStringViewToString(info.description);
	Map<wgpu::AdapterType, String> adapterTypes{
		{wgpu::AdapterType::DiscreteGPU, "DiscreteGPU"},
		{wgpu::AdapterType::IntegratedGPU, "IntegratedGPU"},
		{wgpu::AdapterType::CPU, "CPU"},
		{wgpu::AdapterType::Unknown, "Unknown"},
	};
	auto adapterIt = adapterTypes.find(info.adapterType);
	SGD_LOG << "Adapter type:" << (adapterIt != adapterTypes.end() ? adapterIt->second : "???");
	Map<wgpu::BackendType, String> backendTypes{
		{wgpu::BackendType::Undefined, "Undefined"}, {wgpu::BackendType::D3D12, "D3D12"},
		{wgpu::BackendType::D3D11, "D3D11"},		 {wgpu::BackendType::Vulkan, "Vulkan"},
		{wgpu::BackendType::Null, "Null"},			 {wgpu::BackendType::Metal, "Metal"},
		{wgpu::BackendType::OpenGL, "OpenGL"},		 {wgpu::BackendType::OpenGLES, "OpenGLES"},
		{wgpu::BackendType::WebGPU, "WebGPU"},
	};
	auto backendIt = backendTypes.find(info.backendType);
	SGD_LOG << "Backend type:" << (backendIt != backendTypes.end() ? backendIt->second : "???");

#if 0 // TODO
	SGD_LOG << "";
	SGD_LOG << "Dawn WGPU adapter supported limits:";
	wgpu::SupportedLimits limits;
	wgpuAdapterGetLimits(adapter.Get(), reinterpret_cast<WGPUSupportedLimits*>(&limits));
	LIMIT(maxTextureDimension1D);
	LIMIT(maxTextureDimension2D);
	LIMIT(maxTextureDimension3D);
	LIMIT(maxTextureArrayLayers);
	LIMIT(maxBindGroups);
	LIMIT(maxBindGroupsPlusVertexBuffers);
	LIMIT(maxBindingsPerBindGroup);
	LIMIT(maxDynamicUniformBuffersPerPipelineLayout);
	LIMIT(maxDynamicStorageBuffersPerPipelineLayout);
	LIMIT(maxSampledTexturesPerShaderStage);
	LIMIT(maxSamplersPerShaderStage);
	LIMIT(maxStorageBuffersPerShaderStage);
	LIMIT(maxStorageTexturesPerShaderStage);
	LIMIT(maxUniformBuffersPerShaderStage);
	LIMIT(maxUniformBufferBindingSize);
	LIMIT(maxStorageBufferBindingSize);
	LIMIT(minUniformBufferOffsetAlignment);
	LIMIT(minStorageBufferOffsetAlignment);
	LIMIT(maxVertexBuffers);
	LIMIT(maxBufferSize);
	LIMIT(maxVertexAttributes);
	LIMIT(maxVertexBufferArrayStride);
	LIMIT(maxInterStageShaderComponents);
	LIMIT(maxInterStageShaderVariables);
	LIMIT(maxColorAttachments);
	LIMIT(maxColorAttachmentBytesPerSample);
	LIMIT(maxComputeWorkgroupStorageSize);
	LIMIT(maxComputeInvocationsPerWorkgroup);
	LIMIT(maxComputeWorkgroupSizeX);
	LIMIT(maxComputeWorkgroupSizeY);
	LIMIT(maxComputeWorkgroupSizeZ);
	LIMIT(maxComputeWorkgroupsPerDimension);
#endif
}

} // namespace

const wgpu::Instance& getWGPUInstance() {
	static wgpu::Instance instance;
	if (instance) return instance;

	dawnProcSetProcs(&dawn::native::GetProcs());

	wgpu::InstanceDescriptor desc{};
	static const wgpu::InstanceFeatureName features[]{wgpu::InstanceFeatureName::TimedWaitAny};
	desc.requiredFeatureCount = std::size(features);
	desc.requiredFeatures = features;

	return instance = wgpu::CreateInstance(&desc);
}

void requestWGPUDevice(const wgpu::RequestAdapterOptions& adapterOptions,
					   CFunction<void(const wgpu::Device& device)> callbackFunc) {

	auto done = std::make_shared<bool>(false);

	getWGPUInstance().RequestAdapter( //
		&adapterOptions, wgpu::CallbackMode::AllowProcessEvents,
		[=](wgpu::RequestAdapterStatus status, const wgpu::Adapter& adapter, char const* message) {
			if (status != wgpu::RequestAdapterStatus::Success) {
				SGD_ERROR(String("Dawn RequestAdapter() failed: ") + (message ? message : ""));
			}

			logAdapterProps(adapter);

			{
				g_enabledToggles.push_back("allow_unsafe_apis");
				if (!g_validationEnabled) g_enabledToggles.push_back("skip_validation");
				if (!g_robustnessEnabled) g_enabledToggles.push_back("disable_robustness");
				addEnvToggles("SGD_DAWN_ENABLE_TOGGLES", g_enabledToggles);
				addEnvToggles("SGD_DAWN_DISABLE_TOGGLES", g_disabledToggles);
			}

			{
				for (auto feature : {wgpu::FeatureName::TimestampQuery,		   //
									 wgpu::FeatureName::Float32Filterable,	   //
									 wgpu::FeatureName::Unorm16TextureFormats, //
									 wgpu::FeatureName::TextureFormatsTier1}) {
					if (adapter.HasFeature(feature)) {
						g_requiredFeatures.push_back(feature);
					}
				}
			}

			wgpu::DeviceDescriptor devDesc{};
			wgpu::DawnTogglesDescriptor togglesDesc{};
			{
				devDesc.nextInChain = &togglesDesc;
				devDesc.requiredFeatureCount = g_requiredFeatures.size();
				devDesc.requiredFeatures = g_requiredFeatures.data();
				devDesc.SetDeviceLostCallback(wgpu::CallbackMode::AllowProcessEvents, &dawnDeviceLostCallback);
				devDesc.SetUncapturedErrorCallback(&dawnErrorCallback);

				togglesDesc.enabledToggleCount = g_enabledToggles.size();
				togglesDesc.enabledToggles = g_enabledToggles.data();
				togglesDesc.disabledToggleCount = g_disabledToggles.size();
				togglesDesc.disabledToggles = g_disabledToggles.data();
			}

			adapter.RequestDevice( //
				&devDesc, wgpu::CallbackMode::AllowProcessEvents,
				[=](wgpu::RequestDeviceStatus status, const wgpu::Device& device, const char* message) {
					if (status != wgpu::RequestDeviceStatus::Success) {
						SGD_ERROR(String("Dawn RequestDevice() failed: ") + (message ? message : ""));
					}
					device.SetLoggingCallback(&dawnLoggingCallback);
					callbackFunc(device);
					*done = true;
				});
		});

#if !SGD_OS_EMSCRIPTEN
	// Callbacks are only delivered from ProcessEvents(), and the adapter/device requests can complete after the first
	// call, so pump from out here until the device has arrived rather than relying on a single (or nested) call.
	auto start = millis();
	while (!*done) {
		getWGPUInstance().ProcessEvents();
		if (*done) break;
		if (millis() - start > 30000) SGD_ERROR("Timed out waiting for Dawn adapter/device");
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
#else
	getWGPUInstance().ProcessEvents();
#endif
}

wgpu::Device createWGPUDevice(const wgpu::RequestAdapterOptions& adapterOptions) {

	wgpu::Device result;
	CondVar<bool> ready;

	runOnMainThread(
		[&] {
			requestWGPUDevice(adapterOptions, [&](const wgpu::Device& device) { //
				result = device;
				ready = true;
			});
		},
		true);

	ready.waiteq(true);
	SGD_ASSERT(result);
	return result;
}

CVector<const char*> wgpuEnabledToggles() {
	return g_enabledToggles;
}

CVector<wgpu::FeatureName> wgpuRequiredFeatures() {
	return g_requiredFeatures;
}

wgpu::Surface createWGPUSurface(const wgpu::Device& device, GLFWwindow* window) {

	wgpu::Surface result;
	CondVar<bool> ready;

	runOnMainThread(
		[&] {
#if SGD_OS_WINDOWS
			wgpu::SurfaceSourceWindowsHWND nativeDesc{};
			nativeDesc.hwnd = glfwGetWin32Window(window);
			nativeDesc.hinstance = GetModuleHandle(nullptr);
#elif SGD_OS_LINUX
			wgpu::SurfaceSourceXlibWindow nativeDesc{};
			nativeDesc.window = glfwGetX11Window(window);
			nativeDesc.display = glfwGetX11Display();
#elif SGD_OS_MACOS
			wgpu::SurfaceSourceMetalLayer nativeDesc{};
			nativeDesc.layer = createMetalLayer(window);
#elif SGD_OS_EMSCRIPTEN
			wgpu::EmscriptenSurfaceSourceCanvasHTMLSelector nativeDesc{};
			nativeDesc.selector = "#canvas";
#endif
			wgpu::SurfaceDescriptor surfaceDesc{};
			surfaceDesc.nextInChain = &nativeDesc;

			wgpu::Surface surface = wgpuInstanceCreateSurface(getWGPUInstance().Get(), (WGPUSurfaceDescriptor*)&surfaceDesc);
			// Can't use this, causes weird missing symbols errors on Linux!
			// wgpu::Surface surface = getWGPUInstance().CreateSurface(&surfaceDesc);

			result = surface;
			ready = true;
		},
		true);

	ready.waiteq(true);
	SGD_ASSERT(result);
	return result;
}

#if SGD_OS_EMSCRIPTEN

EM_CALLBACK void sgd_requestAnimationFrameOK(Function<void()>* func) {
	(*func)();
	delete func;
}

// clang-format off
EM_JS(void, sgd_requestAnimationFrame, (Function<void()>* func), {
	requestAnimationFrame(() => {
		_sgd_requestAnimationFrameOK(func);
	});
});
// clang-format on

void requestRender(CFunction<void()> renderFunc) {

	CondVar<bool> ready;

	auto func = new Function<void()>([&ready, renderFunc] { //
		ready.set(true);
		renderFunc();
	});

	runOnMainThread(
		[func] { //
			sgd_requestAnimationFrame(func);
		},
		true);

	ready.waiteq(true);
}

#else

void requestRender(CFunction<void()> renderFunc) {
	runOnMainThread(renderFunc, true);
}

#endif

} // namespace sgd
