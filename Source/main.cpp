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



#define VK_USE_PLATFORM_WIN32_KHR
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include "stb_image.h"
#include "stb_image_write.h"

// Nodos
#include "CommonEvents_generated.h"
#include <nosFlatBuffersCommon.h>
#include <Nodos/AppAPI.h>
#include "nosVulkanSubsystem/nosVulkanSubsystem.h"


using namespace nos::vk;

rc<Context> context;
rc<Device> GVkDevice;
rc<CommandPool> pool;

VkSurfaceKHR surface;
GLFWwindow* window;
const uint32_t WIDTH = 800;
const uint32_t HEIGHT = 600;

VkSwapchainKHR swapchain;

std::vector<rc<Semaphore>> WaitSemaphores;
std::vector<rc<Semaphore>> SignalSemaphores;

rc<Image> ShaderInput;
rc<Image> ShaderOutput;

void CreateTexturePinsInNodos();

struct SampleEventDelegates : nos::app::IEventDelegates
{
	SampleEventDelegates(nos::app::IAppServiceClient* client) : Client(client) {}

	nos::app::IAppServiceClient* Client;
	nos::fb::UUID NodeId{};

	void OnAppConnected(const nos::fb::Node* appNode) override
	{
		std::cout << "Connected to Nodos" << std::endl;
		if (appNode)
		{
			NodeId = *appNode->id();
		}
	}
	void OnNodeUpdated(nos::fb::Node const& appNode) override 
	{
		std::cout << "Node updated from Nodos" << std::endl;
		NodeId = *appNode.id();

		CreateTexturePinsInNodos();
		
	}
	void OnContextMenuRequested(nos::app::AppContextMenuRequest const& request) override {}
	void OnContextMenuCommandFired(nos::app::AppContextMenuAction const& action) override {}
	void OnNodeRemoved() override {}
	void OnPinValueChanged(nos::fb::UUID const& pinId, uint8_t const* data, size_t size, bool reset, uint64_t frameNumber) override {}
	void OnPinShowAsChanged(nos::fb::UUID const& pinId, nos::fb::ShowAs newShowAs) override {}
	void OnExecuteAppInfo(nos::app::AppExecuteInfo const* appExecuteInfo) override {}
	void OnFunctionCall(nos::fb::UUID const& nodeId, nos::fb::Node const& function) override {}
	void OnNodeSelected(nos::fb::UUID const& nodeId) override {}
	void OnNodeImported(nos::fb::Node const& appNode) override {}
	void OnConnectionClosed() override {}
	void OnStateChanged(nos::app::ExecutionState newState) override {}
	void OnConsoleCommand(nos::app::ConsoleCommand const* consoleCommand) override {}
	void OnConsoleAutoCompleteSuggestionRequest(nos::app::ConsoleAutoCompleteSuggestionRequest const* consoleAutoCompleteSuggestionRequest) override {}
	void OnLoadNodesOnPaths(nos::app::LoadNodesOnPaths const* loadNodesOnPathsRequest) override {}
	void OnCloseApp() override {}
	void OnExecuteStart(nos::app::AppExecuteStart const* appExecuteStart) override {}
};

nos::app::IAppServiceClient* client;
SampleEventDelegates* eventDelegates;

struct SwapchainInfo
{
	VkSwapchainKHR Handle = 0;
	uint32_t FrameCount = 0;
	std::vector<rc<Image>> Images{};
};

SwapchainInfo swapchainInfo;

std::vector<u8> ReadSpirv(std::string const& file)
{
    std::basic_ifstream<u8, std::char_traits<u8>> f(file, std::ios::in | std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<u8>(f)),
                           std::istreambuf_iterator<u8>());
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
	
		auto vkImg = swapchainInfo.Images.emplace_back(
				Image::New(GVkDevice.get(),
						   img,
						   VkExtent2D{WIDTH, HEIGHT},
						   VK_FORMAT_B8G8R8A8_UNORM,
						   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT));
	}

	for (int i = 0; i < swapchainInfo.FrameCount; i++)
	{
		WaitSemaphores.push_back(Semaphore::New(GVkDevice.get(), true));
		SignalSemaphores.push_back(Semaphore::New(GVkDevice.get(), true));
	}

	return err == VK_SUCCESS ? true : false;
}

