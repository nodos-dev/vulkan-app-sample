/*
 * Copyright MediaZ Teknoloji A.S. All Rights Reserved.
 */

#define NOMINMAX 1


#include "nosVulkan/Device.h"
#include "nosVulkan/Command.h"
#include "nosVulkan/Shader.h"
#include "nosVulkan/Pipeline.h"
#include "nosVulkan/Renderpass.h"
#include "nosVulkan/Image.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <random>
#include <deque>
#include <mutex>
#include <optional>



#define VK_USE_PLATFORM_WIN32_KHR
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "stb_image.h"
#include "stb_image_write.h"

// Nodos
#if defined(_WIN32)
#include <Windows.h>
#elif defined(__linux__)
#include <dlfcn.h>
#endif
#include "CommonEvents_generated.h"
#include <nosFlatBuffersCommon.h>
#include <Nodos/AppAPI.h>
#include <Nodos/AppHelpers.hpp>
#include <Nodos/UUID.hpp>
#include <nosSysVulkan/nosVulkanSubsystem.h>
#include <nosSysVulkan/ResourceShare_generated.h>


using namespace nos::vk;
// rc<> moved out of nos::vk into nosCppUtilities' nos namespace. Pulled in by name rather than
// with a using-directive, since nos::Buffer would otherwise collide with nos::vk::Buffer.
using nos::rc;

rc<Context> context;
rc<Device> GVkDevice;
rc<CommandPool> pool;

VkSurfaceKHR surface;
GLFWwindow* window;
const uint32_t WIDTH = 1920;
const uint32_t HEIGHT = 1080;

VkSwapchainKHR swapchain;

std::vector<rc<Semaphore>> WaitSemaphores;
std::vector<rc<Semaphore>> SignalSemaphores;

rc<Image> ShaderInput;
rc<Image> ShaderOutput;

// Loads App SDK entry points out of the shared library for AppApi/AppServiceClient. The SDK no
// longer exposes the FN_* procedures as nos::app members for the app to resolve by hand.
struct SampleProcLoader final : nos::app::IAppApiProcLoader
{
#if defined(_WIN32)
	using ModuleHandle = HMODULE;
#else
	using ModuleHandle = void*;
#endif

	explicit SampleProcLoader(ModuleHandle module) : Module(module) {}
	~SampleProcLoader()
	{
#if defined(_WIN32)
		::FreeLibrary(Module);
#else
		::dlclose(Module);
#endif
	}

	ProcFuncPtr GetProcAddress(const char* funcName) const override
	{
#if defined(_WIN32)
		return reinterpret_cast<ProcFuncPtr>(::GetProcAddress(Module, funcName));
#else
		return reinterpret_cast<ProcFuncPtr>(::dlsym(Module, funcName));
#endif
	}

	ModuleHandle Module;
};

std::unique_ptr<SampleProcLoader> procLoader;
std::unique_ptr<nos::app::AppServiceClient> client;

// Mailbox between the SDK's API thread and the render thread.
//
// Every AppEventDelegates callback arrives on the API thread, so none of them touch Vulkan or the
// sync protocol directly -- they only record what happened. The render loop drains this at the
// top of each iteration and does the real work on the thread that owns the device.
//
// This is deliberately raw rather than NodosCommunicator/IAppNode. The communicator answers a
// skipped range of N frames with a single ExecutionCompleted, while the engine decrements
// SyncedState.OutstandingExecuteRequests once per message -- so every skipped range leaks N-1
// outstanding requests, EarliestUnansweredFrameTime never clears, and hang detection fires. Here
// the accounting is ours: one ExecutionCompleted per AppExecuteStart, always.
struct SampleEventDelegates final : nos::app::AppEventDelegates
{
	std::mutex Mutex;

	nos::fb::UUID NodeId{};
	bool NodeImported = false;
	std::optional<nos::app::ExecutionState> PendingState;
	// Frame counters the engine has asked us to execute and we have not answered yet.
	std::deque<uint64_t> PendingExecutes;

	void OnAppConnected() override { std::cout << "Connected to Nodos" << std::endl; }

	void OnConnectionClosed() override
	{
		std::lock_guard lock(Mutex);
		PendingExecutes.clear();
	}

	void OnNodeImported(nos::fb::Node const& appNode) override
	{
		std::cout << "Node imported from Nodos" << std::endl;
		std::lock_guard lock(Mutex);
		NodeId = *appNode.id();
		NodeImported = true;
	}

