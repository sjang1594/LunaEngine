#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>

#include "imgui.h"
#include "vulkan/vulkan.h"
#include "Layer.h" 

void check_vk_result(VkResult err);

struct GLFWwindow;

namespace Luna {
	struct ApplicationSpec
	{
		std::string Name = "LunaEngine App";
		uint32_t app_width = 1600;
		uint32_t app_height = 900;
	};

	class Application
	{
	public:
		Application(const ApplicationSpec& applicationSpec = ApplicationSpec());
		~Application();

		static Application& Get();

		void Run();
		void SetMenubarCallback(const std::function<void()>& menubarCallback) {}

		template<typename T>
		void PushLayer()
		{
			static_assert(std::is_base_of<Layer, T>::value, "Pushed type is not subclass of Layer");
			m_LayerStack.emplace_back(std::make_shared<T>())->OnAttach();
		}

		void PushLayer(const std::shared_ptr<Layer>& layer) { m_LayerStack.emplace_back(layer); layer->OnAttach(); }
		
		void Close();
		float GetTime() { return m_TimeStep; }
	
		GLFWwindow* GetWindowHandle() const { return m_WindowsHandle; }
		
		static VkInstance GetInstance();
		static VkPhysicalDevice GetPhysicalDevice();
		static VkDevice GetDevice();
	
		static VkCommandBuffer GetcommandBuffer(bool begin);
		static void FlushCommandBuffer(VkCommandBuffer commandBuffer);
		static void SubmitResourceFree(std::function<void()>&& func);
	
	private:
		void Init();
		void ShutDown();

	private:
		ApplicationSpec						m_spec;
		GLFWwindow*							m_WindowsHandle = nullptr;
		bool								m_running = false;
		float								m_TimeStep = 0.0f;
		float								m_LastFrameTime = 0.0f;

		std::vector<std::shared_ptr<Layer>> m_LayerStack;
		std::function<void()>				m_menubarCallback;
	};

	Application* CreateApplication(int argc, char** argv);
}