rc<Renderpass> CreatePass()
{
	{
		//vertex shader
		std::vector<u8> spirv = ReadSpirv("triangle.vert.spv");
		spirv.resize(spirv.size() & ~3);
		if (auto vks = Shader::Create(GVkDevice.get(), spirv))
		{
			GVkDevice->RegisterGlobal<rc<Shader>>("TriangleVertex", std::move(vks));
		}
	}

	{
		//fragment shader
		std::vector<u8> spirv = ReadSpirv("triangle.frag.spv");
		spirv.resize(spirv.size() & ~3);
		if (auto vks = Shader::Create(GVkDevice.get(), spirv))
		{
			GVkDevice->RegisterGlobal<rc<Shader>>("TriangleFragment", std::move(vks));
		}
	}

	rc<Shader> VS = GVkDevice->GetGlobal<rc<Shader>>("TriangleVertex");
	VS = nullptr;
	rc<Shader> PS = GVkDevice->GetGlobal<rc<Shader>>("TriangleFragment");
	GraphicsPipeline::BlendMode blendMode;
	blendMode.Enable = false;
	blendMode.SrcColorFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	blendMode.DstColorFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blendMode.SrcAlphaFactor = VK_BLEND_FACTOR_ONE;
	blendMode.DstAlphaFactor = VK_BLEND_FACTOR_ONE;
	blendMode.ColorOp = VK_BLEND_OP_ADD;
	blendMode.AlphaOp = VK_BLEND_OP_MAX;

	rc<Renderpass> RP = Renderpass::New(std::make_shared<GraphicsPipeline>(GVkDevice.get(), PS, VS,
		blendMode, 1));
	GVkDevice->RegisterGlobal<rc<Renderpass>>("TrianglePass", RP);
	

	return RP;
	
}