	void OnNodeRemoved() override
	{
		std::lock_guard lock(Mutex);
		PendingExecutes.clear();
		PendingState = nos::app::ExecutionState::IDLE;
	}

	void OnStateChanged(nos::app::ExecutionState newState) override
	{
		std::lock_guard lock(Mutex);
		PendingState = newState;
		if (newState != nos::app::ExecutionState::SYNCED)
			PendingExecutes.clear();
	}

	void OnExecuteStart(nos::app::AppExecuteStart const* appExecuteStart) override
	{
		std::lock_guard lock(Mutex);
		// The engine sends this with reset on path stop; the queued frames are abandoned and must
		// not be answered.
		if (appExecuteStart->reset())
		{
			PendingExecutes.clear();
			return;
		}
		PendingExecutes.push_back(appExecuteStart->frame_counter());
	}
};

SampleEventDelegates eventDelegates;

// Pin identity and sync state, owned by the render thread.
nos::uuid ShaderInputPinId;
nos::uuid ShaderOutputPinId;

// Exported timeline semaphores shared with nos.sys.vulkan, recreated on every entry into SYNCED.
rc<Semaphore> InputSemaphore;
rc<Semaphore> OutputSemaphore;

// Last frame whose submit carried the handshake. Needed to free that submit if it ends up parked
// on a value the engine stopped producing.
std::optional<uint64_t> LastSubmittedFrame;

struct SwapchainInfo
{
	VkSwapchainKHR Handle = 0;
	uint32_t FrameCount = 0;
	std::vector<rc<Image>> Images{};
};

SwapchainInfo swapchainInfo;

std::vector<u8> ReadSpirv(std::string const& file)
{
	if(!std::filesystem::exists(file))
	{
		return {};
	}
	std::vector<uint8_t> spirv;
	{
		std::ifstream fileStream(file, std::ios::binary);
		fileStream.seekg(0, std::ios::end);
		spirv.resize(fileStream.tellg());
		fileStream.seekg(0, std::ios::beg);
		fileStream.read((char*)spirv.data(), spirv.size());
	}
	return spirv;
}

std::vector<uint8_t> generateRandomBytes(size_t numBytes) {
    std::vector<uint8_t> randomBytes(numBytes);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint16_t> dis(0, std::numeric_limits<uint8_t>::max());

    for (size_t i = 0; i < numBytes; ++i) {
        randomBytes[i] = static_cast<uint8_t>(dis(gen));
    }

    return randomBytes;}

bool InitWindow()
{
    glfwInit();

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    window = glfwCreateWindow(WIDTH, HEIGHT, "Vulkan", nullptr, nullptr);
	return true;
}

bool CreateSurface()
{
	if (glfwCreateWindowSurface(context->Instance, window, nullptr, &surface) != VK_SUCCESS) {
		throw std::runtime_error("failed to create window surface!");
		return false;
	}
	return true;
}

bool CreateSwapchain()
{
	VkSwapchainCreateInfoKHR sci{};
	sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	sci.surface = surface;
	sci.minImageCount = 3;
	sci.imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
	sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	sci.imageExtent = {.width = WIDTH, .height = HEIGHT};
	sci.imageArrayLayers = 1;
	sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	sci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	sci.clipped = true;

	//Create swapchain
	VkResult err =
		GVkDevice->CreateSwapchainKHR(&sci, nullptr, &swapchain);
	std::vector<VkImage> images;
	uint32_t imageCount = 0;
	GVkDevice->GetSwapchainImagesKHR(swapchain, &imageCount, 0);
	images.resize(imageCount);
	GVkDevice->GetSwapchainImagesKHR(swapchain, &imageCount, images.data());
	
	swapchainInfo.Handle = swapchain;
	swapchainInfo.FrameCount = imageCount;
	for (auto img : images)
	{
		// The swapchain owns these images, so they are wrapped rather than allocated: no
		// Allocation, and the initial state is undefined until the first transition.
		swapchainInfo.Images.emplace_back(
				Image::FromExisting(GVkDevice.get(),
									img,
									VkExtent3D{WIDTH, HEIGHT, 1},
									VK_FORMAT_B8G8R8A8_UNORM,
									VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
									ImageState{.StageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
											   .AccessMask = VK_ACCESS_2_NONE,
											   .Layout = VK_IMAGE_LAYOUT_UNDEFINED},
									/*allocation*/ std::nullopt,
									/*size*/ 0,
									VK_IMAGE_TYPE_2D));
	}

	for (int i = 0; i < swapchainInfo.FrameCount; i++)
	{
		WaitSemaphores.push_back(
			Semaphore::New(GVkDevice.get(), VkSemaphoreType::VK_SEMAPHORE_TYPE_BINARY, /*shouldExport*/ false));
		SignalSemaphores.push_back(
			Semaphore::New(GVkDevice.get(), VkSemaphoreType::VK_SEMAPHORE_TYPE_BINARY, /*shouldExport*/ false));
	}

	return err == VK_SUCCESS ? true : false;
}

rc<Renderpass> CreatePass()
{
	{
		//fragment shader
		std::vector<u8> spirv = ReadSpirv("triangle.frag.spv");
		spirv.resize(spirv.size() & ~3);
		if (auto vks = Shader::Create(GVkDevice.get(), spirv))
		{
			GVkDevice->RegisterGlobal<rc<Shader>>("TriangleFragment", std::move(vks));
		}
	}

	rc<Shader> PS = GVkDevice->GetGlobal<rc<Shader>>("TriangleFragment");
	nos::vk::BlendMode blendMode;
	blendMode.Enable = false;
	blendMode.SrcColorFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	blendMode.DstColorFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blendMode.SrcAlphaFactor = VK_BLEND_FACTOR_ONE;
	blendMode.DstAlphaFactor = VK_BLEND_FACTOR_ONE;
	blendMode.ColorOp = VK_BLEND_OP_ADD;
	blendMode.AlphaOp = VK_BLEND_OP_MAX;

	rc<Renderpass> RP = Renderpass::New(std::make_shared<GraphicsPipeline>(GVkDevice.get(), PS, nullptr,
		blendMode, 1));
	GVkDevice->RegisterGlobal<rc<Renderpass>>("TrianglePass", RP);
	

	return RP;
	
}



int InitNosSDK()
{
	// Initialize Nodos SDK
#if defined(_WIN32)
	SampleProcLoader::ModuleHandle sdkModule = LoadLibrary(NODOS_APP_SDK_DLL);
#elif defined(__linux__)
	SampleProcLoader::ModuleHandle sdkModule = dlopen(NODOS_APP_SDK_DLL, RTLD_LAZY);
#else
#error "Unsupported platform"
#endif
	if (!sdkModule) {
		std::cerr << "Failed to load Nodos SDK" << std::endl;
		return -1;
	}

	procLoader = std::make_unique<SampleProcLoader>(sdkModule);

	nosApplicationInfo appInfo{
		.AppKey = "Sample-Vulkan-App",
		.AppName = "Sample Vulkan App"
	};

	// CreateClient resolves the SDK entry points and checks version compatibility itself.
	auto clientResult = nos::app::AppServiceClient::CreateClient(*procLoader, "localhost:50053", appInfo);
	if (!clientResult) {
		std::cerr << "Failed to create App Service Client: " << *clientResult.Error() << std::endl;
		return -1;
	}
	client = std::move(*clientResult.Ok());
	client->SetEventDelegates(eventDelegates);

	while (!client->TryConnect())
	{
		std::cout << "Connecting to Nodos..." << std::endl;
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
	return 0;
}

// Describes an image we exported to Nodos. TTexture no longer carries resolution/unmanaged/
// unscaled/handle: width and height are always literal, `unscaled` moved into the pin's
// texture_options extension, and the memory offset moved into ExternalMemory.
nos::sys::vulkan::TTexture MakeTextureDef(rc<Image> const& image)
{
	auto const& exportInfo = image->GetExportInfo();

	nos::sys::vulkan::TTexture texture;
	texture.width = image->GetExtent().width;
	texture.height = image->GetExtent().height;
	texture.format = nos::sys::vulkan::Format(image->GetFormat());
	texture.usage = nos::sys::vulkan::ImageUsage(image->Usage);
	texture.size_in_bytes = image->Size;
	auto& ext = texture.external_memory;
	ext.mutate_handle_type(exportInfo.HandleType);
	ext.mutate_handle((u64)exportInfo.Handle);
	ext.mutate_allocation_size((u64)exportInfo.AllocationSize);
	ext.mutate_pid((u64)exportInfo.PID);
	ext.mutate_offset((u64)exportInfo.Offset);
	return texture;
}

// `unscaled` tells nos.sys.vulkan to take the exported image at its own size instead of
// rescaling it to the graph resolution. It used to live on the texture value; it is now a pin
// extension carrying an opaque TexturePinOptions blob, hence the nested builder.
flatbuffers::Offset<nos::fb::PinExtension> CreateUnscaledTextureOptions(flatbuffers::FlatBufferBuilder& fbb)
{
	flatbuffers::FlatBufferBuilder optionsBuilder;
	optionsBuilder.Finish(nos::sys::vulkan::CreateTexturePinOptions(optionsBuilder, /*unscaled*/ true));
	const std::vector<uint8_t> optionsData(optionsBuilder.GetBufferPointer(),
										   optionsBuilder.GetBufferPointer() + optionsBuilder.GetSize());
	return nos::fb::CreatePinExtensionDirect(fbb, "texture_options",
											 nos::sys::vulkan::TexturePinOptions::GetFullyQualifiedName(), &optionsData);
}

// App <-> subsystem resource traffic goes to nos.sys.vulkan as a ResourceShareMessage wrapped in
// an app CustomMessage. The ResourceShareMessage is a self-contained buffer carried as an opaque
// payload inside the CustomMessage, so two builders are required.
void SendResourceShareMessage(nos::sys::vulkan::ResourceShareMessageUnion messageType,
							  flatbuffers::Offset<void> message,
							  flatbuffers::FlatBufferBuilder& messageBuilder)
{
	messageBuilder.Finish(nos::sys::vulkan::CreateResourceShareMessage(messageBuilder, messageType, message));
	const std::vector<uint8_t> payload(messageBuilder.GetBufferPointer(),
									   messageBuilder.GetBufferPointer() + messageBuilder.GetSize());

	flatbuffers::FlatBufferBuilder eventBuilder;
	eventBuilder.Finish(nos::CreateAppEventOffset(
		eventBuilder,
		nos::app::CreateCustomMessageDirect(eventBuilder, "nos.sys.vulkan",
											nos::sys::vulkan::ResourceShareMessage::GetFullyQualifiedName(), &payload)));
	nos::Buffer buffer = eventBuilder.Release();
	client->Send(buffer.As<nos::app::AppEvent>());
}

// Hands the exported image's memory to nos.sys.vulkan for the given pin. Publishing the texture
// as a pin value is not enough on its own: without this the subsystem never imports our external
// memory and the pin stays empty.
void SendImportResource(nos::uuid const& pinId, nos::sys::vulkan::TTexture const& texture)
{
	flatbuffers::FlatBufferBuilder mb;
	auto packed = nos::sys::vulkan::Texture::Pack(mb, &texture);
	auto importResource =
		nos::sys::vulkan::CreateImportResource(mb, &pinId, nos::sys::vulkan::ResourceUnion::Texture, packed.Union());
	SendResourceShareMessage(nos::sys::vulkan::ResourceShareMessageUnion::ImportResource, importResource.Union(), mb);
}

nos::uuid GeneratePinId()
{
	const std::vector<uint8_t> randomBytes = generateRandomBytes(16);
	return nos::uuid(std::span<const uint8_t, 16>(randomBytes.data(), 16));
}

void CreateTexturePinsInNodos()
{
	// Stable across re-imports, so CLEAR_PINS + re-add replaces the same two pins instead of
	// accumulating a new pair every time the node comes back.
	if (ShaderInputPinId == nos::uuid{})
	{
		ShaderInputPinId = GeneratePinId();
		ShaderOutputPinId = GeneratePinId();
	}

	std::vector<flatbuffers::Offset<nos::fb::Pin>> pins;
	flatbuffers::FlatBufferBuilder fbb;

	const nos::sys::vulkan::TTexture inputTexture = MakeTextureDef(ShaderInput);
	const nos::sys::vulkan::TTexture outputTexture = MakeTextureDef(ShaderOutput);

	// CreatePinDirect now takes a vector of visualizers where the category string used to be, an
	// extensions vector right after the data, and no longer has an advanced_property parameter.
	auto addTexturePin = [&fbb, &pins](nos::uuid const& id,
									   const char* name,
									   const char* displayName,
									   nos::fb::ShowAs showAs,
									   nos::fb::CanShowAs canShowAs,
									   nos::sys::vulkan::TTexture const& texture) {
		flatbuffers::FlatBufferBuilder valueBuilder;
		valueBuilder.Finish(nos::sys::vulkan::CreateTexture(valueBuilder, &texture));
		nos::Buffer buffer = valueBuilder.Release();
		std::vector<uint8_t> data = buffer;

		std::vector<flatbuffers::Offset<nos::fb::PinExtension>> extensions{CreateUnscaledTextureOptions(fbb)};

		pins.push_back(nos::fb::CreatePinDirect(
			fbb, &id, name, nos::sys::vulkan::Texture::GetFullyQualifiedName(), showAs, canShowAs,
			/*visualizers*/ nullptr, &data, &extensions,
			/*referred_by*/ nullptr, /*min*/ nullptr, /*max*/ nullptr, /*def*/ nullptr,
			/*step*/ 0.0f, /*readonly*/ false, /*transient*/ false, /*meta_data_map*/ nullptr,
			/*live*/ false, nos::fb::PinContents::JobPin, /*contents*/ 0, /*orphan_state*/ 0,
			nos::fb::PinValueDisconnectBehavior::KEEP_LAST_VALUE, "Example tooltip", displayName));
	};

	addTexturePin(ShaderInputPinId, "Shader Input", "Texture Input", nos::fb::ShowAs::INPUT_PIN,
				  nos::fb::CanShowAs::INPUT_PIN_ONLY, inputTexture);
	addTexturePin(ShaderOutputPinId, "Shader Output", "Texture Output", nos::fb::ShowAs::OUTPUT_PIN,
				  nos::fb::CanShowAs::OUTPUT_PIN_ONLY, outputTexture);

	auto offset = nos::CreatePartialNodeUpdateDirect(fbb, &eventDelegates.NodeId, nos::ClearFlags::CLEAR_PINS, 0, &pins);
	fbb.Finish(offset);
	nos::Buffer update = fbb.Release();
	client->SendPartialNodeUpdate(update.As<nos::PartialNodeUpdate>());

	SendImportResource(ShaderInputPinId, inputTexture);
	SendImportResource(ShaderOutputPinId, outputTexture);
}

// Until nos.sys.vulkan has both semaphores it reports the node as not sync-ready and the engine
// logs "Sync not ready, skipping execution" and skips every frame. Receiving this is also what
// makes the subsystem subscribe to the node's execution.
void SendSyncSemaphoresToNodos()
{
	if (!InputSemaphore || !OutputSemaphore || !InputSemaphore->OsHandle || !OutputSemaphore->OsHandle)
	{
		std::cerr << "Sync semaphores are not exportable, Nodos will not execute this node" << std::endl;
		return;
	}

	flatbuffers::FlatBufferBuilder mb;
	auto semaphores = nos::sys::vulkan::CreateSetInputOutputSyncSemaphores(mb,
																		  (u64)PlatformGetCurrentProcessId(),
																		  (u64)*InputSemaphore->OsHandle,
																		  (u64)*OutputSemaphore->OsHandle);
	SendResourceShareMessage(nos::sys::vulkan::ResourceShareMessageUnion::SetInputOutputSyncSemaphores,
							 semaphores.Union(), mb);
}

// AppEventDelegates::OnStateChanged only reports the new state, so the previous one is tracked
// here to keep the from->to logging.
nos::app::ExecutionState currentExecutionState = nos::app::ExecutionState::IDLE;

void SignalIfBelow(rc<Semaphore> const& semaphore, uint64_t value);
void ReleaseParkedSubmit(uint64_t frameNumber);

// Runs on the render thread, from the mailbox drain at the top of the loop -- creating Vulkan
// semaphores on the API thread would race the device this loop owns.
void ApplyExecutionStateChange(nos::app::ExecutionState newState)
{
	std::cout << "Execution state changed from " << nos::app::EnumNameExecutionState(currentExecutionState)
			  << " to " << nos::app::EnumNameExecutionState(newState) << std::endl;
	currentExecutionState = newState;

	// Release, then drain, then slam -- in that order. A submit of ours can be parked on a value
	// the engine will never produce now, and destroying a semaphore with operations still pending
	// on it is invalid, as is waiting for a device that can never go idle.
	if (InputSemaphore && OutputSemaphore)
	{
		if (LastSubmittedFrame)
			ReleaseParkedSubmit(*LastSubmittedFrame);
		// Now nothing of ours can be stuck, so this completes and leaves nothing queued.
		GVkDevice->DeviceWaitIdle();
		// Safe only once our queue is empty: with no queued signals left there is nothing to land
		// below it. Frees anything the engine still has parked further ahead than we reached.
		SignalIfBelow(InputSemaphore, UINT64_MAX);
		SignalIfBelow(OutputSemaphore, UINT64_MAX);
	}

	InputSemaphore.reset();
	OutputSemaphore.reset();
	LastSubmittedFrame.reset();

	if (newState == nos::app::ExecutionState::SYNCED)
	{
		InputSemaphore = Semaphore::New(GVkDevice.get(), VK_SEMAPHORE_TYPE_TIMELINE, /*shouldExport*/ true);
		OutputSemaphore = Semaphore::New(GVkDevice.get(), VK_SEMAPHORE_TYPE_TIMELINE, /*shouldExport*/ true);
		SendSyncSemaphoresToNodos();
	}
}

// Anchors the timelines to wherever the engine's frame counter actually is, before executing
// frame N.
//
// Assuming a fresh semaphore may start at 0 is a deadlock. The engine has two paths back into
// SYNCED and they differ: ProcessNodeContext::OnHangDetected does `SyncedState = {}` first, so its
// counter really does restart at 0 -- but ProcessNodeContext::RestartSync just toggles
// IDLE->SYNCED and leaves the counter running. The app gets the same StateChanged event either way
// and cannot tell them apart. After a path restart the engine resumes at frame N and its input
// copies wait for 2N on a semaphore we just created at 0; nothing ever signals that, because we
// would be waiting on 2N+1, which only the engine produces.
//
// Entering frame N the protocol says input should be at 2N (what we signalled at N-1) and output
// at 2N (what the engine signalled at N-1), so that is what we re-anchor to. Timeline waits are
// >=, so this also covers frames we skipped: catching up to the newest request satisfies every
// engine wait for the ones in between. At steady state the values already match and it is a no-op.
// vkSignalSemaphore rejects a value that does not advance the timeline, hence the guard.
void SignalIfBelow(rc<Semaphore> const& semaphore, uint64_t value)
{
	if (semaphore && semaphore->GetValue() < value)
		semaphore->Signal(value);
}

void RebaseTimelines(uint64_t frameNumber)
{
	if (!InputSemaphore || !OutputSemaphore)
		return;

	const uint64_t base = 2 * frameNumber;
	SignalIfBelow(InputSemaphore, base);
	SignalIfBelow(OutputSemaphore, base);
}

// Frees our own submit when it is parked on a value the engine has stopped producing.
//
// The submit waits on input 2f+1 and output 2f, and goes on to signal 2f+2 and 2f+1. Signalling
// exactly the values it waits on releases it while staying strictly below what it signals, so
// nothing ever decreases -- which a Vulkan timeline forbids outright. (Slamming UINT64_MAX would
// release it too, but then its own queued signals land below that and are illegal. D3D12 tolerates
// that, which is why the Unreal plugin can slam; we cannot.)
void ReleaseParkedSubmit(uint64_t frameNumber)
{
	SignalIfBelow(InputSemaphore, 2 * frameNumber + 1);
	SignalIfBelow(OutputSemaphore, 2 * frameNumber);
}

// Puts this frame's half of the handshake onto the command buffer that reads ShaderInput and
// writes ShaderOutput. Mirrors nos.sys.vulkan's ExternalResourceSynchronizer, which for frame f
// runs input copies waiting 2f / signalling 2f+1 and output copies waiting 2f+1 / signalling 2f+2.
void AddFrameSyncToCmd(rc<CommandBuffer> cmd, uint64_t frameNumber)
{
	if (!InputSemaphore || !OutputSemaphore)
		return;

	const uint64_t f = frameNumber;

	// Wait until the engine's input copies for frame f have landed in ShaderInput (it signals
	// 2f+1), and until its output copies for the previous frame released ShaderOutput (2f).
	cmd->WaitGroup[InputSemaphore->Handle] = {2 * f + 1, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
	cmd->WaitGroup[OutputSemaphore->Handle] = {2 * f, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};

	// Release ShaderInput for the engine's next input copies (they wait 2(f+1) = 2f+2), and hand
	// this frame's ShaderOutput to its output copies (they wait 2f+1).
	cmd->SignalGroup[InputSemaphore->Handle] = 2 * f + 2;
	cmd->SignalGroup[OutputSemaphore->Handle] = 2 * f + 1;
}

// Exactly one of these per AppExecuteStart received, or the engine's OutstandingExecuteRequests
// never returns to zero and hang detection eventually fires.
void SendExecutionCompleted(uint64_t frameNumber)
{
	flatbuffers::FlatBufferBuilder fbb;
	nos::Table<nos::app::AppEvent> event = nos::CreateAppEvent(
		fbb, nos::app::CreateExecutionCompleted(fbb, &eventDelegates.NodeId, frameNumber));
	client->Send(event.As<nos::app::AppEvent>());
}

// Drains the API-thread mailbox and returns the frame to execute this iteration, if any.
std::optional<uint64_t> ServiceNodosEvents()
{
	bool importPins = false;
	std::optional<nos::app::ExecutionState> stateChange;
	std::optional<uint64_t> executeFrame;
	size_t queueDepth = 0;
	{
		std::lock_guard lock(eventDelegates.Mutex);
		std::swap(importPins, eventDelegates.NodeImported);
		stateChange = std::exchange(eventDelegates.PendingState, std::nullopt);
		queueDepth = eventDelegates.PendingExecutes.size();
		// Oldest first, one per iteration. Skipping to the newest and host-advancing past the rest
		// looks like a latency win and is corruption: a frame the engine RAN is not a frame the
		// engine skipped. For each one it has already queued copies -- input waits 2f / signals
		// 2f+1, output waits 2f+1 / signals 2f+2. Pushing the timeline past them releases those
		// copies to signal below where we just moved it, which is an illegal decrease.
		//
		// Falling behind is self-limiting: the engine's copies are parked on values only we
		// produce, so its path stalls and it stops issuing requests until we catch up.
		if (!eventDelegates.PendingExecutes.empty())
		{
			executeFrame = eventDelegates.PendingExecutes.front();
			eventDelegates.PendingExecutes.pop_front();
		}
	}

	if (stateChange)
		ApplyExecutionStateChange(*stateChange);
	if (importPins)
		CreateTexturePinsInNodos();

	if (!executeFrame)
		return std::nullopt;

	// Anchors a fresh session's timelines to the engine's counter, and heals them if they fall
	// behind mid-session. A no-op in steady state.
	RebaseTimelines(*executeFrame);

	std::cout << "Execute requested for frame " << *executeFrame << " (queued: " << queueDepth << ")" << std::endl;
	return executeFrame;
}

int main() 
{
	GHandleImporter = {
		.DuplicateHandle = [](NOS_PID pid, NOS_HANDLE handle) -> std::optional<NOS_HANDLE>
		{
			return client->DuplicateHandle(handle);
		},
		.CloseHandle = [](NOS_HANDLE handle)
		{
			client->CloseHandle(handle);
		}
	};
	context = Context::New();
	if (context->Devices.empty())
		return 0;
	GVkDevice = context->Devices[0];
	pool = GVkDevice->GetCommandPool();

	auto RP = CreatePass();

	InitWindow();
	CreateSurface();
	CreateSwapchain();

	// The default ResourceCreateRequest already asks for the platform's external memory handle
	// type, which is what makes these images shareable with Nodos.
	ImageCreateRequest createInfo = {
		.Extent = {1920, 1080, 1},
		.Format = VK_FORMAT_R8G8B8A8_UNORM,
		.Usage = VK_IMAGE_USAGE_SAMPLED_BIT |
				  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
				  VK_IMAGE_USAGE_STORAGE_BIT |
				  VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
				  VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	};

	auto shaderInputResult = Image::Create(GVkDevice.get(), createInfo);
	if (!shaderInputResult)
	{
		std::cout << "Failed to create input image: " << *shaderInputResult.Error() << std::endl;
		return 0;
	}
	ShaderInput = *shaderInputResult.Ok();

	auto shaderOutputResult = Image::Create(GVkDevice.get(), createInfo);
	if (!shaderOutputResult)
	{
		std::cout << "Failed to create output image: " << *shaderOutputResult.Error() << std::endl;
		return 0;
	}
	ShaderOutput = *shaderOutputResult.Ok();

	InitNosSDK();
	
	int frame = 0;
	while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
		if (!client->IsConnected())
		{
			std::cout << "Reconnecting to Nodos..." << std::endl;
			while (!client->TryConnect())
			{
				std::this_thread::sleep_for(std::chrono::seconds(1));
			}
		}

		// Non-blocking: applies whatever the API thread recorded and tells us whether Nodos is
		// waiting on a frame. The window keeps rendering at its own rate either way, so the loop
		// never parks waiting for the engine.
		const std::optional<uint64_t> executeFrame = ServiceNodosEvents();

		uint32_t imageIndex;
		GVkDevice->AcquireNextImageKHR(swapchain, 10000, WaitSemaphores[frame]->Handle, 0, &imageIndex);
		auto cmd = pool->BeginCmd();

		RP->BindResource("Input", ShaderInput, VkFilter::VK_FILTER_NEAREST);
		RP->TransitionInput(cmd, "Input", ShaderInput);

		// A pass can now render to several attachments, so OutImage became the OutImages vector,
		// and DeltaSeconds is gone.
		Renderpass::ExecPassInfo info{
		  .BeginInfo = {.OutImages = {ShaderOutput},
		  			  .DepthAttachment = std::nullopt,
		  			  .Wireframe = false,
		  			  .Clear = true,
		  			  .FrameNumber = 0,
		  			  .ClearCol = {0.0f,0.0f,0.0f,1.0f}},
					  .VtxData = std::nullopt};

		if (auto err = RP->Exec(cmd, info))
		{
			std::cerr << "Failed to execute pass: " << *err << std::endl;
			break;
		}

		//copy from texture to swapchain image
		swapchainInfo.Images[imageIndex]->CopyFrom(cmd, ShaderOutput);

		swapchainInfo.Images[imageIndex]->Transition(cmd, ImageState{
				  .StageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
				  .AccessMask = VK_ACCESS_MEMORY_READ_BIT,
				  .Layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			});
		cmd->WaitGroup[WaitSemaphores[frame]->Handle] = {1, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
		cmd->SignalGroup[SignalSemaphores[frame]->Handle] = 1;

		// This one submit both consumes ShaderInput and produces ShaderOutput, so it carries both
		// halves of the handshake with the engine.
		if (executeFrame)
		{
			AddFrameSyncToCmd(cmd, *executeFrame);
			LastSubmittedFrame = *executeFrame;
		}

		VkResult submitResult = VK_SUCCESS;
		cmd->Submit(&submitResult);
		if (submitResult != VK_SUCCESS)
		{
			std::cerr << "Failed to submit command buffer: " << vk_result_string(submitResult) << std::endl;
			break;
		}

		// This submit waits on values the engine drives, and it is the same submit that drives the
		// swapchain -- so a timeout here is the window freezing, not just a late frame. It happens
		// in ordinary use: disconnecting the input pin leaves the node SYNCED while nos.sys.vulkan
		// stops producing 2f+1 entirely, because ProcessInputCopies builds that copy from the pin.
		//
		// Waiting it out is what made the app hang until the engine's recovery force-signalled.
		// Instead, free our own submit at the values it is parked on and carry on. This is the same
		// self-heal the per-frame rebase provides, applied now rather than next iteration -- the
		// loop cannot reach the next iteration while it is blocked here.
		if (!cmd->Wait())
		{
			if (executeFrame)
			{
				std::cerr << "Nodos stopped driving frame " << *executeFrame << "; releasing our submit"
						  << std::endl;
				ReleaseParkedSubmit(*executeFrame);
			}
			if (!cmd->Wait())
			{
				std::cerr << "Frame " << executeFrame.value_or(0) << " is stuck on something other than "
						  << "Nodos sync" << std::endl;
				break;
			}
		}

		VkPresentInfoKHR pi{};
		pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		pi.pSwapchains = &swapchain;
		pi.swapchainCount = 1;
		pi.pImageIndices = &imageIndex;
		pi.pWaitSemaphores = &SignalSemaphores[frame]->Handle;
		pi.waitSemaphoreCount = 1;
		VkResult res;
		pi.pResults = &res;
		NOSVK_ASSERT(GVkDevice->MainQueue->PresentKHR(&pi));

		if (executeFrame)
			SendExecutionCompleted(*executeFrame);

		frame = (frame + 1) % swapchainInfo.FrameCount;
    }

	vkDestroySurfaceKHR(context->Instance, surface, nullptr);
	glfwDestroyWindow(window);
    glfwTerminate();

	return 0;
}