int InitNosSDK()
{
	// Initialize Nodos SDK
	nos::app::FN_CheckSDKCompatibility* pfnCheckSDKCompatibility = nullptr;
	nos::app::FN_MakeAppServiceClient* pfnMakeAppServiceClient = nullptr;
	nos::app::FN_ShutdownClient* pfnShutdownClient = nullptr;

	HMODULE sdkModule = LoadLibrary(NODOS_APP_SDK_DLL);
	if (sdkModule) {
		pfnCheckSDKCompatibility = (nos::app::FN_CheckSDKCompatibility*)GetProcAddress(sdkModule, "CheckSDKCompatibility");
		pfnMakeAppServiceClient = (nos::app::FN_MakeAppServiceClient*)GetProcAddress(sdkModule, "MakeAppServiceClient");
		pfnShutdownClient = (nos::app::FN_ShutdownClient*)GetProcAddress(sdkModule, "ShutdownClient");
	}
	else {
		std::cerr << "Failed to load Nodos SDK" << std::endl;
		return -1;
	}

	if (!pfnCheckSDKCompatibility || !pfnMakeAppServiceClient || !pfnShutdownClient) {
		std::cerr << "Failed to load Nodos SDK functions" << std::endl;
		return -1;
	}

	if (!pfnCheckSDKCompatibility(NOS_APPLICATION_SDK_VERSION_MAJOR, NOS_APPLICATION_SDK_VERSION_MINOR, NOS_APPLICATION_SDK_VERSION_PATCH)) {
		std::cerr << "Incompatible Nodos SDK version" << std::endl;
		return -1;
	}

	client = pfnMakeAppServiceClient("localhost:50053", nos::app::ApplicationInfo{ 
		.AppKey = "Sample-Vulkan-App",
		.AppName = "Sample Vulkan App"
	});

	if (!client) {
		std::cerr << "Failed to create App Service Client" << std::endl;
		return -1;
	}
	// TODO: Shutdown client
	eventDelegates = new SampleEventDelegates(client);
	client->RegisterEventDelegates(eventDelegates);

	while (!client->TryConnect())
	{
		std::cout << "Connecting to Nodos..." << std::endl;
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
}

void CreateTexturePinsInNodos()
{
	{
		nos::sys::vulkan::TTexture Texture;
		Texture.resolution = nos::sys::vulkan::SizePreset::CUSTOM;
		Texture.width = ShaderInput->GetExtent().width;
		Texture.height = ShaderInput->GetExtent().height;
		Texture.format = nos::sys::vulkan::Format(ShaderInput->GetFormat());
		Texture.usage = nos::sys::vulkan::ImageUsage(NOS_IMAGE_USAGE_RENDER_TARGET | NOS_IMAGE_USAGE_SAMPLED | NOS_IMAGE_USAGE_TRANSFER_SRC | NOS_IMAGE_USAGE_TRANSFER_DST);
		auto& Ext = Texture.external_memory;
		Ext.mutate_handle_type(NOS_EXTERNAL_MEMORY_HANDLE_TYPE_WIN32);
		Ext.mutate_handle((u64)ShaderInput->GetExportInfo().Handle);
		Ext.mutate_allocation_size((u64)ShaderInput->GetExportInfo().AllocationSize);
		Ext.mutate_pid((u64)ShaderInput->GetExportInfo().PID);
		Texture.unmanaged = false;
		Texture.unscaled = true;
		Texture.handle = 0;
		Texture.offset = ShaderInput->GetExportInfo().Offset;

		flatbuffers::FlatBufferBuilder fb;
		auto offset1 = nos::sys::vulkan::CreateTexture(fb, &Texture);
		fb.Finish(offset1);
		nos::Buffer buffer = fb.Release();
		std::vector<uint8_t>data = buffer;

		size_t numBytes = 16;
		std::vector<uint8_t> randomBytes = generateRandomBytes(numBytes);


		std::vector<flatbuffers::Offset<nos::fb::Pin>> pins;
		flatbuffers::FlatBufferBuilder fbb;
		pins.push_back(nos::fb::CreatePinDirect(fbb, (nos::fb::UUID*)randomBytes.data(), "Shader Input", "nos.sys.vulkan.Texture", nos::fb::ShowAs::INPUT_PIN, nos::fb::CanShowAs::INPUT_PIN_ONLY, "Shader Vars", 0, &data, 0, 0, 0, 0, 0, false, false, false, 0, 0, nos::fb::PinContents::JobPin, 0, 0, false, nos::fb::PinValueDisconnectBehavior::KEEP_LAST_VALUE, "Hello there!", "WHAT IS THAT!??"));
		auto offset = nos::CreatePartialNodeUpdateDirect(fbb, &eventDelegates->NodeId, nos::ClearFlags::NONE, 0, &pins, 0, 0, 0, 0);
		fbb.Finish(offset);
		auto buf = fbb.Release();
		auto root = flatbuffers::GetRoot<nos::PartialNodeUpdate>(buf.data());
		client->SendPartialNodeUpdate(*root);
	}
	{
		nos::sys::vulkan::TTexture Texture;
		Texture.resolution = nos::sys::vulkan::SizePreset::CUSTOM;
		Texture.width = ShaderOutput->GetExtent().width;
		Texture.height = ShaderOutput->GetExtent().height;
		Texture.format = nos::sys::vulkan::Format(ShaderOutput->GetFormat());
		Texture.usage = nos::sys::vulkan::ImageUsage(NOS_IMAGE_USAGE_RENDER_TARGET | NOS_IMAGE_USAGE_SAMPLED | NOS_IMAGE_USAGE_TRANSFER_SRC | NOS_IMAGE_USAGE_TRANSFER_DST);
		auto& Ext = Texture.external_memory;
		Ext.mutate_handle_type(NOS_EXTERNAL_MEMORY_HANDLE_TYPE_WIN32);
		Ext.mutate_handle((u64)ShaderOutput->GetExportInfo().Handle);
		Ext.mutate_allocation_size((u64)ShaderOutput->GetExportInfo().AllocationSize);
		Ext.mutate_pid((u64)ShaderOutput->GetExportInfo().PID);
		Texture.unmanaged = false;
		Texture.unscaled = true;
		Texture.handle = 0;
		Texture.offset = ShaderOutput->GetExportInfo().Offset;

		flatbuffers::FlatBufferBuilder fb;
		auto offset1 = nos::sys::vulkan::CreateTexture(fb, &Texture);
		fb.Finish(offset1);
		nos::Buffer buffer = fb.Release();
		std::vector<uint8_t>data = buffer;

		size_t numBytes = 16;
		std::vector<uint8_t> randomBytes = generateRandomBytes(numBytes);

		std::vector<flatbuffers::Offset<nos::fb::Pin>> pins;
		flatbuffers::FlatBufferBuilder fbb;
		pins.push_back(nos::fb::CreatePinDirect(fbb, (nos::fb::UUID*)randomBytes.data(), "Shader Output", "nos.sys.vulkan.Texture", nos::fb::ShowAs::OUTPUT_PIN, nos::fb::CanShowAs::OUTPUT_PIN_ONLY, "Shader Vars", 0, &data, 0, 0, 0, 0, 0, false, false, false, 0, 0, nos::fb::PinContents::JobPin, 0, 0, false, nos::fb::PinValueDisconnectBehavior::KEEP_LAST_VALUE, "Hello there!", "WHAT IS THAT OUTPUTTT!??"));
		auto offset = nos::CreatePartialNodeUpdateDirect(fbb, &eventDelegates->NodeId, nos::ClearFlags::NONE, 0, &pins, 0, 0, 0, 0);
		fbb.Finish(offset);
		auto buf = fbb.Release();
		auto root = flatbuffers::GetRoot<nos::PartialNodeUpdate>(buf.data());
		client->SendPartialNodeUpdate(*root);
	}


}



int main() 
{
	context = Context::New();
	if (context->Devices.empty())
		return 0;
	GVkDevice = context->Devices[0];
	pool = GVkDevice->GetPool();
	
	auto RP = CreatePass();

	InitWindow();
	CreateSurface();
	CreateSwapchain();



	ImageCreateInfo createInfo = {
		.Extent = {800, 600},
		.Format = VK_FORMAT_R8G8B8A8_UNORM,
		.Usage = VK_IMAGE_USAGE_SAMPLED_BIT |
				  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
				  VK_IMAGE_USAGE_STORAGE_BIT |
				  VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
				  VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.ExternalMemoryHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
	};

	VkResult re;
	ShaderInput = Image::New(GVkDevice.get(), createInfo, &re);
	if (re != VK_SUCCESS)
	{
		std::cout << "Failed to create input image" << std::endl;
		return 0;
	}
	ShaderOutput = Image::New(GVkDevice.get(), createInfo, &re);
	if (re != VK_SUCCESS)
	{
		std::cout << "Failed to create output image" << std::endl;
		return 0;
	}

	InitNosSDK();
	
	int frame = 0;
	while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

		uint32_t imageIndex; 
		GVkDevice->AcquireNextImageKHR(swapchain, 10000, WaitSemaphores[frame]->Handle, 0, &imageIndex);
		auto cmd = pool->BeginCmd();

		RP->BindResource("Input", ShaderInput, VkFilter::VK_FILTER_NEAREST);
		RP->TransitionInput(cmd, "Input", ShaderInput);

		//vk::Buffer
		//VertexData verts;
		//verts = VertexData{
		//	.Buffer = FindBuffer(v.Buffer),
		//	.VertexOffset = v.VertexOffset,
		//	.IndexOffset = v.IndexOffset,
		//	.NumIndices = v.IndexCount,
		//	.DepthWrite = (bool)v.DepthWrite,
		//	.DepthTest = (bool)v.DepthTest,
		//	.DepthFunc = (VkCompareOp)v.DepthFunc,
		//};

		Renderpass::ExecPassInfo info{
		  .BeginInfo = {.OutImage = ShaderOutput,
		  			  .DepthAttachment = std::nullopt,
		  			  .Wireframe = false,
		  			  .Clear = true,
		  			  .FrameNumber = 0,
		  			  .DeltaSeconds = 0.f,
		  			  .ClearCol = {0.0f,0.0f,0.0f,1.0f}},
		  	//.VtxData = ConvertVertices(params->Vertices, &verts)};
						  .VtxData = 0};

		RP->Exec(cmd, info);

		//auto texture = renderTarget->Download(cmd);

		//cmd->Submit();
		//cmd->Wait();

		//u8* textureRaw = texture->Map();
		//stbi_write_png("output.png", 800, 600, 4, textureRaw, 800 * 4);

		
		//copy from texture to swapchain image
		swapchainInfo.Images[imageIndex]->CopyFrom(cmd, ShaderOutput);

		swapchainInfo.Images[imageIndex]->Transition(cmd, ImageState{
				  .StageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
				  .AccessMask = VK_ACCESS_MEMORY_READ_BIT,
				  .Layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			});
		cmd->WaitGroup[WaitSemaphores[frame]->Handle] = {1, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
		cmd->SignalGroup[SignalSemaphores[frame]->Handle] = 1;

		cmd->Submit();
		cmd->Wait();

		VkPresentInfoKHR pi{};
		pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		pi.pSwapchains = &swapchain;
		pi.swapchainCount = 1;
		pi.pImageIndices = &imageIndex;
		pi.pWaitSemaphores = &SignalSemaphores[frame]->Handle;
		pi.waitSemaphoreCount = 1;
		VkResult res;
		pi.pResults = &res;
		NOSVK_ASSERT(GVkDevice->Queue->PresentKHR(&pi));

		frame = (frame + 1) % swapchainInfo.FrameCount;
    }



	vkDestroySurfaceKHR(context->Instance, surface, nullptr);
	glfwDestroyWindow(window);
    glfwTerminate();

	return 0;
}